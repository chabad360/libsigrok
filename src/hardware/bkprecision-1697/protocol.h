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

struct dev_context {
	struct sr_sw_limits limits;
	GMutex mutex;
	gboolean opened, faulted;
	gboolean active, stopping, sending;
	uint64_t generation;
	int64_t next_command;
	struct bk1697_response maximum;
};

/* Query/probe callers hold devc->mutex throughout the wire transaction. */
SR_PRIV int bk1697_query(const struct sr_dev_inst *sdi, const char *command,
	enum bk1697_reply kind, struct bk1697_response *response);
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
