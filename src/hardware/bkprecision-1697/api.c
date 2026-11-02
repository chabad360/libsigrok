/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>
#include <string.h>
#include "protocol.h"

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_POWER_SUPPLY,
};

static const uint32_t devopts[] = {
	SR_CONF_CONN | SR_CONF_GET,
	SR_CONF_CONTINUOUS,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_MSEC | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_VOLTAGE | SR_CONF_GET,
	SR_CONF_CURRENT | SR_CONF_GET,
	SR_CONF_VOLTAGE_TARGET | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_CURRENT_LIMIT | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_ENABLED | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_REGULATION | SR_CONF_GET,
};

static void clear_private(void *priv)
{
	struct dev_context *devc;

	devc = priv;
	g_mutex_clear(&devc->mutex);
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di, clear_private);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	if (devc->opened) {
		ret = devc->faulted ? SR_ERR : SR_OK;
		goto out;
	}
	ret = serial_open(sdi->conn, SERIAL_RDWR);
	if (ret != SR_OK)
		goto out;
	/* Let any previous bounded transaction finish before discarding stale
	 * input on an explicit reconnect. Never purge within a transaction. */
	g_usleep(2000000);
	ret = serial_flush(sdi->conn);
	if (ret != SR_OK) {
		serial_close(sdi->conn);
		goto out;
	}
	devc->opened = TRUE;
	devc->faulted = FALSE;
	devc->next_command = g_get_monotonic_time() + 100000;
	/* Revalidate after reconnect, using read-only commands exclusively. */
	ret = bk1697_probe(sdi);
	if (ret != SR_OK) {
		serial_close(sdi->conn);
		devc->opened = FALSE;
	}
 out:
	g_mutex_unlock(&devc->mutex);
	return ret;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	ret = devc->opened ? serial_close(sdi->conn) : SR_OK;
	devc->opened = FALSE;
	/* In particular, no SOUT, remote/local or key-lock commands here. */
	g_mutex_unlock(&devc->mutex);
	return ret;
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	struct sr_config *src;
	GSList *l;
	const char *conn;

	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		if (src->key == SR_CONF_CONN)
			conn = g_variant_get_string(src->data, NULL);
		else
			return NULL;
	}
	/* The protocol has no model ID. Never enumerate/probe arbitrary ports:
	 * the explicit connection is the user's assertion that this is a 1697.
	 * GMAX/GETS/GETD/GPAL only establish protocol plausibility, not identity. */
	if (!conn || !*conn)
		return NULL;

	sdi = g_malloc0(sizeof(*sdi));
	sdi->status = SR_ST_INACTIVE;
	sdi->inst_type = SR_INST_SERIAL;
	sdi->vendor = g_strdup("BK Precision");
	sdi->model = g_strdup("1697");
	sdi->connection_id = g_strdup(conn);
	sdi->conn = sr_serial_dev_inst_new(conn, "9600/8n1");
	devc = g_malloc0(sizeof(*devc));
	sdi->priv = devc;
	g_mutex_init(&devc->mutex);
	sr_sw_limits_init(&devc->limits);
	if (dev_open(sdi) != SR_OK) {
		sr_serial_dev_inst_free(sdi->conn);
		clear_private(devc);
		g_free(devc);
		sr_dev_inst_free(sdi);
		return NULL;
	}
	dev_close(sdi);
	sr_channel_new(sdi, 0, SR_CHANNEL_ANALOG, TRUE, "V");
	sr_channel_new(sdi, 1, SR_CHANNEL_ANALOG, TRUE, "I");
	return std_scan_complete(di, g_slist_append(NULL, sdi));
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct bk1697_response r;
	int ret;

	(void)cg;
	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	ret = SR_OK;
	switch (key) {
	case SR_CONF_CONN:
		*data = g_variant_new_string(sdi->connection_id);
		break;
	case SR_CONF_LIMIT_SAMPLES:
	case SR_CONF_LIMIT_MSEC:
		ret = sr_sw_limits_config_get(&devc->limits, key, data);
		break;
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
		ret = bk1697_query(sdi, "GETS00", BK1697_VALUES, &r);
		if (ret == SR_OK)
			*data = g_variant_new_double(key == SR_CONF_VOLTAGE_TARGET ? r.voltage : r.current);
		break;
	case SR_CONF_VOLTAGE:
	case SR_CONF_CURRENT:
	case SR_CONF_REGULATION:
		ret = bk1697_query(sdi, "GETD00", BK1697_MEASUREMENT, &r);
		if (ret != SR_OK)
			break;
		if (key == SR_CONF_REGULATION)
			*data = g_variant_new_string(r.cc ? "CC" : "CV");
		else
			*data = g_variant_new_double(key == SR_CONF_VOLTAGE ? r.voltage : r.current);
		break;
	case SR_CONF_ENABLED:
		ret = bk1697_query(sdi, "GPAL00", BK1697_PANEL, &r);
		if (ret == SR_OK)
			*data = g_variant_new_boolean(r.enabled);
		break;
	default:
		ret = SR_ERR_NA;
	}
	g_mutex_unlock(&devc->mutex);
	return ret;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct bk1697_response r;
	unsigned int counts;
	char command[16];
	double value, maximum;
	int ret;

	(void)cg;
	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	ret = SR_OK;
	switch (key) {
	case SR_CONF_LIMIT_SAMPLES:
	case SR_CONF_LIMIT_MSEC:
		ret = sr_sw_limits_config_set(&devc->limits, key, data);
		break;
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
		value = g_variant_get_double(data);
		if (key == SR_CONF_VOLTAGE_TARGET) {
			maximum = MIN(40.0, devc->maximum.voltage);
			if (!bk1697_setting(value, 1.0, maximum, 10.0, &counts)) {
				ret = SR_ERR_ARG;
				break;
			}
			g_snprintf(command, sizeof(command), "VOLT00%03u", counts);
		} else {
			maximum = MIN(5.0, devc->maximum.current);
			if (!bk1697_setting(value, 0.01, maximum, 100.0, &counts)) {
				ret = SR_ERR_ARG;
				break;
			}
			g_snprintf(command, sizeof(command), "CURR00%03u", counts);
		}
		ret = bk1697_query(sdi, command, BK1697_ACK, &r);
		break;
	case SR_CONF_ENABLED:
		/* Zero enables output; one disables. Accept ACK-only firmware and
		 * manual V/I-plus-ACK, consuming both lines in the latter case. */
		ret = bk1697_query(sdi, g_variant_get_boolean(data) ? "SOUT000" : "SOUT001",
			BK1697_OUTPUT, &r);
		break;
	default:
		ret = SR_ERR_NA;
	}
	g_mutex_unlock(&devc->mutex);
	return ret;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	devc = sdi ? sdi->priv : NULL;
	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
		if (!devc)
			return SR_ERR_ARG;
		g_mutex_lock(&devc->mutex);
		if (key == SR_CONF_VOLTAGE_TARGET)
			*data = std_gvar_min_max_step(1.0, MIN(40.0, devc->maximum.voltage), 0.1);
		else
			*data = std_gvar_min_max_step(0.01, MIN(5.0, devc->maximum.current), 0.01);
		g_mutex_unlock(&devc->mutex);
		return SR_OK;
	default:
		return SR_ERR_NA;
	}
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc;
	int ret;

	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	if (!devc->opened || devc->faulted) {
		g_mutex_unlock(&devc->mutex);
		return SR_ERR;
	}
	ret = serial_source_add(sdi->session, sdi->conn, G_IO_IN, 100,
		bk1697_receive_data, (void *)sdi);
	if (ret == SR_OK)
		sr_sw_limits_acquisition_start(&devc->limits);
	g_mutex_unlock(&devc->mutex);
	if (ret == SR_OK)
		std_session_send_df_header(sdi);
	return ret;
}

static struct sr_dev_driver bkprecision_1697_driver_info = {
	.name = "bkprecision-1697",
	.longname = "BK Precision 1697 power supply",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = std_serial_dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(bkprecision_1697_driver_info);
