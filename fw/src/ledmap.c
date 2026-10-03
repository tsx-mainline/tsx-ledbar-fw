// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Board variant and LED map.
 *
 * The controller board has three variant pins, PB13, PB14 and PB15.
 * Resistors on the board set their levels. The stock firmware sets the
 * pins as inputs with no pull-up or pull-down, reads them once at start,
 * and uses PB13 as bit 0, PB14 as bit 1 and PB15 as bit 2. This firmware
 * does the same. The value is 0..7.
 *
 * An LED map gives the TLC59116 output of each LED index (tsx.h: 0..7 are
 * R1..R8, 8..15 are L1..L8). Output n of the red, green and blue chip
 * drives the same LED, so one map serves the three chips. ledmaps[] holds
 * one map for each known value. To add a bar model, add one entry and its
 * test (docs/leds.md, "Board variant and LED map").
 *
 * A value with no entry is unknown. The firmware then uses the map
 * "outputs" (LED index n is output n) and keeps all its functions: the
 * host color, the effects, the power limits and the console. Only the
 * position of each LED can be wrong. LEDMAP on the console selects
 * another map until the next start, without a rebuild.
 */
#include <stddef.h>
#include <strings.h>
#include "tsx.h"

#ifndef TSX_QEMU
#include <libopencm3/stm32/gpio.h>
#define VARIANT_PINS	(GPIO13 | GPIO14 | GPIO15)
#define VARIANT_SHIFT	13
#endif

struct ledmap {
	uint8_t variant;		/* the value of the variant pins */
	const char *name;		/* the LED bar model */
	uint8_t out[NLEDS];		/* LED index to TLC59116 output */
};

/* the known values, each value at most once */
static const struct ledmap ledmaps[] = {
	{ 1, "TSW-1060-LB", {
		15, 6, 0, 1, 2, 3, 4, 5,	/* R1..R8: right side, top to bottom */
		14, 13, 12, 11, 10, 9, 8, 7,	/* L1..L8: left side, top to bottom */
	} },
};
#define NMAPS	(sizeof(ledmaps) / sizeof(ledmaps[0]))

/* the map of an unknown value: LED index n is output n */
static const struct ledmap plain = { 0xFF, "outputs", {
	0, 1, 2, 3, 4, 5, 6, 7,
	8, 9, 10, 11, 12, 13, 14, 15,
} };

static unsigned variant;		/* the value read at start */
static const struct ledmap *by_pins;	/* the map of that value, NULL when unknown */
static const struct ledmap *cur = &plain;
static bool chosen;			/* LEDMAP selected the map, not the pins */

#ifdef TSX_QEMU
/*
 * QEMU build: the netduino2 machine has no GPIO model. A test sets the
 * pins with a word in RAM before the start (qemu/run-test.py,
 * "-device loader"): 0x54535600 | value. Without that word the value is 1,
 * the value of the tested bar.
 */
#define QEMU_VARIANT_MAGIC	0x54535600U	/* "TSV" and the value */
extern volatile uint32_t tsx_qemu_variant;	/* app.ld */

static unsigned read_pins(void)
{
	uint32_t w = tsx_qemu_variant;

	return (w & 0xFFFFFF00U) == QEMU_VARIANT_MAGIC ? (w & 7U) : 1U;
}
#else
static unsigned read_pins(void)
{
	/* as the stock firmware: inputs, no pull-up or pull-down, one read */
	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, VARIANT_PINS);
	return (unsigned)(gpio_get(GPIOB, VARIANT_PINS) >> VARIANT_SHIFT) & 7U;
}
#endif

static const struct ledmap *map_of_value(unsigned v)
{
	for (size_t i = 0; i < NMAPS; i++) {
		if (ledmaps[i].variant == v)
			return &ledmaps[i];
	}
	return NULL;
}

void ledmap_init(void)
{
	variant = read_pins();
	by_pins = map_of_value(variant);
	cur = by_pins ? by_pins : &plain;
	chosen = false;
}

unsigned ledmap_variant(void)
{
	return variant;
}

bool ledmap_known(void)
{
	return by_pins != NULL;
}

const char *ledmap_name(void)
{
	return cur->name;
}

bool ledmap_chosen(void)
{
	return chosen;
}

const uint8_t *ledmap_outputs(void)
{
	return cur->out;
}

void ledmap_auto(void)
{
	cur = by_pins ? by_pins : &plain;
	chosen = false;
}

/* a variant value 0..7, or the name of a map (not case-sensitive) */
bool ledmap_select(const char *what)
{
	const struct ledmap *m = NULL;

	if (!what || !*what)
		return false;
	if (what[0] >= '0' && what[0] <= '7' && what[1] == 0) {
		m = map_of_value((unsigned)(what[0] - '0'));
	} else if (strcasecmp(what, plain.name) == 0) {
		m = &plain;
	} else {
		for (size_t i = 0; i < NMAPS && !m; i++) {
			if (strcasecmp(what, ledmaps[i].name) == 0)
				m = &ledmaps[i];
		}
	}
	if (!m)
		return false;
	cur = m;
	chosen = true;
	return true;
}

/* calls fn for each map: the variant value (-1 for "outputs") and the name */
void ledmap_each(void (*fn)(int, const char *))
{
	for (size_t i = 0; i < NMAPS; i++)
		fn(ledmaps[i].variant, ledmaps[i].name);
	fn(-1, plain.name);
}
