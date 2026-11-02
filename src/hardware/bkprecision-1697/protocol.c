/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>
#include <string.h>
#include "protocol.h"

#define COMMAND_INTERVAL_US 100000
#define TRANSACTION_TIMEOUT_US 2000000

static int fault(struct dev_context *devc)
{
	/* No retry, including OFF: a delayed reply could acknowledge the wrong
	 * command. Only an explicit close/open and read-only probe can recover. */
	devc->faulted = TRUE;
	sr_err("Serial transaction failed; close/reopen the device before continuing.");
	return SR_ERR_DATA;
}

SR_PRIV int bk1697_query(const struct sr_dev_inst *sdi, const char *command,
	enum bk1697_reply kind, struct bk1697_response *response)
{
	struct dev_context *devc;
	struct sr_serial_dev_inst *serial;
	char frame[16], reply[BK1697_REPLY_MAX], byte;
	int ret, timeout;
	size_t len, frame_len;
	int64_t delay, deadline, remaining;

	devc = sdi->priv;
	serial = sdi->conn;
	if (!devc->opened || devc->faulted)
		return SR_ERR;

	delay = devc->next_command - g_get_monotonic_time();
	if (delay > 0)
		g_usleep(delay);
	/* Never silently flush an unexpected old response before a command. */
	ret = serial_read_nonblocking(serial, &byte, 1);
	if (ret != 0)
		return fault(devc);
	frame_len = g_snprintf(frame, sizeof(frame), "%s\r", command);
	if (frame_len >= sizeof(frame))
		return SR_ERR_ARG;
	deadline = g_get_monotonic_time() + TRANSACTION_TIMEOUT_US;
	ret = serial_write_blocking(serial, frame, frame_len, 500);
	if (ret != (int)frame_len)
		return fault(devc);

	len = 0;
	while (len < sizeof(reply)) {
		remaining = deadline - g_get_monotonic_time();
		if (remaining <= 0)
			return fault(devc);
		timeout = MIN(500, (remaining + 999) / 1000);
		ret = serial_read_blocking(serial, reply + len, 1, timeout);
		if (ret != 1)
			return fault(devc);
		len++;
		ret = bk1697_parse_reply(reply, len, kind, response);
		if (ret < 0)
			return fault(devc);
		if (ret == 1) {
			devc->next_command = g_get_monotonic_time() + COMMAND_INTERVAL_US;
			return SR_OK;
		}
	}
	return fault(devc);
}

SR_PRIV int bk1697_probe(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	struct bk1697_response r;
	int ret;

	devc = sdi->priv;
	ret = bk1697_query(sdi, "GMAX00", BK1697_VALUES, &r);
	if (ret != SR_OK)
		return ret;
	if (!bk1697_valid_maximum(&r))
		return fault(devc);
	devc->maximum = r;
	ret = bk1697_query(sdi, "GETS00", BK1697_VALUES, &r);
	if (ret != SR_OK)
		return ret;
	/* Readback can legitimately include the 40.2 V / 5.02 A headroom. */
	if (r.voltage < 1.0 || r.voltage > devc->maximum.voltage ||
		r.current < 0.01 || r.current > devc->maximum.current)
		return fault(devc);
	ret = bk1697_query(sdi, "GETD00", BK1697_MEASUREMENT, &r);
	if (ret != SR_OK)
		return ret;
	if (r.voltage > 40.2 || r.current > 5.02)
		return fault(devc);
	return bk1697_query(sdi, "GPAL00", BK1697_PANEL, &r);
}

/* Called with the wire mutex held. Claim END once, including close, which
 * cannot use sr_dev_acquisition_stop() after sdi becomes inactive. */
SR_PRIV int bk1697_stop_locked(const struct sr_dev_inst *sdi, gboolean *end)
{
	struct dev_context *devc = sdi->priv;

	*end = FALSE;
	if (!devc->active)
		return SR_OK;
	devc->active = FALSE;
	devc->stopping = TRUE;
	/* If a packet is currently in a user callback, defer END until it
	 * unwinds. Otherwise another thread could deliver END before that
	 * packet reaches all callbacks. stopping blocks restart until END. */
	*end = !devc->sending;
	return serial_source_remove(sdi->session, sdi->conn);
}

