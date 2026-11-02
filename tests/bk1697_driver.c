/*
 * Offline integration tests for the real BK Precision 1697 driver.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Only the serial boundary is replaced, via GNU ld --wrap. The internal
 * header supplies boundary prototypes, not access to driver private state.
 * See bk1697-driver.md for the optional build/run hook.
 */
#include <config.h>
#include <math.h>
#include <string.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

enum injection {
	NORMAL, TIMEOUT, READ_ERROR, SHORT_WRITE, WRITE_ERROR, MALFORMED,
	MISSING_ACK, CRLF, OVERLONG, BAD_PANEL, PENDING, PRE_READ_ERROR,
	SLOW_REPLY
};

static struct {
	GMutex lock;
	GCond cond;
	GPtrArray *commands;
	GString *rx;
	gboolean opened, wide, output_data, enabled;
	gboolean ignore_setting, fail_readback, bad_measurement, bad_panel, over_voltage;
	gboolean hold, entered, contender, release, configured;
	unsigned int opens, closes, flushes, adds, removes, prereads;
	unsigned int serial_sources, timer_calls;
	sr_receive_data_callback timer_cb;
	void *timer_data;
	unsigned int headers, ends, frames, frame_ends, analogs, metas, ovp_metas;
	unsigned int voltage_count, current_count;
	double voltage, current;
	char maximum[7];
	enum injection inject;
	gint64 completed;
	gboolean measure_gaps;
	gint64 max_command_gap, voltage_readback_min, current_readback_min, other_gap_total;
	unsigned int voltage_readbacks, current_readbacks;
	gint64 last_gets, last_gpal, gets_gap, gpal_gap;
} wire;
static struct sr_context *ctx;
static struct sr_dev_driver *driver;
static struct sr_dev_inst *device;
static struct sr_session *session;
static gboolean reentrant, stream_ended, close_packet, repeat_stop, live_status;
static gboolean observer_ended, setter_fault, setter_fault_readback;
static unsigned int observer_ends;
static int stop_packet;
static unsigned int ovp_mode, ovp_stage;

int __wrap_serial_open(struct sr_serial_dev_inst *serial, int flags)
{
	(void)serial;
	(void)flags;
	g_assert(!(wire.opened));
	wire.opened = TRUE;
	wire.opens++;
	return SR_OK;
}

int __wrap_serial_close(struct sr_serial_dev_inst *serial)
{
	(void)serial;
	g_assert(wire.opened);
	wire.opened = FALSE;
	wire.closes++;
	return SR_OK;
}

int __wrap_serial_flush(struct sr_serial_dev_inst *serial)
{
	(void)serial;
	g_assert(wire.opened);
	/* Discard queued input on explicit open, never within a transaction. */
	g_string_truncate(wire.rx, 0);
	wire.completed = 0;
	wire.flushes++;
	return SR_OK;
}

int __wrap_serial_read_nonblocking(struct sr_serial_dev_inst *serial,
	void *buf, size_t count)
{
	(void)serial;
	g_assert_cmpuint(count, ==, 1);
	wire.prereads++;
	if (wire.inject == PRE_READ_ERROR) {
		wire.inject = NORMAL;
		return SR_ERR;
	}
	if (wire.inject == PENDING) {
		wire.inject = NORMAL;
		*(char *)buf = 'X';
		return 1;
	}
	if (!wire.rx->len)
		return 0;
	*(char *)buf = wire.rx->str[0];
	g_string_erase(wire.rx, 0, 1);
	return 1;
}

int __wrap_serial_write_blocking(struct sr_serial_dev_inst *serial,
	const void *buf, size_t count, unsigned int timeout_ms)
{
	char *command, panel[73];
	gint64 gap;
	(void)serial;
	g_assert(wire.opened);
	g_assert_cmpuint(timeout_ms, <=, 500);
	g_assert_cmpuint(count, >, 1);
	g_assert_cmpint(((const char *)buf)[count - 1], ==, '\r');
	g_assert_cmpuint(wire.rx->len, ==, 0);
	if (wire.measure_gaps && wire.completed) {
		gap = g_get_monotonic_time() - wire.completed;
		wire.max_command_gap = MAX(wire.max_command_gap, gap);
		if (g_str_has_prefix(g_ptr_array_index(wire.commands, wire.commands->len - 1), "VOLT00") &&
			count == 7 && !memcmp(buf, "GETS00\r", 7)) {
			wire.voltage_readback_min = MIN(wire.voltage_readback_min, gap);
			wire.voltage_readbacks++;
		} else if (g_str_has_prefix(g_ptr_array_index(wire.commands, wire.commands->len - 1), "CURR00") &&
			count == 7 && !memcmp(buf, "GETS00\r", 7)) {
			wire.current_readback_min = MIN(wire.current_readback_min, gap);
			wire.current_readbacks++;
		} else {
			wire.other_gap_total += gap;
		}
	}
	command = g_strndup(buf, count - 1);
	g_ptr_array_add(wire.commands, command);
	if (wire.inject == SHORT_WRITE || wire.inject == WRITE_ERROR) {
		int ret = wire.inject == SHORT_WRITE ? (int)count - 1 : SR_ERR;
		wire.inject = NORMAL;
		return ret;
	}
	if (wire.inject == TIMEOUT || wire.inject == READ_ERROR)
		return count;
	if (wire.inject != NORMAL && wire.inject != SLOW_REPLY) {
		switch (wire.inject) {
		case MALFORMED: g_string_assign(wire.rx, "12X050\rOK\r"); break;
		case MISSING_ACK: g_string_assign(wire.rx, "120050\r"); break;
		case CRLF: g_string_assign(wire.rx, "120050\r\nOK\r"); break;
		case OVERLONG:
			g_string_set_size(wire.rx, 80);
			memset(wire.rx->str, '1', 80);
			break;
		case BAD_PANEL:
			memset(panel, '0', 68);
			memcpy(panel + 68, "\rOK\r", 5);
			g_string_assign(wire.rx, panel);
			break;
		default: g_assert_not_reached();
		}
		wire.inject = NORMAL;
		return count;
	}
	if (!strcmp(command, "GMAX00"))
		g_string_printf(wire.rx, "%s\rOK\r", wire.maximum);
	else if (!strcmp(command, "GETS00")) {
		gint64 now = g_get_monotonic_time();
		if (wire.last_gets)
			wire.gets_gap = MAX(wire.gets_gap, now - wire.last_gets);
		wire.last_gets = now;
		g_string_printf(wire.rx, "%03u%03u\rOK\r",
			wire.voltage_count, wire.current_count);
		if (wire.fail_readback)
			wire.inject = READ_ERROR;
	}
	else if (!strcmp(command, "GETD00"))
		g_string_assign(wire.rx, wire.bad_measurement ? "4030000\rOK\r" :
			wire.wide ? "123405671\rOK\r" : "1230560\rOK\r");
	else if (!strcmp(command, "GPAL00")) {
		gint64 now = g_get_monotonic_time();
		if (wire.last_gpal)
			wire.gpal_gap = MAX(wire.gpal_gap, now - wire.last_gpal);
		wire.last_gpal = now;
		memset(panel, '0', 68);
		panel[64] = wire.over_voltage ? '0' : '1';
		memcpy(panel + 65, wire.bad_panel ? "00" : wire.enabled ? "01" : "10", 2);
		memcpy(panel + 68, "\rOK\r", 5);
		g_string_assign(wire.rx, panel);
	} else if (g_str_has_prefix(command, "VOLT00")) {
		g_assert_cmpuint(strlen(command), ==, 9);
		if (!wire.ignore_setting)
			wire.voltage_count = g_ascii_strtoull(command + 6, NULL, 10);
		g_string_assign(wire.rx, "OK\r");
	} else if (g_str_has_prefix(command, "CURR00")) {
		g_assert_cmpuint(strlen(command), ==, 9);
		if (!wire.ignore_setting)
			wire.current_count = g_ascii_strtoull(command + 6, NULL, 10);
		g_string_assign(wire.rx, "OK\r");
	} else if (!strcmp(command, "SOUT000") || !strcmp(command, "SOUT001")) {
		if (!wire.ignore_setting)
			wire.enabled = command[6] == '0';
		g_string_assign(wire.rx, wire.output_data ? "120050\rOK\r" : "OK\r");
	} else
		g_error("Unexpected command: %s", command);
	return count;
}

