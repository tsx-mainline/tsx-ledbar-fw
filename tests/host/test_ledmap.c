// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the board variant and the LED map (fw/src/ledmap.c, the
 * bar build). A fake GPIO port B records the pin setup and returns a
 * chosen input data register. Checks: the pin setup of the stock firmware
 * (PB13..PB15 inputs, no pull-up or pull-down, set before the read), the
 * bit order (PB13 is bit 0), the map of each value, the "outputs" map of
 * an unknown value, and LEDMAP (select and auto).
 */
#include <string.h>
#include <libopencm3/stm32/gpio.h>
#include "check.h"
#include "tsx.h"

static const uint8_t map1[NLEDS] = { 15, 6, 0, 1, 2, 3, 4, 5, 14, 13, 12, 11, 10, 9, 8, 7 };
static const uint8_t plain[NLEDS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static uint16_t idr;		/* input data register of port B */
static int setups, reads, bad_setups, bad_reads, read_first;
static int nmaps, map_values[8];
static const char *map_names[8];

void gpio_mode_setup(uint32_t port, uint8_t mode, uint8_t pull, uint16_t gpios)
{
	setups++;
	if (port != GPIOB || mode != GPIO_MODE_INPUT || pull != GPIO_PUPD_NONE ||
	    gpios != (GPIO13 | GPIO14 | GPIO15))
		bad_setups++;
}

uint16_t gpio_get(uint32_t port, uint16_t gpios)
{
	reads++;
	if (!setups)
		read_first++;
	if (port != GPIOB)
		bad_reads++;
	return idr & gpios;
}

static void start(uint16_t reg)
{
	idr = reg;
	setups = reads = bad_setups = bad_reads = read_first = 0;
	ledmap_init();
}

static bool is_perm(const uint8_t *out)
{
	bool seen[NLEDS] = { false };

	for (int i = 0; i < NLEDS; i++) {
		if (out[i] >= NLEDS || seen[out[i]])
			return false;
		seen[out[i]] = true;
	}
	return true;
}

static void each_map(int v, const char *name)
{
	if (nmaps < 8) {
		map_values[nmaps] = v;
		map_names[nmaps] = name;
	}
	nmaps++;
}

int main(void)
{
	/* the pins and the value: PB13 bit 0, PB14 bit 1, PB15 bit 2. The other pins do not count. */
	for (unsigned v = 0; v < 8; v++) {
		start((uint16_t)(v << 13 | 0x1FFF));
		CHECK(ledmap_variant() == v, "pins %u%u%u (PB15 PB14 PB13), other pins high: value %u, got %u",
		      v >> 2 & 1, v >> 1 & 1, v & 1, v, ledmap_variant());
		CHECK(setups == 1 && bad_setups == 0, "value %u: PB13..PB15 set once as inputs with no pull (%d setups, %d bad)",
		      v, setups, bad_setups);
		CHECK(reads == 1 && bad_reads == 0 && read_first == 0, "value %u: one read of port B after the setup", v);
	}
	start(GPIO13);
	CHECK(ledmap_variant() == 1, "PB13 alone is value 1");
	start(GPIO14);
	CHECK(ledmap_variant() == 2, "PB14 alone is value 2");
	start(GPIO15);
	CHECK(ledmap_variant() == 4, "PB15 alone is value 4");
	start(0x1FFF);
	CHECK(ledmap_variant() == 0, "PB0..PB12 high, PB13..PB15 low: value 0");

	/* value 1: the map of the tested bar */
	start(GPIO13);
	CHECK(ledmap_known() && !ledmap_chosen(), "value 1 is known, the map comes from the pins");
	CHECK(strcmp(ledmap_name(), "TSW-1060-LB") == 0, "value 1: map TSW-1060-LB (%s)", ledmap_name());
	CHECK(memcmp(ledmap_outputs(), map1, NLEDS) == 0, "value 1: outputs 15 6 0 1 2 3 4 5 14 13 12 11 10 9 8 7");

	/* the other values are unknown: plain output order, all functions stay */
	for (unsigned v = 0; v < 8; v++) {
		if (v == 1)
			continue;
		start((uint16_t)(v << 13));
		CHECK(!ledmap_known() && !ledmap_chosen() && strcmp(ledmap_name(), "outputs") == 0 &&
		      memcmp(ledmap_outputs(), plain, NLEDS) == 0,
		      "value %u is unknown: map outputs, LED index n is output n", v);
	}

	/* LEDMAP N|NAME|AUTO on an unknown value */
	start(5U << 13);
	CHECK(ledmap_select("1") && ledmap_chosen() && strcmp(ledmap_name(), "TSW-1060-LB") == 0 &&
	      memcmp(ledmap_outputs(), map1, NLEDS) == 0, "value 5, LEDMAP 1: the map of value 1");
	CHECK(ledmap_variant() == 5 && !ledmap_known(), "LEDMAP keeps the value of the pins and its state");
	ledmap_auto();
	CHECK(!ledmap_chosen() && strcmp(ledmap_name(), "outputs") == 0, "LEDMAP AUTO: back to outputs");
	CHECK(ledmap_select("tsw-1060-lb") && strcmp(ledmap_name(), "TSW-1060-LB") == 0,
	      "LEDMAP takes a map name, not case-sensitive");
	CHECK(ledmap_select("OUTPUTS") && memcmp(ledmap_outputs(), plain, NLEDS) == 0 && ledmap_chosen(),
	      "LEDMAP OUTPUTS: the plain order");
	ledmap_select("1");
	CHECK(!ledmap_select("3") && strcmp(ledmap_name(), "TSW-1060-LB") == 0,
	      "LEDMAP 3: no map for value 3, the map stays");
	CHECK(!ledmap_select("8") && !ledmap_select("12") && !ledmap_select("") && !ledmap_select(NULL) &&
	      !ledmap_select("-1") && !ledmap_select("TSW") && ledmap_chosen() &&
	      strcmp(ledmap_name(), "TSW-1060-LB") == 0, "bad LEDMAP arguments change nothing");
	start(5U << 13);
	CHECK(!ledmap_chosen() && strcmp(ledmap_name(), "outputs") == 0, "a new start forgets the LEDMAP choice");

	/* LEDMAP on a known value */
	start(GPIO13);
	CHECK(ledmap_select("outputs") && strcmp(ledmap_name(), "outputs") == 0 && ledmap_known(),
	      "value 1, LEDMAP OUTPUTS: plain order, the value stays known");
	ledmap_auto();
	CHECK(memcmp(ledmap_outputs(), map1, NLEDS) == 0 && !ledmap_chosen(), "LEDMAP AUTO: back to the map of value 1");

	/* every map: a value 0..7 at most once, a name, each output once */
	ledmap_each(each_map);
	CHECK(nmaps >= 2 && nmaps <= 8 && map_values[nmaps - 1] == -1 &&
	      strcmp(map_names[nmaps - 1], "outputs") == 0, "the list ends with the map outputs (%d maps)", nmaps);
	for (int i = 0; i < nmaps && i < 8; i++) {
		bool once = true;

		for (int j = 0; j < i; j++)
			once = once && map_values[j] != map_values[i];
		CHECK(map_names[i] && *map_names[i] && once && map_values[i] >= -1 && map_values[i] <= 7,
		      "map %d: value %d once, name %s", i, map_values[i], map_names[i] ? map_names[i] : "(none)");
		CHECK(ledmap_select(map_names[i]) && is_perm(ledmap_outputs()),
		      "map %s: each output 0..15 once", map_names[i]);
	}
	DONE("test_ledmap");
}
