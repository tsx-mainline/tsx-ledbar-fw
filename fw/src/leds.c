// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LED engine. It runs every 10 ms and writes one GRPPWM byte per color
 * when the duty of that color changed (three short I2C writes at most).
 *
 * Host model (as the stock firmware): a color lights while its digital
 * join is on, at its analog join level 0..100. A blink time (analog joins
 * 0..2, 100 ms units) toggles the color. Host changes can be smoothed:
 * "smooth" is the ramp time between two host colors (0 = at once).
 *
 * Effects run on top of the host color until FX OFF or until the host
 * sends a new join. FADE goes to a color over a time and holds it. BLINK,
 * BREATHE and RAINBOW repeat until they are stopped.
 *
 * Color math: a level 0..100 is a lightness. It maps to a duty 0..65535
 * of full brightness with the CIE 1976 lightness curve, so a linear fade
 * in level looks linear, and the curve is linear near zero, so a fade
 * does not dwell at the bottom. Ramps and effects work in level units
 * with 8 fraction bits, and the duty is interpolated between the table
 * entries. The duty goes to the chips as PWMx x GRPPWM (tlc_set_dim),
 * so the lowest step is 1/65025 of full brightness. A power cap keeps the
 * sum of the three duties at or below "cap" percent of one full channel
 * (default 110, the total the stock firmware allows).
 */
#include <string.h>
#include "tsx.h"

#define TICK_MS		10
#define FP		256		/* 8 fraction bits, level units */
#define LEVEL_MAX	(100 * FP)

enum fx { FX_NONE, FX_FADE, FX_BLINK, FX_BREATHE, FX_RAINBOW, FX_DIRECT };

static struct led_state st;
static uint32_t smooth_ms;
static unsigned cap_percent = 110;

static int32_t cur[NCOLORS];		/* current level, FP */
static int32_t ramp_from[NCOLORS], ramp_to[NCOLORS];
static uint32_t ramp_start, ramp_ms;
static int32_t host_target[NCOLORS];	/* last host target, to detect changes */

static enum fx fx;
static uint8_t fx_rgb[NCOLORS];
static uint32_t fx_t0, fx_a, fx_b;
static uint8_t fx_level;
static uint16_t direct_duty[NCOLORS];
static uint32_t last_tick;

/* CIE 1976: Y = L / 903.3 for L <= 8, else ((L + 16) / 116)^3, x 65535 */
static const uint16_t cie_table[101] = {
	0, 73, 145, 218, 290, 363, 435, 508, 580, 656,
	738, 826, 922, 1024, 1134, 1251, 1376, 1509, 1650, 1800,
	1959, 2127, 2304, 2491, 2687, 2894, 3111, 3338, 3576, 3826,
	4087, 4359, 4643, 4940, 5248, 5569, 5903, 6251, 6611, 6985,
	7373, 7775, 8192, 8623, 9069, 9530, 10006, 10498, 11006, 11530,
	12071, 12628, 13202, 13793, 14401, 15027, 15671, 16333, 17014, 17713,
	18431, 19168, 19924, 20700, 21497, 22313, 23149, 24007, 24885, 25784,
	26705, 27648, 28612, 29598, 30607, 31639, 32694, 33771, 34872, 35997,
	37146, 38319, 39516, 40738, 41986, 43258, 44555, 45879, 47228, 48603,
	50005, 51434, 52890, 54372, 55883, 57421, 58987, 60581, 62203, 63855,
	65535,
};

#ifdef TSX_QEMU
#include <stdio.h>
bool leds_trace;
#endif

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* level in FP units to a duty 0..65535, interpolated between entries */
static uint32_t level_to_duty(int32_t lvl)
{
	int32_t i, f, a, b;

	lvl = clamp(lvl, 0, LEVEL_MAX);
	i = lvl / FP;
	f = lvl % FP;
	a = cie_table[i];
	b = cie_table[i < 100 ? i + 1 : 100];
	return (uint32_t)(a + ((b - a) * f) / FP);
}

/*
 * Breathe: phase 0..4095 over one period. 0..2047 rises, 2048..4095
 * falls. Half a cosine wave from a 33-entry table, 0..65535.
 */
#define BREATHE_STEPS	4096