int __wrap_serial_read_blocking(struct sr_serial_dev_inst *serial, void *buf,
	size_t count, unsigned int timeout_ms)
{
	(void)serial;
	g_assert_cmpuint(count, ==, 1);
	g_assert_cmpuint(timeout_ms, >, 0);
	g_assert_cmpuint(timeout_ms, <=, 500);
	g_mutex_lock(&wire.lock);
	if (wire.hold) {
		wire.entered = TRUE;
		g_cond_broadcast(&wire.cond);
		while (!wire.release)
			g_cond_wait(&wire.cond, &wire.lock);
		wire.hold = FALSE;
	}
	g_mutex_unlock(&wire.lock);
	/* Bytes arrive, but the whole response exceeds the overall deadline. */
	if (wire.inject == SLOW_REPLY)
		g_usleep(MIN(timeout_ms, 400) * 1000UL);
	if (wire.inject == READ_ERROR) {
		wire.inject = NORMAL;
		return SR_ERR;
	}
	if (!wire.rx->len) {
		g_usleep((gulong)timeout_ms * 1000);
		wire.inject = NORMAL;
		return 0;
	}
	*(char *)buf = wire.rx->str[0];
	g_string_erase(wire.rx, 0, 1);
	if (!wire.rx->len)
		wire.completed = g_get_monotonic_time();
	return 1;
}

int __wrap_serial_source_add(struct sr_session *s,
	struct sr_serial_dev_inst *serial, int events, int timeout,
	sr_receive_data_callback cb, void *data)
{
	(void)s;
	(void)serial;
	(void)events;
	(void)timeout;
	(void)cb;
	(void)data;
	wire.serial_sources++;
	/* Never hide a serial-readiness source behind a fake timer. */
	return SR_ERR;
}

int __wrap_serial_source_remove(struct sr_session *s,
	struct sr_serial_dev_inst *serial)
{
	(void)s;
	(void)serial;
	return SR_ERR;
}

int __real_sr_session_fd_source_add(struct sr_session *s, void *key,
	gintptr fd, int events, int timeout, sr_receive_data_callback cb, void *data);
int __real_sr_session_source_remove_internal(struct sr_session *s, void *key);

static int timer_dispatch(int fd, int revents, void *data)
{
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpint(revents, ==, 0);
	wire.timer_calls++;
	return wire.timer_cb(fd, revents, data);
}

int __wrap_sr_session_fd_source_add(struct sr_session *s, void *key,
	gintptr fd, int events, int timeout, sr_receive_data_callback cb, void *data)
{
	g_assert(key == data);
	g_assert(data == device);
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpint(events, ==, 0);
	g_assert_cmpint(timeout, ==, 10);
	wire.adds++;
	wire.timer_cb = cb;
	wire.timer_data = data;
	return __real_sr_session_fd_source_add(s, key, fd, events, timeout,
		timer_dispatch, data);
}

int __wrap_sr_session_source_remove_internal(struct sr_session *s, void *key)
{
	g_assert(key == wire.timer_data);
	wire.removes++;
	return __real_sr_session_source_remove_internal(s, key);
}

static int set(uint32_t key, GVariant *value)
{
	int ret;
	g_variant_ref_sink(value);
	ret = sr_config_set(device, NULL, key, value);
	g_variant_unref(value);
	return ret;
}

static GVariant *get(uint32_t key)
{
	GVariant *value = NULL;
	g_assert_cmpint(sr_config_get(driver, device, NULL, key, &value), ==, SR_OK);
	g_assert((value) != NULL);
	return value;
}

static void check_double(uint32_t key, double expected)
{
	GVariant *value = get(key);
	g_assert(fabs(g_variant_get_double(value) - expected) < 0.00001);
	g_variant_unref(value);
}

static GSList *scan(const char *conn)
{
	struct sr_config option;
	GSList node = { &option, NULL };
	GSList *result;
	option.key = SR_CONF_CONN;
	option.data = g_variant_ref_sink(g_variant_new_string(conn));
	result = sr_driver_scan(driver, &node);
	g_variant_unref(option.data);
	return result;
}

static void probe_at(unsigned int at)
{
	static const char *expected[] = { "GMAX00", "GETS00", "GETD00", "GPAL00" };
	unsigned int i;
	for (i = 0; i < G_N_ELEMENTS(expected); i++)
		g_assert_cmpstr(g_ptr_array_index(wire.commands, at + i), ==, expected[i]);
}

static void setup(void)
{
	struct sr_dev_driver **drivers;
	GSList *devices;
	unsigned int i;
	memset(&wire, 0, sizeof(wire));
	g_mutex_init(&wire.lock);
	g_cond_init(&wire.cond);
	wire.commands = g_ptr_array_new_with_free_func(g_free);
	wire.rx = g_string_new(NULL);
	wire.voltage_count = 120;
	wire.current_count = 50;
	memcpy(wire.maximum, "402502", 7);
	g_assert_cmpint(sr_init(&ctx), ==, SR_OK);
	driver = NULL;
	drivers = sr_driver_list(ctx);
	for (i = 0; drivers[i]; i++)
		if (!strcmp(drivers[i]->name, "bkprecision-1697"))
			driver = drivers[i];
	g_assert((driver) != NULL);
	g_assert_cmpint(sr_driver_init(ctx, driver), ==, SR_OK);
	g_assert((sr_driver_scan(driver, NULL)) == NULL);
	g_assert((scan("")) == NULL);
	g_assert_cmpuint(wire.opens, ==, 0);
	devices = scan("/offline/bk1697-no-hardware");
	g_assert_cmpuint(g_slist_length(devices), ==, 1);
	device = devices->data;
	g_slist_free(devices);
	g_assert_cmpuint(wire.commands->len, ==, 4);
	probe_at(0);
	g_assert_cmpuint(wire.closes, ==, 1);
	g_assert(!(wire.opened));
	g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, 8);
	probe_at(4);
	g_assert_cmpint(sr_dev_open(device), ==, SR_ERR);
	g_assert_cmpuint(wire.commands->len, ==, 8);
	session = NULL;
	reentrant = FALSE;
	live_status = FALSE;
	stream_ended = FALSE;
	close_packet = FALSE;
	repeat_stop = FALSE;
	observer_ended = FALSE;
	setter_fault = FALSE;
	setter_fault_readback = FALSE;
	observer_ends = 0;
	stop_packet = -1;
	ovp_mode = ovp_stage = 0;
}

