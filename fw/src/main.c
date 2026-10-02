// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * tsx-ledbar: open firmware for the USB LED bar of the Crestron TSW-xx60
 * panels (STM32F205RC, three TLC59116 LED drivers).
 *
 * Start order: guard (watchdog and start counter), clocks, LED chips
 * (with retries), console, Cresnet, USB. The main loop polls USB, runs
 * the console and Cresnet handlers, ticks the LED engine and kicks the
 * watchdog.
 *
 * A firmware that runs but never reaches "USB configured" would be out
 * of reach for the update tool. So the first configuration must come
 * within USB_START_TIMEOUT_MS: otherwise the firmware resets, which
 * counts as a failed start for the guard (three of them hand the bar to
 * the bootloader). After the first configuration there is no limit: a
 * panel reboot leaves the bar powered without a host for a while.
 */
#include "tsx.h"

#define USB_START_TIMEOUT_MS	60000

int main(void)
{
	guard_boot();
	board_init();
#ifdef TSX_QEMU
	qemu_uart_init();
	console_printf("tsx-ledbar qemu start %lu mailbox 0x%08lX\r\n",
		       (unsigned long)guard_start_count(),
		       (unsigned long)*(volatile uint32_t *)0x200000F4);
#endif
	if (guard_reset_flags() & (1 << 29))	/* RCC_CSR_IWDGRSTF */
		errlog_add(ERR_WATCHDOG, 0);
	if (guard_start_count() > 1)
		errlog_add(ERR_GUARD, (uint16_t)guard_start_count());

	delay_ms(50);		/* let the LED driver supply settle */
	leds_init();
	console_init();
	cresnet_init();
	usb_init();

	bool was_configured = false;

	for (;;) {
		usb_poll();
		console_poll();
		cresnet_poll();
		leds_tick();
		guard_kick();
		if (!was_configured) {
			if (usb_configured())
				was_configured = true;
			else if (millis() > USB_START_TIMEOUT_MS)
				system_reset();
		}
	}
}
