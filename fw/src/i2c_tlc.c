// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The three TLC59116 LED drivers: red at 0x60, green at 0x61, blue at
 * 0x62 on I2C1 (PB6 SCL, PB7 SDA, 100 kHz). PB12 is their shared reset
 * line.
 *
 * The driver is a small polled I2C master with timeouts, so a stuck bus
 * cannot hang the firmware. A bus recovery (up to 18 SCL clocks while
 * SDA is low, then a STOP) runs before the init and after each failed
 * transfer. Each failed transfer of a chip adds to the error counters of
 * that chip (AF, ARLO, BERR, timeout, busy bus).
 *
 * Init writes registers 0x00..0x17 in one auto-increment transfer:
 * MODE1 = 0x80 (auto-increment on, oscillator on, all-call address off),
 * MODE2 = 0, PWM0..15 = 0xFF, GRPPWM = 0, GRPFREQ = 0, LEDOUT0..3 = 0xFF
 * (every output in mode 3: PWM and group control).
 *
 * In mode 3 an output is on for PWMx/256 of the 97 kHz individual period
 * and only inside the GRPPWM/256 window of the 190 Hz group period, so
 * its brightness is the product PWMx x GRPPWM. The LED engine uses both
 * (one PWMx per output, one GRPPWM per chip): the lowest step is then
 * 1/65025 of full brightness, not the 1/255 of GRPPWM alone, which shows
 * as a jump in a dark room. MODE2 OCH = 0 makes the registers change at
 * the STOP condition. The 190 Hz group counter does not restart on a
 * write, so a GRPPWM write inside the group window can give a wrong
 * window for one period. The LED engine changes GRPPWM only when it must
 * (see leds.c).
 *
 * The QEMU build has no I2C model: the chip registers are kept in RAM.
 * The console command TEST CHIPS sets which chips answer.
 */
#include "tsx.h"

#define TLC_ADDR(color)	(0x60 + (color))
#define TLC_AUTOINC	0x80
#define REG_MODE1	0x00
#define REG_MODE2	0x01
#define REG_PWM0	TLC_REG_PWM0
#define REG_GRPPWM	TLC_REG_GRPPWM
#define REG_GRPFREQ	0x13
#define REG_LEDOUT0	0x14
#define REG_LAST	0x1E	/* EFLAG2: the last register of the chip */
#define MODE2_DMBLNK	0x20

#define ALL_CHIPS	((1 << NCOLORS) - 1)
#define PROBE_ROUNDS	5
#define INIT_TRIES	5
#define RECOVERY_ROUNDS	2
#define INIT_WAIT_MS	50
#define I2C_TIMEOUT_MS	5
#define PINS_BUS	(TLC_PIN_SDA | TLC_PIN_SCL)

static bool ready[NCOLORS];
static uint8_t shadow[NCOLORS][0x18];
static struct tlc_chip_info info[NCOLORS];
static struct tlc_start_info start_info = { .pins_power = TLC_PINS_NONE,
					    .pins_reset = TLC_PINS_NONE };

/* a failed transfer of chip color with the TLC_E flags err */
static void note_err(int color, uint8_t err)
{
	struct tlc_chip_info *i = &info[color];

	i->last_err = err;
	if (err & TLC_E_AF)
		i->af++;
	if (err & TLC_E_ARLO)
		i->arlo++;
	if (err & TLC_E_BERR)
		i->berr++;
	if (err & TLC_E_TIMEOUT)
		i->timeout++;
	if (err & TLC_E_BUSY)
		i->busy++;
}

#ifndef TSX_QEMU

#include <libopencm3/cm3/dwt.h>
#include <libopencm3/stm32/gpio.h>
#include <libopencm3/stm32/i2c.h>
#include <libopencm3/stm32/rcc.h>

#define I2C		I2C1
#define SCL_PIN		GPIO6
#define SDA_PIN		GPIO7
#define RESET_PIN	GPIO12

#define ADDR_ERRORS	(I2C_SR1_AF | I2C_SR1_ARLO | I2C_SR1_BERR)

static uint8_t i2c_err;	/* TLC_E flags of the last failed transfer */
static bool bus_slow;	/* I2CSCAN SLOW: 10 kHz instead of 100 kHz */