static void teardown(void)
{
	unsigned int count = wire.commands->len;
	if (session) {
		g_assert_cmpint(sr_session_destroy(session), ==, SR_OK);
		session = NULL;
	}
	g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
	g_assert_cmpint(sr_dev_close(device), ==, SR_ERR_DEV_CLOSED);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpint(sr_exit(ctx), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpuint(wire.opens, ==, wire.closes);
	g_ptr_array_free(wire.commands, TRUE);
	g_string_free(wire.rx, TRUE);
	g_mutex_clear(&wire.lock);
	g_cond_clear(&wire.cond);
}

static void test_settings(void)
{
	GVariant *value;
	unsigned int i, count;
	const double bad_v[] = { 0, 0.9, 40.1, 40.2, 12.34, NAN, INFINITY, -INFINITY };
	const double bad_i[] = { 0, -0.01, 5.01, 5.02, 0.015, NAN, INFINITY };
	setup();
	check_double(SR_CONF_VOLTAGE_TARGET, 12.0);
	check_double(SR_CONF_CURRENT_LIMIT, 0.5);
	check_double(SR_CONF_VOLTAGE, 12.3);
	check_double(SR_CONF_CURRENT, 0.56);
	value = get(SR_CONF_REGULATION);
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "CV");
	g_variant_unref(value);
	wire.wide = TRUE;
	/* Idle GETD cache must expire before switching the emulated format. */
	g_usleep(260000);
	check_double(SR_CONF_VOLTAGE, 12.34);
	check_double(SR_CONF_CURRENT, 0.567);
	value = get(SR_CONF_REGULATION);
	g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "CC");
	g_variant_unref(value);
	for (i = 0; i < 2; i++) {
		wire.output_data = i;
		g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(TRUE)), ==, SR_OK);
		value = get(SR_CONF_ENABLED);
		g_assert(g_variant_get_boolean(value));
		g_variant_unref(value);
		g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(FALSE)), ==, SR_OK);
		value = get(SR_CONF_ENABLED);
		g_assert(!(g_variant_get_boolean(value)));
		g_variant_unref(value);
	}
	count = wire.commands->len;
	for (i = 0; i < G_N_ELEMENTS(bad_v); i++)
		g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(bad_v[i])), ==, SR_ERR_ARG);
	for (i = 0; i < G_N_ELEMENTS(bad_i); i++)
		g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(bad_i[i])), ==, SR_ERR_ARG);
	g_assert_cmpuint(wire.commands->len, ==, count);
	check_double(SR_CONF_VOLTAGE_TARGET, 12.0);
	check_double(SR_CONF_CURRENT_LIMIT, 0.5);
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(1)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(0.01)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(40)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(5)), ==, SR_OK);
	check_double(SR_CONF_VOLTAGE_TARGET, 40);
	check_double(SR_CONF_CURRENT_LIMIT, 5);
	teardown();
}

static void reconnect(void)
{
	unsigned int count = wire.commands->len;
	GVariant *value = NULL;
	g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
	g_assert_cmpint(sr_config_get(driver, device, NULL, SR_CONF_VOLTAGE, &value), !=, SR_OK);
	g_assert(value == NULL);
	g_assert_cmpuint(wire.commands->len, ==, count);
	/* A late ACK must be flushed, not accepted by the next transaction. */
	g_string_assign(wire.rx, "OK\r");
	g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count + 4);
	probe_at(count);
	g_assert_cmpuint(wire.rx->len, ==, 0);
}

static void assert_latched(void)
{
	unsigned int count = wire.commands->len, prereads = wire.prereads;
	GVariant *value = NULL;
	g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(FALSE)), !=, SR_OK);
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), !=, SR_OK);
	g_assert_cmpint(sr_config_get(driver, device, NULL, SR_CONF_CURRENT, &value), !=, SR_OK);
	g_assert((value) == NULL);
	g_assert_cmpint(sr_config_get(driver, device, NULL,
		SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE, &value), !=, SR_OK);
	g_assert(value == NULL);
	g_assert_cmpint(sr_dev_open(device), !=, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpuint(wire.prereads, ==, prereads);
}

static void test_faults(void)
{
	static const enum injection faults[] = {
		TIMEOUT, READ_ERROR, SHORT_WRITE, WRITE_ERROR, MALFORMED,
		MISSING_ACK, CRLF, OVERLONG, BAD_PANEL, PENDING, PRE_READ_ERROR
	};
	unsigned int i, count;
	GVariant *value;
	setup();
	for (i = 0; i < G_N_ELEMENTS(faults); i++) {
		g_test_message("fault injection %u", faults[i]);
		/* Force the GPAL getter to the transport for fault injection. */
		if (faults[i] == BAD_PANEL)
			g_usleep(260000);
		wire.inject = faults[i];
		count = wire.commands->len;
		if (faults[i] == BAD_PANEL) {
			value = NULL;
			g_assert_cmpint(sr_config_get(driver, device, NULL, SR_CONF_ENABLED, &value), !=, SR_OK);
			g_assert((value) == NULL);
		} else
			g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(TRUE)), !=, SR_OK);
		g_assert_cmpuint(wire.commands->len, ==,
			count + (faults[i] == PENDING || faults[i] == PRE_READ_ERROR ? 0 : 1));
		assert_latched();
		reconnect();
		check_double(SR_CONF_VOLTAGE_TARGET, 12);
	}
	teardown();
}

static void test_deadline(void)
{
	GVariant *value = NULL;
	gint64 start, elapsed;
	setup();
	/* Do not let the probe-seeded snapshot mask a transport deadline test. */
	g_usleep(260000);
	wire.inject = SLOW_REPLY;
	start = g_get_monotonic_time();
	g_assert_cmpint(sr_config_get(driver, device, NULL, SR_CONF_VOLTAGE, &value), !=, SR_OK);
	elapsed = g_get_monotonic_time() - start;
	g_assert((value) == NULL);
	g_assert_cmpint(elapsed, >=, 2000000);
	/* Allow scheduler slack, not another reply deadline. */
	g_assert_cmpint(elapsed, <, 3000000);
	assert_latched();
	wire.inject = NORMAL;
	reconnect();
	check_double(SR_CONF_VOLTAGE, 12.3);
	teardown();
}

static void test_revalidation(void)
{
	unsigned int count;
	setup();
	g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
	memcpy(wire.maximum, "403502", 7);
	count = wire.commands->len;
	g_assert_cmpint(sr_dev_open(device), !=, SR_OK);
	g_assert(!(wire.opened));
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count), ==, "GMAX00");
	memcpy(wire.maximum, "402502", 7);
	wire.voltage_count = 403;
	count = wire.commands->len;
	g_assert_cmpint(sr_dev_open(device), !=, SR_OK);
	g_assert(!(wire.opened));
	g_assert_cmpuint(wire.commands->len, ==, count + 2);
	wire.voltage_count = 402;
	wire.current_count = 502;
	count = wire.commands->len;
	g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
	probe_at(count);
	/* Readback headroom is legal; writes above nominal remain forbidden. */
	check_double(SR_CONF_VOLTAGE_TARGET, 40.2);
	check_double(SR_CONF_CURRENT_LIMIT, 5.02);
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(40.2)), ==, SR_ERR_ARG);
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(5.02)), ==, SR_ERR_ARG);
	g_assert_cmpuint(wire.commands->len, ==, count);
	teardown();
}

