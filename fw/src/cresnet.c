// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cresnet packets on USB interface 1, one packet per transfer:
 *
 *   dest(1) len(1) payload(len)
 *
 * analog join:  payload 14 JH JL VH VL, more join/value pairs may follow
 *               joins 3/4/5 = red/green/blue level 0..100
 *               joins 0/1/2 = blink time in 100 ms units
 * digital join: payload 00 JL JH, bit 7 of JH = off
 *               joins 0/1/2 = red/green/blue control (on/off)
 *
 * Answers from the bar start with 02. After each host packet the bar
 * sends its three levels: 02 0D 14 00 03 00 rr 00 04 00 gg 00 05 00 bb.
 * After it starts it sends the update request 02 02 03 00, and the host
 * driver answers with the full state.
 *
 * Firmware update: the host sends 02 02 04 04 ("prepare for download").
 * The application hands the bar to the bootloader (mailbox "UPG") and
 * resets. See docs/update-protocol.md.
 */
#include "tsx.h"

#define TYPE_DIGITAL	0x00
#define TYPE_DOWNLOAD	0x04
#define SUB_PREPARE	0x04
#define TYPE_ANALOG	0x14
#define JOIN_BLINK0	0
#define JOIN_LEVEL0	3

static bool send_update_request;
static bool send_state;

static void send_levels(void)
{
	const struct led_state *s = leds_state();
	uint8_t pkt[15] = { 0x02, 0x0D, TYPE_ANALOG };

	for (int c = 0; c < NCOLORS; c++) {
		pkt[3 + 4 * c] = 0;
		pkt[4 + 4 * c] = (uint8_t)(JOIN_LEVEL0 + c);
		pkt[5 + 4 * c] = 0;
		pkt[6 + 4 * c] = s->level[c];
	}
	cresnet_send(pkt, sizeof(pkt));
}

void cresnet_rx(const uint8_t *data, size_t n)
{
	size_t len;

	if (n < 3)
		return;
	len = data[1];
	if (len + 2 > n)
		return;
	switch (data[2]) {
	case TYPE_ANALOG:
		for (size_t i = 3; i + 3 < len + 2; i += 4) {
			unsigned join = (unsigned)data[i] << 8 | data[i + 1];
			unsigned value = (unsigned)data[i + 2] << 8 | data[i + 3];

			if (join >= JOIN_LEVEL0 && join < JOIN_LEVEL0 + NCOLORS)
				leds_set_level((int)(join - JOIN_LEVEL0), value);
			else if (join < JOIN_BLINK0 + NCOLORS)
				leds_set_blink_time((int)(join - JOIN_BLINK0), value);
		}
		send_state = true;
		break;
	case TYPE_DOWNLOAD:
		if (len >= 2 && data[3] == SUB_PREPARE) {
			delay_ms(50);	/* let the transfer finish on the bus */
			guard_request_bootloader();
		}
		break;
	case TYPE_DIGITAL:
		if (len >= 3) {
			unsigned join = (unsigned)(data[4] & 0x7F) << 8 | data[3];
			bool on = !(data[4] & 0x80);

			if (join < NCOLORS)
				leds_set_control((int)join, on);
		}
		send_state = true;
		break;
	default:
		break;
	}
}

void cresnet_init(void)
{
	send_update_request = true;
}

void cresnet_poll(void)
{
	static bool was_configured;
	static const uint8_t update_request[4] = { 0x02, 0x02, 0x03, 0x00 };
	bool now = usb_configured();

	if (now && !was_configured)
		send_update_request = true;
	was_configured = now;
	if (!now)
		return;
	if (send_update_request && cresnet_send(update_request, sizeof(update_request)))
		send_update_request = false;
	if (send_state) {
		send_levels();
		send_state = false;
	}
}
