// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Text console on USB interface 0. One command per line (CR or LF).
 * Commands are case-insensitive. The stock commands that the host tools
 * use keep their names and their answer texts: tsx-ledbard reads
 * "output mode is" and "LED driver not initialized" from TLCOUTMODE.
 *
 * LED names: R1..R8 (right side, top to bottom), L1..L8 (left side, top
 * to bottom), or the index 0..15 (R1..R8 are 0..7, L1..L8 are 8..15).
 * The LED map (ledmap.c, LEDMAP) gives the chip output of each LED.
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "tsx.h"

#define LINE_MAX 96
#define ARGS_MAX 10
#define RX_RING	512

static char line[LINE_MAX];
static unsigned line_len;
static uint8_t rx_ring[RX_RING];
static unsigned rx_head, rx_tail;

static const char *const color_names[NCOLORS] = { "RED", "GREEN", "BLUE" };

/*
 * Received bytes go into a ring. One USB packet can hold more than one
 * line, so console_poll takes the lines out of the ring one by one. When
 * the ring is full, the rest of the data is lost.
 */
void console_rx(const uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned next = (rx_head + 1) % RX_RING;

		if (next == rx_tail)
			break;
		rx_ring[rx_head] = data[i];
		rx_head = next;
	}
}

/* move bytes from the ring into the line: true when a line is complete */
static bool line_from_ring(void)
{
	while (rx_tail != rx_head) {
		char c = (char)rx_ring[rx_tail];

		rx_tail = (rx_tail + 1) % RX_RING;
		if (c == '\r' || c == '\n') {
			if (line_len) {
				line[line_len] = 0;
				return true;
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
	return false;
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

/* "R3" or "L8" or "0".."15" to the LED index, -1 when bad */
static int led_id(const char *s, size_t n)
{
	int v = 0;

	if (n == 2 && (s[0] == 'R' || s[0] == 'r' || s[0] == 'L' || s[0] == 'l') &&
	    s[1] >= '1' && s[1] <= '8')
		return (s[0] == 'L' || s[0] == 'l' ? NROWS : 0) + s[1] - '1';
	if (n < 1 || n > 2)
		return -1;
	for (size_t i = 0; i < n; i++) {
		if (!isdigit((unsigned char)s[i]))
			return -1;
		v = v * 10 + s[i] - '0';
	}
	return v < NLEDS ? v : -1;
}

/*
 * LED selection: one LED (R3, L1, 5), a range by index (R1-R4, 8-11,
 * R7-L2), a side (R or L) or ALL.
 */
static bool which_arg(const char *s, int *first, int *last)
{
	const char *dash;
	int a, b;

	if (!s)
		return false;
	if (eq(s, "ALL")) {
		*first = 0;
		*last = NLEDS - 1;
		return true;
	}
	if (eq(s, "R") || eq(s, "L")) {
		*first = eq(s, "L") ? NROWS : 0;
		*last = *first + NROWS - 1;
		return true;
	}
	dash = strchr(s, '-');
	if (dash) {
		a = led_id(s, (size_t)(dash - s));
		b = led_id(dash + 1, strlen(dash + 1));
	} else {
		a = b = led_id(s, strlen(s));
	}
	if (a < 0 || b < 0)
		return false;
	*first = a < b ? a : b;
	*last = a < b ? b : a;
	return true;
}

static void led_name(int i, char name[4])
{
	name[0] = i < NROWS ? 'R' : 'L';
	name[1] = (char)('1' + i % NROWS);
	name[2] = 0;
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
		"STATUS                    joins, duties, effect, uptime, board variant\r\n"
		"LEDMAP [N|NAME|AUTO]      board variant and LED map: show, or use another map until the next start\r\n"
		"ERRLOG | CLEARERR         error log\r\n"
		"REBOOT                    restart the application\r\n"
		"IMGUPD                    restart into the bootloader (USB update mode)\r\n"
		"TLCOUTMODE COLOR NUM|ALL  output mode of a TLC59116 output\r\n"
		"TLCOUTMODE COLOR NUM MODE, TLCGROUPMODE, TLCBRIGHTNESS   refused: use LED SET, FX\r\n"
		"TLCRESET                  reset and init the LED driver chips\r\n"
		"TLCREGS COLOR             read the registers of a TLC59116\r\n"
		"SELFTEST LED ON|OFF [COLOR|ALL] [PERCENT]\r\n"
		"LED COLOR LEVEL|CONTROL|BLINK VALUE   the host joins, by hand\r\n"
		"LED SET LEDS R G B        LEDS: R1..R8, L1..L8, 0..15, A-B, R, L or ALL\r\n"
		"LED SIDE R|L R G B        one side\r\n"
		"LED GET [LEDS]            level, duty and PWM of each LED\r\n"
		"LED CLEAR                 drop the LED pattern, show the host color\r\n"
		"FX OFF | FADE R G B MS | BLINK R G B ON_MS OFF_MS | BREATHE R G B MS\r\n"
		"FX RAINBOW MS [LEVEL] | SMOOTH MS | CAP PERCENT\r\n"
		"FX CHASE R G B MS | FILL R G B PERCENT\r\n"
		"FX SPECTRUM MS [LEVEL] [RING|ROWS]   RING (default) turns around the bar\r\n"
		"FX SPLIT R G B R G B      right side color, left side color\r\n"
		"FX FREEZE ON|OFF | STEP MS  stop the effect clock, move it forward\r\n");
}

/* the board variant line of STATUS and LEDMAP */
static void ledmap_line(void)
{
	console_printf("variant %u %s map %s %s\r\n", ledmap_variant(),
		       ledmap_known() ? "known" : "unknown", ledmap_name(),
		       ledmap_chosen() ? "console" : "auto");
}

static void print_map(int v, const char *name)
{
	if (v < 0)
		console_printf("map - %s\r\n", name);
	else
		console_printf("map %d %s\r\n", v, name);
}

/*
 * LEDMAP: show the variant line and the maps. LEDMAP N uses the map of
 * variant value N, LEDMAP NAME the map with that name ("outputs": LED
 * index n is output n), LEDMAP AUTO the map of the variant pins. The
 * choice holds until the next start.
 */
static void cmd_ledmap(int argc, char **argv)
{
	if (argc > 1) {
		if (eq(argv[1], "AUTO")) {
			ledmap_auto();
		} else if (!ledmap_select(argv[1])) {
			console_printf("no LED map %s\r\n", argv[1]);
			ledmap_each(print_map);
			return;
		}
	}
	ledmap_line();
	if (argc < 2)
		ledmap_each(print_map);
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
	console_printf("leds %s%s (LED GET shows each LED)\r\n", s->pattern_on ? "pattern" : "host",
		       s->limited ? " limit" : "");
	console_printf("chips %s %s %s uptime %lu ms start %lu fails %lu reset flags 0x%08lX errors %d\r\n",
		       tlc_ready(RED) ? "ok" : "BAD", tlc_ready(GREEN) ? "ok" : "BAD",
		       tlc_ready(BLUE) ? "ok" : "BAD", (unsigned long)millis(),
		       (unsigned long)guard_start_count(), (unsigned long)guard_fails(),
		       (unsigned long)guard_reset_flags(), errlog_count());
	ledmap_line();
}

/*
 * The stock TLC commands. The LED engine owns the chip registers: it
 * applies the per-LED limit and the bar cap, and it keeps a copy of the
 * registers it wrote. A raw write would skip both limits (LEDOUT mode 1 is
 * fully on, blink mode runs on PWMx alone, a raw PWMx or GRPPWM skips the
 * limits), and the copy would no longer match the chip. So only the read
 * form runs: TLCOUTMODE COLOR NUM|ALL shows the output mode, as the stock
 * firmware does (tsx-ledbard reads it). The write forms answer "refused".
 * LED SET and FX set the light.
 */
static void cmd_tlc(int argc, char **argv)
{
	long n;
	int out, c = color_arg(argv[1]);

	if (c < 0) {
		console_write("usage: see HELP\r\n");
		return;
	}
	if (!eq(argv[0], "TLCOUTMODE") || argc > 3) {
		console_printf("%s refused: the LED engine sets the chip registers, with the power limits. "
			       "Use LED SET or FX\r\n", argv[0]);
		return;
	}
	if (!tlc_ready(c)) {
		console_write("LED driver not initialized!\r\n");
		return;
	}
	if (eq(argv[2], "ALL")) {
		out = -1;
	} else if (num_arg(argv[2], 0, 15, &n)) {
		out = (int)n;
	} else {
		console_write("usage: TLCOUTMODE COLOR NUM|ALL\r\n");
		return;
	}
	console_printf("%s %d output mode is:%d\r\n", color_names[c], out,
		       tlc_get_out_mode(c, out < 0 ? 0 : out));
}

/*
 * TLCREGS COLOR: read registers 0x00..0x17 from the chip (not from the
 * copy in RAM), and the number of GRPPWM changes the engine wrote.
 */
static void cmd_tlcregs(const char *arg)
{
	int c = arg ? color_arg(arg) : -1;
	uint8_t r[0x18];

	if (c < 0) {
		console_write("usage: TLCREGS COLOR\r\n");
		return;
	}
	if (!tlc_ready(c)) {
		console_write("LED driver not initialized!\r\n");
		return;
	}
	if (!tlc_read(c, 0, r, sizeof(r))) {
		console_write("TLCREGS failed\r\n");
		return;
	}
	console_printf("%s mode %02X %02X grp %u freq %u ledout %02X %02X %02X %02X grp changes %lu\r\n",
		       color_names[c], r[0], r[1], r[0x12], r[0x13], r[0x14], r[0x15], r[0x16], r[0x17],
		       (unsigned long)leds_grp_changes(c));
	console_printf("%s pwm %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\r\n", color_names[c],
		       r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
		       r[10], r[11], r[12], r[13], r[14], r[15], r[16], r[17]);
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

static void led_get(int first, int last)
{
	const struct led_state *s = leds_state();
	uint8_t grp[NCOLORS], pwm;

	for (int c = 0; c < NCOLORS; c++)
		tlc_get_dim(c, &pwm, &grp[c]);
	console_printf("leds %s fx %s grp %u %u %u%s\r\n", s->pattern_on ? "pattern" : "host",
		       leds_fx_name(), grp[0], grp[1], grp[2], s->limited ? " limit" : "");
	for (int i = first; i <= last; i++) {
		int o = leds_output(i);
		uint8_t lvl[NCOLORS];
		char name[4];

		leds_base_level(i, lvl);
		led_name(i, name);
		console_printf("%d %s out %d level %u %u %u duty %u %u %u pwm %u %u %u%s\r\n",
			       i, name, o, lvl[0], lvl[1], lvl[2],
			       s->led_duty[i][0], s->led_duty[i][1], s->led_duty[i][2],
			       tlc_get_pwm(RED, o), tlc_get_pwm(GREEN, o), tlc_get_pwm(BLUE, o),
			       s->limited & (1U << i) ? " limit" : "");
	}
}

static void cmd_led(int argc, char **argv)
{
	int c = color_arg(argv[1]);
	int first, last;
	uint8_t rgb[3];
	long v;

	if (eq(argv[1], "SET") || eq(argv[1], "SIDE")) {
		bool side = eq(argv[1], "SIDE");

		if (argc < 6 || (side && !eq(argv[2], "R") && !eq(argv[2], "L")) ||
		    !which_arg(argv[2], &first, &last) || !rgb_args(argv + 3, rgb)) {
			console_write(side ? "usage: LED SIDE R|L R G B\r\n" : "usage: LED SET LEDS R G B\r\n");
			return;
		}
		leds_pattern_set(first, last, rgb);
		console_write("ok\r\n");
		return;
	}
	if (eq(argv[1], "CLEAR")) {
		leds_pattern_clear();
		console_write("ok\r\n");
		return;
	}
	if (eq(argv[1], "GET")) {
		first = 0;
		last = NLEDS - 1;
		if (argc > 2 && !which_arg(argv[2], &first, &last)) {
			console_write("usage: LED GET [LEDS]\r\n");
			return;
		}
		led_get(first, last);
		return;
	}
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
	uint8_t rgb[3], rgb2[3];
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
	} else if (eq(argv[1], "CHASE") && argc >= 6 && rgb_args(argv + 2, rgb) &&
		   num_arg(argv[5], 100, 600000, &a)) {
		leds_fx_chase(rgb, (uint32_t)a);
	} else if (eq(argv[1], "FILL") && argc >= 6 && rgb_args(argv + 2, rgb) &&
		   num_arg(argv[5], 0, 100, &a)) {
		leds_fx_fill(rgb, (unsigned)a);
	} else if (eq(argv[1], "SPECTRUM") && argc >= 3 && num_arg(argv[2], 100, 600000, &a)) {
		bool ring = true;
		int n = 3;

		b = 100;
		if (argc > n && !eq(argv[n], "RING") && !eq(argv[n], "ROWS")) {
			if (!num_arg(argv[n], 0, 100, &b)) {
				console_write("bad level\r\n");
				return;
			}
			n++;
		}
		if (argc > n) {
			if (!eq(argv[n], "RING") && !eq(argv[n], "ROWS")) {
				console_write("usage: see HELP\r\n");
				return;
			}
			ring = eq(argv[n], "RING");
		}
		leds_fx_spectrum((uint32_t)a, (uint8_t)b, ring);
	} else if (eq(argv[1], "SPLIT") && argc >= 8 && rgb_args(argv + 2, rgb) &&
		   rgb_args(argv + 5, rgb2)) {
		leds_fx_split(rgb, rgb2);
	} else if (eq(argv[1], "FREEZE")) {
		if (argc > 2)
			leds_freeze(!eq(argv[2], "OFF"));
		console_printf("freeze %s\r\n", leds_frozen() ? "on" : "off");
		return;
	} else if (eq(argv[1], "STEP") && argc >= 3 && num_arg(argv[2], 1, 600000, &a)) {
		leds_step((uint32_t)a);
		console_printf("step %ld ms\r\n", a);
		return;
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

#ifdef TSX_QEMU
/*
 * QEMU build only: events that the QEMU machine cannot make. For the
 * tests of the start guard: HOST, a host runs on the bus (bus reset and
 * SOF packets) with no configuration. CONFIG: USB configured. FAULT: a
 * hard fault. RESET: a reset that the firmware did not plan, as the
 * watchdog gives (QEMU has no watchdog model). For the console: RX gives
 * three lines to console_rx in one call, as one USB packet does.
 */
static void cmd_test(const char *what)
{
	static const char three[] = "VER\r\nUPTIME\r\nCAPS\r\n";

	if (eq(what, "RX")) {
		console_rx((const uint8_t *)three, sizeof(three) - 1);
	} else if (eq(what, "HOST")) {
		guard_usb_host();
	} else if (eq(what, "CONFIG")) {
		guard_usb_configured();
	} else if (eq(what, "FAULT")) {
		__builtin_trap();
	} else if (eq(what, "RESET")) {
		system_reset();
	} else {
		console_write("usage: TEST HOST|CONFIG|FAULT|RESET|RX\r\n");
		return;
	}
	console_printf("test %s\r\n", what);
}
#endif

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
		console_write("tsx-ledbar fade blink breathe rainbow smooth cap status leds16 chase fill spectrum split ledmap\r\n");
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
		usb_flush(100);
		guard_reboot();
	} else if (eq(argv[0], "IMGUPD")) {
		if (argc > 1 && eq(argv[1], "?")) {
			console_write(" IMGUPD - reboot into bootloader\r\n");
			return;
		}
		console_write("Rebooting into the bootloader\r\n");
		usb_flush(100);
		guard_request_bootloader();
	} else if (eq(argv[0], "TLCRESET")) {
		console_printf("TLC reset %s\r\n", tlc_init() ? "ok" : "FAILED");
		leds_resync();
	} else if (eq(argv[0], "TLCREGS")) {
		cmd_tlcregs(argc > 1 ? argv[1] : NULL);
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
	} else if (eq(argv[0], "LEDMAP")) {
		cmd_ledmap(argc, argv);
	} else if (eq(argv[0], "FX")) {
		cmd_fx(argc, argv);
#ifdef TSX_QEMU
	} else if (eq(argv[0], "TRACE")) {
		extern bool leds_trace, leds_trace_pix, leds_trace_regs;

		/*
		 * ON: "led MS R G B" per tick, PIX: also "pix MS" and 48 duties,
		 * REGS: also "reg MS" and PWM0..15 and GRPPWM of red, green, blue
		 */
		leds_trace_regs = argc > 1 && eq(argv[1], "REGS");
		leds_trace_pix = leds_trace_regs || (argc > 1 && eq(argv[1], "PIX"));
		leds_trace = leds_trace_pix || (argc > 1 && eq(argv[1], "ON"));
		console_printf("trace %s\r\n", leds_trace_regs ? "regs" : leds_trace_pix ? "pix" :
			       leds_trace ? "on" : "off");
	} else if (eq(argv[0], "TEST")) {
		cmd_test(argc > 1 ? argv[1] : NULL);
#endif
	} else {
		console_printf("unknown command: %s (HELP for a list)\r\n", argv[0]);
	}
}

void console_init(void)
{
	line_len = 0;
	rx_head = rx_tail = 0;
}

/* run each complete line in the ring */
void console_poll(void)
{
	while (line_from_ring()) {
		run_line();
		line_len = 0;
	}
}