/* the error flags in SR1, or a timeout when none is set */
static uint8_t sr1_err(void)
{
	uint32_t sr1 = I2C_SR1(I2C);
	uint8_t e = 0;

	if (sr1 & I2C_SR1_AF)
		e |= TLC_E_AF;
	if (sr1 & I2C_SR1_ARLO)
		e |= TLC_E_ARLO;
	if (sr1 & I2C_SR1_BERR)
		e |= TLC_E_BERR;
	return e ? e : TLC_E_TIMEOUT;
}

static bool wait_flag(volatile uint32_t *reg, uint32_t mask, bool set)
{
	uint32_t start = millis();

	while (((*reg & mask) != 0) != set) {
		if (millis() - start > I2C_TIMEOUT_MS) {
			i2c_err = sr1_err();
			return false;
		}
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

/*
 * The levels of SDA, SCL and RESET. SDA and SCL are read as GPIO inputs
 * with the pull-ups of the bus, so the level is what the chips do, not
 * what the I2C peripheral drives. RESET is read as it is: an output
 * high after the start, so a 0 means that the line is shorted to ground.
 */
static uint8_t pins_read(void)
{
	uint16_t v;

	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_PULLUP, SCL_PIN | SDA_PIN);
	short_wait();
	v = gpio_get(GPIOB, SCL_PIN | SDA_PIN | RESET_PIN);
	i2c_pins_af();
	return (uint8_t)((v & SDA_PIN ? TLC_PIN_SDA : 0) | (v & SCL_PIN ? TLC_PIN_SCL : 0) |
			 (v & RESET_PIN ? TLC_PIN_RESET : 0));
}

/*
 * Clock SCL until SDA is released, then a STOP condition. True when SDA
 * and SCL are both high at the end. Clocks cannot free a bus with SCL
 * held low.
 */
static bool bus_recover(void)
{
	bool ok;

	gpio_set(GPIOB, SCL_PIN | SDA_PIN);
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, SCL_PIN | SDA_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, SCL_PIN | SDA_PIN);
	short_wait();
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
	ok = gpio_get(GPIOB, SDA_PIN | SCL_PIN) == (SDA_PIN | SCL_PIN);
	i2c_pins_af();
	return ok;
}

static void i2c_setup(void)
{
	rcc_periph_clock_enable(RCC_I2C1);
	rcc_periph_reset_pulse(RST_I2C1);
	i2c_peripheral_disable(I2C);
	i2c_set_speed(I2C, i2c_speed_sm_100k, rcc_apb1_frequency / 1000000);
	if (bus_slow)	/* CCR = APB1 / (2 x 10 kHz) */
		i2c_set_ccr(I2C, rcc_apb1_frequency / 1000000 * 50);
	i2c_peripheral_enable(I2C);
}

/* reset the peripheral and free the bus: true when SDA and SCL are high */
static bool i2c_fail(void)
{
	bool ok;

	i2c_peripheral_disable(I2C);
	I2C_CR1(I2C) |= I2C_CR1_SWRST;
	I2C_CR1(I2C) &= ~I2C_CR1_SWRST;
	ok = bus_recover();
	i2c_setup();
	return ok;
}

/*
 * START and the address. The address phase ends with ADDR (ACK), AF (no
 * ACK: chip absent or not ready), ARLO (arbitration lost: the peripheral
 * drops to slave mode and sets neither ADDR nor AF) or BERR (a bus error).
 * A glitch can also give none of them, so the wait has the same time
 * limit as the other phases. On a failure i2c_err holds the flags, and
 * the caller runs i2c_fail (bus recovery).
 */
static bool i2c_start_addr(uint8_t addr, bool read)
{
	uint32_t start, sr1;

	i2c_send_start(I2C);
	if (!wait_flag(&I2C_SR1(I2C), I2C_SR1_SB, true))
		return false;
	i2c_send_7bit_address(I2C, addr, read ? I2C_READ : I2C_WRITE);
	start = millis();
	while (!((sr1 = I2C_SR1(I2C)) & I2C_SR1_ADDR)) {
		if ((sr1 & ADDR_ERRORS) || millis() - start > I2C_TIMEOUT_MS) {
			i2c_err = sr1_err();
			I2C_SR1(I2C) &= ~ADDR_ERRORS;
			i2c_send_stop(I2C);
			return false;
		}
	}
	(void)I2C_SR2(I2C);	/* clears ADDR */
	return true;
}