SR_PRIV void bk1697_send_end(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	/* Session callbacks may re-enter stop/close/configuration. */
	std_session_send_df_end(sdi);
	g_mutex_lock(&devc->mutex);
	devc->stopping = FALSE;
	g_mutex_unlock(&devc->mutex);
}

/* Reserve one packet under the wire mutex, but never hold it across a
 * session callback. A stopped/restarted acquisition cannot inherit a sample
 * from a previous callback generation. */
SR_PRIV gboolean bk1697_send_begin(const struct sr_dev_inst *sdi, uint64_t generation)
{
	struct dev_context *devc = sdi->priv;
	gboolean send;

	g_mutex_lock(&devc->mutex);
	send = devc->active && devc->generation == generation && !devc->sending;
	if (send)
		devc->sending = TRUE;
	g_mutex_unlock(&devc->mutex);
	return send;
}

SR_PRIV void bk1697_send_done(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	gboolean end;

	g_mutex_lock(&devc->mutex);
	devc->sending = FALSE;
	end = devc->stopping;
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
}

SR_PRIV int bk1697_acquisition_stop(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	gboolean end;
	int ret;

	g_mutex_lock(&devc->mutex);
	ret = bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	return ret;
}

SR_PRIV int bk1697_receive_data(int fd, int revents, void *cb_data)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct bk1697_response r;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_analog analog;
	struct sr_analog_encoding encoding;
	struct sr_analog_meaning meaning;
	struct sr_analog_spec spec;
	struct sr_channel *ch;
	GSList *l;
	float value;
	gboolean stop, end;
	uint64_t generation;
	int ret;

	(void)fd;
	(void)revents;
	sdi = cb_data;
	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	if (!devc->active) {
		g_mutex_unlock(&devc->mutex);
		return FALSE;
	}
	generation = devc->generation;
	stop = sr_sw_limits_check(&devc->limits);
	ret = stop ? SR_ERR : bk1697_query(sdi, "GETD00", BK1697_MEASUREMENT, &r);
	end = FALSE;
	if (ret != SR_OK)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	if (ret != SR_OK)
		return TRUE;

	/* Each send can synchronously stop/close acquisition via user callbacks.
	 * Recheck state before every subsequent packet, without holding the
	 * wire mutex during any send. */
	if (!bk1697_send_begin(sdi, generation))
		return TRUE;
	std_session_send_df_frame_begin(sdi);
	bk1697_send_done(sdi);
	for (l = sdi->channels; l; l = l->next) {
		ch = l->data;
		if (!ch->enabled)
			continue;
		if (!bk1697_send_begin(sdi, generation))
			return TRUE;
		sr_analog_init(&analog, &encoding, &meaning, &spec,
			ch->index == 0 ? r.voltage_digits : r.current_digits);
		value = ch->index == 0 ? r.voltage : r.current;
		analog.data = &value;
		analog.num_samples = 1;
		meaning.channels = g_slist_append(NULL, ch);
		meaning.mq = ch->index == 0 ? SR_MQ_VOLTAGE : SR_MQ_CURRENT;
		meaning.unit = ch->index == 0 ? SR_UNIT_VOLT : SR_UNIT_AMPERE;
		meaning.mqflags = SR_MQFLAG_DC;
		packet.type = SR_DF_ANALOG;
		packet.payload = &analog;
		sr_session_send(sdi, &packet);
		g_slist_free(meaning.channels);
		bk1697_send_done(sdi);
	}
	if (!bk1697_send_begin(sdi, generation))
		return TRUE;
	std_session_send_df_frame_end(sdi);
	bk1697_send_done(sdi);
	if (!bk1697_send_begin(sdi, generation))
		return TRUE;
	sr_session_send_meta(sdi, SR_CONF_REGULATION,
		g_variant_new_string(r.cc ? "CC" : "CV"));
	bk1697_send_done(sdi);
	g_mutex_lock(&devc->mutex);
	if (!devc->active || devc->generation != generation) {
		g_mutex_unlock(&devc->mutex);
		return TRUE;
	}
	sr_sw_limits_update_samples_read(&devc->limits, 1);
	stop = sr_sw_limits_check(&devc->limits);
	end = FALSE;
	if (stop)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	return TRUE;
}
