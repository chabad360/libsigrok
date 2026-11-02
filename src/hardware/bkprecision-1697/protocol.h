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
	int64_t next_command;
	struct bk1697_response maximum;
};

/* All wire access and shared state require devc->mutex, held by the caller. */
SR_PRIV int bk1697_query(const struct sr_dev_inst *sdi, const char *command,
	enum bk1697_reply kind, struct bk1697_response *response);
SR_PRIV int bk1697_probe(const struct sr_dev_inst *sdi);
SR_PRIV int bk1697_receive_data(int fd, int revents, void *cb_data);

#endif
