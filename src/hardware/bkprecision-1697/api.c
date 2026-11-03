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
	SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE | SR_CONF_GET,
	SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
};

static void clear_private(void *priv)
{
	struct dev_context *devc;

	devc = priv;
	g_mutex_clear(&devc->mutex);
	g_mutex_clear(&devc->cache_mutex);
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
	bk1697_invalidate(devc);
	ret = serial_open(sdi->conn, SERIAL_RDWR);
	if (ret != SR_OK)
		goto out;
	/* Explicit open discards queued input before the read-only probe. */
	ret = serial_flush(sdi->conn);
	if (ret != SR_OK) {
		serial_close(sdi->conn);
		goto out;
	}
	devc->opened = TRUE;
	devc->faulted = FALSE;
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
	int ret, close_ret;
	gboolean end;

	devc = sdi->priv;
	g_mutex_lock(&devc->mutex);
	/* sr_dev_close() has already made sdi inactive, so the public
	 * acquisition-stop dispatcher cannot do this cleanup for us. */
	ret = bk1697_stop_locked(sdi, &end);
	close_ret = devc->opened ? serial_close(sdi->conn) : SR_OK;
	devc->opened = FALSE;
	bk1697_invalidate(devc);
	/* In particular, no SOUT, remote/local or key-lock commands here. */
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	return ret == SR_OK ? close_ret : ret;
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
	 * GMAX/GETS/GETD/GPAL/GOVP establish plausibility, not identity. */
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
	g_mutex_init(&devc->cache_mutex);
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
	ret = SR_OK;
	switch (key) {
	case SR_CONF_CONN:
		*data = g_variant_new_string(sdi->connection_id);
		break;
	case SR_CONF_LIMIT_SAMPLES:
	case SR_CONF_LIMIT_MSEC:
		g_mutex_lock(&devc->cache_mutex);
		ret = sr_sw_limits_config_get(&devc->limits, key, data);
		g_mutex_unlock(&devc->cache_mutex);
		break;
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
		ret = bk1697_get(sdi, BK1697_GETS, &r);
		if (ret == SR_OK)
			*data = g_variant_new_double(key == SR_CONF_VOLTAGE_TARGET ? r.voltage : r.current);
		break;
	case SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD:
		ret = bk1697_get(sdi, BK1697_GOVP, &r);
		if (ret == SR_OK)
			*data = g_variant_new_double(r.voltage);
		break;
	case SR_CONF_VOLTAGE:
	case SR_CONF_CURRENT:
	case SR_CONF_REGULATION:
		ret = bk1697_get(sdi, BK1697_GETD, &r);
		if (ret != SR_OK)
			break;
		if (key == SR_CONF_REGULATION)
			*data = g_variant_new_string(r.cc ? "CC" : "CV");
		else
			*data = g_variant_new_double(key == SR_CONF_VOLTAGE ? r.voltage : r.current);
		break;
	case SR_CONF_ENABLED:
	case SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE:
		ret = bk1697_get(sdi, BK1697_GPAL, &r);
		if (ret == SR_OK)
			*data = g_variant_new_boolean(key == SR_CONF_ENABLED ? r.enabled : r.ovp_active);
		break;
	default:
		ret = SR_ERR_NA;
	}
	return ret;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct bk1697_response r;
	unsigned int counts;
	char command[16];
	enum bk1697_group group;
	double value, maximum;
	gboolean end;
	int ret;

	(void)cg;
	devc = sdi->priv;
	if (key == SR_CONF_LIMIT_SAMPLES || key == SR_CONF_LIMIT_MSEC) {
		g_mutex_lock(&devc->cache_mutex);
		ret = sr_sw_limits_config_set(&devc->limits, key, data);
		g_mutex_unlock(&devc->cache_mutex);
		return ret;
	}
	g_mutex_lock(&devc->mutex);
	ret = SR_OK;
	switch (key) {
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
		value = g_variant_get_double(data);
		if (key == SR_CONF_VOLTAGE_TARGET) {
			maximum = MIN(40.0, devc->maximum.voltage);
			if (!bk1697_setting(value, 1.0, maximum, 10.0, &counts)) {
				ret = SR_ERR_ARG;
				break;
			}
			/* A fresh actual ceiling is required even during acquisition. */
			ret = bk1697_query_group(sdi, BK1697_GOVP, &r);
			if (ret != SR_OK)
				break;
			if (counts > (unsigned int)(r.voltage * 10.0 + 0.5)) {
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
		group = BK1697_GETS;
		ret = bk1697_query(sdi, command, BK1697_ACK, &r);
		break;
	case SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD:
		/* Confirm explicit edits once, even if validation rejects the request.
		 * Valid limits are >=1 V; zero leaves the next actual poll pending. */
		g_mutex_lock(&devc->cache_mutex);
		devc->notified_upper_voltage = 0;
		g_mutex_unlock(&devc->cache_mutex);
		value = g_variant_get_double(data);
		if (!bk1697_setting(value, 1.0, devc->maximum.voltage, 10.0, &counts)) {
			ret = SR_ERR_ARG;
			break;
		}
		/* Lowering behavior is undocumented: never change target or output. */
		ret = bk1697_query_group(sdi, BK1697_GETS, &r);
		if (ret != SR_OK)
			break;
		if (counts < (unsigned int)(r.voltage * 10.0 + 0.5)) {
			ret = SR_ERR_ARG;
			break;
		}
		group = BK1697_GOVP;
		g_snprintf(command, sizeof(command), "SOVP00%03u", counts);
		ret = bk1697_query(sdi, command, BK1697_ACK, &r);
		break;
	case SR_CONF_ENABLED:
		/* Zero enables output; one disables. Accept ACK-only firmware and
		 * manual V/I-plus-ACK, consuming both lines in the latter case. */
		group = BK1697_GPAL;
		ret = bk1697_query(sdi, g_variant_get_boolean(data) ? "SOUT000" : "SOUT001",
			BK1697_OUTPUT, &r);
		break;
	default:
		ret = SR_ERR_NA;
	}
	if (ret == SR_OK) {
		/* Retain the previous actual snapshot for active readers until
		 * readback, but expire idle reuse immediately after the ACK. */
		g_mutex_lock(&devc->cache_mutex);
		devc->cache[group].updated = 0;
		g_mutex_unlock(&devc->cache_mutex);
		/* Observed firmware drops immediate GETS after VOLT/CURR ACK:
		 * parent stress logs found 23/30 and 26/30 successes at 0 ms,
		 * both 30/30 at 1 ms. SOUT001->GPAL passed 30/30 without a gap.
		 * This is an observed workaround, not a manufacturer timing spec. */
		if (key == SR_CONF_VOLTAGE_TARGET || key == SR_CONF_CURRENT_LIMIT)
			g_usleep(1000);
		ret = bk1697_query_group(sdi, group, &r);
		if (ret == SR_OK && key == SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD &&
			r.voltage != counts / 10.0) {
			sr_err("Upper voltage limit SET was not applied: requested %.1f V, read back %.1f V.",
				counts / 10.0, r.voltage);
			/* Valid actual readback stays cached; no retry or transport fault. */
			ret = SR_ERR;
		}
	}
	end = FALSE;
	if (devc->faulted)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	return ret;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct bk1697_response r;
	int ret;

	devc = sdi ? sdi->priv : NULL;
	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		return STD_CONFIG_LIST(key, data, sdi, cg, scanopts, drvopts, devopts);
	case SR_CONF_VOLTAGE_TARGET:
	case SR_CONF_CURRENT_LIMIT:
	case SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD:
		if (!devc)
			return SR_ERR_ARG;
		if (key == SR_CONF_VOLTAGE_TARGET) {
			ret = bk1697_get(sdi, BK1697_GOVP, &r);
			if (ret != SR_OK)
				return ret;
		}
		g_mutex_lock(&devc->cache_mutex);
		if (key == SR_CONF_VOLTAGE_TARGET)
			*data = std_gvar_min_max_step(1.0,
				MIN(40.0, MIN(devc->maximum.voltage, r.voltage)), 0.1);
		else if (key == SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD)
			*data = std_gvar_min_max_step(1.0, devc->maximum.voltage, 0.1);
		else
			*data = std_gvar_min_max_step(0.01, MIN(5.0, devc->maximum.current), 0.01);
		g_mutex_unlock(&devc->cache_mutex);
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
	if (!devc->opened || devc->faulted || devc->active || devc->stopping) {
		g_mutex_unlock(&devc->mutex);
		return SR_ERR;
	}
	ret = sr_session_fd_source_add(sdi->session, (void *)sdi, -1, 0, 10,
		bk1697_receive_data, (void *)sdi);
	if (ret == SR_OK) {
		devc->active = TRUE;
		g_mutex_lock(&devc->cache_mutex);
		devc->cache_active = TRUE;
		g_mutex_unlock(&devc->cache_mutex);
		devc->generation++;
		/* Reserve HEADER before releasing the mutex: close/stop can then
		 * defer END until HEADER has reached every session callback. */
		devc->sending = TRUE;
		g_mutex_lock(&devc->cache_mutex);
		sr_sw_limits_acquisition_start(&devc->limits);
		g_mutex_unlock(&devc->cache_mutex);
	}
	g_mutex_unlock(&devc->mutex);
	if (ret == SR_OK) {
		std_session_send_df_header(sdi);
		bk1697_send_done(sdi);
	}
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
	.dev_acquisition_stop = bk1697_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(bkprecision_1697_driver_info);
