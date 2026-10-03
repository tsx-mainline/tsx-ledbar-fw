// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Start guard. The bootloader jumps to the application at every start
 * without a console window. An application that hangs, or that cannot
 * come up on USB, would lock the USB update path. The guard:
 *
 * 1. starts the independent watchdog (about 4 s) first thing,
 * 2. counts the failed starts in a row,
 * 3. after three failed starts in a row, writes "UPG" into the bootloader
 *    mailbox at 0x200000F4 and resets. The bootloader then stays in its
 *    Cresnet update mode on USB and takes a new image.
 *
 * A start failed when one of these ended it:
 * - a reset that the firmware did not plan: the watchdog, a lockup or the
 *   reset pin. The reset flags show the watchdog, or the start is still
 *   marked as running at the next start.
 * - a hard fault. The fault handler marks the start and resets at once.
 * - the USB rule: a host reset the bus and then sent SOF packets, and no
 *   configuration came within GUARD_USB_MS after the first SOF. The guard
 *   then resets.
 *
 * A start without a USB host is not a failed start. The panel keeps the
 * bar powered while it stops in U-Boot, runs an installer, or hangs in a
 * kernel panic. The bar then runs without a limit and keeps its light.
 * A bus reset alone does not start the USB rule: a host that powers up
 * or goes down can give a short reset, but only a running host sends SOF
 * packets. The count goes to 0 when USB is configured, and when a start
 * runs GUARD_USB_MS with no host. A planned reset (REBOOT, IMGUPD, the
 * prepare packet) does not change the count.
 *
 * Limit: a firmware that never connects to the bus gets no bus reset. The
 * guard cannot tell it from a bar without a host, so it does not count
 * such starts.
 *
 * Two RAM words hold the state: the magic word at 0x200000F8 and the state
 * word at 0x200000FC. They are in the first 256 bytes of RAM, which the
 * linker script keeps out of .data and .bss. A system reset keeps RAM. A
 * power cycle gives random values, so the magic word validates the state.
 * State word: bits 0..7 failed starts in a row, bits 8..15 the run state,
 * bits 16..31 the number of this start since power-on.
 */
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/iwdg.h>
#include <libopencm3/stm32/rcc.h>
#include "tsx.h"

#define GUARD_MAGIC	0x32475354	/* "TSG2" (0.1.3 used "TSXG" and a plain counter) */
#define GUARD_MAX_FAILS	3
#define IWDG_PERIOD_MS	4000

/* the time after the first SOF in which USB must reach "configured" */
#ifndef GUARD_USB_MS
#ifdef TSX_QEMU
#define GUARD_USB_MS	20000	/* QEMU time runs about 7 times faster than the wall clock */
#else
#define GUARD_USB_MS	60000
#endif
#endif

/* run states */
#define GS_CLEAN	0	/* power-on, or a planned reset */
#define GS_RUN		1	/* the firmware runs */
#define GS_FAIL_USB	2	/* reset by the USB rule */
#define GS_FAIL_FAULT	3	/* reset by the hard fault handler */

extern volatile uint32_t tsx_mailbox;
extern volatile uint32_t tsx_guard_magic;
extern volatile uint32_t tsx_guard_state;

static uint32_t reset_flags;
static uint32_t starts;		/* this start since power-on */
static uint32_t fails;		/* failed starts in a row */
static uint32_t last;		/* how the last start ended: a run state */
static bool host_seen;		/* SOF packets after a bus reset */
static uint32_t host_ms;
static bool done;		/* USB configured, or no host for GUARD_USB_MS */
static bool configured;

static void store(uint32_t state)
{
	tsx_guard_state = (starts & 0xFFFF) << 16 | state << 8 | (fails & 0xFF);
}

void guard_boot(void)
{
	uint32_t w;

	reset_flags = RCC_CSR & RCC_CSR_RESET_FLAGS;
	RCC_CSR |= RCC_CSR_RMVF;

	iwdg_set_period_ms(IWDG_PERIOD_MS);
	iwdg_start();

	host_seen = false;
	done = false;
	configured = false;
	if (tsx_guard_magic != GUARD_MAGIC) {
		tsx_guard_magic = GUARD_MAGIC;
		tsx_guard_state = 0;
	}
	w = tsx_guard_state;
	fails = w & 0xFF;
	last = (w >> 8) & 0xFF;
	starts = ((w >> 16) + 1) & 0xFFFF;
	if (last > GS_FAIL_FAULT)
		last = GS_RUN;		/* not a known state: count it */
	if (last == GS_CLEAN && (reset_flags & RCC_CSR_IWDGRSTF))
		last = GS_RUN;
	if (last != GS_CLEAN)
		fails++;
	if (fails > GUARD_MAX_FAILS) {
		fails = 0;
		store(GS_CLEAN);
		tsx_mailbox = 0x00475055;	/* "UPG", see guard_request_bootloader */
		system_reset();
	}
	store(GS_RUN);
}

/* error log entries about the end of the last start */
void guard_report(void)
{
	if (reset_flags & RCC_CSR_IWDGRSTF)
		errlog_add(ERR_WATCHDOG, 0);
	else if (last == GS_RUN)
		errlog_add(ERR_WATCHDOG, 1);	/* no watchdog flag: the reset pin, or the flags were cleared */
	if (last == GS_FAIL_FAULT)
		errlog_add(ERR_FAULT, 0);
	if (last == GS_FAIL_USB)
		errlog_add(ERR_NOCONFIG, 0);
	if (fails)
		errlog_add(ERR_GUARD, (uint16_t)fails);
}

/* main loop: the USB rule, and the count goes to 0 after a start without a host */
void guard_poll(void)
{
	uint32_t now;

	if (configured)
		return;
	now = millis();
	if (host_seen) {
		if (now - host_ms >= GUARD_USB_MS) {
			store(GS_FAIL_USB);
			system_reset();
		}
	} else if (!done && now >= GUARD_USB_MS) {
		done = true;
		fails = 0;
		store(GS_RUN);
	}
}

void guard_usb_host(void)
{
	if (!host_seen && !configured) {
		host_seen = true;
		host_ms = millis();
	}
}

void guard_usb_configured(void)
{
	configured = true;
	done = true;
	fails = 0;
	store(GS_RUN);
}

void guard_kick(void)
{
	iwdg_reset();
}

void guard_reboot(void)
{
	store(GS_CLEAN);
	system_reset();
}

void guard_request_bootloader(void)
{
	/*
	 * "UPG\0", read by the bootloader as a 4-byte string: Cresnet update
	 * mode on USB. ("UPE" selects the text MONITOR, which is on a UART.)
	 */
	tsx_mailbox = 0x00475055;
	guard_reboot();
}

/*
 * A hard fault (the bus, memory and usage faults escalate to it, because
 * the firmware does not enable them). Mark the start as failed and reset
 * at once. Without this handler the library loops until the watchdog.
 */
void hard_fault_handler(void)
{
	store(GS_FAIL_FAULT);
	system_reset();
}

uint32_t guard_reset_flags(void)
{
	return reset_flags;
}

uint32_t guard_start_count(void)
{
	return starts;
}

uint32_t guard_fails(void)
{
	return fails;
}