/* a bus that stays busy (SDA or SCL low, or no STOP seen) */
static bool i2c_idle(void)
{
	i2c_err = 0;
	if (wait_flag(&I2C_SR2(I2C), I2C_SR2_BUSY, false))
		return true;
	i2c_err = TLC_E_BUSY;
	return false;
}

static bool i2c_write_bytes(uint8_t addr, const uint8_t *data, size_t n)
{
	if (!i2c_idle() || !i2c_start_addr(addr, false))
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
	if (!i2c_idle() || !i2c_start_addr(addr, false))
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

/*
 * Once, at the first init: read the pins as they are after the power-up
 * (RESET still an input), then drive RESET high, free the bus and start
 * the peripheral.
 */
static void hw_init(void)
{
	static bool done;

	if (done)
		return;
	done = true;
	start_info.pins_power = pins_read();
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, RESET_PIN);
	gpio_set(GPIOB, RESET_PIN);
	i2c_pins_af();
	bus_recover();
	i2c_setup();
}

static void reset_pulse(void)
{
	gpio_clear(GPIOB, RESET_PIN);
	delay_ms(5);
	gpio_set(GPIOB, RESET_PIN);
	delay_ms(5);
}

static bool bus_reinit(void)
{
	return i2c_fail();
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
		note_err(color, i2c_err);
		i2c_fail();
		return false;
	}
	return true;
}

static bool raw_read(int color, uint8_t reg, uint8_t *data, size_t n)
{
	if (!i2c_read_reg(TLC_ADDR(color), reg | TLC_AUTOINC, data, n)) {
		note_err(color, i2c_err);
		i2c_fail();
		return false;
	}
	return true;
}

/*
 * I2CSCAN: START, the address with the write bit, STOP. No data, so the
 * software reset address 0x6B only answers and resets nothing.
 */
uint8_t tlc_probe(uint8_t addr)
{
	uint8_t err;

	hw_init();
	if (!i2c_idle() || !i2c_start_addr(addr, false)) {
		err = i2c_err;
		i2c_fail();
		return err;
	}
	i2c_send_stop(I2C);
	return 0;
}

uint8_t tlc_pins(void)
{
	hw_init();
	return pins_read();
}

/*
 * The pull-up test of I2CSCAN: drive a line low, release it with no
 * internal pull-up, and read it after a short wait. A line with a
 * pull-up resistor is high again. A line with no resistor (for example a
 * broken trace to the resistor) stays low. SDA is tested while SCL is
 * low, so the chips see no START or STOP. The peripheral starts again
 * afterwards, because it saw the lines move.
 */
uint8_t tlc_pullups(void)
{
	uint8_t r = 0;

	hw_init();
	gpio_set(GPIOB, SCL_PIN | SDA_PIN);
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, SCL_PIN | SDA_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, SCL_PIN | SDA_PIN);
	gpio_clear(GPIOB, SCL_PIN);
	short_wait();
	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, SCL_PIN);
	short_wait();
	if (gpio_get(GPIOB, SCL_PIN))
		r |= TLC_PIN_SCL;
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, SCL_PIN);	/* SCL low */
	gpio_clear(GPIOB, SDA_PIN);
	short_wait();
	gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, SDA_PIN);
	short_wait();
	if (gpio_get(GPIOB, SDA_PIN))
		r |= TLC_PIN_SDA;
	gpio_set(GPIOB, SDA_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_PULLUP, SDA_PIN);
	short_wait();
	gpio_set(GPIOB, SCL_PIN);
	short_wait();
	i2c_fail();
	return r;
}

void tlc_bus_slow(bool on)
{
	hw_init();
	bus_slow = on;
	i2c_fail();
}

#define RISE_MAX_CYCLES	12000	/* 100 us at 120 MHz */

/* hold a line low for about 10 us */
static void hold_low(uint16_t pin)
{
	gpio_clear(GPIOB, pin);
	short_wait();
	short_wait();
}

/*
 * Release PIN (an open-drain output that is low, or RESET as an input)
 * and count the CPU cycles until it reads high. The count includes the
 * loop, about 20 cycles.
 */
