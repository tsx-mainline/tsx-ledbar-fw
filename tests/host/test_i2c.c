// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the I2C driver in fw/src/i2c_tlc.c (the bar build, not the
 * QEMU build). A small model of the I2C peripheral answers each address
 * phase with an ACK, no ACK (AF), arbitration lost (ARLO), a bus error
 * (BERR) or nothing. Each millis() call moves the time on by 1 ms, so a
 * wait of the driver ends after a known number of polls. An alarm ends
 * the test when the driver hangs.
 */
#include <signal.h>
#include <unistd.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/rcc.h>
#include "check.h"
#include "tsx.h"

uint32_t fake_rcc_csr;
volatile uint32_t fake_i2c_sr1, fake_i2c_sr2, fake_i2c_cr1;
uint32_t rcc_apb1_frequency = 30000000;

enum answer { ACK, NACK, ARLO, BERR, SILENT };
static const char *const answer_names[] = { "ACK", "no ACK (AF)", "arbitration lost (ARLO)",
					    "bus error (BERR)", "no answer" };
static enum answer answer;
static uint32_t now;
static int stops, setups;
static uint8_t sent[64];
static int nsent;
static uint8_t chip_regs[NCOLORS][0x18];
static int cur_chip;
static uint8_t cur_reg;

uint32_t millis(void)
{
	return now++;
}

void delay_ms(uint32_t ms)
{
	now += ms;
}

void rcc_periph_clock_enable(enum rcc_periph_clken clken)
{
	(void)clken;
}

/* i2c_setup: once at the init and once in each bus recovery */
void rcc_periph_reset_pulse(enum rcc_periph_rst rst)
{
	(void)rst;
	setups++;
}

void gpio_mode_setup(uint32_t port, uint8_t mode, uint8_t pull, uint16_t gpios)
{
	(void)port; (void)mode; (void)pull; (void)gpios;
}

void gpio_set_output_options(uint32_t port, uint8_t otype, uint8_t speed, uint16_t gpios)
{
	(void)port; (void)otype; (void)speed; (void)gpios;
}

void gpio_set_af(uint32_t port, uint8_t af, uint16_t gpios)
{
	(void)port; (void)af; (void)gpios;
}

void gpio_set(uint32_t port, uint16_t gpios)
{
	(void)port; (void)gpios;
}

void gpio_clear(uint32_t port, uint16_t gpios)
{
	(void)port; (void)gpios;
}

/* SDA and SCL are high: the bus recovery needs no clocks */
uint16_t gpio_get(uint32_t port, uint16_t gpios)
{
	(void)port;
	return gpios;
}

void i2c_peripheral_enable(uint32_t i2c)
{
	(void)i2c;
}

void i2c_peripheral_disable(uint32_t i2c)
{
	(void)i2c;
}

void i2c_set_speed(uint32_t i2c, enum i2c_speeds speed, uint32_t clock_megahz)
{
	(void)i2c; (void)speed; (void)clock_megahz;
}

void i2c_send_start(uint32_t i2c)
{
	(void)i2c;
	fake_i2c_sr1 = I2C_SR1_SB;
}

void i2c_send_stop(uint32_t i2c)
{
	(void)i2c;
	stops++;
}

void i2c_send_7bit_address(uint32_t i2c, uint8_t slave, uint8_t readwrite)
{
	(void)i2c;
	fake_i2c_sr1 &= ~I2C_SR1_SB;
	cur_chip = slave - 0x60;
	switch (answer) {
	case ACK:
		if (readwrite == I2C_READ) {
			fake_i2c_sr1 |= I2C_SR1_ADDR | I2C_SR1_RxNE;
		} else {
			fake_i2c_sr1 |= I2C_SR1_ADDR | I2C_SR1_TxE;
			nsent = 0;
		}
		break;
	case NACK:
		fake_i2c_sr1 |= I2C_SR1_AF;
		break;
	case ARLO:
		fake_i2c_sr1 |= I2C_SR1_ARLO;
		break;
	case BERR:
		fake_i2c_sr1 |= I2C_SR1_BERR;
		break;
	case SILENT:
		break;
	}
}

