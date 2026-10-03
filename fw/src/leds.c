// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LED engine. It runs every 10 ms, keeps a level for each of the 16 LEDs
 * and each color, and writes the chip registers only when they change.
 *
 * LED index (tsx.h): 0..7 are R1..R8, the right side from top to bottom.
 * 8..15 are L1..L8, the left side from top to bottom. led_out[] maps an
 * index to the TLC59116 output. Output n of the red, green and blue chip
 * drives the same LED.
 *
 * Host model (as the stock firmware): a color lights while its digital
 * join is on, at its analog join level 0..100. A blink time (analog joins
 * 0..2, 100 ms units) toggles the color. The host color goes to all 16
 * LEDs.
 *
 * Base: the bar shows the host color or, after a LED command on the
 * console, the LED pattern (a level for each LED and color). The first
 * LED command copies the steady host color into the pattern. LED CLEAR
 * and every host join drop the pattern. "smooth" is the ramp time
 * between two base colors (0 = at once).
 *
 * Effects run on top of the base until FX OFF, a LED command or a host
 * join. FX OFF ramps back to the base: the pattern when one is set,
 * otherwise the host color. A host join drops the pattern and the effect
 * and ramps to the host color. FADE, FILL and SPLIT go to a color (FADE
 * over its own time, FILL and SPLIT over the smooth time) and hold it.
 * BLINK, BREATHE, RAINBOW, CHASE and SPECTRUM repeat until they stop.
 *
 * Color math: a level 0..100 is a lightness. It maps to a duty 0..65535
 * of full brightness with the CIE 1976 lightness curve, so a linear fade
 * in level looks linear, and the curve is linear near zero, so a fade
 * does not dwell at the bottom. Ramps and effects work in level units
 * with 8 fraction bits, and the duty is interpolated between the table
 * entries.
 *
 * Power limits: one value "cap" (FX CAP, default 110) sets two limits.
 * 1. The per-LED limit: the sum of the red, green and blue duty of one LED
 *    stays at or below cap percent of one full channel (65535). When the
 *    sum is above the limit, the three duties of that LED scale by the
 *    same factor, so the LED keeps its hue. The stock firmware never
 *    gives one LED more than 110 %. This is the cap of 0.1.2.
 * 2. The bar cap: the sum of the 48 duties stays at or below cap percent
 *    of one full channel on all 16 LEDs (16 x 65535). When the sum is
 *    above the cap, all duties scale by the same factor.
 * The engine applies the per-LED limit first. Then the sum of the 48
 * duties is never above the bar cap, so the bar cap is a second guard.
 * With one color on the whole bar, both limits give the duties of 0.1.2.
 * CHASE and FILL apply the per-LED limit to their color before they share
 * it between rows, so a dot or a part of a row keeps its part of the light.
 *
 * Dimming: brightness on the chip is PWMx x GRPPWM (see i2c_tlc.c), with
 * one PWMx per output and one GRPPWM per chip. GRPPWM is the smallest
 * value that still lets the brightest output of the chip reach its duty
 * with PWMx <= 255. The step of every output is then GRPPWM / 65025 of
 * full brightness, the smallest possible, so the darkest lit LED keeps
 * fine steps. When the whole bar is dark, the chip is dark at the finest
 * step: a single dim LED gets GRPPWM 1 and steps of 1/65025. One transfer
 * writes the registers from the first changed one to the last changed
 * one, so PWMx and GRPPWM change together at the STOP condition.
 */
#include <string.h>
#include "tsx.h"

#define TICK_MS		10
#define FP		256		/* 8 fraction bits, level units */
#define LEVEL_MAX	(100 * FP)

/* LED index to TLC59116 output, see the comment at the top */
static const uint8_t led_out[NLEDS] = {
	15, 6, 0, 1, 2, 3, 4, 5,	/* R1..R8 */
	14, 13, 12, 11, 10, 9, 8, 7,	/* L1..L8 */
};

/*
 * The ring: the LED index in ring order, clockwise seen from the front.
 * Down the right side (R1..R8), then up the left side (L8..L1).
 */
static const uint8_t ring_led[NLEDS] = {
	0, 1, 2, 3, 4, 5, 6, 7,		/* R1..R8 */
	15, 14, 13, 12, 11, 10, 9, 8,	/* L8..L1 */
};

enum fx {
	FX_NONE, FX_FADE, FX_BLINK, FX_BREATHE, FX_RAINBOW, FX_DIRECT,
	FX_CHASE, FX_FILL, FX_SPECTRUM, FX_SPLIT,
};