static uint32_t rise(uint16_t pin, bool release_by_set)
{
	uint32_t t0 = dwt_read_cycle_counter();

	if (release_by_set)
		gpio_set(GPIOB, pin);
	else
		gpio_mode_setup(GPIOB, GPIO_MODE_INPUT, GPIO_PUPD_NONE, pin);
	while (!gpio_get(GPIOB, pin)) {
		if (dwt_read_cycle_counter() - t0 > RISE_MAX_CYCLES)
			return TLC_RISE_NEVER;
	}
	return dwt_read_cycle_counter() - t0;
}

#define NSAMPLES	64
#define RISE_REPEATS	8

/*
 * The rise time of SDA or SCL in ns: release the line (an open-drain
 * output that is low) and read the port 64 times in a row, as fast as the
 * CPU can. The first high sample gives the time. The step between two
 * samples is about 30 ns: *step_ns gives it. The test runs 8 times and
 * keeps the shortest time, so an interrupt in the window does no harm. A
 * line that is not high after the 64 samples gets the slow count of
 * rise().
 */
static uint32_t rise_ns(uint16_t pin, uint32_t *step_ns)
{
	uint16_t smp[NSAMPLES];
	uint32_t mhz = rcc_ahb_frequency / 1000000, best = TLC_RISE_NEVER, t0, t1, c;

	for (int r = 0; r < RISE_REPEATS; r++) {
		hold_low(pin);
		t0 = dwt_read_cycle_counter();
		GPIO_BSRR(GPIOB) = pin;
#pragma GCC unroll 64
		for (int i = 0; i < NSAMPLES; i++)
			smp[i] = (uint16_t)GPIO_IDR(GPIOB);
		t1 = dwt_read_cycle_counter();
		*step_ns = (t1 - t0) * 1000 / NSAMPLES / mhz;
		for (int i = 0; i < NSAMPLES; i++) {
			if (smp[i] & pin) {
				if ((uint32_t)i * *step_ns < best)
					best = (uint32_t)i * *step_ns;
				break;
			}
		}
	}
	if (best == TLC_RISE_NEVER) {
		hold_low(pin);
		c = rise(pin, true);
		if (c != TLC_RISE_NEVER)
			best = c * 1000 / mhz;
	}
	return best;
}

/* SDA or SCL as an open-drain output, with or without the internal pull-up */
static void od_pin(uint16_t pin, bool pullup)
{
	gpio_set_output_options(GPIOB, GPIO_OTYPE_OD, GPIO_OSPEED_2MHZ, pin);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, pullup ? GPIO_PUPD_PULLUP : GPIO_PUPD_NONE, pin);
}

/*
 * PINTEST: the rise time of SDA and SCL after the firmware releases them,
 * with only the pull-up resistors of the board and with the internal
 * pull-up too, and the reset line: driven low, driven high, and released
 * after a low level. SDA moves only while SCL is low, so the chips see no
 * START or STOP. The reset test resets the chips: the caller starts them
 * again.
 */
void tlc_pintest(struct tlc_pintest *t)
{
	hw_init();
	dwt_enable_cycle_counter();
	gpio_set(GPIOB, SCL_PIN | SDA_PIN);
	od_pin(SCL_PIN | SDA_PIN, false);
	short_wait();
	t->scl_ext = rise_ns(SCL_PIN, &t->step_ns);
	od_pin(SCL_PIN, true);
	t->scl_int = rise_ns(SCL_PIN, &t->step_ns);
	od_pin(SCL_PIN, false);
	gpio_clear(GPIOB, SCL_PIN);
	t->sda_ext = rise_ns(SDA_PIN, &t->step_ns);
	od_pin(SDA_PIN, true);
	t->sda_int = rise_ns(SDA_PIN, &t->step_ns);
	gpio_set(GPIOB, SCL_PIN);
	short_wait();
	i2c_pins_af();
	i2c_fail();

	gpio_clear(GPIOB, RESET_PIN);
	short_wait();
	t->rst_low = gpio_get(GPIOB, RESET_PIN) ? 1 : 0;
	gpio_set(GPIOB, RESET_PIN);
	short_wait();
	t->rst_high = gpio_get(GPIOB, RESET_PIN) ? 1 : 0;
	hold_low(RESET_PIN);
	t->rst_rise = rise(RESET_PIN, false);
	if (t->rst_rise != TLC_RISE_NEVER)
		t->rst_rise = t->rst_rise * 1000 / (rcc_ahb_frequency / 1000000);
	gpio_set(GPIOB, RESET_PIN);
	gpio_mode_setup(GPIOB, GPIO_MODE_OUTPUT, GPIO_PUPD_NONE, RESET_PIN);
	/* the latency of the test: RESET is a push-pull output, it drives the edge */
	t->floor_ns = rise_ns(RESET_PIN, &t->step_ns);
	gpio_set(GPIOB, RESET_PIN);
}

