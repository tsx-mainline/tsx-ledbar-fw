// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsx-ledbar: open firmware for the USB LED bar of the Crestron TSW-xx60
 * panels (STM32F205RC, three TLC59116 LED drivers).
 *
 * Start order: guard (watchdog and failed start count), clocks, board
 * variant pins (the LED map), LED chips (with retries), console, Cresnet,
 * USB. The main loop polls USB, runs
 * the console and Cresnet handlers, ticks the LED engine, runs the USB
 * rule of the guard and kicks the watchdog. guard.c tells which starts
 * count as failed.
 */
#include "tsx.h"

int main(void)
{
	guard_boot();
	board_init();
#ifdef TSX_QEMU
	qemu_uart_init();
	console_printf("tsx-ledbar qemu start %lu mailbox 0x%08lX fails %lu\r\n",
		       (unsigned long)guard_start_count(),
		       (unsigned long)*(volatile uint32_t *)0x200000F4,
		       (unsigned long)guard_fails());
#endif
	guard_report();
	ledmap_init();

	delay_ms(50);		/* let the LED driver supply settle */
	leds_init();
	console_init();
	cresnet_init();
	usb_init();

	for (;;) {
		usb_poll();
		console_poll();
		cresnet_poll();
		leds_tick();
		guard_poll();
		guard_kick();
	}
}
