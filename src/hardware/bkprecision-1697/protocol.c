/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <config.h>
#include <string.h>
#include "protocol.h"

#define TRANSACTION_TIMEOUT_US 2000000
#define CACHE_TTL_US 250000
#define STATUS_REFRESH_US 500000

static const struct {
	const char *command;
	enum bk1697_reply reply;
} group_queries[] = {
	{ "GETD00", BK1697_MEASUREMENT },
	{ "GETS00", BK1697_VALUES },
	{ "GPAL00", BK1697_PANEL },
	{ "GOVP00", BK1697_UPPER_VOLTAGE },
};

static const enum bk1697_group poll_sequence[] = {
	BK1697_GETD, BK1697_GETS, BK1697_GETD,
	BK1697_GPAL, BK1697_GETD, BK1697_GOVP,
};

/* Called under the wire mutex. Invalidate all groups as one short operation. */
SR_PRIV void bk1697_invalidate(struct dev_context *devc)
{
	g_mutex_lock(&devc->cache_mutex);
	devc->cache_ready = FALSE;
	g_mutex_unlock(&devc->cache_mutex);
}

static int cached(struct dev_context *devc, enum bk1697_group group,
	struct bk1697_response *response)
{
	int ret;

	g_mutex_lock(&devc->cache_mutex);
	ret = SR_ERR;
	if (devc->cache_ready &&
		(devc->cache_active || g_get_monotonic_time() -
		devc->cache[group].updated < CACHE_TTL_US)) {
		*response = devc->cache[group].response;
		ret = SR_OK;
	} else if (devc->cache_ready && !devc->cache_active) {
		ret = SR_ERR_NA; /* Idle miss: caller may acquire the wire lock. */
	}
	g_mutex_unlock(&devc->cache_mutex);
	return ret;
}

SR_PRIV int bk1697_get(const struct sr_dev_inst *sdi, enum bk1697_group group,
	struct bk1697_response *response)
{
	struct dev_context *devc = sdi->priv;
	int ret;

	ret = cached(devc, group, response);
	if (ret != SR_ERR_NA)
		return ret;
	/* Never acquire wire while holding cache. Recheck after a competing query. */
	g_mutex_lock(&devc->mutex);
	ret = cached(devc, group, response);
	if (ret == SR_ERR_NA)
		ret = bk1697_query_group(sdi, group, response);
	g_mutex_unlock(&devc->mutex);
	return ret;
}

static int fault(struct dev_context *devc)
{
	/* No retry, including OFF: a delayed reply could acknowledge the wrong
	 * command. Only an explicit close/open and read-only probe can recover. */
	devc->faulted = TRUE;
	bk1697_invalidate(devc);
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
	int64_t deadline, remaining;

	devc = sdi->priv;
	serial = sdi->conn;
	if (!devc->opened || devc->faulted)
		return SR_ERR;

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
		if (ret == 1)
			return SR_OK;
	}
	return fault(devc);
}

/* Caller holds wire. Only this typed path validates and publishes groups. */
SR_PRIV int bk1697_query_group(const struct sr_dev_inst *sdi,
	enum bk1697_group group, struct bk1697_response *response)
{
	struct dev_context *devc = sdi->priv;
	int ret;

