// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of fw/src/guard.c, the start guard. A reset of the bar is a
 * longjmp back to the test. Each start runs guard_boot and guard_report,
 * as main() does. The RAM words keep their values between starts, as a
 * system reset keeps RAM. GUARD_USB_MS is 60000 in this build.
 */
#include <setjmp.h>
#include <string.h>
#include <libopencm3/cm3/nvic.h>
#include <libopencm3/stm32/rcc.h>
#include "check.h"
#include "tsx.h"

#define UPG	0x00475055
#define MS	60000

volatile uint32_t tsx_mailbox, tsx_guard_magic, tsx_guard_state;
uint32_t fake_rcc_csr;

static uint32_t now;
static jmp_buf reset_jmp;
static int resets;

uint32_t millis(void)
{
	return now;
}

void system_reset(void)
{
	resets++;
	longjmp(reset_jmp, 1);
}

void iwdg_set_period_ms(uint32_t period)
{
	(void)period;
}

void iwdg_start(void)
{
}

void iwdg_reset(void)
{
}

/* one start with these reset flags: 1 when the guard reset at once (handover) */
static int boot(uint32_t csr)
{
	fake_rcc_csr = csr;
	now = 0;
	errlog_clear();
	if (setjmp(reset_jmp))
		return 1;
	guard_boot();
	guard_report();
	return 0;
}

/* the main loop for ms milliseconds: 1 when the guard reset */
static int run(uint32_t ms)
{
	uint32_t end = now + ms;

	if (setjmp(reset_jmp))
		return 1;
	for (; now < end; now++)
		guard_poll();
	return 0;
}

/* call a function that resets: 1 when it reset */
static int call(void (*fn)(void))
{
	if (setjmp(reset_jmp))
		return 1;
	fn();
	return 0;
}

static uint16_t want_sub, want_cause;
static int found;

static void find_cb(int i, uint32_t ms, uint16_t sub, uint16_t cause)
{
	(void)i;
	(void)ms;
	if (sub == want_sub && cause == want_cause)
		found = 1;
}

static int logged(uint16_t sub, uint16_t cause)
{
	want_sub = sub;
	want_cause = cause;
	found = 0;
	errlog_each(find_cb);
	return found;
}

#define SFT	RCC_CSR_SFTRSTF
#define POR	RCC_CSR_PORRSTF
#define PINRST	RCC_CSR_PINRSTF
#define IWDG	RCC_CSR_IWDGRSTF

