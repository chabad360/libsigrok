/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef LIBSIGROK_HARDWARE_BKPRECISION_1697_PARSE_H
#define LIBSIGROK_HARDWARE_BKPRECISION_1697_PARSE_H

#include <stddef.h>
#include <libsigrok/libsigrok.h>

/* Pure wire-format helpers, also compiled directly by the offline tests. */
enum bk1697_reply {
	BK1697_ACK = 10000,
	BK1697_VALUES,
	BK1697_MEASUREMENT,
	BK1697_PANEL,
	BK1697_OUTPUT,
};

struct bk1697_response {
	double voltage, current;
	int cc, enabled;
	int voltage_digits, current_digits;
};

/* 68 panel characters, CR, OK, CR. No LF or unsolicited trailing data. */
#define BK1697_REPLY_MAX 72

/* Returns 0 for incomplete, 1 for complete and valid, -1 for invalid. */
SR_PRIV int bk1697_parse_reply(const char *buf, size_t len, enum bk1697_reply kind,
	struct bk1697_response *response);
SR_PRIV int bk1697_valid_maximum(const struct bk1697_response *response);
/* Reject rather than round unsupported precision; allow float round trips. */
SR_PRIV int bk1697_setting(double value, double minimum, double maximum,
	double scale, unsigned int *counts);

#endif
