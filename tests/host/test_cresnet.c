// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the Cresnet parser in fw/src/cresnet.c. Each packet goes
 * to cresnet_rx in a heap buffer of its exact size, so the address
 * sanitizer stops the test at a read past the end. The LED, USB and guard
 * functions are stubs that count the calls.
 */
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "tsx.h"

static struct led_state st;
static int levels[NCOLORS], blinks[NCOLORS], controls[NCOLORS];
static int level_calls, blink_calls, control_calls, prepares, sends;
static uint8_t last_send[64];
static size_t last_send_len;

const struct led_state *leds_state(void)
{
	return &st;
}

void leds_set_level(int color, unsigned level)
{
	if (color >= 0 && color < NCOLORS)
		levels[color] = (int)level;
	level_calls++;
}

void leds_set_blink_time(int color, unsigned n100ms)
{
	if (color >= 0 && color < NCOLORS)
		blinks[color] = (int)n100ms;
	blink_calls++;
}

void leds_set_control(int color, bool on)
{
	if (color >= 0 && color < NCOLORS)
		controls[color] = on;
	control_calls++;
}

void usb_debug_state(uint32_t v[7])
{
	for (int i = 0; i < 7; i++)
		v[i] = 0x11111111U * (uint32_t)i;
}

bool cresnet_send(const uint8_t *pkt, size_t n)
{
	if (n > sizeof(last_send))
		return false;
	memcpy(last_send, pkt, n);
	last_send_len = n;
	sends++;
	return true;
}

bool usb_configured(void)
{
	return true;
}

void delay_ms(uint32_t ms)
{
	(void)ms;
}

void guard_request_bootloader(void)
{
	prepares++;	/* the bar resets here, the test goes on */
}

static void reset_counts(void)
{
	memset(levels, 0, sizeof(levels));
	memset(blinks, 0, sizeof(blinks));
	memset(controls, 0, sizeof(controls));
	level_calls = blink_calls = control_calls = prepares = sends = 0;
	last_send_len = 0;
}

/* one packet in a buffer of exactly n bytes */
static void rx(const uint8_t *p, size_t n)
{
	uint8_t *b = malloc(n ? n : 1);

	if (n)
		memcpy(b, p, n);
	cresnet_rx(b, n);
	free(b);
}

#define RX(...) do {							\
	static const uint8_t p_[] = { __VA_ARGS__ };			\
	rx(p_, sizeof(p_));						\
} while (0)

static int calls(void)
{
	return level_calls + blink_calls + control_calls + prepares;
}

int main(void)
{
	uint32_t seed = 12345;

	cresnet_init();
	cresnet_poll();
	CHECK(sends == 1 && last_send_len == 4 && memcmp(last_send, "\x02\x02\x03\x00", 4) == 0,
	      "after the start: the update request 02 02 03 00");

	reset_counts();
	rx(NULL, 0);
	RX(0x00);
	RX(0x00, 0x05);
	CHECK(calls() == 0, "packets of 0, 1 and 2 bytes: no action");
	RX(0x00, 0x05, 0x14, 0x00, 0x03, 0x00);
	CHECK(calls() == 0, "a length byte above the packet size: no action");
	RX(0x00, 0xFF, 0x14, 0x00, 0x03, 0x00, 0x32);
	CHECK(calls() == 0, "length 255 in a 7-byte packet: no action");

	reset_counts();
	RX(0x00, 0x05, 0x14, 0x00, 0x03, 0x00, 0x32);
	CHECK(level_calls == 1 && levels[RED] == 50, "analog join 3 = 50: red level 50");
	cresnet_poll();
	CHECK(sends == 1 && last_send_len == 15 && last_send[0] == 0x02 && last_send[2] == 0x14,
	      "the bar answers with its three levels (15 bytes)");

	reset_counts();
	RX(0x00, 0x07, 0x14, 0x00, 0x03, 0x00, 0x32, 0x00, 0x04);
	CHECK(level_calls == 1, "a join without its value at the end: only the full pair runs");
	RX(0x00, 0x09, 0x14, 0x00, 0x04, 0x00, 0x14, 0x00, 0x01, 0x02, 0x58);
	CHECK(level_calls == 2 && levels[GREEN] == 20 && blink_calls == 1 && blinks[GREEN] == 600,
	      "two pairs: green level 20, green blink 600");
	RX(0x00, 0x05, 0x14, 0x00, 0x06, 0x00, 0x32);
	RX(0x00, 0x05, 0x14, 0xFF, 0xFF, 0xFF, 0xFF);
	CHECK(level_calls == 2 && blink_calls == 1, "joins 6 and 0xFFFF: no action");
	RX(0x00, 0x02, 0x14, 0x00);
	CHECK(level_calls == 2, "an analog packet with no pair: no action");

	reset_counts();
	RX(0x00, 0x03, 0x00, 0x01, 0x00);
	RX(0x00, 0x03, 0x00, 0x02, 0x80);
	CHECK(control_calls == 2 && controls[GREEN] == 1 && controls[BLUE] == 0,
	      "digital joins: green on, blue off");
	RX(0x00, 0x02, 0x00, 0x01);
	RX(0x00, 0x03, 0x00, 0x03, 0x00);
	RX(0x00, 0x03, 0x00, 0x00, 0x01);
	CHECK(control_calls == 2, "a short digital packet, join 3 and join 256: no action");

	reset_counts();
	RX(0x02, 0x02, 0x04, 0x04);
	CHECK(prepares == 1, "the prepare packet 02 02 04 04 hands over to the bootloader");
	RX(0x02, 0x01, 0x04);
	RX(0x02, 0x02, 0x04);
	RX(0x02, 0x02, 0x04, 0x05);
	RX(0x02, 0x02, 0x04, 0x01);
	CHECK(prepares == 1, "short download packets and other sub-types: no handover");

	reset_counts();
	RX(0x00, 0x01, 0x7F);
	CHECK(sends == 1 && last_send_len == 31 && last_send[2] == 0x7F, "the debug packet: a 31-byte answer");
	RX(0x00, 0x01, 0x55);
	CHECK(sends == 1 && calls() == 0, "an unknown type: no action");

	/* random packets: the sanitizers check every read */
	reset_counts();
	for (int i = 0; i < 200000; i++) {
		uint8_t p[64], mask = (i & 2) ? 0x07 : 0xFF;	/* small bytes: many joins hit 0..5 */
		size_t n;

		seed = seed * 1103515245U + 12345U;
		n = (seed >> 16) % 65;
		for (size_t k = 0; k < n; k++) {
			seed = seed * 1103515245U + 12345U;
			p[k] = (uint8_t)(seed >> 16) & mask;
		}
		if (n >= 3 && (i & 1)) {
			p[1] = (uint8_t)(n - 2);	/* a length that matches: the payload is parsed */
			p[2] = (uint8_t[]){ 0x00, 0x04, 0x14, 0x7F }[(seed >> 8) & 3];
		}
		rx(p, n);
		cresnet_poll();
	}
	CHECK(level_calls + control_calls > 1000, "200000 random packets: no read past the end (%d joins run)",
	      level_calls + blink_calls + control_calls);
	DONE("test_cresnet");
}