int main(void)
{
	uint32_t s;

	/* power-on: random RAM */
	tsx_guard_magic = 0xDEADBEEF;
	tsx_guard_state = 0x12345678;
	tsx_mailbox = 0;
	CHECK(boot(POR | PINRST) == 0, "power-on: the start runs");
	CHECK(guard_start_count() == 1 && guard_fails() == 0 && errlog_count() == 0,
	      "power-on: start 1, no failed start, no log (start %u fails %u log %d)",
	      guard_start_count(), guard_fails(), errlog_count());

	/* planned resets are not failed starts */
	for (int n = 2; n <= 6; n++) {
		CHECK(call(guard_reboot) == 1, "REBOOT resets");
		CHECK(boot(SFT | PINRST) == 0 && guard_start_count() == (uint32_t)n &&
		      guard_fails() == 0 && errlog_count() == 0,
		      "REBOOT: start %d, no failed start (start %u fails %u)", n,
		      guard_start_count(), guard_fails());
	}
	CHECK(tsx_mailbox != UPG, "five REBOOTs: no handover");

	/* no host for an hour: no reset */
	CHECK(run(3600000) == 0 && guard_fails() == 0, "no host for an hour: no reset, no failed start");

	/* a reset that was not planned (the start was still running) */
	CHECK(boot(PINRST) == 0 && guard_fails() == 1 && logged(ERR_WATCHDOG, 1) && logged(ERR_GUARD, 1),
	      "unplanned reset without the watchdog flag: failed start 1, log 121/1 and 122/1");
	/* the watchdog */
	CHECK(boot(IWDG | PINRST) == 0 && guard_fails() == 2 && logged(ERR_WATCHDOG, 0) && logged(ERR_GUARD, 2),
	      "watchdog reset: failed start 2, log 121/0 and 122/2");
	/* a hard fault */
	CHECK(call(hard_fault_handler) == 1, "the hard fault handler resets at once");
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 3 && logged(ERR_FAULT, 0) && !logged(ERR_WATCHDOG, 1),
	      "hard fault: failed start 3, log 124, no 121");
	/* the fourth failed start hands the bar to the bootloader */
	s = guard_start_count();
	CHECK(boot(IWDG | PINRST) == 1 && tsx_mailbox == UPG, "fourth failed start: mailbox UPG, reset at once");
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 0 && errlog_count() == 0 && guard_start_count() == s + 2,
	      "after the handover: count 0, no log, start %u", guard_start_count());
	tsx_mailbox = 0;

	/* the watchdog flag also counts after a planned reset mark */
	CHECK(call(guard_reboot) == 1 && boot(IWDG | PINRST) == 0 && guard_fails() == 1,
	      "watchdog flag after a clean mark: failed start");
	CHECK(call(guard_reboot) == 1 && boot(SFT | PINRST) == 0 && guard_fails() == 1 && logged(ERR_GUARD, 1),
	      "a planned reset keeps the count");

	/* the USB rule: a host runs, no configuration */
	CHECK(run(1000) == 0, "1 s without a host");
	guard_usb_host();
	CHECK(run(MS) == 0, "host from 1000 ms: no reset before %d ms", 1000 + MS);
	CHECK(run(1) == 1 && now == 1000 + MS, "host without configuration: reset at %u ms", now);
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 2 && logged(ERR_NOCONFIG, 0) && logged(ERR_GUARD, 2),
	      "USB rule: failed start 2, log 125 and 122/2");

	/* a host that configures in time */
	guard_usb_host();
	CHECK(run(MS - 1000) == 0, "host, 59 s without a configuration: no reset");
	guard_usb_configured();
	CHECK(guard_fails() == 0, "USB configured: count 0");
	guard_usb_host();
	CHECK(run(10 * MS) == 0, "after the configuration: no limit, also with a new host");
	CHECK(call(guard_reboot) == 1 && boot(SFT | PINRST) == 0 && guard_fails() == 0,
	      "the next start after a configured one: no failed start");

	/* a start with no host for the USB time sets the count to 0 */
	CHECK(boot(PINRST) == 0 && guard_fails() == 1, "unplanned reset: failed start 1");
	CHECK(run(MS - 1) == 0 && guard_fails() == 1, "no host, %d ms: the count stays", MS - 1);
	CHECK(run(2) == 0 && guard_fails() == 0, "no host for the USB time: count 0, no reset");
	/* a host after that time still starts the USB rule */
	guard_usb_host();
	s = now;
	CHECK(run(MS) == 0 && run(1) == 1 && now == s + MS,
	      "a host after the USB time without a configuration: reset %d ms later", MS);
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 1, "late host: failed start 1");

	/* a state that the guard does not know counts as a failed start */
	tsx_guard_state = (tsx_guard_state & 0xFFFF00FFU) | 0x7F00U;
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 2, "unknown run state: failed start 2");

	/* IMGUPD and the prepare packet: mailbox UPG, a planned reset */
	CHECK(call(guard_request_bootloader) == 1 && tsx_mailbox == UPG, "IMGUPD: mailbox UPG, reset");
	CHECK(boot(SFT | PINRST) == 0 && guard_fails() == 2 && !logged(ERR_WATCHDOG, 1),
	      "IMGUPD is a planned reset: the count stays 2");

	/* power-on again */
	tsx_guard_magic = 0;
	CHECK(boot(POR | PINRST) == 0 && guard_fails() == 0 && guard_start_count() == 1,
	      "power-on: count 0, start 1");
	CHECK(resets > 0, "%d resets", resets);
	DONE("test_guard");
}