static void feed(const struct sr_dev_inst *sdi,
	const struct sr_datafeed_packet *packet, void *data)
{
	const struct sr_datafeed_analog *analog;
	const struct sr_datafeed_meta *meta;
	const struct sr_config *config;
	GVariant *value;
	float sample;
	(void)sdi;
	(void)data;
	if (packet->type == SR_DF_HEADER)
		stream_ended = FALSE;
	else
		g_assert(!stream_ended);
	switch (packet->type) {
	case SR_DF_HEADER: wire.headers++; break;
	case SR_DF_END:
		wire.ends++;
		stream_ended = TRUE;
		/* A stop from END must be idempotent, not recursively emit END. */
		if (repeat_stop)
			g_assert_cmpint(sr_session_stop(session), ==, SR_OK);
		break;
	case SR_DF_FRAME_BEGIN: wire.frames++; break;
	case SR_DF_FRAME_END: wire.frame_ends++; break;
	case SR_DF_META:
		meta = packet->payload;
		g_assert_cmpuint(g_slist_length(meta->config), ==, 1);
		config = meta->config->data;
		if (config->key == SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE) {
			unsigned int count = wire.commands->len;
			wire.ovp_metas++;
			g_assert_cmpint(g_variant_get_boolean(config->data), ==, wire.over_voltage);
			value = get(SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE);
			g_assert_cmpint(g_variant_get_boolean(value), ==, wire.over_voltage);
			g_variant_unref(value);
			value = get(SR_CONF_ENABLED);
			g_assert_cmpint(g_variant_get_boolean(value), ==, wire.enabled);
			g_variant_unref(value);
			g_assert_cmpuint(wire.commands->len, ==, count);
			if (ovp_mode) {
				if (ovp_stage == 0) {
					g_assert(wire.over_voltage);
					ovp_stage = 1;
					wire.over_voltage = FALSE;
				} else {
					g_assert(!wire.over_voltage);
					ovp_stage = 2;
				}
				if (ovp_mode == 3)
					g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
				else if (ovp_mode == 2 || ovp_stage == 2)
					g_assert_cmpint(sr_session_stop(session), ==, SR_OK);
			}
		} else {
			wire.metas++;
			g_assert_cmpuint(config->key, ==, SR_CONF_REGULATION);
			g_assert_cmpstr(g_variant_get_string(config->data, NULL), ==, wire.wide ? "CC" : "CV");
		}
		break;
	case SR_DF_ANALOG:
		/* Keep acquisition active until the contending settings/readbacks
		 * finish. This also checks callbacks execute outside the wire lock. */
		g_mutex_lock(&wire.lock);
		while (wire.contender && !wire.configured)
			g_cond_wait(&wire.cond, &wire.lock);
		g_mutex_unlock(&wire.lock);
		wire.analogs++;
		analog = packet->payload;
		g_assert_cmpuint(analog->num_samples, ==, 1);
		g_assert_cmpuint(g_slist_length(analog->meaning->channels), ==, 1);
		g_assert_cmpint(sr_analog_to_float(analog, &sample), ==, SR_OK);
		if (analog->meaning->mq == SR_MQ_VOLTAGE) {
			wire.voltage = sample;
			g_assert_cmpint(analog->meaning->unit, ==, SR_UNIT_VOLT);
			g_assert_cmpint(analog->encoding->digits, ==, wire.wide ? 2 : 1);
		} else {
			g_assert_cmpint(analog->meaning->mq, ==, SR_MQ_CURRENT);
			wire.current = sample;
			g_assert_cmpint(analog->meaning->unit, ==, SR_UNIT_AMPERE);
			g_assert_cmpint(analog->encoding->digits, ==, wire.wide ? 3 : 2);
		}
		/* Re-entry would deadlock if the driver held its wire mutex here. */
		if (reentrant) {
			value = get(SR_CONF_ENABLED);
			g_variant_unref(value);
		}
		break;
	default: g_error("Unexpected packet type %d", packet->type);
	}
	if (setter_fault && packet->type == SR_DF_ANALOG) {
		setter_fault = FALSE;
		if (setter_fault_readback)
			wire.fail_readback = TRUE;
		else
			wire.inject = READ_ERROR;
		g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), !=, SR_OK);
		/* Source removal is immediate, but END must wait until this packet
		 * has reached the later observer callback. */
		g_assert_cmpuint(wire.removes, ==, 1);
		g_assert_cmpuint(wire.ends, ==, 0);
	}
	if (live_status && packet->type == SR_DF_FRAME_END && wire.frames >= 60) {
		unsigned int count = wire.commands->len;
		check_double(SR_CONF_VOLTAGE_TARGET, 13.5);
		check_double(SR_CONF_CURRENT_LIMIT, 0.78);
		value = get(SR_CONF_ENABLED);
		g_assert(g_variant_get_boolean(value));
		g_variant_unref(value);
		check_double(SR_CONF_VOLTAGE, 12.3);
		check_double(SR_CONF_CURRENT, 0.56);
		value = get(SR_CONF_REGULATION);
		g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "CV");
		g_variant_unref(value);
		g_assert_cmpuint(wire.commands->len, ==, count);
	}
	if (packet->type == stop_packet) {
		unsigned int count = wire.commands->len;
		stop_packet = -1;
		if (close_packet)
			g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
		else
			g_assert_cmpint(sr_session_stop(session), ==, SR_OK);
		g_assert_cmpuint(wire.commands->len, ==, count);
	}
}

static void new_session(uint64_t samples, uint64_t msec)
{
	/* Public libsigrok rejects zero limit writes; leave defaults untouched. */
	if (samples)
		g_assert_cmpint(set(SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(samples)), ==, SR_OK);
	if (msec)
		g_assert_cmpint(set(SR_CONF_LIMIT_MSEC, g_variant_new_uint64(msec)), ==, SR_OK);
	g_assert_cmpint(sr_session_new(ctx, &session), ==, SR_OK);
	g_assert_cmpint(sr_session_dev_add(session, device), ==, SR_OK);
	g_assert_cmpint(sr_session_datafeed_callback_add(session, feed, NULL), ==, SR_OK);
}

static void observe_order(const struct sr_dev_inst *sdi,
	const struct sr_datafeed_packet *packet, void *data)
{
	(void)sdi;
	(void)data;
	if (packet->type == SR_DF_HEADER)
		observer_ended = FALSE;
	else
		g_assert(!observer_ended);
	if (packet->type == SR_DF_END) {
		observer_ended = TRUE;
		observer_ends++;
	}
}

static void test_callback_stop(gconstpointer data)
{
	int kind = GPOINTER_TO_INT(data);
	unsigned int count;
	setup();
	new_session(0, 0);
	close_packet = (kind & 0x10000) != 0;
	repeat_stop = TRUE;
	/* The first callback stops; this later callback must still see the
	 * triggering packet before END, not receive it after a nested END. */
	g_assert_cmpint(sr_session_datafeed_callback_add(session, observe_order, NULL), ==, SR_OK);
	stop_packet = kind & 0xffff;
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpint(stop_packet, ==, -1);
	g_assert_cmpuint(observer_ends, ==, 1);
	g_assert_cmpuint(wire.headers, ==, 1);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpuint(wire.adds, ==, 1);
	g_assert_cmpuint(wire.removes, ==, 1);
	g_assert_cmpuint(wire.frames, ==, kind == SR_DF_HEADER ? 0 : 1);
	if ((kind & 0xffff) == SR_DF_ANALOG)
		g_assert_cmpuint(wire.analogs, ==, 1);
	count = wire.commands->len;
	if (close_packet) {
		g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
		probe_at(count);
		g_assert_cmpuint(wire.commands->len, ==, count + 4);
	}
	teardown();
}

