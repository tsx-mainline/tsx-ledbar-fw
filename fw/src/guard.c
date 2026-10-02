// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Start guard. The bootloader jumps to the application at every power-on
 * without a console window, so an application that hangs before it can
 * take IMGUPD would lock the USB update path. This guard:
 *
 * 1. starts the independent watchdog (about 4 s) first thing,
 * 2. counts the starts that did not reach "USB configured",
 * 3. after three such starts in a row, writes "UPG" into the bootloader
 *    mailbox at 0x200000F4 and resets. The bootloader then stays in its
 *    Cresnet update mode on USB and takes a new image.
 *
 * The mailbox and the counter live in the first 256 bytes of RAM, which
 * the linker script keeps out of .data and .bss. A system reset keeps
 * RAM. A power cycle gives random values, so a magic word validates the
 * counter.
 */
#include <libopencm3/cm3/scb.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>
#include "tsx.h"

#define GUARD_MAGIC	0x54535847	/* "TSXG" */
#define GUARD_MAX_STARTS 3
#define IWDG_PERIOD_MS	4000

extern volatile uint32_t tsx_mailbox;
extern volatile uint32_t tsx_guard_magic;
extern volatile uint32_t tsx_guard_count;

static uint32_t reset_flags;
static uint32_t start_count;

void guard_boot(void)
{
	reset_flags = RCC_CSR & RCC_CSR_RESET_FLAGS;
	RCC_CSR |= RCC_CSR_RMVF;

	iwdg_set_period_ms(IWDG_PERIOD_MS);
	iwdg_start();

	if (tsx_guard_magic != GUARD_MAGIC) {
		tsx_guard_magic = GUARD_MAGIC;
		tsx_guard_count = 0;
	}
	tsx_guard_count++;
	start_count = tsx_guard_count;
	if (tsx_guard_count > GUARD_MAX_STARTS) {
		tsx_guard_count = 0;
		guard_request_bootloader();
	}
}

void guard_usb_configured(void)
{
	tsx_guard_count = 0;
}

void guard_kick(void)
{
	iwdg_reset();
}

void guard_request_bootloader(void)
{
	/*
	 * "UPG\0", read by the bootloader as a 4-byte string: Cresnet update
	 * mode on USB. ("UPE" selects the text MONITOR, which is on a UART.)
	 */
	tsx_mailbox = 0x00475055;
	system_reset();
}

uint32_t guard_reset_flags(void)
{
	return reset_flags;
}

uint32_t guard_start_count(void)
{
	return start_count;
}
