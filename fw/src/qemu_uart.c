// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * QEMU build only: the console on USART1 (PA9/PA10), which the netduino2
 * machine connects to -serial. Polled, no interrupts.
 */
#ifdef TSX_QEMU
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/rcc.h>
#include <libopencm3/stm32/usart.h>
#include "tsx.h"

void qemu_uart_init(void)
{
	rcc_periph_clock_enable(RCC_USART1);
	gpio_mode_setup(GPIOA, GPIO_MODE_AF, GPIO_PUPD_NONE, GPIO9 | GPIO10);
	gpio_set_af(GPIOA, GPIO_AF7, GPIO9 | GPIO10);
	usart_set_baudrate(USART1, 115200);
	usart_set_databits(USART1, 8);
	usart_set_stopbits(USART1, USART_STOPBITS_1);
	usart_set_mode(USART1, USART_MODE_TX_RX);
	usart_set_parity(USART1, USART_PARITY_NONE);
	usart_set_flow_control(USART1, USART_FLOWCONTROL_NONE);
	usart_enable(USART1);
}

void qemu_uart_write(const char *s)
{
	while (*s)
		usart_send_blocking(USART1, (uint8_t)*s++);
}

int qemu_uart_read(void)
{
	if (!(USART_SR(USART1) & USART_SR_RXNE))
		return -1;
	return (int)usart_recv(USART1);
}
#endif