#else /* TSX_QEMU */

/* the chips that answer: TEST CHIPS sets them */
static uint8_t qemu_chips = ALL_CHIPS;

void tlc_qemu_chips(uint8_t mask)
{
	qemu_chips = mask & ALL_CHIPS;
}

static void hw_init(void)
{
	start_info.pins_power = TLC_PINS_ALL;
}

static void reset_pulse(void)
{
}

static bool bus_reinit(void)
{
	return true;
}

static bool raw_write(int color, uint8_t reg, const uint8_t *data, size_t n)
{
	(void)reg; (void)data; (void)n;
	if (!(qemu_chips >> color & 1)) {
		note_err(color, TLC_E_AF);
		return false;
	}
	return true;
}

static bool raw_read(int color, uint8_t reg, uint8_t *data, size_t n)
{
	if (!(qemu_chips >> color & 1)) {
		note_err(color, TLC_E_AF);
		return false;
	}
	for (size_t i = 0; i < n; i++)
		data[i] = reg + i < sizeof(shadow[0]) ? shadow[color][reg + i] : 0;
	return true;
}

uint8_t tlc_probe(uint8_t addr)
{
	if (addr >= 0x60 && addr < 0x60 + NCOLORS && (qemu_chips >> (addr - 0x60) & 1))
		return 0;
	if (addr == TLC_ADDR_SWRST && qemu_chips)
		return 0;
	return TLC_E_AF;
}

uint8_t tlc_pins(void)
{
	return TLC_PINS_ALL;
}

uint8_t tlc_pullups(void)
{
	return TLC_PIN_SDA | TLC_PIN_SCL;
}

void tlc_bus_slow(bool on)
{
	(void)on;
}

/* QEMU has no GPIO model: fixed values */
void tlc_pintest(struct tlc_pintest *t)
{
	t->sda_ext = t->scl_ext = 120;
	t->sda_int = t->scl_int = 90;
	t->rst_low = 0;
	t->rst_high = 1;
	t->rst_rise = 1000;
	t->step_ns = 30;
	t->floor_ns = 60;
}

#endif

void tlc_reset_pulse(void)
{
	hw_init();
	reset_pulse();
}

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

