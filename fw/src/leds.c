// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * LED engine. It runs every 10 ms, keeps a duty for each of the 16 LEDs
 * and each color, and writes the chip registers only when they change.
 *
 * LED index (tsx.h): 0..7 are R1..R8, the right side from top to bottom.
 * 8..15 are L1..L8, the left side from top to bottom. The LED map
 * (ledmap.c) gives the TLC59116 output of each index. Output n of the
 * red, green and blue chip drives the same LED. The engine reads the map
 * at each tick, so a new map (LEDMAP) shows at the next tick.
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
 * of full brightness with the CIE 1976 lightness curve, and the curve is
 * linear near zero, so a fade does not dwell at the bottom. The duty is
 * interpolated between the table entries (8 fraction bits of level).
 * An effect that dims a color dims it in light: all three duties get the
 * same factor, so the duty ratios (the hue) stay the same. The factor
 * follows the CIE curve of the lightness of the effect, so the fade looks
 * even. BREATHE, the top row of FILL, and the level of RAINBOW and
 * SPECTRUM work this way. A ramp (FADE, FILL, SPLIT, FX OFF, the smooth
 * time of the host color) goes on a straight line between the duties of
 * its two ends, timed so the lightness of the sum of the duties changes
 * linearly. Dimming each color on its own would change the hue: 84 38 100
 * would turn green and pink near zero. When a fade from or to black would
 * round one color to 0 while another is still lit, the LED goes dark as a
 * whole.
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
 * CHASE, FILL, BREATHE, RAINBOW, SPECTRUM and ramps apply the per-LED
 * limit to their full color before they share or dim it. So a dot or a
 * part of a row keeps its part of the light, and a BREATHE does not sit
 * at the limit for a part of each period.
 *
 * Dimming: brightness on the chip is PWMx x GRPPWM (see i2c_tlc.c), with
 * one PWMx per output and one GRPPWM per chip. PWMx runs at 97 kHz,
 * GRPPWM is the duty of a free-running 190 Hz window. A lower GRPPWM that
 * the chip gets while its window is open lets the window run to the end
 * of the period, so a lit output flashes too bright for up to 5 ms. On
 * the bar this showed as one-color flashes in CHASE (0.1.3 wrote a new
 * GRPPWM on most ticks) and in the low part of a BREATHE down ramp. A
 * higher GRPPWM is safe. So for each chip and tick, with
 * need = ceil(tmax / 255), the smallest GRPPWM that lets the brightest
 * output reach its target with PWMx <= 255:
 * 1. need > GRPPWM: raise GRPPWM to 1.5 x tmax / 255 (at most 255).
 * 2. GRPPWM >= 4 x need (the brightest PWMx is 64 or less):
 *    a. the chip was dark at the last write, or its light has not fallen
 *       for STEADY_TICKS ticks: write the new GRPPWM with PWMx 0 (dark for
 *       this tick), light the chip on the next tick;
 *    b. else (a falling light) keep GRPPWM. An output below half a step
 *       rounds to 0.
 * 3. Else keep GRPPWM, so only PWMx changes.
 * A chip that went dark by rounding on a light that does not rise stays dark
 * until the light rises again or stops falling for STEADY_TICKS ticks
 * (held[]), and lowers GRPPWM in the dark. So
 * a BREATHE goes dark a little before the bottom and starts again from
 * GRPPWM 1 with steps of 1/65025. Chips that come out of the dark light
 * on the same tick. When an output rounds to 0 and the other colors of
 * that LED are at their last steps (PWMx 2 or less), the LED goes dark as
 * a whole, so the three colors go dark together. Light that changes
 * inside a range of 2 (CHASE: the dot shares its light between two rows)
 * or keeps its peak (SPECTRUM, FILL, SPLIT) runs on a constant GRPPWM.
 * One transfer writes the registers from the first changed one to the
 * last changed one.
 */
#include <string.h>
#include "tsx.h"

#define TICK_MS		10
#define FP		256		/* 8 fraction bits, level units */
#define LEVEL_MAX	(100 * FP)

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