	ret = bk1697_query(sdi, group_queries[group].command,
		group_queries[group].reply, response);
	if (ret != SR_OK)
		return ret;
	if (group == BK1697_GETS &&
		(response->voltage < 1.0 || response->voltage > devc->maximum.voltage ||
		 response->current < 0.01 || response->current > devc->maximum.current))
		return fault(devc);
	if (group == BK1697_GOVP &&
		(response->voltage < 1.0 || response->voltage > devc->maximum.voltage))
		return fault(devc);
	if (group == BK1697_GETD &&
		(response->voltage > 40.2 || response->current > 5.02))
		return fault(devc);
	g_mutex_lock(&devc->cache_mutex);
	devc->cache[group].response = *response;
	devc->cache[group].updated = g_get_monotonic_time();
	g_mutex_unlock(&devc->cache_mutex);
	return SR_OK;
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
	g_mutex_lock(&devc->cache_mutex);
	devc->maximum = r;
	g_mutex_unlock(&devc->cache_mutex);
	ret = bk1697_query_group(sdi, BK1697_GETS, &r);
	if (ret != SR_OK)
		return ret;
	/* Query validates the readback envelope before publishing snapshots. */
	ret = bk1697_query_group(sdi, BK1697_GETD, &r);
	if (ret != SR_OK)
		return ret;
	ret = bk1697_query_group(sdi, BK1697_GPAL, &r);
	if (ret != SR_OK)
		return ret;
	ret = bk1697_query_group(sdi, BK1697_GOVP, &r);
	if (ret == SR_OK) {
		g_mutex_lock(&devc->cache_mutex);
		devc->notified_upper_voltage = r.voltage;
		devc->cache_ready = TRUE;
		g_mutex_unlock(&devc->cache_mutex);
	}
	return ret;
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
	g_mutex_lock(&devc->cache_mutex);
	devc->cache_active = FALSE;
	g_mutex_unlock(&devc->cache_mutex);
	devc->stopping = TRUE;
	/* If a packet is currently in a user callback, defer END until it
	 * unwinds. Otherwise another thread could deliver END before that
	 * packet reaches all callbacks. stopping blocks restart until END. */
	*end = !devc->sending;
	return sr_session_source_remove_internal(sdi->session, (void *)sdi);
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
	gboolean stop, end, changed;
	uint64_t generation;
	int ret;
	enum bk1697_group group;
	int64_t updated;

	(void)fd;
	(void)revents;
	sdi = cb_data;
	devc = sdi->priv;
	if (!g_mutex_trylock(&devc->mutex))
		return TRUE;
	if (!devc->active) {
		g_mutex_unlock(&devc->mutex);
		return FALSE;
	}
	generation = devc->generation;
	g_mutex_lock(&devc->cache_mutex);
	stop = sr_sw_limits_check(&devc->limits);
	group = poll_sequence[devc->refresh_group];
	/* Fresh status slots fall back to measurement without delaying polling. */
	if (group != BK1697_GETD) {
		updated = devc->cache[group].updated;
		if (g_get_monotonic_time() - updated < STATUS_REFRESH_US)
			group = BK1697_GETD;
	}
	g_mutex_unlock(&devc->cache_mutex);
	/* No inter-command delay: physical reads remain synchronous and retain
	 * the complete-reply framing and bounded deadline guarantees. */
	ret = stop ? SR_ERR : bk1697_query_group(sdi, group, &r);
	devc->refresh_group = (devc->refresh_group + 1) % G_N_ELEMENTS(poll_sequence);
	end = FALSE;
	if (ret != SR_OK)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	if (ret != SR_OK)
		return TRUE;
	if (group == BK1697_GOVP) {
		if (bk1697_send_begin(sdi, generation)) {
			/* Reserve before consuming the change: a stopped generation must
			 * leave it pending, even if a read/setter already cached it. */
			g_mutex_lock(&devc->cache_mutex);
			changed = r.voltage != devc->notified_upper_voltage;
			if (changed)
				devc->notified_upper_voltage = r.voltage;
			g_mutex_unlock(&devc->cache_mutex);
			if (changed)
				sr_session_send_meta(sdi, SR_CONF_OVER_VOLTAGE_PROTECTION_THRESHOLD,
					g_variant_new_double(r.voltage));
			bk1697_send_done(sdi);
		}
		return TRUE;
	}
	if (group == BK1697_GPAL) {
		/* Like RDTech, report protection state as metadata. Reserve the
		 * packet so a reentrant stop/close still delivers END afterward. */
		if (bk1697_send_begin(sdi, generation)) {
			sr_session_send_meta(sdi, SR_CONF_OVER_VOLTAGE_PROTECTION_ACTIVE,
				g_variant_new_boolean(r.ovp_active));
			bk1697_send_done(sdi);
		}
		return TRUE;
	}
	if (group != BK1697_GETD)
		return TRUE;

	/* Each send can synchronously stop/close acquisition via user callbacks.
	 * Recheck state before every subsequent packet, without holding the
	 * wire mutex during any send. */
	if (!bk1697_send_begin(sdi, generation))
		return TRUE;
	std_session_send_frame_begin(sdi);
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
	std_session_send_frame_end(sdi);
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
	g_mutex_lock(&devc->cache_mutex);
	sr_sw_limits_update_samples_read(&devc->limits, 1);
	stop = sr_sw_limits_check(&devc->limits);
	g_mutex_unlock(&devc->cache_mutex);
	end = FALSE;
	if (stop)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	return TRUE;
}
