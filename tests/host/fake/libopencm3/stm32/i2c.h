/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Fake for the host tests: the status and control registers are
 * variables, the bits are as on the STM32F2. The test file defines the
 * functions and models the bus.
 */
#ifndef FAKE_I2C_H
#define FAKE_I2C_H
#include <stdint.h>
#include "fake_regs.h"

#define I2C1			1U
#define I2C_SR1(i2c)		fake_i2c_sr1
#define I2C_SR2(i2c)		fake_i2c_sr2
#define I2C_CR1(i2c)		fake_i2c_cr1
#define I2C_CR1_SWRST		(1 << 15)
#define I2C_SR1_AF		(1 << 10)
#define I2C_SR1_ARLO		(1 << 9)
#define I2C_SR1_BERR		(1 << 8)
#define I2C_SR1_TxE		(1 << 7)
#define I2C_SR1_RxNE		(1 << 6)
#define I2C_SR1_BTF		(1 << 2)
#define I2C_SR1_ADDR		(1 << 1)
#define I2C_SR1_SB		(1 << 0)
#define I2C_SR2_BUSY		(1 << 1)
#define I2C_WRITE		0
#define I2C_READ		1

enum i2c_speeds { i2c_speed_sm_100k };

void i2c_peripheral_enable(uint32_t i2c);
void i2c_peripheral_disable(uint32_t i2c);
void i2c_set_speed(uint32_t i2c, enum i2c_speeds speed, uint32_t clock_megahz);
void i2c_send_start(uint32_t i2c);
void i2c_send_stop(uint32_t i2c);
void i2c_send_7bit_address(uint32_t i2c, uint8_t slave, uint8_t readwrite);
void i2c_send_data(uint32_t i2c, uint8_t data);
uint8_t i2c_get_data(uint32_t i2c);
void i2c_enable_ack(uint32_t i2c);
void i2c_disable_ack(uint32_t i2c);
#endif