static uint32_t cur[NLEDS][NCOLORS];	/* current duty 0..65535, before the limits */
static uint32_t ramp_a[NLEDS][NCOLORS], ramp_b[NLEDS][NCOLORS];	/* the ends, after the per-LED limit */
static uint32_t ramp_raw[NLEDS][NCOLORS];	/* the target as given, shown at the end */
static uint32_t ramp_sa[NLEDS], ramp_sb[NLEDS], ramp_smax[NLEDS];
static int32_t ramp_la[NLEDS], ramp_lb[NLEDS];
static uint16_t ramp_lim, ramp_extra;
static uint32_t ramp_start, ramp_ms;
static int32_t base_target[NLEDS][NCOLORS];	/* last base target, to detect changes */

static enum fx fx;
static uint8_t fx_rgb[NCOLORS];
static uint32_t fx_t0, fx_a, fx_b;
static uint8_t fx_level;
static bool fx_ring;		/* SPECTRUM: the ring, not the rows */
static uint16_t direct_duty[NCOLORS];
static uint32_t last_tick;
/* LEDs whose color the per-LED limit scaled before the engine dimmed or shared it */
static uint16_t lim_bits;
static bool fx_frozen;		/* FX FREEZE: the effect clock stands still */
static uint32_t fx_step_ms;	/* FX STEP: move the effect clock forward */

/* the registers PWM0..15 and GRPPWM as the engine last wrote them */
static uint8_t sent[NCOLORS][TLC_DIM_REGS];
static uint32_t grp_changes[NCOLORS];	/* GRPPWM writes of the engine, for TLCREGS */
static bool held[NCOLORS];		/* dark while the light falls */
static uint32_t prev_tmax[NCOLORS];
static uint8_t steady[NCOLORS];		/* ticks without a fall of tmax */

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
bool leds_trace_regs;	/* TRACE REGS: also one "reg" line with PWM0..15 and GRPPWM of each chip */
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

static void fill_all(uint32_t out[NLEDS][NCOLORS], const uint32_t rgb[NCOLORS])
{
	for (int i = 0; i < NLEDS; i++)
		memcpy(out[i], rgb, sizeof(out[i]));
}

static void levels_to_duties(int32_t lv[NLEDS][NCOLORS], uint32_t out[NLEDS][NCOLORS])
{
	for (int i = 0; i < NLEDS; i++) {
		for (int c = 0; c < NCOLORS; c++)
			out[i][c] = level_to_duty(lv[i][c]);
	}
}

/* the light (0..65535) of a lightness that is the fraction k (0..65535) of the full range */
static uint32_t envelope(uint32_t k)
{
	return level_to_duty((int32_t)((uint64_t)k * LEVEL_MAX / 65535U));
}

/*
 * Dim a color in light: each duty times e / 65535, so the duty ratios,
 * the hue, stay the same. WHOLE: when a lit channel would round to 0, the
 * LED goes dark as a whole, so the three colors go dark together.
 */
static void scale_color(const uint32_t full[NCOLORS], uint32_t e, uint32_t out[NCOLORS], bool whole)
{
	bool cut = false, lit = false;

	for (int c = 0; c < NCOLORS; c++) {
		out[c] = (uint32_t)((uint64_t)full[c] * e / 65535U);
		cut = cut || (full[c] && !out[c]);
		lit = lit || out[c];
	}
	if (whole && cut && lit)
		memset(out, 0, NCOLORS * sizeof(out[0]));
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
		held[c] = false;
	}
}

/* FX FREEZE: stop or run the effect clock. FX STEP: move it forward by MS */
void leds_freeze(bool on)
{
	fx_frozen = on;
}

bool leds_frozen(void)
{
	return fx_frozen;
}

void leds_step(uint32_t ms)
{
	fx_step_ms += ms;
}

