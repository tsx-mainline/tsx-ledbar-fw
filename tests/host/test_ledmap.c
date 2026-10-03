// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the LED maps and the board variant (fw/src/ledmap.c, the
 * bar build). A fake GPIO port B records the pin setup and returns a
 * chosen input data register. Checks: the pin setup of the stock firmware
 * (PB13..PB15 inputs, no pull-up or pull-down, set before the read), the
 * bit order (PB13 is bit 0), the default map with each value (the value
 * is information only), the map table, and the parser of the console
 * command LEDMAP [NAME [PANEL] | DEFAULT].
 */
#include <string.h>
#include <strings.h>
#include <libopencm3/stm32/gpio.h>
#include "check.h"
#include "tsx.h"

static const uint8_t map1060[NLEDS] = { 15, 6, 0, 1, 2, 3, 4, 5, 14, 13, 12, 11, 10, 9, 8, 7 };
static const uint8_t plain[NLEDS] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };

static uint16_t idr;		/* input data register of port B */
static int setups, reads, bad_setups, bad_reads, read_first;
static int nmaps;
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

static void each_map(const char *name)
{
	if (nmaps < 8)
		map_names[nmaps] = name;
	nmaps++;
}

/* the map in use is NAME with these outputs and SOURCE */
static bool now(const char *name, const uint8_t *outs, const char *source)
{
	return strcmp(ledmap_name(), name) == 0 && memcmp(ledmap_outputs(), outs, NLEDS) == 0 &&
	       strcmp(ledmap_source(), source) == 0;
}

/* run the console command LEDMAP with the words of LINE */
static enum ledmap_cmd ledmap(const char *line)
{
	static char buf[96];
	char *argv[8];
	int argc = 0;

	strncpy(buf, line, sizeof(buf) - 1);
	for (char *w = strtok(buf, " "); w && argc < 8; w = strtok(NULL, " "))
		argv[argc++] = w;
	return ledmap_command(argc, argv);
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

	/* the value is information only: each value starts with the default map */
	for (unsigned v = 0; v < 8; v++) {
		start((uint16_t)(v << 13));
		CHECK(now("TSW-1060-LB", map1060, "default"),
		      "value %u: map TSW-1060-LB, source default (%s %s)", v, ledmap_name(), ledmap_source());
	}

	/* LEDMAP with no argument shows and changes nothing */
	start(GPIO13);
	CHECK(ledmap("LEDMAP") == LEDMAP_SHOW && now("TSW-1060-LB", map1060, "default"),
	      "LEDMAP: show, the map stays");

	/* LEDMAP NAME from the console, LEDMAP NAME PANEL from the panel */
	CHECK(ledmap("LEDMAP outputs") == LEDMAP_DONE && now("outputs", plain, "console"),
	      "LEDMAP outputs: LED index n is output n, source console");
	CHECK(ledmap("LEDMAP TSW-1060-LB PANEL") == LEDMAP_DONE && now("TSW-1060-LB", map1060, "panel"),
	      "LEDMAP TSW-1060-LB PANEL: source panel");
	CHECK(ledmap("LEDMAP OUTPUTS panel") == LEDMAP_DONE && now("outputs", plain, "panel"),
	      "the map name and PANEL are not case-sensitive");
	CHECK(ledmap("LEDMAP tsw-1060-lb") == LEDMAP_DONE && now("TSW-1060-LB", map1060, "console"),
	      "LEDMAP tsw-1060-lb: source console");
	CHECK(ledmap("LEDMAP DEFAULT") == LEDMAP_DONE && now("TSW-1060-LB", map1060, "default"),
	      "LEDMAP DEFAULT: the default map, source default");
	ledmap("LEDMAP outputs");
	CHECK(ledmap("LEDMAP default") == LEDMAP_DONE && now("TSW-1060-LB", map1060, "default"),
	      "LEDMAP default after LEDMAP outputs: back to the default map");

	/* no selection by the variant value, unknown names and bad forms change nothing */
	ledmap("LEDMAP outputs PANEL");
	for (unsigned v = 0; v < 8; v++) {
		char line[16] = "LEDMAP 0";

		line[7] = (char)('0' + v);
		CHECK(ledmap(line) == LEDMAP_NO_MAP && now("outputs", plain, "panel"),
		      "%s: no map, the map stays", line);
	}
	CHECK(ledmap("LEDMAP TSW") == LEDMAP_NO_MAP && ledmap("LEDMAP AUTO") == LEDMAP_NO_MAP &&
	      ledmap("LEDMAP PANEL") == LEDMAP_NO_MAP && ledmap("LEDMAP TSW-1060-LBX PANEL") == LEDMAP_NO_MAP &&
	      now("outputs", plain, "panel"), "unknown map names change nothing");
	CHECK(ledmap("LEDMAP TSW-1060-LB CONSOLE") == LEDMAP_USAGE && ledmap("LEDMAP DEFAULT PANEL") == LEDMAP_USAGE &&
	      ledmap("LEDMAP TSW-1060-LB PANEL X") == LEDMAP_USAGE && now("outputs", plain, "panel"),
	      "bad forms: usage, the map stays");
	CHECK(!ledmap_select(NULL, LEDMAP_CONSOLE) && !ledmap_select("", LEDMAP_CONSOLE) && now("outputs", plain, "panel"),
	      "an empty name selects nothing");

	/* a new start forgets the choice */
	start(GPIO13);
	CHECK(now("TSW-1060-LB", map1060, "default"), "a new start: the default map again");

	/* every map: a name once, each output once, the bar models first, "outputs" last */
	ledmap_each(each_map);
	CHECK(nmaps >= 2 && nmaps <= 8 && strcmp(map_names[0], "TSW-1060-LB") == 0 &&
	      strcmp(map_names[nmaps - 1], "outputs") == 0, "the list: TSW-1060-LB first, outputs last (%d maps)", nmaps);
	for (int i = 0; i < nmaps && i < 8; i++) {
		bool once = true;

		for (int j = 0; j < i; j++)
			once = once && strcasecmp(map_names[j], map_names[i]) != 0;
		CHECK(map_names[i] && *map_names[i] && once && strcasecmp(map_names[i], "DEFAULT") != 0 &&
		      strcasecmp(map_names[i], "PANEL") != 0 && !strchr(map_names[i], ' '),
		      "map %d: name %s once, one word, not a keyword", i, map_names[i] ? map_names[i] : "(none)");
		CHECK(ledmap_select(map_names[i], LEDMAP_CONSOLE) && is_perm(ledmap_outputs()),
		      "map %s: each output 0..15 once", map_names[i]);
	}
	DONE("test_ledmap");
}