/* all registers, 0x00..0x1E: the diagnosis registers too (IREF, EFLAG) */
bool tlc_read_all(int color, uint8_t regs[TLC_NREGS])
{
	if (color < 0 || color >= NCOLORS)
		return false;
	return raw_read(color, 0, regs, TLC_NREGS);
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

/* init the chips that are not in have: the chips that answered, with have */
static uint8_t init_missing(uint8_t have)
{
	for (int c = 0; c < NCOLORS; c++) {
		if (!(have >> c & 1) && chip_init(c))
			have |= (uint8_t)(1 << c);
	}
	return have;
}

static void set_phase(uint8_t mask, uint8_t phase)
{
	for (int c = 0; c < NCOLORS; c++) {
		if ((mask >> c & 1) && info[c].phase == TLC_PHASE_NONE)
			info[c].phase = phase;
	}
}

/*
 * Start the chips, in three steps. Each chip that answers stays in use,
 * also when another chip does not answer.
 *
 * 1. Probe: write the registers with no reset pulse, as the stock
 *    firmware does. When some chips answer, the supply is up: ask the
 *    others again, still with no pulse, up to PROBE_ROUNDS times.
 * 2. Reset: when a chip is still missing, pulse the shared reset line and
 *    init all chips again, up to INIT_TRIES times. A pulse resets every
 *    chip, so every chip needs its registers again. A driver supply that
 *    comes up late at power-up gives no answer to the probe, and the
 *    pulses with their wait give it time. The steps stop early when SDA
 *    or SCL stays low after a pulse, or when a pulse costs a chip that
 *    answered before.
 * 3. Recovery: when a chip is still missing, free the bus (SCL clocks and
 *    a STOP) and init the missing chips again with no pulse.
 */
bool tlc_init(void)
{
	struct tlc_start_info *s = &start_info;
	uint32_t t0;
	uint8_t have, before, pins;

	hw_init();
	t0 = millis();
	for (int c = 0; c < NCOLORS; c++) {
		ready[c] = false;
		info[c] = (struct tlc_chip_info){ 0 };
	}
	s->pins_idle = tlc_pins();
	s->pins_reset = TLC_PINS_NONE;
	s->probe = s->reset = s->recovery = 0;
	s->probe_rounds = s->reset_tries = 0;
	s->recovery_ran = s->stuck_after_reset = s->bus_free = false;

	/* 1. probe */
	have = 0;
	do {
		if (s->probe_rounds)
			delay_ms(INIT_WAIT_MS);
		have = init_missing(have);
		s->probe_rounds++;
	} while (have && have != ALL_CHIPS && s->probe_rounds < PROBE_ROUNDS);
	s->probe = have;
	set_phase(have, TLC_PHASE_PROBE);

	/* 2. reset pulses */
	before = have;
	while (have != ALL_CHIPS && s->reset_tries < INIT_TRIES) {
		reset_pulse();
		delay_ms(INIT_WAIT_MS);
		pins = tlc_pins();
		if (!s->reset_tries)
			s->pins_reset = pins;
		s->reset_tries++;
		if ((pins & PINS_BUS) != PINS_BUS) {
			s->stuck_after_reset = true;
			have = 0;
			break;
		}
		have = init_missing(0);
		s->reset = have;
		if ((have & before) != before)
			break;
	}
	if (s->reset_tries)
		set_phase(have, TLC_PHASE_RESET);

	/* 3. recovery */
	for (int r = 0; have != ALL_CHIPS && r < RECOVERY_ROUNDS; r++) {
		uint8_t now;

		if (r)
			delay_ms(INIT_WAIT_MS);
		s->recovery_ran = true;
		bus_reinit();
		now = init_missing(have);
		s->recovery |= (uint8_t)(now & ~have);
		have = now;
	}
	set_phase(s->recovery, TLC_PHASE_RECOVERY);

	s->bus_free = (tlc_pins() & PINS_BUS) == PINS_BUS;
	s->ms = millis() - t0;
	for (int c = 0; c < NCOLORS; c++) {
		ready[c] = have >> c & 1;
		if (!ready[c])
			errlog_add(ERR_TLC_INIT, (uint16_t)(info[c].last_err << 8 | c));
	}
	if (have != ALL_CHIPS)
		errlog_add(ERR_TLC_START, (uint16_t)(s->probe | s->reset << 4 | s->recovery << 8 |
						     (s->stuck_after_reset ? 0x1000 : 0) |
						     (s->bus_free ? 0 : 0x2000)));
	return have == ALL_CHIPS;
}

bool tlc_ready(int color)
{
	return color >= 0 && color < NCOLORS && ready[color];
}

const struct tlc_chip_info *tlc_chip_info(int color)
{
	return &info[color < 0 || color >= NCOLORS ? 0 : color];
}

const struct tlc_start_info *tlc_start_info(void)
{
	return &start_info;
}

/*
 * "ok", or why the chip is not in use: "BAD(nack)" when its last failed
 * transfer saw only a missing ACK (the chip is absent, has no supply or
 * has another address), "BAD(bus)" for an arbitration loss, a bus error,
 * a timeout or a busy bus (a line held low, or a chip that answers wrong).
 */
const char *tlc_state_name(int color)
{
	uint8_t e;

	if (tlc_ready(color))
		return "ok";
	e = tlc_chip_info(color)->last_err;
	return e && !(e & ~TLC_E_AF) ? "BAD(nack)" : "BAD(bus)";
}

bool tlc_set_group_pwm(int color, uint8_t duty)
{
	return tlc_ready(color) && tlc_write(color, REG_GRPPWM, &duty, 1);
}

void tlc_get_dim(int color, uint8_t *pwm, uint8_t *grp)
{
	*pwm = shadow[color][REG_PWM0];
	*grp = shadow[color][REG_GRPPWM];
}

uint8_t tlc_get_pwm(int color, int out)
{
	return shadow[color][REG_PWM0 + (out & 15)];
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
