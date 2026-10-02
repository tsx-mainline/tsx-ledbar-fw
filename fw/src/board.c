// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Board: STM32F205RC, HSE 25 MHz. SYSCLK 120 MHz (PLL M=25 N=240 P=2),
 * USB 48 MHz (Q=5), APB1 30 MHz, APB2 60 MHz, 3 flash wait states.
 * The bootloader leaves PRIMASK set and SysTick running, and does not
 * write VTOR. The QEMU build (netduino2 machine) has no RCC model, so it
 * keeps the reset clock and skips the PLL.
 */
#include <libopencm3/cm3/cortex.h>
#include <libopencm3/cm3/scb.h>
#include <libopencm3/cm3/systick.h>
#include <libopencm3/cm3/vector.h>
#include <libopencm3/stm32/flash.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include "tsx.h"

#ifdef TSX_QEMU
#define SYSCLK_HZ 16000000U	/* HSI after reset */
#else
#define SYSCLK_HZ 120000000U
#endif

static const struct rcc_clock_scale clock_120mhz_hse25 = {
	.pllm = 25,
	.plln = 240,
	.pllp = 2,
	.pllq = 5,
	.hpre = RCC_CFGR_HPRE_NODIV,
	.ppre1 = RCC_CFGR_PPRE_DIV4,
	.ppre2 = RCC_CFGR_PPRE_DIV2,
	.flash_config = FLASH_ACR_DCEN | FLASH_ACR_ICEN | FLASH_ACR_LATENCY_3WS,
	.apb1_frequency = 30000000,
	.apb2_frequency = 60000000,
};

static volatile uint32_t ms_ticks;

void sys_tick_handler(void)
{
	ms_ticks++;
}

void board_init(void)
{
	cm_disable_interrupts();
	SCB_VTOR = (uint32_t)&vector_table;

#ifndef TSX_QEMU
	rcc_clock_setup_hse_3v3(&clock_120mhz_hse25);
#else
	rcc_ahb_frequency = SYSCLK_HZ;
	rcc_apb1_frequency = SYSCLK_HZ;
	rcc_apb2_frequency = SYSCLK_HZ;
#endif

	rcc_periph_clock_enable(RCC_GPIOA);
	rcc_periph_clock_enable(RCC_GPIOB);
	rcc_periph_clock_enable(RCC_GPIOC);

	systick_counter_disable();
	systick_set_clocksource(STK_CSR_CLKSOURCE_AHB);
	systick_set_reload(SYSCLK_HZ / 1000 - 1);
	systick_clear();
	systick_interrupt_enable();
	systick_counter_enable();

	cm_enable_interrupts();
}

uint32_t millis(void)
{
	return ms_ticks;
}

void delay_ms(uint32_t ms)
{
	uint32_t start = ms_ticks;

	while (ms_ticks - start < ms)
		guard_kick();
}

void system_reset(void)
{
	cm_disable_interrupts();
	scb_reset_system();
	for (;;)
		;
}
