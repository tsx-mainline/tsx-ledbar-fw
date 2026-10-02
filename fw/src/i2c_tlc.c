// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The three TLC59116 LED drivers: red at 0x60, green at 0x61, blue at
 * 0x62 on I2C1 (PB6 SCL, PB7 SDA, 100 kHz). PB12 is their reset line.
 *
 * The driver is a small polled I2C master with timeouts, so a stuck bus
 * cannot hang the firmware. A bus recovery (up to 18 SCL clocks while
 * SDA is low) runs before the init and after an error.
 *
 * Init writes registers 0x00..0x17 in one auto-increment transfer:
 * MODE1 = 0x80 (auto-increment on, oscillator on), MODE2 = 0, PWM0..15
 * = 0xFF, GRPPWM = 0, GRPFREQ = 0, LEDOUT0..3 = 0xFF (every output in
 * mode 3: PWM and group control).
 *
 * In mode 3 an output is on for PWMx/256 of the 97 kHz individual period
 * and only inside the GRPPWM/256 window of the 190 Hz group period, so
 * its brightness is the product PWMx x GRPPWM. The LED engine uses both
 * (tlc_set_dim): the lowest step is then 1/65025 of full brightness, not
 * the 1/255 of GRPPWM alone, which shows as a jump in a dark room. MODE2
 * OCH = 0 makes the outputs change at the STOP condition, so the PWM and
 * GRPPWM bytes of one transfer take effect together.
 *
 * The QEMU build has no I2C model: the chip registers are kept in RAM.
 */
#include "tsx.h"

#define TLC_ADDR(color)	(0x60 + (color))
#define TLC_AUTOINC	0x80
#define REG_MODE1	0x00
#define REG_MODE2	0x01
#define REG_PWM0	0x02
#define REG_GRPPWM	0x12
#define REG_GRPFREQ	0x13
#define REG_LEDOUT0	0x14
#define MODE2_DMBLNK	0x20

#define INIT_TRIES	5
#define INIT_WAIT_MS	50
#define I2C_TIMEOUT_MS	5

static bool ready[NCOLORS];
static uint8_t shadow[NCOLORS][0x18];

#ifndef TSX_QEMU

#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/rcc.h>

#define I2C		I2C1
#define SCL_PIN		GPIO6
#define SDA_PIN		GPIO7
#define RESET_PIN	GPIO12

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, bool set)
{
	uint32_t start = millis();

	while (((*reg & mask) != 0) != set) {
		if (millis() - start > I2C_TIMEOUT_MS)
			return false;
	}
	return true;
}

static void i2c_pins_af(void)
{
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, SCL_PIN | SDA_PIN);
	gpio_set_af(GPIOB, GPIO_AF4, SCL_PIN | SDA_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_AF, GPIO_PUPD_PULLUP, SCL_PIN | SDA_PIN);
}

static void short_wait(void)
{
	for (volatile int i = 0; i < 600; i++)	/* about 5 us at 120 MHz */
		;
}

/* clock SCL until SDA is released, then a STOP condition */
static void bus_recover(void)
{
	gpio_set(GPIOB, SCL_PIN | SDA_PIN);
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, SCL_PIN | SDA_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, SCL_PIN | SDA_PIN);
	for (int i = 0; i < 18 && !gpio_get(GPIOB, SDA_PIN); i++) {
		gpio_clear(GPIOB, SCL_PIN);
		short_wait();
		gpio_set(GPIOB, SCL_PIN);
		short_wait();
	}
	/* STOP: SDA low to high while SCL is high */
	gpio_clear(GPIOB, SDA_PIN);
	short_wait();
	gpio_set(GPIOB, SCL_PIN);
	short_wait();
	gpio_set(GPIOB, SDA_PIN);
	short_wait();
	i2c_pins_af();
}

static void i2c_setup(void)
{
	rcc_periph_clock_enable(RCC_I2C1);
	rcc_periph_reset_pulse(RST_I2C1);
	i2c_peripheral_disable(I2C);
	i2c_set_speed(I2C, i2c_speed_sm_100k, rcc_apb1_frequency / 1000000);
	i2c_peripheral_enable(I2C);
}

static void i2c_fail(void)
{
	/* reset the peripheral, free the bus */
	i2c_peripheral_disable(I2C);
	I2C_CR1(I2C) |= I2C_CR1_SWRST;
	I2C_CR1(I2C) &= ~I2C_CR1_SWRST;
	bus_recover();
	i2c_setup();
}

