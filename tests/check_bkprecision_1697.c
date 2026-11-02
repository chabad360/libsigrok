/*
 * This file is part of the libsigrok project.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone offline test executable. Compile this file with
 * src/hardware/bkprecision-1697/parse.c and GLib; no serial device or driver
 * enablement is needed. Do not link against internal libsigrok symbols.
 */

#include <math.h>
#include <string.h>
#include "../src/hardware/bkprecision-1697/parse.h"

static struct bk1697_response complete(const char *wire, enum bk1697_reply kind)
{
	struct bk1697_response r;
	size_t i, len;

	len = strlen(wire);
	/* Replay every byte boundary, including partial payload and partial OK. */
	for (i = 0; i < len; i++)
		g_assert_cmpint(bk1697_parse_reply(wire, i, kind, &r), ==, 0);
	g_assert_cmpint(bk1697_parse_reply(wire, len, kind, &r), ==, 1);
	return r;
}

static void captured_firmware(void)
{
	struct bk1697_response r;

	r = complete("402502\rOK\r", BK1697_VALUES);
	g_assert_true(bk1697_valid_maximum(&r));
	g_assert_cmpfloat(r.voltage, ==, 40.2);
	g_assert_cmpfloat(r.current, ==, 5.02);
	r = complete("010502\rOK\r", BK1697_VALUES);
	g_assert_cmpfloat(r.voltage, ==, 1.0);
	g_assert_cmpfloat(r.current, ==, 5.02);
	r = complete("000300000\rOK\r", BK1697_MEASUREMENT);
	g_assert_cmpfloat(r.voltage, ==, 0.03);
	g_assert_cmpfloat(r.current, ==, 0.0);
	g_assert_cmpint(r.cc, ==, 0);
	g_assert_cmpint(r.voltage_digits, ==, 2);
	g_assert_cmpint(r.current_digits, ==, 3);
	r = complete("00;?3?4?0;?3?3?3?03?3?3?3?000000000111100863?010>=3?5;11000110011101\rOK\r",
		BK1697_PANEL);
	g_assert_cmpint(r.enabled, ==, 0);
	r = complete("030100000\rOK\r", BK1697_MEASUREMENT);
	g_assert_cmpfloat(r.voltage, ==, 3.01);
	r = complete("00<?3?060;?3?3?3?0;?3?3?3?000000000111100<?3?010;?3?0611000110011011\rOK\r",
		BK1697_PANEL);
	g_assert_cmpint(r.enabled, ==, 1);
	complete("OK\r", BK1697_OUTPUT);
	complete("OK\r", BK1697_ACK);
}

static void manual_and_synthetic(void)
{
	struct bk1697_response r;

	r = complete("0104561\rOK\r", BK1697_MEASUREMENT);
	g_assert_cmpfloat(r.voltage, ==, 1.0);
	g_assert_cmpfloat(r.current, ==, 4.56);
	g_assert_cmpint(r.cc, ==, 1);
	g_assert_cmpint(r.voltage_digits, ==, 1);
	g_assert_cmpint(r.current_digits, ==, 2);
	r = complete("123456\rOK\r", BK1697_VALUES);
	g_assert_cmpfloat(r.voltage, ==, 12.3);
	g_assert_cmpfloat(r.current, ==, 4.56);
	complete("123456\rOK\r", BK1697_OUTPUT);
	/* Synthetic: nonzero nine-digit current scaling is not a load capture. */
	r = complete("012345601\rOK\r", BK1697_MEASUREMENT);
	g_assert_cmpfloat(r.voltage, ==, 1.23);
	g_assert_cmpfloat(r.current, ==, 4.56);
	g_assert_cmpint(r.cc, ==, 1);
}

