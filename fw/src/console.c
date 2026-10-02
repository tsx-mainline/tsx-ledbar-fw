// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Text console on USB interface 0. One command per line (CR or LF).
 * Commands are case-insensitive. The stock commands that the host tools
 * use keep their names and their answer texts: tsx-ledbard reads
 * "output mode is" and "LED driver not initialized" from TLCOUTMODE.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "tsx.h"

#define LINE_MAX 96
#define ARGS_MAX 8

static char line[LINE_MAX];
static unsigned line_len;
static bool line_ready;

static const char *const color_names[NCOLORS] = { "RED", "GREEN", "BLUE" };

void console_rx(const uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		char c = (char)data[i];

		if (line_ready)
			break;		/* one line at a time */
		if (c == '\r' || c == '\n') {
			if (line_len) {
				line[line_len] = 0;
				line_ready = true;
			}
			continue;
		}
		if (c == '\b' || c == 127) {
			if (line_len)
				line_len--;
			continue;
		}
		if (line_len < LINE_MAX - 1 && c >= ' ')
			line[line_len++] = c;
	}
}

static bool eq(const char *a, const char *b)
{
	return a && strcasecmp(a, b) == 0;
}

static int color_arg(const char *s)
{
	for (int c = 0; c < NCOLORS; c++) {
		if (eq(s, color_names[c]))
			return c;
	}
	return -1;
}

static bool num_arg(const char *s, long lo, long hi, long *out)
{
	char *end;
	long v;

	if (!s || !*s)
		return false;
	v = strtol(s, &end, 0);
	if (*end || v < lo || v > hi)
		return false;
	*out = v;
	return true;
}

static bool rgb_args(char *const *a, uint8_t rgb[3])
{
	long v;

	for (int c = 0; c < 3; c++) {
		if (!num_arg(a[c], 0, 100, &v))
			return false;
		rgb[c] = (uint8_t)v;
	}
	return true;
}

static void print_err(int i, uint32_t ms, uint16_t sub, uint16_t cause)
{
	console_printf("  %d: %lu ms subsystem %u cause 0x%04X\r\n", i, (unsigned long)ms, sub, cause);
}

static void cmd_help(void)
{
	console_write(
		"VER                       firmware name and version\r\n"
		"CAPS                      capabilities of this firmware\r\n"
		"STATUS                    joins, duties, effect, uptime\r\n"
		"ERRLOG | CLEARERR         error log\r\n"
		"REBOOT                    restart the application\r\n"
		"IMGUPD                    restart into the bootloader (USB update mode)\r\n"
		"TLCOUTMODE COLOR NUM MODE output mode of a TLC59116 output (NUM 0-15 or ALL)\r\n"
		"TLCGROUPMODE COLOR MODE   0 = group dimming, 1 = group blinking\r\n"
		"TLCBRIGHTNESS COLOR NUM|GROUP PERCENT\r\n"
		"TLCRESET                  reset and init the LED driver chips\r\n"
		"SELFTEST LED ON|OFF [COLOR|ALL] [PERCENT]\r\n"
		"LED COLOR LEVEL|CONTROL|BLINK VALUE   the host joins, by hand\r\n"
		"FX OFF | FADE R G B MS | BLINK R G B ON_MS OFF_MS | BREATHE R G B MS\r\n"
		"FX RAINBOW MS [LEVEL] | SMOOTH MS | CAP PERCENT\r\n");
}

static void cmd_status(void)
{
	const struct led_state *s = leds_state();

	console_printf("level %u %u %u control %u %u %u blink %u %u %u\r\n",
		       s->level[0], s->level[1], s->level[2],
		       s->control[0], s->control[1], s->control[2],
		       s->blink_100ms[0], s->blink_100ms[1], s->blink_100ms[2]);
	uint8_t pwm[NCOLORS], grp[NCOLORS];

	for (int c = 0; c < NCOLORS; c++)
		tlc_get_dim(c, &pwm[c], &grp[c]);
	console_printf("duty %u %u %u fx %s smooth %lu ms cap %u%%\r\n",
		       s->duty[0], s->duty[1], s->duty[2], leds_fx_name(),
		       (unsigned long)leds_get_smooth(), leds_get_cap());
	console_printf("pwm:grp %u:%u %u:%u %u:%u (duty of 65535)\r\n",
		       pwm[0], grp[0], pwm[1], grp[1], pwm[2], grp[2]);
	console_printf("chips %s %s %s uptime %lu ms start %lu reset flags 0x%08lX errors %d\r\n",
		       tlc_ready(RED) ? "ok" : "BAD", tlc_ready(GREEN) ? "ok" : "BAD",
		       tlc_ready(BLUE) ? "ok" : "BAD", (unsigned long)millis(),
		       (unsigned long)guard_start_count(), (unsigned long)guard_reset_flags(),
		       errlog_count());
}