static bool i2c_start_addr(uint8_t addr, bool read)
{
	i2c_send_start(I2C);
	if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_SB, true))
		return false;
	i2c_send_7bit_address(I2C, addr, read ? I2C_READ : I2C_WRITE);
	while (!(I2C_SR1(I2C) & I2C_SR1_ADDR)) {
		if (I2C_SR1(I2C) & I2C_SR1_AF) {
			I2C_SR1(I2C) &= ~I2C_SR1_AF;
			i2c_send_stop(I2C);
			return false;	/* no ACK: chip absent or not ready */
		}
	}
	(void)I2C_SR2(I2C);	/* clears ADDR */
	return true;
}

static bool i2c_write_bytes(uint8_t addr, const uint8_t *data, size_t n)
{
	if (!wait_flag(&I2C_SR2(I2C), I2C_SR2_BUSY, false))
		return false;
	if (!i2c_start_addr(addr, false))
		return false;
	for (size_t i = 0; i < n; i++) {
		if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_TxE, true))
			return false;
		i2c_send_data(I2C, data[i]);
	}
	if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_BTF, true))
		return false;
	i2c_send_stop(I2C);
	return true;
}

static bool i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *data, size_t n)
{
	if (!wait_flag(&I2C_SR2(I2C), I2C_SR2_BUSY, false))
		return false;
	if (!i2c_start_addr(addr, false))
		return false;
	if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_TxE, true))
		return false;
	i2c_send_data(I2C, reg);
	if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_BTF, true))
		return false;
	/* repeated start */
	i2c_enable_ack(I2C);
	if (!i2c_start_addr(addr, true))
		return false;
	for (size_t i = 0; i < n; i++) {
		if (i + 1 == n) {
			i2c_disable_ack(I2C);
			i2c_send_stop(I2C);
		}
		if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_RxNE, true))
			return false;
		data[i] = i2c_get_data(I2C);
	}
	return true;
}

static void hw_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, RESET_PIN);
	gpio_set(GPIOB, RESET_PIN);
	i2c_pins_af();
	bus_recover();
	i2c_setup();
}

void tlc_reset_pulse(void)
{
	hw_init();
	gpio_clear(GPIOB, RESET_PIN);
	delay_ms(5);
	gpio_set(GPIOB, RESET_PIN);
	delay_ms(5);
}

static bool raw_write(int color, uint8_t reg, const uint8_t *data, size_t n)
{
	uint8_t buf[1 + sizeof(shadow[0])];

	if (n > sizeof(shadow[0]))
		return false;
	buf[0] = reg | TLC_AUTOINC;
	for (size_t i = 0; i < n; i++)
		buf[1 + i] = data[i];
	if (!i2c_write_bytes(TLC_ADDR(color), buf, n + 1)) {
		i2c_fail();
		return false;
	}
	return true;
}

static bool raw_read(int color, uint8_t reg, uint8_t *data, size_t n)
{
	if (!i2c_read_reg(TLC_ADDR(color), reg | TLC_AUTOINC, data, n)) {
		i2c_fail();
		return false;
	}
	return true;
}

#else /* TSX_QEMU */

void tlc_reset_pulse(void)
{
}

static void hw_init(void)
{
}

static bool raw_write(int color, uint8_t reg, const uint8_t *data, size_t n)
{
	(void)color; (void)reg; (void)data; (void)n;
	return true;
}

static bool raw_read(int color, uint8_t reg, uint8_t *data, size_t n)
{
	for (size_t i = 0; i < n && reg + i < sizeof(shadow[0]); i++)
		data[i] = shadow[color][reg + i];
	return true;
}

#endif

bool tlc_write(int color, uint8_t reg, const uint8_t *data, size_t n)
{
	if (color < 0 || color >= NCOLORS || reg + n > sizeof(shadow[0]))
		return false;
	if (!raw_write(color, reg, data, n)) {
		errlog_add(ERR_I2C, (uint16_t)(color << 8 | reg));
		return false;
	}
	for (size_t i = 0; i < n; i++)
		shadow[color][reg + i] = data[i];
	return true;
}

bool tlc_read(int color, uint8_t reg, uint8_t *data, size_t n)
{
	if (color < 0 || color >= NCOLORS || reg + n > sizeof(shadow[0]))
		return false;
	return raw_read(color, reg, data, n);
}

