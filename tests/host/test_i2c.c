// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Host test of the I2C driver in fw/src/i2c_tlc.c (the bar build, not the
 * QEMU build). A small model of the bus and the three chips:
 *
 * - The address phase of a chip gives an ACK, no ACK (AF), arbitration
 *   lost (ARLO), a bus error (BERR) or nothing. The variable "answer"
 *   forces one of these for every address.
 * - A chip can be absent, can answer only after a reset pulse, or can
 *   answer only after a time (a driver supply that comes up late).
 * - SDA can be held low: from the start, or after a reset pulse. The
 *   bus recovery frees it after a number of SCL clocks, or never.
 *   While SDA is low, the I2C peripheral reports a busy bus.
 * - The reset line can be shorted to ground: no chip answers then.
 *
 * Each millis() call moves the time on by 1 ms, so a wait of the driver
 * ends after a known number of polls. An alarm ends the test when the
 * driver hangs.
 */
#include <signal.h>
#include <string.h>
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
static uint8_t chip_regs[NCOLORS][TLC_NREGS];
static int cur_chip;
static uint8_t cur_reg;

/* the bus and chip model */
static bool present[NCOLORS];		/* the chip is there */
static bool need_reset[NCOLORS];	/* the chip answers only after a reset pulse */
static bool had_reset;
static uint32_t supply_at;		/* the chips answer from this time */
static bool rst_level = true;		/* the level that the firmware drives on RESET */
static bool rst_shorted;		/* RESET is held low */
static int pulses;			/* reset pulses */
static bool sda_low;			/* SDA held low now */
static bool sda_low_after_reset;	/* a reset pulse makes a chip hold SDA low */
static int release_clocks;		/* SCL clocks that free SDA, -1: never */
static int clocks;			/* SCL clocks since SDA went low */
static bool pullup_sda, pullup_scl;	/* the lines have a pull-up resistor */
static uint16_t floating;		/* pins set as inputs with no internal pull-up */

static void model_reset(void)
{
	answer = ACK;
	for (int c = 0; c < NCOLORS; c++) {
		present[c] = true;
		need_reset[c] = false;
	}
	had_reset = false;
	supply_at = 0;
	rst_shorted = false;
	sda_low = sda_low_after_reset = false;
	release_clocks = 9;
	clocks = 0;
	pulses = 0;
	pullup_sda = pullup_scl = true;
	fake_i2c_sr2 = 0;
	errlog_clear();
}

static void set_sda_low(bool low)
{
	sda_low = low;
	clocks = 0;
	if (low)
		fake_i2c_sr2 |= I2C_SR2_BUSY;
	else
		fake_i2c_sr2 &= ~I2C_SR2_BUSY;
}

static bool chip_answers(int c)
{
	return c >= 0 && c < NCOLORS && present[c] && !rst_shorted && now >= supply_at &&
	       (!need_reset[c] || had_reset);
}

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
	(void)port;
	if (mode == GPIO_MODE_INPUT && pull == GPIO_PUPD_NONE)
		floating |= gpios;
	else
		floating &= (uint16_t)~gpios;
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
	(void)port;
	if ((gpios & GPIO12) && !rst_level) {
		/* the end of a reset pulse */
		rst_level = true;
		pulses++;
		had_reset = true;
		if (sda_low_after_reset)
			set_sda_low(true);
	}
}

void gpio_clear(uint32_t port, uint16_t gpios)
{
	(void)port;
	if (gpios & GPIO12)
		rst_level = false;
	/* an SCL clock of the bus recovery */
	if ((gpios & GPIO6) && sda_low && release_clocks >= 0 && ++clocks >= release_clocks)
		set_sda_low(false);
}

uint16_t gpio_get(uint32_t port, uint16_t gpios)
{
	uint16_t v = GPIO6;

	(void)port;
	if (!sda_low)
		v |= GPIO7;
	/* a floating line with no pull-up resistor stays low */
	if ((floating & GPIO7) && !pullup_sda)
		v &= (uint16_t)~GPIO7;
	if ((floating & GPIO6) && !pullup_scl)
		v &= (uint16_t)~GPIO6;
	if (rst_level && !rst_shorted)
		v |= GPIO12;
	return v & gpios;
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
	fake_i2c_sr1 = sda_low ? 0 : I2C_SR1_SB;
}