static void test_close_active(void)
{
	unsigned int count;
	setup();
	new_session(0, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	count = wire.commands->len;
	g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
	/* Fail directly rather than leave the unfixed session hung in run(). */
	g_assert_cmpuint(wire.removes, ==, 1);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
	probe_at(count);
	count = wire.commands->len;
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpuint(wire.frames, ==, 0);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpint(sr_session_destroy(session), ==, SR_OK);
	session = NULL;
	/* Reopening alone must not resurrect acquisition; explicit start works. */
	new_session(1, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.frames, ==, 1);
	g_assert_cmpuint(wire.ends, ==, 2);
	g_assert_cmpuint(wire.adds, ==, wire.removes);
	teardown();
}

static void test_acquisition(void)
{
	unsigned int i;
	GVariant *value;
	setup();
	for (i = 0; i < 2; i++) {
		wire.wide = i;
		reentrant = TRUE;
		new_session(2, 0);
		value = get(SR_CONF_LIMIT_SAMPLES);
		g_assert_cmpuint(g_variant_get_uint64(value), ==, 2);
		g_variant_unref(value);
		g_assert_cmpint(sr_session_start(session), ==, SR_OK);
		g_assert_cmpint(sr_session_run(session), ==, SR_OK);
		g_assert_cmpuint(wire.headers, ==, i + 1);
		g_assert_cmpuint(wire.ends, ==, i + 1);
		g_assert_cmpuint(wire.frames, ==, (i + 1) * 2);
		g_assert_cmpuint(wire.frame_ends, ==, wire.frames);
		g_assert_cmpuint(wire.analogs, ==, (i + 1) * 4);
		g_assert_cmpuint(wire.metas, ==, wire.frames);
		g_assert(fabs(wire.voltage - (i ? 12.34 : 12.3)) < 0.00001);
		g_assert(fabs(wire.current - (i ? 0.567 : 0.56)) < 0.00001);
		g_assert_cmpint(sr_session_destroy(session), ==, SR_OK);
		session = NULL;
	}
	g_assert_cmpuint(wire.adds, ==, wire.removes);
	teardown();
}

static void test_time_limit(void)
{
	setup();
	new_session(0, 1);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.frames, ==, 0);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpuint(wire.adds, ==, wire.removes);
	teardown();
}

static void test_acquisition_fault(void)
{
	setup();
	new_session(2, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	wire.inject = TIMEOUT;
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.frames, ==, 0);
	g_assert_cmpuint(wire.ends, ==, 1);
	assert_latched();
	g_assert_cmpint(sr_session_destroy(session), ==, SR_OK);
	session = NULL;
	reconnect();
	new_session(1, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.frames, ==, 1);
	g_assert_cmpuint(wire.ends, ==, 2);
	teardown();
}

static gpointer configure_thread(gpointer unused)
{
	GVariant *value;
	(void)unused;
	g_mutex_lock(&wire.lock);
	while (!wire.entered)
		g_cond_wait(&wire.cond, &wire.lock);
	wire.contender = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13.2)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(0.67)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(TRUE)), ==, SR_OK);
	check_double(SR_CONF_VOLTAGE_TARGET, 13.2);
	check_double(SR_CONF_CURRENT_LIMIT, 0.67);
	value = get(SR_CONF_ENABLED);
	g_assert(g_variant_get_boolean(value));
	g_variant_unref(value);
	g_mutex_lock(&wire.lock);
	wire.configured = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	return NULL;
}

static gpointer release_thread(gpointer unused)
{
	unsigned int count, prereads;
	(void)unused;
	g_mutex_lock(&wire.lock);
	while (!wire.contender)
		g_cond_wait(&wire.cond, &wire.lock);
	count = wire.commands->len;
	prereads = wire.prereads;
	g_mutex_unlock(&wire.lock);
	/* Let the configuration thread contend with the in-flight GETD reply. */
	g_usleep(50000);
	g_mutex_lock(&wire.lock);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_assert_cmpuint(wire.prereads, ==, prereads);
	wire.release = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	return NULL;
}

static void test_concurrent(void)
{
	GThread *configurer, *releaser;
	setup();
	wire.wide = TRUE;
	wire.output_data = TRUE;
	new_session(3, 0);
	wire.hold = TRUE;
	configurer = g_thread_new("settings", configure_thread, NULL);
	releaser = g_thread_new("release", release_thread, NULL);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_thread_join(configurer);
	g_thread_join(releaser);
	g_assert_cmpuint(wire.frames, ==, 3);
	g_assert_cmpuint(wire.analogs, ==, 6);
	g_assert(fabs(wire.voltage - 12.34) < 0.00001);
	g_assert(fabs(wire.current - 0.567) < 0.00001);
	g_assert_cmpuint(wire.rx->len, ==, 0);
	g_assert_cmpuint(wire.ends, ==, 1);
	teardown();
}

static void test_smaller_maximum(void)
{
	GVariant *value;
	double low, high, step;
	unsigned int count;
	setup();
	memcpy(wire.maximum, "200200", 7);
	reconnect();
	g_assert_cmpint(sr_config_list(driver, device, NULL, SR_CONF_VOLTAGE_TARGET, &value), ==, SR_OK);
	g_assert_cmpuint(g_variant_n_children(value), ==, 3);
	g_variant_get_child(value, 0, "d", &low);
	g_variant_get_child(value, 1, "d", &high);
	g_variant_get_child(value, 2, "d", &step);
	g_assert(low == 1 && high == 20 && step == 0.1);
	g_variant_unref(value);
	g_assert_cmpint(sr_config_list(driver, device, NULL, SR_CONF_CURRENT_LIMIT, &value), ==, SR_OK);
	g_assert_cmpuint(g_variant_n_children(value), ==, 3);
	g_variant_get_child(value, 0, "d", &low);
	g_variant_get_child(value, 1, "d", &high);
	g_variant_get_child(value, 2, "d", &step);
	g_assert(low == 0.01 && high == 2 && step == 0.01);
	g_variant_unref(value);
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(20.1)), ==, SR_ERR_ARG);
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(2.01)), ==, SR_ERR_ARG);
	g_assert_cmpuint(wire.commands->len, ==, count);
	check_double(SR_CONF_VOLTAGE_TARGET, 12);
	check_double(SR_CONF_CURRENT_LIMIT, 0.5);
	teardown();
}

static gint64 getter_elapsed;
static unsigned int getter_commands;

static gpointer snapshot_getter(gpointer unused)
{
	static const uint32_t keys[] = { SR_CONF_VOLTAGE, SR_CONF_CURRENT,
		SR_CONF_REGULATION, SR_CONF_VOLTAGE_TARGET, SR_CONF_CURRENT_LIMIT,
		SR_CONF_ENABLED, SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE,
		SR_CONF_CONN, SR_CONF_LIMIT_SAMPLES };
	GVariant *value;
	gint64 start;
	unsigned int i, count;
	(void)unused;
	g_mutex_lock(&wire.lock);
	while (!wire.entered)
		g_cond_wait(&wire.cond, &wire.lock);
	count = wire.commands->len;
	g_mutex_unlock(&wire.lock);
	start = g_get_monotonic_time();
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		value = get(keys[i]);
		if (keys[i] == SR_CONF_VOLTAGE_TARGET)
			g_assert_cmpfloat(g_variant_get_double(value), ==, 12.0);
		g_variant_unref(value);
	}
	g_assert_cmpint(sr_config_list(driver, device, NULL, SR_CONF_VOLTAGE_TARGET, &value), ==, SR_OK);
	g_variant_unref(value);
	getter_elapsed = g_get_monotonic_time() - start;
	g_mutex_lock(&wire.lock);
	getter_commands = wire.commands->len - count;
	wire.configured = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	return NULL;
}