static void malformed(void)
{
	static const char *bad[] = {
		"00100456\rOK\r", "01x4561\rOK\r", "0104562\rOK\r",
		"030x00000\rOK\r", "0301000x0\rOK\r", "030100002\rOK\r",
		"0104561\rNO\r", "0104561\nOK\n", "0104561\rOK\n",
		"0104561\r\nOK\r", "0104561\rOK\rOK\r",
	};
	static const char *bad_output[] = {
		"NO\r", "12345x\rOK\r", "12345\rOK\r", "OK\n", "OK\rOK\r",
	};
	struct bk1697_response r;
	char panel[73], oversized[74];
	size_t i;

	for (i = 0; i < G_N_ELEMENTS(bad); i++)
		g_assert_cmpint(bk1697_parse_reply(bad[i], strlen(bad[i]),
			BK1697_MEASUREMENT, &r), ==, -1);
	for (i = 0; i < G_N_ELEMENTS(bad_output); i++)
		g_assert_cmpint(bk1697_parse_reply(bad_output[i], strlen(bad_output[i]),
			BK1697_OUTPUT, &r), ==, -1);
	memset(panel, '1', 68);
	memcpy(panel + 68, "\rOK\r", 5);
	g_assert_cmpint(bk1697_parse_reply(panel, 72, BK1697_PANEL, &r), ==, -1);
	panel[65] = '0';
	g_assert_cmpint(bk1697_parse_reply(panel, 72, BK1697_PANEL, &r), ==, 1);
	panel[1] = '@';
	g_assert_cmpint(bk1697_parse_reply(panel, 72, BK1697_PANEL, &r), ==, -1);
	memset(oversized, '0', sizeof(oversized));
	g_assert_cmpint(bk1697_parse_reply(oversized, 69, BK1697_PANEL, &r), ==, -1);
	g_assert_cmpint(bk1697_parse_reply(oversized, sizeof(oversized), BK1697_PANEL, &r), ==, -1);
}

static void safe_settings(void)
{
	static const double bad_voltage[] = { -1, 0, 0.9, 40.1, 12.34, INFINITY, NAN };
	static const double bad_current[] = { -1, 0, 5.01, 0.001, INFINITY, NAN };
	struct bk1697_response r;
	unsigned int counts;
	size_t i;

	for (i = 0; i < G_N_ELEMENTS(bad_voltage); i++)
		g_assert_false(bk1697_setting(bad_voltage[i], 1, 40, 10, &counts));
	for (i = 0; i < G_N_ELEMENTS(bad_current); i++)
		g_assert_false(bk1697_setting(bad_current[i], 0.01, 5, 100, &counts));
	g_assert_true(bk1697_setting(40, 1, 40, 10, &counts));
	g_assert_cmpuint(counts, ==, 400);
	g_assert_true(bk1697_setting(5, 0.01, 5, 100, &counts));
	g_assert_cmpuint(counts, ==, 500);
	g_assert_true(bk1697_setting((double)12.3f, 1, 40, 10, &counts));
	g_assert_cmpuint(counts, ==, 123);
	g_assert_true(bk1697_setting((double)0.01f, 0.01, 5, 100, &counts));
	g_assert_cmpuint(counts, ==, 1);
	r = complete("100100\rOK\r", BK1697_VALUES);
	g_assert_true(bk1697_valid_maximum(&r));
	g_assert_false(bk1697_setting(12.3, 1, r.voltage, 10, &counts));
	g_assert_false(bk1697_setting(1.01, 0.01, r.current, 100, &counts));
	r = complete("600500\rOK\r", BK1697_VALUES);
	g_assert_false(bk1697_valid_maximum(&r));
	r = complete("400999\rOK\r", BK1697_VALUES);
	g_assert_false(bk1697_valid_maximum(&r));
	r = complete("000000\rOK\r", BK1697_VALUES);
	g_assert_false(bk1697_valid_maximum(&r));
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);
	g_test_add_func("/bk1697/captured-firmware", captured_firmware);
	g_test_add_func("/bk1697/manual-and-synthetic", manual_and_synthetic);
	g_test_add_func("/bk1697/malformed", malformed);
	g_test_add_func("/bk1697/safe-settings", safe_settings);
	return g_test_run();
}