void i2c_send_stop(uint32_t i2c)
{
	(void)i2c;
	stops++;
}

void i2c_send_7bit_address(uint32_t i2c, uint8_t slave, uint8_t readwrite)
{
	enum answer a = answer;

	(void)i2c;
	fake_i2c_sr1 &= ~I2C_SR1_SB;
	cur_chip = slave - 0x60;
	if (a == ACK) {
		if (slave == TLC_ADDR_SWRST)
			a = chip_answers(RED) || chip_answers(GREEN) || chip_answers(BLUE) ? ACK : NACK;
		else
			a = chip_answers(cur_chip) ? ACK : NACK;
	}
	switch (a) {
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
	return chip_regs[cur_chip][cur_reg++ % TLC_NREGS];
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

static int ready_mask(void)
{
	return (tlc_ready(RED) ? 1 : 0) | (tlc_ready(GREEN) ? 2 : 0) | (tlc_ready(BLUE) ? 4 : 0);
}

static void test_write_errors(void)
{
	uint8_t pwm[TLC_DIM_REGS], g = 7;

	model_reset();
	CHECK(tlc_init() && ready_mask() == 7, "init with ACK: the three chips are ready");
	CHECK(nsent == 25 && sent[0] == 0x80 && sent[1] == 0x80 && sent[3] == 0xFF && sent[19] == 0 &&
	      sent[21] == 0xFF && sent[24] == 0xFF,
	      "init: one transfer of 25 bytes from register 0 with auto-increment (%d bytes)", nsent);
	for (int i = 0; i < TLC_DIM_REGS; i++)
		pwm[i] = (uint8_t)i;
	CHECK(tlc_write(RED, TLC_REG_PWM0, pwm, sizeof(pwm)) && nsent == 18 && sent[0] == 0x82 && sent[17] == 16,
	      "a write of PWM0..GRPPWM: 18 bytes from 0x82");

	for (enum answer a = NACK; a <= SILENT; a++) {
		static const uint8_t flag[] = { 0, TLC_E_AF, TLC_E_ARLO, TLC_E_BERR, TLC_E_TIMEOUT };
		const struct tlc_chip_info *gi = tlc_chip_info(GREEN);
		int s0 = stops, u0 = setups;
		uint16_t counts0[5] = { gi->af, gi->arlo, gi->berr, gi->timeout, gi->busy };
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
		CHECK(gi->last_err == flag[a] &&
		      gi->af + gi->arlo + gi->berr + gi->timeout + gi->busy ==
		      counts0[0] + counts0[1] + counts0[2] + counts0[3] + counts0[4] + 1,
		      "address phase, %s: the green error counters count it (last 0x%02X)", answer_names[a],
		      gi->last_err);
		CHECK(strcmp(tlc_state_name(GREEN), "ok") == 0,
		      "address phase, %s: a failed write does not take the chip out of use", answer_names[a]);
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
	answer = ACK;
	chip_regs[BLUE][TLC_REG_IREF] = 0xA5;
	chip_regs[BLUE][TLC_REG_EFLAG1 + 1] = 0x81;
	{
		uint8_t r[TLC_NREGS];

		CHECK(tlc_read_all(BLUE, r) && r[TLC_REG_IREF] == 0xA5 && r[TLC_REG_EFLAG1 + 1] == 0x81,
		      "a read of all 31 registers gives IREF and EFLAG2");
	}
}

static void test_start_order(void)
{
	const struct tlc_start_info *s = tlc_start_info();

	/* a good bar: the probe finds all chips, no reset pulse */
	model_reset();
	CHECK(tlc_init() && ready_mask() == 7 && pulses == 0,
	      "good bar: all chips at the probe, no reset pulse (%d pulses)", pulses);
	CHECK(s->probe == 7 && s->probe_rounds == 1 && s->reset_tries == 0 && !s->recovery_ran &&
	      s->bus_free && errlog_count() == 0,
	      "good bar: start probe 7 (1 round), no reset, no recovery, no ERRLOG entry");
	CHECK(tlc_chip_info(RED)->phase == TLC_PHASE_PROBE && s->pins_idle == TLC_PINS_ALL &&
	      s->pins_reset == TLC_PINS_NONE,
	      "good bar: phase probe, pins idle SDA SCL RESET high, no pins after a reset");

	/* a late driver supply: no chip answers the probe, the pulses give it time */
	model_reset();
	supply_at = now + 120;
	CHECK(tlc_init() && ready_mask() == 7 && pulses >= 1,
	      "late supply: all chips start after %d reset pulses", pulses);
	CHECK(s->probe == 0 && s->probe_rounds == 1 && s->reset == 7 && !s->recovery_ran &&
	      errlog_count() == 0 && tlc_chip_info(GREEN)->phase == TLC_PHASE_RESET,
	      "late supply: one probe round, the reset step starts the chips, no ERRLOG entry");

	/* a chip that needs a reset pulse */
	model_reset();
	need_reset[BLUE] = true;
	CHECK(tlc_init() && ready_mask() == 7 && pulses == 1,
	      "blue needs a pulse: all chips start after 1 pulse (%d)", pulses);
	CHECK(s->probe == 3 && s->probe_rounds == 5 && tlc_chip_info(RED)->phase == TLC_PHASE_PROBE &&
	      tlc_chip_info(BLUE)->phase == TLC_PHASE_RESET,
	      "blue needs a pulse: red and green at the probe (5 rounds), blue at the reset step");
}

static void test_partial(void)
{
	const struct tlc_start_info *s = tlc_start_info();
	uint8_t v = 9;

	/* green is absent: red and blue stay in use */
	model_reset();
	present[GREEN] = false;
	CHECK(!tlc_init() && ready_mask() == 5, "green absent: init fails, red and blue are ready");
	CHECK(s->probe == 5 && s->probe_rounds == 5 && s->reset_tries == 5 && s->reset == 5 &&
	      s->recovery_ran && s->recovery == 0,
	      "green absent: probe 5 rounds, 5 reset pulses keep red and blue, recovery finds no more");
	CHECK(strcmp(tlc_state_name(GREEN), "BAD(nack)") == 0 && strcmp(tlc_state_name(RED), "ok") == 0,
	      "green absent: STATUS names ok BAD(nack) ok (%s)", tlc_state_name(GREEN));
	CHECK(tlc_chip_info(GREEN)->af > 0 && tlc_chip_info(GREEN)->last_err == TLC_E_AF,
	      "green absent: %u AF errors on green", tlc_chip_info(GREEN)->af);
	CHECK(logged(ERR_TLC_INIT, TLC_E_AF << 8 | GREEN) && !logged(ERR_TLC_INIT, RED) &&
	      logged(ERR_TLC_START, 0x5 | 0x5 << 4),
	      "green absent: ERRLOG 119 cause 0x0101 and 126 cause 0x0055");
	CHECK(tlc_set_pwm(RED, 3, v) && tlc_set_pwm(BLUE, -1, v) && !tlc_set_pwm(GREEN, 3, v) &&
	      tlc_set_group_pwm(BLUE, v) && tlc_get_out_mode(GREEN, 0) == -1,
	      "green absent: writes to red and blue work, green refuses");

	/* all chips absent */
	model_reset();
	present[RED] = present[GREEN] = present[BLUE] = false;
	CHECK(!tlc_init() && ready_mask() == 0 && s->probe_rounds == 1 && s->reset_tries == 5,
	      "no chips: 1 probe round, 5 pulses, init fails");
	CHECK(strcmp(tlc_state_name(BLUE), "BAD(nack)") == 0 && s->bus_free,
	      "no chips: BAD(nack), the bus is free");
}

static void test_bus_low(void)
{
	const struct tlc_start_info *s = tlc_start_info();
	uint32_t t0;

	/* green absent, and after a reset pulse a chip holds SDA low */
	model_reset();
	present[GREEN] = false;
	sda_low_after_reset = true;
	release_clocks = 9;
	CHECK(!tlc_init() && ready_mask() == 5,
	      "SDA low after a pulse: red and blue stay in use after the recovery");
	CHECK(pulses == 1 && s->stuck_after_reset && s->reset_tries == 1 &&
	      !(s->pins_reset & TLC_PIN_SDA) && (s->pins_reset & TLC_PIN_SCL),
	      "SDA low after a pulse: one pulse only, pins after reset show SDA 0 SCL 1 (%d pulses)", pulses);
	CHECK(s->recovery_ran && s->recovery == 5 && s->bus_free &&
	      tlc_chip_info(RED)->phase == TLC_PHASE_PROBE,
	      "SDA low after a pulse: the recovery starts red and blue again, the bus is free");
	CHECK(logged(ERR_TLC_START, 0x5 | 0x5 << 8 | 0x1000),
	      "SDA low after a pulse: ERRLOG 126 cause 0x1505");

	/* a good bar with the same fault never pulses, so all chips start */
	model_reset();
	sda_low_after_reset = true;
	CHECK(tlc_init() && ready_mask() == 7 && pulses == 0,
	      "SDA low after a pulse, all chips answer the probe: no pulse, all chips start");

	/* SDA held low for good: no chip, no hang */
	model_reset();
	release_clocks = -1;
	set_sda_low(true);
	t0 = now;
	CHECK(!tlc_init() && ready_mask() == 0 && now - t0 < 2000,
	      "SDA held low: init ends after %u ms, no chip", now - t0);
	CHECK(strcmp(tlc_state_name(RED), "BAD(bus)") == 0 && tlc_chip_info(RED)->busy > 0 &&
	      !s->bus_free && !(s->pins_idle & TLC_PIN_SDA) && !(tlc_pins() & TLC_PIN_SDA),
	      "SDA held low: BAD(bus) with busy errors, pins show SDA 0, the bus is not free");
	CHECK(logged(ERR_TLC_INIT, TLC_E_BUSY << 8 | BLUE) && logged(ERR_TLC_START, 0x2000 | 0x1000),
	      "SDA held low: ERRLOG 119 cause 0x1002 and 126 with the bus flags");

	/* the reset line shorted to ground: no chip answers, the pins tell */
	model_reset();
	rst_shorted = true;
	CHECK(!tlc_init() && ready_mask() == 0 && !(s->pins_idle & TLC_PIN_RESET) &&
	      (s->pins_idle & TLC_PIN_SDA),
	      "RESET shorted: no chip, pins show RESET 0");
	rst_shorted = false;
}

static void test_scan(void)
{
	model_reset();
	present[GREEN] = false;
	CHECK(tlc_probe(0x60) == 0 && tlc_probe(0x61) == TLC_E_AF && tlc_probe(0x62) == 0,
	      "scan: red and blue ACK, green AF");
	CHECK(tlc_probe(TLC_ADDR_SWRST) == 0 && tlc_probe(0x65) == TLC_E_AF,
	      "scan: 0x6B ACK, 0x65 AF");
	set_sda_low(true);
	release_clocks = -1;
	CHECK(tlc_probe(0x60) == TLC_E_BUSY, "scan with SDA held low: bus busy");
	model_reset();

	CHECK(tlc_pullups() == (TLC_PIN_SDA | TLC_PIN_SCL), "pull-up test: both lines rise");
	pullup_sda = false;
	CHECK(tlc_pullups() == TLC_PIN_SCL, "pull-up test: SDA with no pull-up stays low");
	pullup_scl = false;
	pullup_sda = true;
	CHECK(tlc_pullups() == TLC_PIN_SDA, "pull-up test: SCL with no pull-up stays low");
	model_reset();
	CHECK(tlc_init() && tlc_chip_info(RED)->af + tlc_chip_info(RED)->busy == 0 && floating == 0,
	      "after the pull-up test: the chips start with no error, no pin floats");
}

int main(void)
{
	alarm(20);	/* a hang in the driver ends the test with SIGALRM */
	test_write_errors();
	test_start_order();
	test_partial();
	test_bus_low();
	test_scan();
	DONE("test_i2c");
}