static gpointer snapshot_watchdog(gpointer unused)
{
	gint64 deadline;
	(void)unused;
	g_mutex_lock(&wire.lock);
	while (!wire.entered)
		g_cond_wait(&wire.cond, &wire.lock);
	deadline = g_get_monotonic_time() + 300000;
	while (!wire.configured && g_cond_wait_until(&wire.cond, &wire.lock, deadline))
		;
	/* Always release: the unfixed getter must fail boundedly, not deadlock. */
	wire.release = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	return NULL;
}

static void test_snapshot_latency(void)
{
	GThread *getter, *watchdog;
	setup();
	new_session(1, 0);
	wire.hold = TRUE;
	getter = g_thread_new("snapshot", snapshot_getter, NULL);
	watchdog = g_thread_new("watchdog", snapshot_watchdog, NULL);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_thread_join(getter);
	g_thread_join(watchdog);
	g_test_message("held-wire getters: %.3f ms, %u extra commands",
		getter_elapsed / 1000.0, getter_commands);
	g_assert_cmpint(getter_elapsed, <, 50000);
	g_assert_cmpuint(getter_commands, ==, 0);
	teardown();
}

static gpointer snapshot_setter(gpointer unused)
{
	(void)unused;
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), ==, SR_OK);
	return NULL;
}

static void test_active_setter_snapshot(void)
{
	GThread *setter, *getter, *watchdog;
	setup();
	new_session(1, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	/* Hold the setter ACK, not an acquisition reply. The previous actual
	 * GETS must remain available without borrowing the setter's wire lock. */
	wire.hold = TRUE;
	setter = g_thread_new("setter", snapshot_setter, NULL);
	getter = g_thread_new("snapshot", snapshot_getter, NULL);
	watchdog = g_thread_new("watchdog", snapshot_watchdog, NULL);
	g_thread_join(getter);
	g_thread_join(watchdog);
	g_thread_join(setter);
	g_test_message("active-setter getters: %.3f ms, %u extra commands",
		getter_elapsed / 1000.0, getter_commands);
	g_assert_cmpint(getter_elapsed, <, 50000);
	g_assert_cmpuint(getter_commands, ==, 0);
	check_double(SR_CONF_VOLTAGE_TARGET, 13);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	teardown();
}

static void test_idle_groups(void)
{
	GVariant *value;
	unsigned int count;
	gint64 start;
	setup();
	/* Probe seeds all groups. GETD and GPAL are still inside the idle TTL. */
	count = wire.commands->len;
	check_double(SR_CONF_VOLTAGE, 12.3);
	check_double(SR_CONF_CURRENT, 0.56);
	value = get(SR_CONF_REGULATION);
	g_variant_unref(value);
	value = get(SR_CONF_ENABLED);
	g_variant_unref(value);
	g_assert_cmpuint(wire.commands->len, ==, count);
	g_usleep(260000);
	wire.voltage_count = 135;
	wire.current_count = 78;
	count = wire.commands->len;
	start = g_get_monotonic_time();
	check_double(SR_CONF_VOLTAGE_TARGET, 13.5);
	check_double(SR_CONF_CURRENT_LIMIT, 0.78);
	g_test_message("idle GETS pair: %.3f ms", (g_get_monotonic_time() - start) / 1000.0);
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count), ==, "GETS00");
	count = wire.commands->len;
	check_double(SR_CONF_VOLTAGE, 12.3);
	check_double(SR_CONF_CURRENT, 0.56);
	value = get(SR_CONF_REGULATION);
	g_variant_unref(value);
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count), ==, "GETD00");
	wire.enabled = TRUE;
	count = wire.commands->len;
	value = get(SR_CONF_ENABLED);
	g_assert(g_variant_get_boolean(value));
	g_variant_unref(value);
	value = get(SR_CONF_ENABLED);
	g_variant_unref(value);
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count), ==, "GPAL00");
	g_usleep(260000);
	wire.voltage_count = 140;
	count = wire.commands->len;
	check_double(SR_CONF_VOLTAGE_TARGET, 14);
	check_double(SR_CONF_CURRENT_LIMIT, 0.78);
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	reconnect();
	check_double(SR_CONF_VOLTAGE_TARGET, 14);
	teardown();
}

static void test_actual_readback(void)
{
	GVariant *value;
	unsigned int count;
	setup();
	/* ACK does not prove the requested value became actual. */
	wire.ignore_setting = TRUE;
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count + 2);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count + 1), ==, "GETS00");
	check_double(SR_CONF_VOLTAGE_TARGET, 12);
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(1)), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count + 2);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count + 1), ==, "GETS00");
	check_double(SR_CONF_CURRENT_LIMIT, 0.5);
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(TRUE)), ==, SR_OK);
	g_assert_cmpuint(wire.commands->len, ==, count + 2);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count + 1), ==, "GPAL00");
	value = get(SR_CONF_ENABLED);
	g_assert(!g_variant_get_boolean(value));
	g_variant_unref(value);
	wire.fail_readback = TRUE;
	count = wire.commands->len;
	g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), !=, SR_OK);
	/* The write ACK must succeed; inject failure in its actual GETS reply. */
	g_assert_cmpuint(wire.commands->len, ==, count + 2);
	g_assert_cmpstr(g_ptr_array_index(wire.commands, count + 1), ==, "GETS00");
	assert_latched();
	wire.fail_readback = FALSE;
	reconnect();
	check_double(SR_CONF_VOLTAGE_TARGET, 12);
	teardown();
}

static void test_live_status(void)
{
	unsigned int start, i, measurements = 0, settings = 0, panels = 0;
	const char *command;
	setup();
	wire.voltage_count = 135;
	wire.current_count = 78;
	wire.enabled = TRUE;
	live_status = TRUE;
	wire.last_gets = wire.last_gpal = g_get_monotonic_time();
	wire.gets_gap = wire.gpal_gap = 0;
	/* With no command spacing, run long enough for two 500 ms refreshes
	 * of each status group; retain exact GETD/sample accounting. */
	new_session(120, 0);
	start = wire.commands->len;
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	for (i = start; i < wire.commands->len; i++) {
		command = g_ptr_array_index(wire.commands, i);
		if (!strcmp(command, "GETD00"))
			measurements++;
		else if (!strcmp(command, "GETS00"))
			settings++;
		else if (!strcmp(command, "GPAL00"))
			panels++;
		else
			g_assert_not_reached();
	}
	g_assert_cmpuint(measurements, ==, 120);
	g_assert_cmpuint(wire.frames, ==, 120);
	g_assert_cmpuint(settings, >=, 2);
	g_assert_cmpuint(panels, >=, 2);
	/* Include the final partial interval, with timer/scheduler slack. */
	g_assert_cmpint(MAX(wire.gets_gap, g_get_monotonic_time() - wire.last_gets), <, 1100000);
	g_assert_cmpint(MAX(wire.gpal_gap, g_get_monotonic_time() - wire.last_gpal), <, 1100000);
	g_test_message("live status maximum gaps: GETS %.3f ms, GPAL %.3f ms",
		wire.gets_gap / 1000.0, wire.gpal_gap / 1000.0);
	teardown();
}

