/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Fake for the host tests: the test file defines these functions. */
#ifndef FAKE_GPIO_H
#define FAKE_GPIO_H
#include <stdint.h>

#define GPIOB			1U
#define GPIO6			(1 << 6)
#define GPIO7			(1 << 7)
#define GPIO12			(1 << 12)
#define GPIO_MODE_OUTPUT	1
#define GPIO_MODE_AF		2
#define GPIO_PUPD_NONE		0
#define GPIO_PUPD_PULLUP	1
#define GPIO_OTYPE_OD		1
#define GPIO_OSPEED_2MHZ	0
#define GPIO_AF4		4

void gpio_mode_setup(uint32_t port, uint8_t mode, uint8_t pull, uint16_t gpios);
void gpio_set_output_options(uint32_t port, uint8_t otype, uint8_t speed, uint16_t gpios);
void gpio_set_af(uint32_t port, uint8_t af, uint16_t gpios);
void gpio_set(uint32_t port, uint16_t gpios);
void gpio_clear(uint32_t port, uint16_t gpios);
uint16_t gpio_get(uint32_t port, uint16_t gpios);
#endif
