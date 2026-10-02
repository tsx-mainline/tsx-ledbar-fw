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
 * Color math: a level 0..100 maps to a duty 0..255 with a gamma-2 curve,
 * so a linear fade in level looks linear. Ramps and effects work in
 * level units with 8 fraction bits. A power cap keeps the sum of the
 * three duties at or below "cap" percent of one full channel (default
 * 110, the total the stock firmware allows).
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
static uint8_t direct_duty[NCOLORS];
static uint32_t last_tick;

static uint8_t gamma_table[101];

#ifdef TSX_QEMU
#include <stdio.h>
bool leds_trace;
#endif

static int32_t clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* level in FP units to duty, with interpolation between table entries */
static uint8_t level_to_duty(int32_t lvl)
{
	int32_t i, f, a, b;

	lvl = clamp(lvl, 0, LEVEL_MAX);
	i = lvl / FP;
	f = lvl % FP;
	a = gamma_table[i];
	b = gamma_table[i < 100 ? i + 1 : 100];
	return (uint8_t)(a + ((b - a) * f) / FP);
}

/* 0..255 -> 0..255, half a cosine wave: 0 at 0, 255 at 128, 0 at 255 */
static uint32_t breathe_curve(uint32_t phase)
{
	static const uint8_t quarter[33] = {
		0, 1, 2, 6, 10, 15, 22, 30, 39, 49, 60, 71, 83, 96, 109, 123,
		137, 150, 164, 177, 189, 201, 212, 222, 231, 238, 245, 250,
		253, 255, 255, 255, 255 };
	uint32_t p = phase & 0xFF;
	uint32_t i = (p < 128 ? p : 255 - p) / 4;	/* 0..31 */

	return quarter[i] + (quarter[i + 1] - quarter[i]) * ((p < 128 ? p : 255 - p) % 4) / 4;
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
	for (int l = 0; l <= 100; l++) {
		uint32_t d = (l * l * 255 + 5000) / 10000;

		if (l > 0 && d == 0)
			d = 1;
		gamma_table[l] = (uint8_t)d;
	}
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
	uint32_t duty[NCOLORS], sum = 0, cap = cap_percent * 255 / 100;

	for (int c = 0; c < NCOLORS; c++) {
		duty[c] = fx == FX_DIRECT ? direct_duty[c] : level_to_duty(cur[c]);
		sum += duty[c];
	}
	if (sum > cap) {
		for (int c = 0; c < NCOLORS; c++)
			duty[c] = duty[c] * cap / sum;
	}
	for (int c = 0; c < NCOLORS; c++) {
		if (duty[c] == st.duty[c])
			continue;
		if (tlc_set_group_pwm(c, (uint8_t)duty[c]))
			st.duty[c] = (uint8_t)duty[c];
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
		uint32_t phase = (now - fx_t0) % fx_a * 256 / fx_a;
		uint32_t k = breathe_curve(phase);

		for (int c = 0; c < NCOLORS; c++)
			cur[c] = (int32_t)(fx_rgb[c] * FP * k / 255);
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
		direct_duty[c] = gamma_table[rgb[c] > 100 ? 100 : rgb[c]];
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