static uint32_t breathe_curve(uint32_t phase)
{
	static const uint16_t half_cos[33] = {
	0, 158, 630, 1411, 2494, 3869, 5522, 7438, 9597, 11980, 14563,
	17321, 20228, 23256, 26375, 29556, 32767, 35979, 39160, 42279, 45307, 48214,
	50972, 53555, 55938, 58097, 60013, 61666, 63041, 64124, 64905, 65377, 65535,
	};
	uint32_t p = phase % BREATHE_STEPS;
	uint32_t x = p < BREATHE_STEPS / 2 ? p : BREATHE_STEPS - 1 - p;	/* 0..2047 */
	uint32_t i = x / 64, f = x % 64;					/* 0..31 */

	return half_cos[i] + (uint32_t)((int32_t)(half_cos[i + 1] - half_cos[i]) * (int32_t)f / 64);
}

/*
 * Duty 0..65535 to PWMx x GRPPWM. The target t is in units of 1/65025
 * (255 x 255). GRPPWM is the smallest value that still reaches t with
 * PWMx <= 255, so PWMx stays in 128..255 above the bottom and the
 * rounding error stays below 0.4 % of the duty.
 */
static void duty_split(uint32_t duty, uint8_t *pwm, uint8_t *grp)
{
	uint32_t t = (duty * 65025U + 32767U) / 65535U, g, p;

	if (t == 0)
		t = 1;
	g = (t + 254) / 255;
	p = (t + g / 2) / g;
	*grp = (uint8_t)g;
	*pwm = (uint8_t)(p > 255 ? 255 : p == 0 ? 1 : p);
}

/* hue 0..1535 (6 x 256), full saturation, value = level */
static void hue_to_rgb(uint32_t hue, uint8_t level, int32_t out[NCOLORS])
{
	uint32_t seg = (hue / 256) % 6, f = hue % 256;
	uint32_t r = 0, g = 0, b = 0;

	switch (seg) {
	case 0: r = 255; g = f; break;
	case 1: r = 255 - f; g = 255; break;
	case 2: g = 255; b = f; break;
	case 3: g = 255 - f; b = 255; break;
	case 4: r = f; b = 255; break;
	default: r = 255; b = 255 - f; break;
	}
	out[RED] = (int32_t)(r * level * FP / 255);
	out[GREEN] = (int32_t)(g * level * FP / 255);
	out[BLUE] = (int32_t)(b * level * FP / 255);
}

void leds_init(void)
{
	memset(&st, 0, sizeof(st));
	fx = FX_NONE;
	tlc_init();
}

static void host_color(int32_t out[NCOLORS])
{
	uint32_t now = millis();

	for (int c = 0; c < NCOLORS; c++) {
		bool on = st.control[c];

		if (on && st.blink_100ms[c])
			on = ((now / (st.blink_100ms[c] * 100U)) & 1) == 0;
		out[c] = on ? st.level[c] * FP : 0;
	}
}

static void start_ramp(const int32_t to[NCOLORS], uint32_t ms)
{
	for (int c = 0; c < NCOLORS; c++) {
		ramp_from[c] = cur[c];
		ramp_to[c] = to[c];
	}
	ramp_start = millis();
	ramp_ms = ms;
}

static void run_ramp(void)
{
	uint32_t t = millis() - ramp_start;

	for (int c = 0; c < NCOLORS; c++) {
		if (ramp_ms == 0 || t >= ramp_ms)
			cur[c] = ramp_to[c];
		else
			cur[c] = ramp_from[c] + (int32_t)((int64_t)(ramp_to[c] - ramp_from[c]) * (int32_t)t / (int32_t)ramp_ms);
	}
}

static void apply(void)
{
	uint32_t duty[NCOLORS], sum = 0, cap = cap_percent * 65535U / 100U;
	uint8_t pwm, grp;
	bool ok;

	for (int c = 0; c < NCOLORS; c++) {
		duty[c] = fx == FX_DIRECT ? direct_duty[c] : level_to_duty(cur[c]);
		sum += duty[c];
	}
	if (sum > cap) {
		for (int c = 0; c < NCOLORS; c++)
			duty[c] = (uint32_t)((uint64_t)duty[c] * cap / sum);
	}
	for (int c = 0; c < NCOLORS; c++) {
		if (duty[c] == st.duty[c])
			continue;
		if (duty[c] == 0) {
			ok = tlc_set_group_pwm(c, 0);
		} else {
			duty_split(duty[c], &pwm, &grp);
			ok = tlc_set_dim(c, pwm, grp);
		}
		if (ok)
			st.duty[c] = (uint16_t)duty[c];
	}
#ifdef TSX_QEMU
	if (leds_trace) {
		char line[48];

		snprintf(line, sizeof(line), "led %lu %u %u %u\r\n", (unsigned long)millis(),
			 st.duty[0], st.duty[1], st.duty[2]);
		qemu_uart_write(line);
	}
#endif
}