static void cmd_tlc(int argc, char **argv)
{
	long v, n;
	int c = color_arg(argv[1]);

	if (c < 0) {
		console_write("usage: see HELP\r\n");
		return;
	}
	if (!tlc_ready(c)) {
		console_write("LED driver not initialized!\r\n");
		return;
	}
	if (eq(argv[0], "TLCOUTMODE")) {
		int out = eq(argv[2], "ALL") ? -1 : -2;

		if (out == -2 && (!num_arg(argv[2], 0, 15, &n) || (out = (int)n) < 0)) {
			console_write("usage: TLCOUTMODE COLOR NUM|ALL [MODE]\r\n");
			return;
		}
		if (argc > 3) {
			if (!num_arg(argv[3], 0, 3, &v) || !tlc_set_out_mode(c, out, (int)v)) {
				console_write("TLCOUTMODE failed\r\n");
				return;
			}
		}
		console_printf("%s %d output mode is:%d\r\n", color_names[c], out,
			       tlc_get_out_mode(c, out < 0 ? 0 : out));
	} else if (eq(argv[0], "TLCGROUPMODE")) {
		if (!num_arg(argv[2], 0, 1, &v) || !tlc_set_group_blink(c, v == 1, 0)) {
			console_write("usage: TLCGROUPMODE COLOR 0|1\r\n");
			return;
		}
		console_printf("%s group mode is:%ld\r\n", color_names[c], v);
	} else if (eq(argv[0], "TLCBRIGHTNESS")) {
		bool ok;

		if (!num_arg(argv[3], 0, 100, &v)) {
			console_write("usage: TLCBRIGHTNESS COLOR NUM|GROUP PERCENT\r\n");
			return;
		}
		if (eq(argv[2], "GROUP"))
			ok = tlc_set_group_pwm(c, (uint8_t)(v * 255 / 100));
		else if (num_arg(argv[2], 0, 15, &n))
			ok = tlc_set_pwm(c, (int)n, (uint8_t)(v * 255 / 100));
		else
			ok = false;
		console_printf("%s brightness %s\r\n", color_names[c], ok ? "set" : "failed");
	}
}

static void cmd_selftest(int argc, char **argv)
{
	uint8_t rgb[3] = { 0, 0, 0 };
	long v = 100;

	if (argc < 3 || !eq(argv[1], "LED")) {
		console_write("usage: SELFTEST LED ON|OFF [COLOR|ALL] [PERCENT]\r\n");
		return;
	}
	if (eq(argv[2], "OFF")) {
		leds_direct_off();
		console_write("selftest off\r\n");
		return;
	}
	if (argc > 4 && !num_arg(argv[4], 0, 100, &v)) {
		console_write("bad percent\r\n");
		return;
	}
	if (argc > 3 && !eq(argv[3], "ALL")) {
		int c = color_arg(argv[3]);

		if (c < 0) {
			console_write("bad color\r\n");
			return;
		}
		rgb[c] = (uint8_t)v;
	} else {
		rgb[0] = rgb[1] = rgb[2] = (uint8_t)v;
	}
	leds_direct(rgb);
	console_printf("selftest on %u %u %u\r\n", rgb[0], rgb[1], rgb[2]);
}

static void cmd_led(int argc, char **argv)
{
	int c = color_arg(argv[1]);
	long v;

	if (argc < 4 || c < 0 || !num_arg(argv[3], 0, 600, &v)) {
		console_write("usage: LED COLOR LEVEL|CONTROL|BLINK VALUE\r\n");
		return;
	}
	if (eq(argv[2], "LEVEL"))
		leds_set_level(c, (unsigned)v);
	else if (eq(argv[2], "CONTROL"))
		leds_set_control(c, v != 0);
	else if (eq(argv[2], "BLINK"))
		leds_set_blink_time(c, (unsigned)v);
	else {
		console_write("usage: LED COLOR LEVEL|CONTROL|BLINK VALUE\r\n");
		return;
	}
	console_write("ok\r\n");
}

