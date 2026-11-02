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

static const char *group_commands[] = { "GETD00", "GETS00", "GPAL00" };
static const enum bk1697_reply group_replies[] = {
	BK1697_MEASUREMENT, BK1697_VALUES, BK1697_PANEL
};

/* Called under the wire mutex. Invalidate all groups as one short operation. */
SR_PRIV void bk1697_invalidate(struct dev_context *devc)
{
	g_mutex_lock(&devc->cache_mutex);
	memset(devc->cache, 0, sizeof(devc->cache));
	devc->cache_ready = FALSE;
	g_mutex_unlock(&devc->cache_mutex);
}

static int cached(struct dev_context *devc, enum bk1697_group group,
	struct bk1697_response *response)
{
	int ret;

	g_mutex_lock(&devc->cache_mutex);
	ret = SR_ERR;
	if (devc->cache_ready && devc->cache[group].valid &&
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
		ret = bk1697_query(sdi, group_commands[group], group_replies[group], response);
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
	unsigned int group;
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
		if (ret == 1) {
			/* An acknowledged write expires idle reuse; active readers may
			 * retain the previous actual snapshot until mandatory readback.
			 * Any failed transaction invalidates every group via fault(). */
			if (kind == BK1697_ACK || kind == BK1697_OUTPUT) {
				group = kind == BK1697_OUTPUT ? BK1697_GPAL : BK1697_GETS;
				g_mutex_lock(&devc->cache_mutex);
				devc->cache[group].updated = 0;
				g_mutex_unlock(&devc->cache_mutex);
			}
			/* Publish only complete, validated actual replies, never ACKs. */
			if (!strcmp(command, "GETS00") &&
				(response->voltage < 1.0 || response->voltage > devc->maximum.voltage ||
				 response->current < 0.01 || response->current > devc->maximum.current))
				return fault(devc);
			if (!strcmp(command, "GETD00") &&
				(response->voltage > 40.2 || response->current > 5.02))
				return fault(devc);
			for (group = 0; group < BK1697_GROUPS; group++) {
				if (strcmp(command, group_commands[group]))
					continue;
				g_mutex_lock(&devc->cache_mutex);
				devc->cache[group].response = *response;
				devc->cache[group].updated = g_get_monotonic_time();
				devc->cache[group].valid = TRUE;
				g_mutex_unlock(&devc->cache_mutex);
			}
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
	g_mutex_lock(&devc->cache_mutex);
	devc->maximum = r;
	g_mutex_unlock(&devc->cache_mutex);
	ret = bk1697_query(sdi, "GETS00", BK1697_VALUES, &r);
	if (ret != SR_OK)
		return ret;
	/* Query validates the readback envelope before publishing snapshots. */
	ret = bk1697_query(sdi, "GETD00", BK1697_MEASUREMENT, &r);
	if (ret != SR_OK)
		return ret;
	ret = bk1697_query(sdi, "GPAL00", BK1697_PANEL, &r);
	if (ret == SR_OK) {
		g_mutex_lock(&devc->cache_mutex);
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
	group = BK1697_GETD;
	/* Measurement slots alternate with fair, age-gated status slots. */
	if (devc->refresh_group == 1 || devc->refresh_group == 3) {
		group = devc->refresh_group == 1 ? BK1697_GETS : BK1697_GPAL;
		updated = devc->cache[group].updated;
		if (g_get_monotonic_time() - updated < STATUS_REFRESH_US)
			group = BK1697_GETD;
	}
	g_mutex_unlock(&devc->cache_mutex);
	/* No inter-command delay: physical reads remain synchronous and retain
	 * the complete-reply framing and bounded deadline guarantees. */
	ret = stop ? SR_ERR : bk1697_query(sdi, group_commands[group], group_replies[group], &r);
	devc->refresh_group = (devc->refresh_group + 1) % 4;
	end = FALSE;
	if (ret != SR_OK)
		bk1697_stop_locked(sdi, &end);
	g_mutex_unlock(&devc->mutex);
	if (end)
		bk1697_send_end(sdi);
	if (ret != SR_OK || group != BK1697_GETD)
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