static bool chip_init(int color)
{
	uint8_t regs[0x18];

	regs[REG_MODE1] = 0x80;
	regs[REG_MODE2] = 0x00;
	for (int i = 0; i < 16; i++)
		regs[REG_PWM0 + i] = 0xFF;
	regs[REG_GRPPWM] = 0x00;
	regs[REG_GRPFREQ] = 0x00;
	for (int i = 0; i < 4; i++)
		regs[REG_LEDOUT0 + i] = 0xFF;
	if (!raw_write(color, 0, regs, sizeof(regs)))
		return false;
	for (size_t i = 0; i < sizeof(regs); i++)
		shadow[color][i] = regs[i];
	return true;
}

/*
 * Reset the chips and write their registers. The stock firmware probes
 * them once, 42 ms after power-up, and gives up ("LED driver not
 * initialized!") when the supply is not ready yet. This init waits and
 * retries, with a new reset pulse between the tries.
 */
bool tlc_init(void)
{
	bool all = true;

	hw_init();
	for (int c = 0; c < NCOLORS; c++)
		ready[c] = false;
	for (int t = 0; t < INIT_TRIES; t++) {
		tlc_reset_pulse();
		delay_ms(INIT_WAIT_MS);
		all = true;
		for (int c = 0; c < NCOLORS; c++) {
			if (!ready[c])
				ready[c] = chip_init(c);
			all = all && ready[c];
		}
		if (all)
			return true;
	}
	for (int c = 0; c < NCOLORS; c++) {
		if (!ready[c])
			errlog_add(ERR_TLC_INIT, (uint16_t)c);
	}
	return all;
}

bool tlc_ready(int color)
{
	return color >= 0 && color < NCOLORS && ready[color];
}

bool tlc_set_group_pwm(int color, uint8_t duty)
{
	return tlc_ready(color) && tlc_write(color, REG_GRPPWM, &duty, 1);
}

/*
 * Brightness as PWMx (all 16 outputs the same) times GRPPWM. One
 * transfer writes PWM0..15 and GRPPWM (registers 0x02..0x12) when the
 * PWM value changes, otherwise only GRPPWM is written.
 */
bool tlc_set_dim(int color, uint8_t pwm, uint8_t grp)
{
	uint8_t v[17];
	bool same = true;

	if (!tlc_ready(color))
		return false;
	for (int i = 0; i < 16; i++)
		same = same && shadow[color][REG_PWM0 + i] == pwm;
	if (!same) {
		for (int i = 0; i < 16; i++)
			v[i] = pwm;
		v[16] = grp;
		return tlc_write(color, REG_PWM0, v, 17);
	}
	if (shadow[color][REG_GRPPWM] == grp)
		return true;
	return tlc_write(color, REG_GRPPWM, &grp, 1);
}

void tlc_get_dim(int color, uint8_t *pwm, uint8_t *grp)
{
	*pwm = shadow[color][REG_PWM0];
	*grp = shadow[color][REG_GRPPWM];
}

bool tlc_set_group_blink(int color, bool blink, uint8_t freq)
{
	uint8_t v[2];

	if (!tlc_ready(color))
		return false;
	v[0] = blink ? MODE2_DMBLNK : 0;
	if (!tlc_write(color, REG_MODE2, v, 1))
		return false;
	return tlc_write(color, REG_GRPFREQ, &freq, 1);
}

bool tlc_set_out_mode(int color, int out, int mode)
{
	uint8_t ledout[4];

	if (!tlc_ready(color) || mode < 0 || mode > 3)
		return false;
	for (int i = 0; i < 4; i++)
		ledout[i] = shadow[color][REG_LEDOUT0 + i];
	for (int o = 0; o < 16; o++) {
		if (out >= 0 && o != out)
			continue;
		ledout[o / 4] &= ~(3 << (2 * (o % 4)));
		ledout[o / 4] |= mode << (2 * (o % 4));
	}
	return tlc_write(color, REG_LEDOUT0, ledout, 4);
}

int tlc_get_out_mode(int color, int out)
{
	uint8_t v;

	if (!tlc_ready(color) || out < 0 || out > 15)
		return -1;
	if (!tlc_read(color, REG_LEDOUT0 + out / 4, &v, 1))
		return -1;
	return (v >> (2 * (out % 4))) & 3;
}

bool tlc_set_pwm(int color, int out, uint8_t duty)
{
	uint8_t v[16];

	if (!tlc_ready(color))
		return false;
	if (out >= 0 && out < 16)
		return tlc_write(color, REG_PWM0 + out, &duty, 1);
	for (int i = 0; i < 16; i++)
		v[i] = duty;
	return tlc_write(color, REG_PWM0, v, 16);
}