static struct led_state st;
static uint32_t smooth_ms;
static unsigned cap_percent = 110;

static int32_t cur[NLEDS][NCOLORS];	/* current level, FP */
static int32_t ramp_from[NLEDS][NCOLORS], ramp_to[NLEDS][NCOLORS];
static uint32_t ramp_start, ramp_ms;
static int32_t base_target[NLEDS][NCOLORS];	/* last base target, to detect changes */

static enum fx fx;
static uint8_t fx_rgb[NCOLORS];
static uint32_t fx_t0, fx_a, fx_b;
static uint8_t fx_level;
static bool fx_ring;		/* SPECTRUM: the ring, not the rows */
static uint16_t direct_duty[NCOLORS];
static uint32_t last_tick;
static bool fx_limited;	/* CHASE, FILL: the per-LED limit scaled the effect color */

/* the registers PWM0..15 and GRPPWM as the engine last wrote them */
static uint8_t sent[NCOLORS][TLC_DIM_REGS];

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
bool leds_trace;	/* TRACE ON: one "led" line per tick */
bool leds_trace_pix;	/* TRACE PIX: also one "pix" line with the 48 duties */
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

/* the inverse: the level in FP units whose duty is d */
static int32_t duty_to_level(uint32_t d)
{
	int lo = 0, hi = 100;

	if (d >= 65535)
		return LEVEL_MAX;
	while (hi - lo > 1) {
		int mid = (lo + hi) / 2;

		if (cie_table[mid] <= d)
			lo = mid;
		else
			hi = mid;
	}
	return lo * FP + (int32_t)((d - cie_table[lo]) * FP / (uint32_t)(cie_table[hi] - cie_table[lo]));
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

static void fill_all(int32_t out[NLEDS][NCOLORS], const int32_t rgb[NCOLORS])
{
	for (int i = 0; i < NLEDS; i++)
		memcpy(out[i], rgb, sizeof(out[i]));
}

/*
 * TLCRESET puts the chips back to their init values. Take the chip
 * registers as the sent state, so the next tick writes what differs.
 */
void leds_resync(void)
{
	for (int c = 0; c < NCOLORS; c++) {
		for (int o = 0; o < 16; o++)
			sent[c][o] = tlc_get_pwm(c, o);
		tlc_get_dim(c, &sent[c][0], &sent[c][16]);
	}
}

void leds_init(void)
{
	memset(&st, 0, sizeof(st));
	fx = FX_NONE;
	tlc_init();
	leds_resync();		/* PWMx 255 and GRPPWM 0 after the init */
}

/* steady host level of each color (no blink), 0..100 */
static void host_steady(uint8_t rgb[NCOLORS])
{
	for (int c = 0; c < NCOLORS; c++)
		rgb[c] = st.control[c] ? st.level[c] : 0;
}

/* the base color of every LED: the pattern, or the host color with blink */
static void base_color(int32_t out[NLEDS][NCOLORS])
{
	uint32_t now = millis();
	int32_t host[NCOLORS];

	if (st.pattern_on) {
		for (int i = 0; i < NLEDS; i++) {
			for (int c = 0; c < NCOLORS; c++)
				out[i][c] = st.pattern[i][c] * FP;
		}
		return;
	}
	for (int c = 0; c < NCOLORS; c++) {
		bool on = st.control[c];

		if (on && st.blink_100ms[c])
			on = ((now / (st.blink_100ms[c] * 100U)) & 1) == 0;
		host[c] = on ? st.level[c] * FP : 0;
	}
	fill_all(out, host);
}

/* the per-LED limit: the highest red + green + blue duty of one LED */
static uint32_t led_limit(void)
{
	return cap_percent * 65535U / 100U;
}

/*
 * Scale the three duties of one LED to the per-LED limit with one factor,
 * so the LED keeps its hue. Return true when the duties change.
 */
static bool limit_led(uint32_t d[NCOLORS])
{
	uint32_t lim = led_limit(), sum = d[RED] + d[GREEN] + d[BLUE];

	if (sum <= lim)
		return false;
	for (int c = 0; c < NCOLORS; c++)
		d[c] = (uint32_t)((uint64_t)d[c] * lim / sum);
	return true;
}

/* the duties of an effect color 0..100 after the per-LED limit */
static bool limited_color(const uint8_t rgb[NCOLORS], uint32_t d[NCOLORS])
{
	for (int c = 0; c < NCOLORS; c++)
		d[c] = level_to_duty((rgb[c] > 100 ? 100 : rgb[c]) * FP);
	return limit_led(d);
}

static void start_ramp(int32_t to[NLEDS][NCOLORS], uint32_t ms)
{
	memcpy(ramp_from, cur, sizeof(cur));
	memcpy(ramp_to, to, sizeof(ramp_to));
	ramp_start = millis();
	ramp_ms = ms;
}

static void run_ramp(void)
{
	uint32_t t = millis() - ramp_start;

	for (int i = 0; i < NLEDS; i++) {
		for (int c = 0; c < NCOLORS; c++) {
			int32_t a = ramp_from[i][c], b = ramp_to[i][c];

			if (ramp_ms == 0 || t >= ramp_ms)
				cur[i][c] = b;
			else
				cur[i][c] = a + (int32_t)((int64_t)(b - a) * (int32_t)t / (int32_t)ramp_ms);
		}
	}
}

/*
 * Duties of one chip to its 17 registers. A duty d is the target
 * t = d x 65025 / 65535 in units of 1/65025. GRPPWM g = ceil(tmax / 255),
 * PWMx = t / g rounded, at least 1 for a lit output. For one color on all
 * outputs this is the split of 0.1.2. A dark chip keeps its PWMx values
 * and gets GRPPWM 0, so it needs a one-byte write.
 */
static void chip_regs(uint32_t duty[NLEDS][NCOLORS], int c, uint8_t v[TLC_DIM_REGS])
{
	uint32_t t[16], tmax = 0, g, p;

	for (int i = 0; i < NLEDS; i++) {
		uint32_t d = duty[i][c], x = 0;

		if (d) {
			x = (d * 65025U + 32767U) / 65535U;
			if (x == 0)
				x = 1;
		}
		t[led_out[i]] = x;
		if (x > tmax)
			tmax = x;
	}
	if (tmax == 0) {
		memcpy(v, sent[c], 16);
		v[16] = 0;
		return;
	}
	g = (tmax + 254) / 255;
	for (int o = 0; o < 16; o++) {
		p = (t[o] + g / 2) / g;
		v[o] = (uint8_t)(t[o] == 0 ? 0 : p > 255 ? 255 : p == 0 ? 1 : p);
	}
	v[16] = (uint8_t)g;
}

/* write the span of registers that changed, in one transfer */
static bool chip_write(int c, const uint8_t v[TLC_DIM_REGS])
{
	int first = -1, last = -1;

	for (int r = 0; r < TLC_DIM_REGS; r++) {
		if (v[r] != sent[c][r]) {
			if (first < 0)
				first = r;
			last = r;
		}
	}
	if (first < 0)
		return true;
	if (!tlc_ready(c) ||
	    !tlc_write(c, (uint8_t)(TLC_REG_PWM0 + first), v + first, (size_t)(last - first + 1)))
		return false;
	memcpy(sent[c], v, TLC_DIM_REGS);
	return true;
}

static void apply(void)
{
	uint32_t duty[NLEDS][NCOLORS];
	uint64_t sum = 0, cap = (uint64_t)NLEDS * led_limit();
	uint16_t limited = 0;
	uint8_t v[TLC_DIM_REGS];

	for (int i = 0; i < NLEDS; i++) {
		for (int c = 0; c < NCOLORS; c++)
			duty[i][c] = fx == FX_DIRECT ? direct_duty[c] : level_to_duty(cur[i][c]);
		if (limit_led(duty[i]) ||
		    (fx_limited && (fx == FX_CHASE || fx == FX_FILL) &&
		     duty[i][RED] + duty[i][GREEN] + duty[i][BLUE] > 0))
			limited |= (uint16_t)(1U << i);
		sum += duty[i][RED] + duty[i][GREEN] + duty[i][BLUE];
	}
	st.limited = limited;
	if (sum > cap) {
		for (int i = 0; i < NLEDS; i++) {
			for (int c = 0; c < NCOLORS; c++)
				duty[i][c] = (uint32_t)((uint64_t)duty[i][c] * cap / sum);
		}
	}
	for (int c = 0; c < NCOLORS; c++) {
		uint32_t max = 0;

		chip_regs(duty, c, v);
		if (!chip_write(c, v))
			continue;	/* the next tick tries again */
		for (int i = 0; i < NLEDS; i++) {
			st.led_duty[i][c] = (uint16_t)duty[i][c];
			if (duty[i][c] > max)
				max = duty[i][c];
		}
		st.duty[c] = (uint16_t)max;
	}
#ifdef TSX_QEMU
	if (leds_trace) {
		char line[48];

		snprintf(line, sizeof(line), "led %lu %u %u %u\r\n", (unsigned long)millis(),
			 st.duty[0], st.duty[1], st.duty[2]);
		qemu_uart_write(line);
	}
	if (leds_trace_pix) {
		char line[16];

		snprintf(line, sizeof(line), "pix %lu", (unsigned long)millis());
		qemu_uart_write(line);
		for (int i = 0; i < NLEDS; i++) {
			for (int c = 0; c < NCOLORS; c++) {
				snprintf(line, sizeof(line), " %u", st.led_duty[i][c]);
				qemu_uart_write(line);
			}
		}
		qemu_uart_write("\r\n");
	}
#endif
}

/* CHASE: a dot runs down both sides, a cross-fade between two rows */
static void run_chase(uint32_t now)
{
	uint32_t pos = (now - fx_t0) % fx_a * (NROWS * 256U) / fx_a;	/* 0..2047 */
	uint32_t full[NCOLORS];

	fx_limited = limited_color(fx_rgb, full);	/* FX CAP can change while it runs */

	for (int r = 0; r < NROWS; r++) {
		int32_t d = (int32_t)pos - r * 256;
		uint32_t k;

		if (d < 0)
			d = -d;
		if (d > NROWS * 128)
			d = NROWS * 256 - d;		/* the dot wraps from the bottom to the top */
		k = d >= 256 ? 0 : 256U - (uint32_t)d;	/* 0..256 */
		for (int c = 0; c < NCOLORS; c++) {
			/* share the light, not the lightness, so the sum stays the same */
			int32_t l = duty_to_level(full[c] * k / 256U);

			cur[r][c] = l;
			cur[NROWS + r][c] = l;
		}
	}
}

/*
 * SPECTRUM: the hue circle over the 16 LEDs in ring order, moving
 * clockwise, or over the 8 rows of each side, moving down. The hue of
 * each LED is 1/16 (ring) or 1/8 (rows) of the circle after the hue of
 * the LED before it.
 */
static void run_spectrum(uint32_t now)
{
	uint32_t phase = (now - fx_t0) % fx_a * 1536U / fx_a;

	if (fx_ring) {
		for (int p = 0; p < NLEDS; p++) {
			uint32_t hue = (p * (1536U / NLEDS) + 1536U - phase) % 1536U;

			hue_to_rgb(hue, fx_level, cur[ring_led[p]]);
		}
		return;
	}
	for (int r = 0; r < NROWS; r++) {
		uint32_t hue = (r * (1536U / NROWS) + 1536U - phase) % 1536U;

		hue_to_rgb(hue, fx_level, cur[r]);
		memcpy(cur[NROWS + r], cur[r], sizeof(cur[r]));
	}
}

void leds_tick(void)
{
	uint32_t now = millis();
	int32_t target[NLEDS][NCOLORS];
	int32_t one[NCOLORS];

	if (now - last_tick < TICK_MS)
		return;
	last_tick = now;

	switch (fx) {
	case FX_NONE:
		base_color(target);
		if (memcmp(target, base_target, sizeof(target)) != 0) {
			memcpy(base_target, target, sizeof(target));
			start_ramp(target, smooth_ms);
		}
		run_ramp();
		break;
	case FX_FADE:
	case FX_FILL:
	case FX_SPLIT:
		run_ramp();
		break;
	case FX_BLINK: {
		uint32_t t = (now - fx_t0) % (fx_a + fx_b);

		for (int c = 0; c < NCOLORS; c++)
			one[c] = t < fx_a ? fx_rgb[c] * FP : 0;
		fill_all(cur, one);
		break;
	}
	case FX_BREATHE: {
		uint32_t phase = (uint32_t)((uint64_t)((now - fx_t0) % fx_a) * BREATHE_STEPS / fx_a);
		uint32_t k = breathe_curve(phase);

		for (int c = 0; c < NCOLORS; c++)
			one[c] = (int32_t)((uint32_t)(fx_rgb[c] * FP) * k / 65535U);
		fill_all(cur, one);
		break;
	}
	case FX_RAINBOW: {
		uint32_t hue = (now - fx_t0) % fx_a * 1536 / fx_a;

		hue_to_rgb(hue, fx_level, one);
		fill_all(cur, one);
		break;
	}
	case FX_CHASE:
		run_chase(now);
		break;
	case FX_SPECTRUM:
		run_spectrum(now);
		break;
	case FX_DIRECT:
		break;
	}
	apply();
}

/*
 * A host join drops the LED pattern and ends an effect: the host is the
 * master of the color. The ramp goes back to the host color also when the
 * join did not change that color, because the effect left another color
 * on the bar. Without an effect, leds_tick starts a ramp when the base
 * color changes.
 */
static void end_effect(void)
{
	if (fx != FX_NONE && fx != FX_DIRECT)
		leds_fx_off();
}

static void host_update(void)
{
	st.pattern_on = false;
	end_effect();
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

int leds_output(int led)
{
	return led >= 0 && led < NLEDS ? led_out[led] : -1;
}

void leds_base_level(int led, uint8_t rgb[3])
{
	if (st.pattern_on)
		memcpy(rgb, st.pattern[led], NCOLORS);
	else
		host_steady(rgb);
}

void leds_pattern_set(int first, int last, const uint8_t rgb[3])
{
	if (!st.pattern_on) {
		uint8_t host[NCOLORS];

		host_steady(host);
		for (int i = 0; i < NLEDS; i++)
			memcpy(st.pattern[i], host, NCOLORS);
		st.pattern_on = true;
	}
	for (int i = first < 0 ? 0 : first; i <= last && i < NLEDS; i++) {
		for (int c = 0; c < NCOLORS; c++)
			st.pattern[i][c] = rgb[c] > 100 ? 100 : rgb[c];
	}
	end_effect();
}

void leds_pattern_clear(void)
{
	st.pattern_on = false;
	end_effect();
}

void leds_fx_off(void)
{
	int32_t target[NLEDS][NCOLORS];

	fx = FX_NONE;
	base_color(target);
	memcpy(base_target, target, sizeof(target));
	start_ramp(target, smooth_ms);
}

void leds_fx_fade(const uint8_t rgb[3], uint32_t ms)
{
	int32_t to[NLEDS][NCOLORS], one[NCOLORS];

	for (int c = 0; c < NCOLORS; c++)
		one[c] = (rgb[c] > 100 ? 100 : rgb[c]) * FP;
	fill_all(to, one);
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

void leds_fx_chase(const uint8_t rgb[3], uint32_t period_ms)
{
	for (int c = 0; c < NCOLORS; c++)
		fx_rgb[c] = rgb[c] > 100 ? 100 : rgb[c];
	fx_a = period_ms < 100 ? 100 : period_ms;
	fx_t0 = millis();
	fx = FX_CHASE;
}

/*
 * FILL: a level bar from the bottom up on both sides. PERCENT 0..100
 * covers the 8 rows. The top row of the bar gets the part of a row that
 * is left, as a part of the level. A new FILL ramps over the smooth time.
 */
void leds_fx_fill(const uint8_t rgb[3], unsigned percent)
{
	int32_t to[NLEDS][NCOLORS], lvl[NCOLORS];
	int32_t fill = (int32_t)((percent > 100 ? 100 : percent) * NROWS * FP / 100U);
	uint32_t full[NCOLORS];

	/* the level of a full row, after the per-LED limit */
	fx_limited = limited_color(rgb, full);
	for (int c = 0; c < NCOLORS; c++)
		lvl[c] = fx_limited ? duty_to_level(full[c]) : (rgb[c] > 100 ? 100 : rgb[c]) * FP;
	for (int r = 0; r < NROWS; r++) {
		int32_t part = clamp(fill - (NROWS - 1 - r) * FP, 0, FP);	/* 0..FP */

		for (int c = 0; c < NCOLORS; c++) {
			to[r][c] = lvl[c] * part / FP;
			to[NROWS + r][c] = to[r][c];
		}
	}
	fx = FX_FILL;
	start_ramp(to, smooth_ms);
}

void leds_fx_spectrum(uint32_t period_ms, uint8_t level, bool ring)
{
	fx_a = period_ms < 100 ? 100 : period_ms;
	fx_level = level > 100 ? 100 : level;
	fx_ring = ring;
	fx_t0 = millis();
	fx = FX_SPECTRUM;
}

/* SPLIT: one color on the right side, one on the left side */
void leds_fx_split(const uint8_t right[3], const uint8_t left[3])
{
	int32_t to[NLEDS][NCOLORS];

	for (int r = 0; r < NROWS; r++) {
		for (int c = 0; c < NCOLORS; c++) {
			to[r][c] = (right[c] > 100 ? 100 : right[c]) * FP;
			to[NROWS + r][c] = (left[c] > 100 ? 100 : left[c]) * FP;
		}
	}
	fx = FX_SPLIT;
	start_ramp(to, smooth_ms);
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
	static const char *const names[] = {
		"off", "fade", "blink", "breathe", "rainbow", "direct",
		"chase", "fill", "spectrum", "split",
	};

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