static void test_no_command_spacing(void)
{
	static const char *expected[] = {
		"VOLT00130", "GETS00", "CURR00070", "GETS00", "SOUT000", "GPAL00",
		"VOLT00130", "GETS00", "CURR00070", "GETS00", "SOUT001", "GPAL00"
	};
	unsigned int start, i;
	gint64 began, elapsed;

	setup();
	start = wire.commands->len;
	wire.measure_gaps = TRUE;
	wire.voltage_readback_min = wire.current_readback_min = G_MAXINT64;
	began = g_get_monotonic_time();
	for (i = 0; i < 2; i++) {
		g_assert_cmpint(set(SR_CONF_VOLTAGE_TARGET, g_variant_new_double(13)), ==, SR_OK);
		g_assert_cmpint(set(SR_CONF_CURRENT_LIMIT, g_variant_new_double(0.7)), ==, SR_OK);
		g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(i == 0)), ==, SR_OK);
	}
	elapsed = g_get_monotonic_time() - began;
	wire.measure_gaps = FALSE;
	/* All commands are actual writes/readbacks, not cache hits. No session
	 * timer, TTL expiry or artificial serial latency is in this window. */
	g_assert_cmpuint(wire.commands->len, ==, start + G_N_ELEMENTS(expected));
	for (i = 0; i < G_N_ELEMENTS(expected); i++)
		g_assert_cmpstr(g_ptr_array_index(wire.commands, start + i), ==, expected[i]);
	g_assert_cmpuint(wire.rx->len, ==, 0);
	g_test_message("12 uncached commands: %.3f ms total, %.3f ms maximum reply-to-command gap",
		elapsed / 1000.0, wire.max_command_gap / 1000.0);
	g_assert_cmpint(wire.max_command_gap, <, 80000);
	g_assert_cmpint(elapsed, <, 80000);
	check_double(SR_CONF_VOLTAGE_TARGET, 13);
	check_double(SR_CONF_CURRENT_LIMIT, 0.7);
	teardown();
}

static void test_ovp_getter(void)
{
	GVariant *value;
	unsigned int count;
	setup();
	count = wire.commands->len;
	value = get(SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE);
	g_assert(!g_variant_get_boolean(value));
	g_variant_unref(value);
	g_assert_cmpuint(wire.commands->len, ==, count);
	wire.over_voltage = TRUE;
	g_usleep(260000);
	value = get(SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE);
	g_assert(g_variant_get_boolean(value));
	g_variant_unref(value);
	value = get(SR_CONF_ENABLED);
	g_assert(!g_variant_get_boolean(value));
	g_variant_unref(value);
	g_assert_cmpuint(wire.commands->len, ==, count + 1);
	wire.over_voltage = FALSE;
	g_usleep(260000);
	value = get(SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE);
	g_assert(!g_variant_get_boolean(value));
	g_variant_unref(value);
	teardown();
}

static void test_ovp_poll(gconstpointer data)
{
	GVariant *value;
	unsigned int count;
	setup();
	value = get(SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE);
	g_assert(!g_variant_get_boolean(value));
	g_variant_unref(value);
	new_session(0, 1800); /* Bounded even if metadata never arrives (RED). */
	ovp_mode = GPOINTER_TO_UINT(data);
	wire.over_voltage = TRUE;
	g_assert_cmpint(sr_session_datafeed_callback_add(session, observe_order, NULL), ==, SR_OK);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(ovp_stage, ==, ovp_mode == 1 ? 2 : 1);
	g_assert_cmpuint(wire.ovp_metas, ==, ovp_mode == 1 ? 2 : 1);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpuint(observer_ends, ==, 1);
	g_assert_cmpuint(wire.frames, ==, wire.frame_ends);
	g_assert_cmpuint(wire.metas, ==, wire.frames);
	g_assert_cmpuint(wire.adds, ==, wire.removes);
	if (ovp_mode == 3) {
		count = wire.commands->len;
		g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
		probe_at(count);
	}
	teardown();
}

static void test_setting_readback_delay(void)
{
	/* Same 12 actual commands as the no-global-spacing regression. */
	test_no_command_spacing();
	g_test_message("VOLT->GETS minimum %.3f ms; CURR->GETS minimum %.3f ms; eight other gaps total %.3f ms",
		wire.voltage_readback_min / 1000.0, wire.current_readback_min / 1000.0,
		wire.other_gap_total / 1000.0);
	g_assert_cmpuint(wire.voltage_readbacks, ==, 2);
	g_assert_cmpint(wire.voltage_readback_min, >=, 1000);
	g_assert_cmpuint(wire.current_readbacks, ==, 2);
	g_assert_cmpint(wire.current_readback_min, >=, 1000);
	/* Generous aggregate scheduling slack, but a blanket 1 ms gap fails. */
	g_assert_cmpint(wire.other_gap_total, <, 5000);
}

static gpointer local_limits_thread(gpointer unused)
{
	GVariant *value;
	gint64 start;
	unsigned int count;
	(void)unused;
	g_mutex_lock(&wire.lock);
	while (!wire.entered)
		g_cond_wait(&wire.cond, &wire.lock);
	count = wire.commands->len;
	g_mutex_unlock(&wire.lock);
	start = g_get_monotonic_time();
	g_assert_cmpint(set(SR_CONF_LIMIT_SAMPLES, g_variant_new_uint64(2)), ==, SR_OK);
	g_assert_cmpint(set(SR_CONF_LIMIT_MSEC, g_variant_new_uint64(5000)), ==, SR_OK);
	value = get(SR_CONF_LIMIT_SAMPLES);
	g_assert_cmpuint(g_variant_get_uint64(value), ==, 2);
	g_variant_unref(value);
	value = get(SR_CONF_LIMIT_MSEC);
	g_assert_cmpuint(g_variant_get_uint64(value), ==, 5000);
	g_variant_unref(value);
	getter_elapsed = g_get_monotonic_time() - start;
	g_mutex_lock(&wire.lock);
	getter_commands = wire.commands->len - count;
	wire.configured = TRUE;
	g_cond_broadcast(&wire.cond);
	g_mutex_unlock(&wire.lock);
	return NULL;
}

static void test_held_wire_limits(void)
{
	GThread *limits, *watchdog;
	setup();
	new_session(2, 0);
	wire.hold = TRUE;
	limits = g_thread_new("limits", local_limits_thread, NULL);
	watchdog = g_thread_new("watchdog", snapshot_watchdog, NULL);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_thread_join(limits);
	g_thread_join(watchdog);
	g_test_message("held-wire limit set/get: %.3f ms, %u extra commands",
		getter_elapsed / 1000.0, getter_commands);
	g_assert_cmpint(getter_elapsed, <, 50000);
	g_assert_cmpuint(getter_commands, ==, 0);
	teardown();
}

static int companion_timer(int fd, int revents, void *data)
{
	unsigned int *calls = data;
	g_assert_cmpint(fd, ==, -1);
	g_assert_cmpint(revents, ==, 0);
	(*calls)++;
	return !wire.ends;
}