void leds_tick(void)
{
	uint32_t now = millis();
	int32_t target[NCOLORS];

	if (now - last_tick < TICK_MS)
		return;
	last_tick = now;

	switch (fx) {
	case FX_NONE:
		host_color(target);
		if (memcmp(target, host_target, sizeof(target)) != 0) {
			memcpy(host_target, target, sizeof(target));
			start_ramp(target, smooth_ms);
		}
		run_ramp();
		break;
	case FX_FADE:
		run_ramp();
		break;
	case FX_BLINK: {
		uint32_t t = (now - fx_t0) % (fx_a + fx_b);

		for (int c = 0; c < NCOLORS; c++)
			cur[c] = t < fx_a ? fx_rgb[c] * FP : 0;
		break;
	}
	case FX_BREATHE: {
		uint32_t phase = (uint32_t)((uint64_t)((now - fx_t0) % fx_a) * BREATHE_STEPS / fx_a);
		uint32_t k = breathe_curve(phase);

		for (int c = 0; c < NCOLORS; c++)
			cur[c] = (int32_t)((uint32_t)(fx_rgb[c] * FP) * k / 65535U);
		break;
	}
	case FX_RAINBOW: {
		uint32_t hue = (now - fx_t0) % fx_a * 1536 / fx_a;

		hue_to_rgb(hue, fx_level, cur);
		break;
	}
	case FX_DIRECT:
		break;
	}
	apply();
}

/* a host join ends an effect: the host is the master of the color */
static void host_update(void)
{
	if (fx != FX_NONE && fx != FX_DIRECT)
		fx = FX_NONE;
}

void leds_set_level(int color, unsigned level)
{
	if (color < 0 || color >= NCOLORS)
		return;
	st.level[color] = level > 100 ? 100 : (uint8_t)level;
	host_update();
}

void leds_set_control(int color, bool on)
{
	if (color < 0 || color >= NCOLORS)
		return;
	st.control[color] = on;
	host_update();
}

void leds_set_blink_time(int color, unsigned n100ms)
{
	if (color < 0 || color >= NCOLORS)
		return;
	st.blink_100ms[color] = (uint16_t)(n100ms > 600 ? 600 : n100ms);
	host_update();
}

const struct led_state *leds_state(void)
{
	return &st;
}

void leds_fx_off(void)
{
	int32_t target[NCOLORS];

	fx = FX_NONE;
	host_color(target);
	memcpy(host_target, target, sizeof(target));
	start_ramp(target, smooth_ms);
}

void leds_fx_fade(const uint8_t rgb[3], uint32_t ms)
{
	int32_t to[NCOLORS];

	for (int c = 0; c < NCOLORS; c++)
		to[c] = (rgb[c] > 100 ? 100 : rgb[c]) * FP;
	fx = FX_FADE;
	start_ramp(to, ms);
}

void leds_fx_blink(const uint8_t rgb[3], uint32_t on_ms, uint32_t off_ms)
{
	memcpy(fx_rgb, rgb, NCOLORS);
	fx_a = on_ms ? on_ms : 1;
	fx_b = off_ms ? off_ms : 1;
	fx_t0 = millis();
	fx = FX_BLINK;
}

void leds_fx_breathe(const uint8_t rgb[3], uint32_t period_ms)
{
	memcpy(fx_rgb, rgb, NCOLORS);
	fx_a = period_ms < 100 ? 100 : period_ms;
	fx_t0 = millis();
	fx = FX_BREATHE;
}

void leds_fx_rainbow(uint32_t period_ms, uint8_t level)
{
	fx_a = period_ms < 100 ? 100 : period_ms;
	fx_level = level > 100 ? 100 : level;
	fx_t0 = millis();
	fx = FX_RAINBOW;
}

void leds_set_smooth(uint32_t ms)
{
	smooth_ms = ms > 60000 ? 60000 : ms;
}

uint32_t leds_get_smooth(void)
{
	return smooth_ms;
}

const char *leds_fx_name(void)
{
	static const char *const names[] = { "off", "fade", "blink", "breathe", "rainbow", "direct" };

	return names[fx];
}

void leds_direct(const uint8_t rgb[3])
{
	for (int c = 0; c < NCOLORS; c++)
		direct_duty[c] = cie_table[rgb[c] > 100 ? 100 : rgb[c]];
	fx = FX_DIRECT;
}

void leds_direct_off(void)
{
	if (fx == FX_DIRECT)
		leds_fx_off();
}

void leds_set_cap(unsigned percent)
{
	cap_percent = percent < 10 ? 10 : percent > 150 ? 150 : percent;
}

unsigned leds_get_cap(void)
{
	return cap_percent;
}