void i2c_send_data(uint32_t i2c, uint8_t data)
{
	(void)i2c;
	if (nsent == 0)
		cur_reg = data & 0x7F;
	if (nsent < (int)sizeof(sent))
		sent[nsent++] = data;
	fake_i2c_sr1 |= I2C_SR1_TxE | I2C_SR1_BTF;
}

uint8_t i2c_get_data(uint32_t i2c)
{
	(void)i2c;
	return chip_regs[cur_chip][cur_reg++ % 0x18];
}

void i2c_enable_ack(uint32_t i2c)
{
	(void)i2c;
}

void i2c_disable_ack(uint32_t i2c)
{
	(void)i2c;
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

int main(void)
{
	uint8_t pwm[TLC_DIM_REGS], g = 7;

	alarm(20);	/* a hang in the driver ends the test with SIGALRM */
	answer = ACK;
	CHECK(tlc_init() && tlc_ready(RED) && tlc_ready(GREEN) && tlc_ready(BLUE),
	      "init with ACK: the three chips are ready");
	CHECK(nsent == 25 && sent[0] == 0x80 && sent[1] == 0x80 && sent[3] == 0xFF && sent[19] == 0 &&
	      sent[21] == 0xFF && sent[24] == 0xFF,
	      "init: one transfer of 25 bytes from register 0 with auto-increment (%d bytes)", nsent);
	for (int i = 0; i < TLC_DIM_REGS; i++)
		pwm[i] = (uint8_t)i;
	CHECK(tlc_write(RED, TLC_REG_PWM0, pwm, sizeof(pwm)) && nsent == 18 && sent[0] == 0x82 && sent[17] == 16,
	      "a write of PWM0..GRPPWM: 18 bytes from 0x82");

	for (enum answer a = NACK; a <= SILENT; a++) {
		int s0 = stops, u0 = setups;
		uint32_t t0;
		bool ok;

		answer = a;
		errlog_clear();
		t0 = now;
		ok = tlc_write(GREEN, TLC_REG_GRPPWM, &g, 1);
		CHECK(!ok, "address phase, %s: the write fails and returns", answer_names[a]);
		CHECK(now - t0 < 20 && (a != SILENT || now - t0 > 5),
		      "address phase, %s: the wait ends after %u ms", answer_names[a], now - t0);
		CHECK(stops > s0, "address phase, %s: STOP sent", answer_names[a]);
		CHECK(setups == u0 + 1, "address phase, %s: the bus recovery ran once", answer_names[a]);
		CHECK((fake_i2c_sr1 & (I2C_SR1_AF | I2C_SR1_ARLO | I2C_SR1_BERR)) == 0,
		      "address phase, %s: the error flags are cleared", answer_names[a]);
		CHECK(errlog_count() == 1 && logged(ERR_I2C, GREEN << 8 | TLC_REG_GRPPWM),
		      "address phase, %s: one ERRLOG entry, subsystem 120 cause 0x0112", answer_names[a]);
		answer = ACK;
		CHECK(tlc_write(GREEN, TLC_REG_GRPPWM, &g, 1) && nsent == 2 && sent[0] == 0x92 && sent[1] == 7,
		      "after %s and the recovery: the next write works", answer_names[a]);
	}

	/* the read path uses the same address phase twice (write, then read) */
	answer = ACK;
	chip_regs[BLUE][0x15] = 0xFF;
	CHECK(tlc_get_out_mode(BLUE, 5) == 3, "a read of LEDOUT1: output 5 in mode 3");
	for (enum answer a = NACK; a <= SILENT; a++) {
		int u0 = setups;

		answer = a;
		CHECK(tlc_get_out_mode(BLUE, 5) == -1 && setups == u0 + 1,
		      "a read with %s fails, the bus recovery ran", answer_names[a]);
	}
	DONE("test_i2c");
}
