/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LIBSIGROK_HARDWARE_BKPRECISION_1697_PROTOCOL_H
#define LIBSIGROK_HARDWARE_BKPRECISION_1697_PROTOCOL_H

#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"
#include "parse.h"

#define LOG_PREFIX "bkprecision-1697"

enum bk1697_group { BK1697_GETD, BK1697_GETS, BK1697_GPAL, BK1697_GOVP, BK1697_GROUPS };

struct bk1697_snapshot {
	struct bk1697_response response;
	int64_t updated;
};

struct dev_context {
	struct sr_sw_limits limits;
	/* Lock order: wire mutex -> cache_mutex; never I/O under cache_mutex.
	 * limits use cache_mutex; maximum writers hold both, readers either. */
	GMutex mutex, cache_mutex;
	gboolean cache_ready, cache_active;
	struct bk1697_snapshot cache[BK1697_GROUPS];
	/* Under cache_mutex; reads/readbacks do not consume notifications.
	 * Zero means pending explicit-edit confirmation (valid limits >=1 V). */
	double notified_upper_voltage;
	unsigned int refresh_group;
	gboolean opened, faulted;
	gboolean active, stopping, sending;
	uint64_t generation;
	struct bk1697_response maximum;
};

SR_PRIV void bk1697_invalidate(struct dev_context *devc);
SR_PRIV int bk1697_get(const struct sr_dev_inst *sdi, enum bk1697_group group,
	struct bk1697_response *response);
/* Query/probe callers hold devc->mutex throughout the wire transaction. */
SR_PRIV int bk1697_query(const struct sr_dev_inst *sdi, const char *command,
	enum bk1697_reply kind, struct bk1697_response *response);
SR_PRIV int bk1697_query_group(const struct sr_dev_inst *sdi,
	enum bk1697_group group, struct bk1697_response *response);
SR_PRIV int bk1697_probe(const struct sr_dev_inst *sdi);
/* Stop under devc->mutex, then send END unlocked only if claimed by end. */
SR_PRIV int bk1697_stop_locked(const struct sr_dev_inst *sdi, gboolean *end);
SR_PRIV void bk1697_send_end(const struct sr_dev_inst *sdi);
/* Packet reservations lock internally; the actual send is always unlocked. */
SR_PRIV gboolean bk1697_send_begin(const struct sr_dev_inst *sdi, uint64_t generation);
SR_PRIV void bk1697_send_done(const struct sr_dev_inst *sdi);
SR_PRIV int bk1697_acquisition_stop(struct sr_dev_inst *sdi);
SR_PRIV int bk1697_receive_data(int fd, int revents, void *cb_data);

#endif