static void test_timer_source(void)
{
	unsigned int companion_calls = 0;
	setup();
	new_session(0, 100);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	/* Serial readiness must not be substituted by the test harness. */
	g_assert_cmpuint(wire.serial_sources, ==, 0);
	/* Another timer with the same fd but a different key coexists in this
	 * session. In particular the device does not reserve the shared -1 key. */
	g_assert_cmpint(__real_sr_session_fd_source_add(session, GINT_TO_POINTER(-1),
		-1, 0, 10, companion_timer, &companion_calls), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.timer_calls, >=, 2);
	g_assert_cmpuint(wire.timer_calls, <=, 20);
	g_assert_cmpuint(companion_calls, >=, 2);
	g_assert_cmpuint(wire.adds, ==, 1);
	g_assert_cmpuint(wire.removes, ==, 1);
	g_test_message("timer-only source: %u callbacks, companion %u, balanced removal",
		wire.timer_calls, companion_calls);
	teardown();
}

static void test_callback_setter_fault(gconstpointer data)
{
	setup();
	new_session(2, 0);
	setter_fault = TRUE;
	setter_fault_readback = GPOINTER_TO_INT(data);
	g_assert_cmpint(sr_session_datafeed_callback_add(session, observe_order, NULL), ==, SR_OK);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.analogs, ==, 1);
	g_assert_cmpuint(wire.frames, ==, 1);
	g_assert_cmpuint(wire.frame_ends, ==, 0);
	g_assert_cmpuint(wire.metas, ==, 0);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpuint(observer_ends, ==, 1);
	assert_latched();
	wire.fail_readback = FALSE;
	reconnect();
	teardown();
}

static void test_active_setter_fault(void)
{
	setup();
	new_session(2, 0);
	g_assert_cmpint(sr_session_start(session), ==, SR_OK);
	wire.inject = TIMEOUT;
	g_assert_cmpint(set(SR_CONF_ENABLED, g_variant_new_boolean(TRUE)), !=, SR_OK);
	/* A frontend need not run another poll to learn the acquisition ended. */
	g_assert_cmpuint(wire.removes, ==, 1);
	g_assert_cmpuint(wire.ends, ==, 1);
	g_assert_cmpint(sr_session_run(session), ==, SR_OK);
	g_assert_cmpuint(wire.frames, ==, 0);
	assert_latched();
	reconnect();
	teardown();
}

static void test_late_probe_validation(void)
{
	GVariant *value;
	unsigned int i, count;
	setup();
	for (i = 0; i < 2; i++) {
		g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
		wire.bad_measurement = i == 0;
		wire.bad_panel = i == 1;
		count = wire.commands->len;
		g_assert_cmpint(sr_dev_open(device), !=, SR_OK);
		g_assert(!wire.opened);
		g_assert_cmpuint(wire.commands->len, ==, count + (i ? 4 : 3));
		count = wire.commands->len;
		value = NULL;
		g_assert_cmpint(sr_config_get(driver, device, NULL, SR_CONF_VOLTAGE_TARGET, &value), !=, SR_OK);
		g_assert(value == NULL);
		g_assert_cmpuint(wire.commands->len, ==, count);
		wire.bad_measurement = wire.bad_panel = FALSE;
		g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
		g_assert_cmpuint(wire.commands->len, ==, count + 4);
		probe_at(count);
		check_double(SR_CONF_VOLTAGE_TARGET, 12);
	}
	teardown();
}

static void test_open_without_delay(void)
{
	unsigned int count, flushes;
	gint64 start, elapsed;

	setup();
	g_assert_cmpint(sr_dev_close(device), ==, SR_OK);
	g_string_assign(wire.rx, "stale\rOK\r");
	count = wire.commands->len;
	flushes = wire.flushes;
	start = g_get_monotonic_time();
	g_assert_cmpint(sr_dev_open(device), ==, SR_OK);
	elapsed = g_get_monotonic_time() - start;
	g_assert_cmpuint(wire.flushes, ==, flushes + 1);
	g_assert_cmpuint(wire.commands->len, ==, count + 4);
	probe_at(count);
	g_assert_cmpuint(wire.rx->len, ==, 0);
	g_test_message("open/flush/probe: %.3f ms", elapsed / 1000.0);
	g_assert_cmpint(elapsed, <, 80000);
	teardown();
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	sr_log_loglevel_set(SR_LOG_NONE);
	g_test_add_data_func("/bk1697/ovp-poll/activate-clear", GUINT_TO_POINTER(1), test_ovp_poll);
	g_test_add_data_func("/bk1697/ovp-poll/callback-stop", GUINT_TO_POINTER(2), test_ovp_poll);
	g_test_add_data_func("/bk1697/ovp-poll/callback-close", GUINT_TO_POINTER(3), test_ovp_poll);
	g_test_add_func("/bk1697/ovp-getter", test_ovp_getter);
	g_test_add_func("/bk1697/setting-readback-delay", test_setting_readback_delay);
	g_test_add_func("/bk1697/late-probe-validation", test_late_probe_validation);
	g_test_add_func("/bk1697/active-setter-fault", test_active_setter_fault);
	g_test_add_data_func("/bk1697/callback-setter-fault/ack", GINT_TO_POINTER(FALSE), test_callback_setter_fault);
	g_test_add_data_func("/bk1697/callback-setter-fault/readback", GINT_TO_POINTER(TRUE), test_callback_setter_fault);
	g_test_add_func("/bk1697/timer-source", test_timer_source);
	g_test_add_func("/bk1697/held-wire-limits", test_held_wire_limits);
	g_test_add_func("/bk1697/open-without-delay", test_open_without_delay);
	g_test_add_func("/bk1697/no-command-spacing", test_no_command_spacing);
	g_test_add_func("/bk1697/active-setter-snapshot", test_active_setter_snapshot);
	g_test_add_func("/bk1697/idle-groups", test_idle_groups);
	g_test_add_func("/bk1697/actual-readback", test_actual_readback);
	g_test_add_func("/bk1697/live-status", test_live_status);
	g_test_add_func("/bk1697/snapshot-latency", test_snapshot_latency);
	g_test_add_func("/bk1697/settings", test_settings);
	g_test_add_func("/bk1697/faults-reconnect", test_faults);
	g_test_add_func("/bk1697/deadline", test_deadline);
	g_test_add_func("/bk1697/revalidation", test_revalidation);
	g_test_add_data_func("/bk1697/callback-stop/header", GINT_TO_POINTER(SR_DF_HEADER), test_callback_stop);
	g_test_add_data_func("/bk1697/callback-stop/frame-begin", GINT_TO_POINTER(SR_DF_FRAME_BEGIN), test_callback_stop);
	g_test_add_data_func("/bk1697/callback-stop/analog", GINT_TO_POINTER(SR_DF_ANALOG), test_callback_stop);
	g_test_add_data_func("/bk1697/callback-stop/frame-end", GINT_TO_POINTER(SR_DF_FRAME_END), test_callback_stop);
	g_test_add_data_func("/bk1697/callback-stop/meta", GINT_TO_POINTER(SR_DF_META), test_callback_stop);
	g_test_add_data_func("/bk1697/callback-close/analog", GINT_TO_POINTER(SR_DF_ANALOG | 0x10000), test_callback_stop);
	g_test_add_func("/bk1697/close-active", test_close_active);
	g_test_add_func("/bk1697/acquisition", test_acquisition);
	g_test_add_func("/bk1697/time-limit", test_time_limit);
	g_test_add_func("/bk1697/acquisition-fault", test_acquisition_fault);
	g_test_add_func("/bk1697/concurrent", test_concurrent);
	g_test_add_func("/bk1697/smaller-maximum", test_smaller_maximum);
	return g_test_run();
}
