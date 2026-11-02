/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <math.h>
#include <string.h>
#include "parse.h"

static unsigned int decimal(const char *p, size_t len)
{
	unsigned int value;

	value = 0;
	while (len--)
		value = value * 10 + (*p++ - '0');
	return value;
}

SR_PRIV int bk1697_parse_reply(const char *buf, size_t len, enum bk1697_reply kind,
	struct bk1697_response *response)
{
	size_t i, width, expected;
	const char *cr;
	struct bk1697_response r;

	if (len > BK1697_REPLY_MAX)
		return -1;
	cr = memchr(buf, '\r', len);
	width = cr ? (size_t)(cr - buf) : len;
	for (i = 0; i < width; i++) {
		if (buf[i] < 0x20 || buf[i] > 0x7e)
			return -1;
	}
	if (!cr)
		return len <= 68 ? 0 : -1;

	memset(&r, 0, sizeof(r));
	if (kind == BK1697_ACK || (kind == BK1697_OUTPUT && width == 2)) {
		if (width != 2 || memcmp(buf, "OK", 2))
			return -1;
		expected = 3;
	} else {
		switch (kind) {
		case BK1697_UPPER_VOLTAGE:
			if (width != 3)
				return -1;
			break;
		case BK1697_VALUES:
		case BK1697_OUTPUT:
			if (width != 6)
				return -1;
			break;
		case BK1697_MEASUREMENT:
			if (width != 7 && width != 9)
				return -1;
			break;
		case BK1697_PANEL:
			if (width != 68)
				return -1;
			break;
		default:
			return -1;
		}
		expected = width + 4;
		/* Validate even partial ACKs, without accepting CRLF. */
		for (i = width + 1; i < len && i < expected; i++) {
			if (buf[i] != "OK\r"[i - width - 1])
				return -1;
		}
		if (kind == BK1697_PANEL) {
			for (i = 0; i < width; i++) {
				if (buf[i] < '0' || buf[i] > '?')
					return -1;
			}
			/* Character 65 (index 64) is the active-low FAULT/OVP lamp. */
			if (buf[64] != '0' && buf[64] != '1')
				return -1;
			r.ovp_active = buf[64] == '0';
			/* Active-low ON and OFF indicators must be complementary. */
			if (memcmp(buf + 65, "01", 2) && memcmp(buf + 65, "10", 2))
				return -1;
			r.enabled = buf[65] == '0';
		} else {
			for (i = 0; i < width; i++) {
				if (buf[i] < '0' || buf[i] > '9')
					return -1;
			}
			if (kind == BK1697_MEASUREMENT) {
				if (buf[width - 1] != '0' && buf[width - 1] != '1')
					return -1;
				r.cc = buf[width - 1] == '1';
			}
			if (kind == BK1697_UPPER_VOLTAGE) {
				r.voltage = decimal(buf, 3) / 10.0;
				r.voltage_digits = 1;
			} else if (width == 9) {
				/* Captured firmware: XX.XX V / X.XXX A. Nonzero
				 * current scaling still needs physical load verification. */
				r.voltage = decimal(buf, 4) / 100.0;
				r.current = decimal(buf + 4, 4) / 1000.0;
				r.voltage_digits = 2;
				r.current_digits = 3;
			} else {
				r.voltage = decimal(buf, 3) / 10.0;
				r.current = decimal(buf + 3, 3) / 100.0;
				r.voltage_digits = 1;
				r.current_digits = 2;
			}
		}
	}
	if (len > expected)
		return -1;
	if (len < expected)
		return 0;
	*response = r;
	return 1;
}

SR_PRIV int bk1697_valid_maximum(const struct bk1697_response *r)
{
	/* Honor smaller limits; accept only the known 1697 envelope/headroom. */
	return r->voltage >= 1.0 && r->voltage <= 40.2 &&
		r->current >= 0.01 && r->current <= 5.02;
}

SR_PRIV int bk1697_setting(double value, double minimum, double maximum,
	double scale, unsigned int *counts)
{
	double scaled, rounded;

	if (!isfinite(value))
		return 0;
	scaled = value * scale;
	rounded = round(scaled);
	if (!isfinite(scaled) || rounded < minimum * scale ||
		rounded > maximum * scale || rounded > 999 ||
		fabs(scaled - rounded) > 1e-4)
		return 0;
	*counts = (unsigned int)rounded;
	return 1;
}