static void cmd_fx(int argc, char **argv)
{
	uint8_t rgb[3];
	long a, b;

	if (argc < 2) {
		console_printf("fx %s\r\n", leds_fx_name());
		return;
	}
	if (eq(argv[1], "OFF")) {
		leds_fx_off();
	} else if (eq(argv[1], "FADE") && argc >= 6 && rgb_args(argv + 2, rgb) &&
		   num_arg(argv[5], 0, 600000, &a)) {
		leds_fx_fade(rgb, (uint32_t)a);
	} else if (eq(argv[1], "BLINK") && argc >= 7 && rgb_args(argv + 2, rgb) &&
		   num_arg(argv[5], 1, 600000, &a) && num_arg(argv[6], 1, 600000, &b)) {
		leds_fx_blink(rgb, (uint32_t)a, (uint32_t)b);
	} else if (eq(argv[1], "BREATHE") && argc >= 6 && rgb_args(argv + 2, rgb) &&
		   num_arg(argv[5], 100, 600000, &a)) {
		leds_fx_breathe(rgb, (uint32_t)a);
	} else if (eq(argv[1], "RAINBOW") && argc >= 3 && num_arg(argv[2], 100, 600000, &a)) {
		b = 100;
		if (argc > 3 && !num_arg(argv[3], 0, 100, &b)) {
			console_write("bad level\r\n");
			return;
		}
		leds_fx_rainbow((uint32_t)a, (uint8_t)b);
	} else if (eq(argv[1], "SMOOTH") && argc >= 3 && num_arg(argv[2], 0, 60000, &a)) {
		leds_set_smooth((uint32_t)a);
	} else if (eq(argv[1], "CAP") && argc >= 3 && num_arg(argv[2], 10, 150, &a)) {
		leds_set_cap((unsigned)a);
	} else {
		console_write("usage: see HELP\r\n");
		return;
	}
	console_printf("fx %s\r\n", leds_fx_name());
}

static void run_line(void)
{
	char *argv[ARGS_MAX + 1];
	int argc = 0;
	char *p = line;

	while (*p && argc < ARGS_MAX) {
		while (*p == ' ' || *p == '\t')
			p++;
		if (!*p)
			break;
		argv[argc++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
		if (*p)
			*p++ = 0;
	}
	for (int i = argc; i <= ARGS_MAX; i++)
		argv[i] = NULL;
	if (argc == 0)
		return;

	if (eq(argv[0], "VER") || eq(argv[0], "VERSION")) {
		console_write(TSX_FW_NAME "\r\n");
	} else if (eq(argv[0], "CAPS")) {
		console_write("tsx-ledbar fade blink breathe rainbow smooth cap status\r\n");
	} else if (eq(argv[0], "HELP") || eq(argv[0], "?")) {
		cmd_help();
	} else if (eq(argv[0], "STATUS")) {
		cmd_status();
	} else if (eq(argv[0], "UPTIME")) {
		console_printf("%lu ms\r\n", (unsigned long)millis());
	} else if (eq(argv[0], "ERRLOG")) {
		console_printf("%d errors\r\n", errlog_count());
		errlog_each(print_err);
	} else if (eq(argv[0], "CLEARERR")) {
		errlog_clear();
		console_write("error log cleared\r\n");
	} else if (eq(argv[0], "REBOOT")) {
		console_write("Rebooting\r\n");
		delay_ms(50);
		system_reset();
	} else if (eq(argv[0], "IMGUPD")) {
		if (argc > 1 && eq(argv[1], "?")) {
			console_write(" IMGUPD - reboot into bootloader\r\n");
			return;
		}
		console_write("Rebooting into the bootloader\r\n");
		delay_ms(50);
		guard_request_bootloader();
	} else if (eq(argv[0], "TLCRESET")) {
		console_printf("TLC reset %s\r\n", tlc_init() ? "ok" : "FAILED");
	} else if (eq(argv[0], "TLCOUTMODE") || eq(argv[0], "TLCGROUPMODE") ||
		   eq(argv[0], "TLCBRIGHTNESS")) {
		if (argc < 3)
			console_write("usage: see HELP\r\n");
		else
			cmd_tlc(argc, argv);
	} else if (eq(argv[0], "SELFTEST")) {
		cmd_selftest(argc, argv);
	} else if (eq(argv[0], "LED")) {
		cmd_led(argc, argv);
	} else if (eq(argv[0], "FX")) {
		cmd_fx(argc, argv);
#ifdef TSX_QEMU
	} else if (eq(argv[0], "TRACE")) {
		extern bool leds_trace;

		leds_trace = argc > 1 && eq(argv[1], "ON");
		console_printf("trace %s\r\n", leds_trace ? "on" : "off");
#endif
	} else {
		console_printf("unknown command: %s (HELP for a list)\r\n", argv[0]);
	}
}

void console_init(void)
{
	line_len = 0;
	line_ready = false;
}

void console_poll(void)
{
	if (!line_ready)
		return;
	run_line();
	line_len = 0;
	line_ready = false;
}