/* the number of GRPPWM changes the engine wrote to the chip since the start */
uint32_t leds_grp_changes(int c)
{
	return c >= 0 && c < NCOLORS ? grp_changes[c] : 0;
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
	for (int i = 0; i < NLEDS; i++)
		memcpy(out[i], host, sizeof(out[i]));
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

/*
 * Ramps work in light: each LED goes on a straight line between the duties
 * of its two ends (after the per-LED limit), so a fade of one color keeps
 * its hue. The time on that line follows the lightness: the lightness of
 * the sum of the three duties moves linearly from one end to the other.
 * TO is the target as duties before the limits. EXTRA: LEDs that show the
 * limit flag for this target (FILL).
 */
/*
 * A hue at full level (the hue math of hue_to_rgb), after the per-LED
 * limit, dimmed in light by e. Return true when the limit scaled it.
 */
static bool hue_color(uint32_t hue, uint32_t e, uint32_t out[NCOLORS])
{
	int32_t lv[NCOLORS];
	uint32_t full[NCOLORS];
	bool lim;

	hue_to_rgb(hue, 100, lv);
	for (int c = 0; c < NCOLORS; c++)
		full[c] = level_to_duty(lv[c]);
	lim = limit_led(full);
	scale_color(full, e, out, false);
	return lim;
}

static void start_ramp(uint32_t to[NLEDS][NCOLORS], uint32_t ms, uint16_t extra)
{
	ramp_lim = lim_bits | extra;
	ramp_extra = extra;
	for (int i = 0; i < NLEDS; i++) {
		uint32_t *a = ramp_a[i], *b = ramp_b[i], sa, sb, smax;

		memcpy(a, cur[i], sizeof(ramp_a[i]));
		memcpy(b, to[i], sizeof(ramp_b[i]));
		if (limit_led(a) | limit_led(b))
			ramp_lim |= (uint16_t)(1U << i);
		sa = a[RED] + a[GREEN] + a[BLUE];
		sb = b[RED] + b[GREEN] + b[BLUE];
		smax = sa > sb ? sa : sb;
		ramp_sa[i] = sa;
		ramp_sb[i] = sb;
		ramp_smax[i] = smax;
		ramp_la[i] = smax ? duty_to_level((uint32_t)((uint64_t)sa * 65535U / smax)) : 0;
		ramp_lb[i] = smax ? duty_to_level((uint32_t)((uint64_t)sb * 65535U / smax)) : 0;
	}
	memcpy(ramp_raw, to, sizeof(ramp_raw));
	ramp_start = millis();
	ramp_ms = ms;
}

static void run_ramp(void)
{
	uint32_t t = millis() - ramp_start;

	if (ramp_ms == 0 || t >= ramp_ms) {
		memcpy(cur, ramp_raw, sizeof(cur));
		lim_bits = ramp_extra;
		return;
	}
	lim_bits = ramp_lim;
	for (int i = 0; i < NLEDS; i++) {
		uint32_t sa = ramp_sa[i], sb = ramp_sb[i], smax = ramp_smax[i];
		int64_t w;	/* 0..65536 along the line */
		bool cut = false, lit = false;

		if (smax == 0) {
			memset(cur[i], 0, sizeof(cur[i]));
			continue;
		}
		if (sa == sb) {
			w = (int64_t)t * 65536 / ramp_ms;
		} else {
			int32_t l = ramp_la[i] + (int32_t)((int64_t)(ramp_lb[i] - ramp_la[i]) * (int32_t)t /
							   (int32_t)ramp_ms);
			int64_t x = (int64_t)level_to_duty(l) * smax / 65535;

			w = (x - (int64_t)sa) * 65536 / ((int64_t)sb - (int64_t)sa);
			w = w < 0 ? 0 : w > 65536 ? 65536 : w;
		}
		for (int c = 0; c < NCOLORS; c++) {
			uint64_t a = ramp_a[i][c], b = ramp_b[i][c];

			cur[i][c] = (uint32_t)((a * (uint64_t)(65536 - w) + b * (uint64_t)w) >> 16);
			cut = cut || ((a || b) && !cur[i][c]);
			lit = lit || cur[i][c];
		}
		/* a fade from or to black: the three colors go dark together */
		if ((sa == 0 || sb == 0) && cut && lit)
			memset(cur[i], 0, sizeof(cur[i]));
	}
}

/*
 * GRPPWM rules, see the comment at the top. GRP_IDLE: a dark chip.
 * GRP_LOWER: GRPPWM is too coarse at this many times the need.
 * STEADY_TICKS: ticks without a fall of the light before a coarse chip
 * goes dark for one tick to lower GRPPWM.
 */
#define GRP_IDLE	1
#define GRP_LOWER	4
#define STEADY_TICKS	3


/* a new GRPPWM for the brightest target tmax: 1.5 x tmax / 255, rounded up */
static uint32_t grp_for(uint32_t tmax)
{
	uint32_t g = (3 * tmax + 509) / 510;

	return g > 255 ? 255 : g;
}

/* the targets of one chip by output, t = d x 65025 / 65535, at least 1 when lit */
static uint32_t chip_targets(uint32_t duty[NLEDS][NCOLORS], int c, uint32_t t[16])
{
	const uint8_t *led_out = ledmap_outputs();
	uint32_t tmax = 0;

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
	return tmax;
}

/* the 17 registers of each chip for these duties, see the comment at the top */
static void chips_regs(uint32_t duty[NLEDS][NCOLORS], uint8_t v[NCOLORS][TLC_DIM_REGS])
{
	uint32_t t[NCOLORS][16], tmax[NCOLORS];
	bool coarse[NCOLORS] = { false }, lowering[NCOLORS] = { false };
	bool released[NCOLORS] = { false }, rise = false, still = true, any_held = false;
	bool release, any_lowering = false;

	for (int c = 0; c < NCOLORS; c++) {
		tmax[c] = chip_targets(duty, c, t[c]);
		steady[c] = tmax[c] < prev_tmax[c] ? 0 : steady[c] < 255 ? steady[c] + 1 : 255;
		if (held[c]) {
			any_held = true;
			rise = rise || tmax[c] > prev_tmax[c];
			still = still && steady[c] >= STEADY_TICKS;
		}
	}
	/* a held chip lights again when a held light rises, or none falls any more */
	release = any_held && (rise || still);
	for (int c = 0; c < NCOLORS; c++) {
		uint32_t g = sent[c][16], need = (tmax[c] + 254) / 255, p;
		bool dark = true;

		for (int o = 0; o < 16; o++)
			dark = dark && (g == 0 || sent[c][o] == 0);
		if (held[c] && release) {
			held[c] = false;
			released[c] = true;
		}
		memset(v[c], 0, 16);
		if (tmax[c] == 0) {
			/* dark since the last write: GRPPWM 1, so the next light is a raise */
			held[c] = false;
			v[c][16] = (uint8_t)(dark ? GRP_IDLE : g);
			continue;
		}
		if (held[c]) {
			/* the light still falls: stay dark, lower GRPPWM in the dark */
			if (dark && g >= GRP_LOWER * need)
				g = grp_for(tmax[c]);
			v[c][16] = (uint8_t)g;
			continue;
		}
		if (need > g) {
			g = grp_for(tmax[c]);		/* a raise is safe */
		} else if (g >= GRP_LOWER * need) {
			if (dark || steady[c] >= STEADY_TICKS) {
				/* lower GRPPWM only in a write with PWMx 0 */
				v[c][16] = (uint8_t)grp_for(tmax[c]);
				lowering[c] = any_lowering = true;
				continue;
			}
			coarse[c] = true;	/* a falling light: keep GRPPWM */
		}
		for (int o = 0; o < 16; o++) {
			p = (t[c][o] + g / 2) / g;
			v[c][o] = (uint8_t)(t[c][o] == 0 ? 0 : p > 255 ? 255 : p == 0 && !coarse[c] ? 1 : p);
		}
		v[c][16] = (uint8_t)g;
	}
	/* chips that come out of the dark light together */
	for (int c = 0; c < NCOLORS; c++) {
		if (released[c] && any_lowering)
			memset(v[c], 0, 16);
	}
	/*
	 * An output of a coarse chip rounds to 0: when the other colors of
	 * that LED are also at their last steps, the LED goes dark as a whole.
	 */
	for (int o = 0; o < 16; o++) {
		bool cut = false, small = true;

		for (int c = 0; c < NCOLORS; c++) {
			cut = cut || (coarse[c] && t[c][o] && !v[c][o]);
			small = small && v[c][o] <= 2;
		}
		if (cut && small) {
			for (int c = 0; c < NCOLORS; c++)
				v[c][o] = 0;
		}
	}
	for (int c = 0; c < NCOLORS; c++) {
		bool off = true;

		for (int o = 0; o < 16; o++)
			off = off && v[c][o] == 0;
		if (tmax[c] && off && tmax[c] <= prev_tmax[c] && !lowering[c])
			held[c] = true;
		prev_tmax[c] = tmax[c];
	}
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
	if (v[16] != sent[c][16])
		grp_changes[c]++;
	memcpy(sent[c], v, TLC_DIM_REGS);
	return true;
}

static void apply(void)
{
	uint32_t duty[NLEDS][NCOLORS];
	uint64_t sum = 0, cap = (uint64_t)NLEDS * led_limit();
	uint16_t limited = 0;
	uint8_t v[NCOLORS][TLC_DIM_REGS];

	for (int i = 0; i < NLEDS; i++) {
		for (int c = 0; c < NCOLORS; c++)
			duty[i][c] = fx == FX_DIRECT ? direct_duty[c] : cur[i][c];
		if (limit_led(duty[i]) ||
		    ((lim_bits >> i & 1) && duty[i][RED] + duty[i][GREEN] + duty[i][BLUE] > 0))
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
	chips_regs(duty, v);
	for (int c = 0; c < NCOLORS; c++) {
		uint32_t max = 0;

		if (!chip_write(c, v[c]))
			continue;	/* the next tick tries again */
		for (int i = 0; i < NLEDS; i++) {
			st.led_duty[i][c] = (uint16_t)duty[i][c];
			if (duty[i][c] > max)
				max = duty[i][c];
		}
		st.duty[c] = (uint16_t)max;
	}
#ifdef TSX_QEMU
	unsigned long ms = (unsigned long)millis();	/* one time for the lines of this tick */

	if (leds_trace) {
		char line[48];

		snprintf(line, sizeof(line), "led %lu %u %u %u\r\n", ms,
			 st.duty[0], st.duty[1], st.duty[2]);
		qemu_uart_write(line);
	}
	if (leds_trace_pix) {
		char line[16];

		snprintf(line, sizeof(line), "pix %lu", ms);
		qemu_uart_write(line);
		for (int i = 0; i < NLEDS; i++) {
			for (int c = 0; c < NCOLORS; c++) {
				snprintf(line, sizeof(line), " %u", st.led_duty[i][c]);
				qemu_uart_write(line);
			}
		}
		qemu_uart_write("\r\n");
	}
	if (leds_trace_regs) {
		char line[16];

		snprintf(line, sizeof(line), "reg %lu", ms);
		qemu_uart_write(line);
		for (int c = 0; c < NCOLORS; c++) {
			for (int r = 0; r < TLC_DIM_REGS; r++) {
				snprintf(line, sizeof(line), " %u", sent[c][r]);
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

	lim_bits = limited_color(fx_rgb, full) ? 0xFFFF : 0;	/* FX CAP can change while it runs */

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
			cur[r][c] = full[c] * k / 256U;
			cur[NROWS + r][c] = cur[r][c];
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
	uint32_t e = level_to_duty(fx_level * FP);
	int n = fx_ring ? NLEDS : NROWS;

	lim_bits = 0;
	for (int p = 0; p < n; p++) {
		uint32_t hue = (p * (1536U / (uint32_t)n) + 1536U - phase) % 1536U;
		int led = fx_ring ? ring_led[p] : p;

		if (hue_color(hue, e, cur[led]))
			lim_bits |= (uint16_t)(1U << led);
		if (!fx_ring) {
			memcpy(cur[NROWS + p], cur[p], sizeof(cur[p]));
			if (lim_bits >> p & 1)
				lim_bits |= (uint16_t)(1U << (NROWS + p));
		}
	}
}

void leds_tick(void)
{
	uint32_t now = millis();
	int32_t target[NLEDS][NCOLORS];
	uint32_t one[NCOLORS], full[NCOLORS], to[NLEDS][NCOLORS];

	if (now - last_tick < TICK_MS)
		return;
	if (fx_frozen) {	/* FX FREEZE: hold the effect clock and the ramp */
		fx_t0 += now - last_tick;
		ramp_start += now - last_tick;
	}
	fx_t0 -= fx_step_ms;	/* FX STEP */
	ramp_start -= fx_step_ms;
	fx_step_ms = 0;
	last_tick = now;

	switch (fx) {
	case FX_NONE:
		base_color(target);
		if (memcmp(target, base_target, sizeof(target)) != 0) {
			memcpy(base_target, target, sizeof(target));
			levels_to_duties(target, to);
			start_ramp(to, smooth_ms, 0);
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
			one[c] = t < fx_a ? level_to_duty(fx_rgb[c] * FP) : 0;
		fill_all(cur, one);
		lim_bits = 0;
		break;
	}
	case FX_BREATHE: {
		uint32_t phase = (uint32_t)((uint64_t)((now - fx_t0) % fx_a) * BREATHE_STEPS / fx_a);

		/* the limit on the full color only, then the color dims in light */
		lim_bits = limited_color(fx_rgb, full) ? 0xFFFF : 0;
		scale_color(full, envelope(breathe_curve(phase)), one, true);
		fill_all(cur, one);
		break;
	}
	case FX_RAINBOW: {
		uint32_t hue = (now - fx_t0) % fx_a * 1536 / fx_a;

		lim_bits = hue_color(hue, level_to_duty(fx_level * FP), one) ? 0xFFFF : 0;
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
		lim_bits = 0;
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
	return led >= 0 && led < NLEDS ? ledmap_outputs()[led] : -1;
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
	uint32_t to[NLEDS][NCOLORS];

	fx = FX_NONE;
	base_color(target);
	memcpy(base_target, target, sizeof(target));
	levels_to_duties(target, to);
	start_ramp(to, smooth_ms, 0);
}

void leds_fx_fade(const uint8_t rgb[3], uint32_t ms)
{
	uint32_t to[NLEDS][NCOLORS], one[NCOLORS];

	for (int c = 0; c < NCOLORS; c++)
		one[c] = level_to_duty((rgb[c] > 100 ? 100 : rgb[c]) * FP);
	fill_all(to, one);
	fx = FX_FADE;
	start_ramp(to, ms, 0);
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
 * is left, as a part of the lightness, dimmed in light so it keeps the
 * hue. A new FILL ramps over the smooth time.
 */
void leds_fx_fill(const uint8_t rgb[3], unsigned percent)
{
	uint32_t to[NLEDS][NCOLORS], full[NCOLORS];
	int32_t fill = (int32_t)((percent > 100 ? 100 : percent) * NROWS * FP / 100U);
	bool lim;

	/* a full row, after the per-LED limit */
	lim = limited_color(rgb, full);
	for (int r = 0; r < NROWS; r++) {
		int32_t part = clamp(fill - (NROWS - 1 - r) * FP, 0, FP);	/* 0..FP */

		scale_color(full, level_to_duty(100 * part), to[r], true);
		memcpy(to[NROWS + r], to[r], sizeof(to[r]));
	}
	fx = FX_FILL;
	start_ramp(to, smooth_ms, lim ? 0xFFFF : 0);
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
	uint32_t to[NLEDS][NCOLORS];

	for (int r = 0; r < NROWS; r++) {
		for (int c = 0; c < NCOLORS; c++) {
			to[r][c] = level_to_duty((right[c] > 100 ? 100 : right[c]) * FP);
			to[NROWS + r][c] = level_to_duty((left[c] > 100 ? 100 : left[c]) * FP);
		}
	}
	fx = FX_SPLIT;
	start_ramp(to, smooth_ms, 0);
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
