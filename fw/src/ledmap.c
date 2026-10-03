// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Board variant and LED map.
 *
 * An LED map gives the TLC59116 output of each LED index (tsx.h: 0..7 are
 * R1..R8, 8..15 are L1..L8). Output n of the red, green and blue chip
 * drives the same LED, so one map serves the three chips. ledmaps[] holds
 * one map for each tested LED bar model, by the name of the model. The map
 * "outputs" (LED index n is output n) is for the bring-up of a bar with no
 * map. Add a map only for a bar model that was measured and tested
 * (docs/leds.md, "Board variant and LED map").
 *
 * The bar cannot tell its model, so the panel tells it: the service
 * tsx-ledbard sends "LEDMAP NAME PANEL" after each plug-in and after each
 * start of the bar. Until then the firmware uses the default map, the map
 * of the TSW-1060-LB. The firmware does not save the choice.
 *
 * The controller board has three variant pins, PB13, PB14 and PB15.
 * Resistors on the board set their levels. The stock firmware sets the
 * pins as inputs with no pull-up or pull-down, reads them once at start,
 * and uses PB13 as bit 0, PB14 as bit 1 and PB15 as bit 2. This firmware
 * reads them the same way and only reports the value 0..7 (STATUS,
 * LEDMAP). The stock firmware has one name for all its LED bars, the name
 * of value 1, so the value does not tell the bar model. The value does not
 * change the behavior of this firmware.
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
	const char *name;		/* the LED bar model */
	uint8_t out[NLEDS];		/* LED index to TLC59116 output */
};

/* the tested LED bar models, each name once. The first entry is the default map. */
static const struct ledmap ledmaps[] = {
	{ "TSW-1060-LB", {
		15, 6, 0, 1, 2, 3, 4, 5,	/* R1..R8: right side, top to bottom */
		14, 13, 12, 11, 10, 9, 8, 7,	/* L1..L8: left side, top to bottom */
	} },
};
#define NMAPS		(sizeof(ledmaps) / sizeof(ledmaps[0]))
#define DEFAULT_MAP	(&ledmaps[0])

/* the map for the bring-up of a bar with no map: LED index n is output n */
static const struct ledmap plain = { "outputs", {
	0, 1, 2, 3, 4, 5, 6, 7,
	8, 9, 10, 11, 12, 13, 14, 15,
} };

static const char *const source_names[] = {
	[LEDMAP_DEFAULT] = "default",
	[LEDMAP_PANEL] = "panel",
	[LEDMAP_CONSOLE] = "console",
};

static unsigned variant;		/* the value of the pins, read at start */
static const struct ledmap *cur = DEFAULT_MAP;
static enum ledmap_source source;

#ifdef TSX_QEMU
/*
 * QEMU build: the netduino2 machine has no GPIO model. A test sets the
 * pins with a word in RAM before the start (qemu/run-test.py,
 * "-device loader"): 0x54535600 | value. Without that word the value is 1,
 * the value of the TSW-1060-LB.
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

void ledmap_init(void)
{
	variant = read_pins();
	ledmap_default();
}

unsigned ledmap_variant(void)
{
	return variant;
}

const char *ledmap_name(void)
{
	return cur->name;
}

const char *ledmap_source(void)
{
	return source_names[source];
}

const uint8_t *ledmap_outputs(void)
{
	return cur->out;
}

void ledmap_default(void)
{
	cur = DEFAULT_MAP;
	source = LEDMAP_DEFAULT;
}

/* the map with this name (not case-sensitive), from the panel or the console */
bool ledmap_select(const char *name, enum ledmap_source src)
{
	const struct ledmap *m = NULL;

	if (!name || !*name)
		return false;
	if (strcasecmp(name, plain.name) == 0)
		m = &plain;
	for (size_t i = 0; i < NMAPS && !m; i++) {
		if (strcasecmp(name, ledmaps[i].name) == 0)
			m = &ledmaps[i];
	}
	if (!m)
		return false;
	cur = m;
	source = src;
	return true;
}

/* calls fn with the name of each map: the bar models, then "outputs" */
void ledmap_each(void (*fn)(const char *))
{
	for (size_t i = 0; i < NMAPS; i++)
		fn(ledmaps[i].name);
	fn(plain.name);
}

/*
 * The console command LEDMAP, argv[0] is "LEDMAP":
 *   LEDMAP               show the map (LEDMAP_SHOW)
 *   LEDMAP NAME          use the map NAME, source "console"
 *   LEDMAP NAME PANEL    use the map NAME, source "panel" (tsx-ledbard)
 *   LEDMAP DEFAULT       use the default map, source "default"
 * A name with no map changes nothing (LEDMAP_NO_MAP). Other forms change
 * nothing (LEDMAP_USAGE). Selection by the variant value does not exist.
 */
enum ledmap_cmd ledmap_command(int argc, char *const *argv)
{
	if (argc == 1)
		return LEDMAP_SHOW;
	if (argc == 2 && strcasecmp(argv[1], "DEFAULT") == 0) {
		ledmap_default();
		return LEDMAP_DONE;
	}
	if (argc == 2)
		return ledmap_select(argv[1], LEDMAP_CONSOLE) ? LEDMAP_DONE : LEDMAP_NO_MAP;
	if (argc == 3 && strcasecmp(argv[2], "PANEL") == 0 && strcasecmp(argv[1], "DEFAULT") != 0)
		return ledmap_select(argv[1], LEDMAP_PANEL) ? LEDMAP_DONE : LEDMAP_NO_MAP;
	return LEDMAP_USAGE;
}
