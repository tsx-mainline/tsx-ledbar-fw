/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TSX_H
#define TSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TSX_VERSION_MAJOR	0
#define TSX_VERSION_MINOR	1
#define TSX_VERSION_BUILD	8
#define TSX_VERSION_STR		"0.1.8"
/* USB string 4 and the VER command: the host tools look for "TSX-LEDBAR" */
#define TSX_FW_NAME		"TSX-LEDBAR [v" TSX_VERSION_STR "]"
#define TSX_PRODUCT_CODE	0xE5

enum { RED, GREEN, BLUE, NCOLORS };

/*
 * The bar has 16 RGB LEDs, 8 on each side. Output n of the red, green and
 * blue chip drives the same LED. The LED index counts by position: 0..7
 * are R1..R8 (right side, top to bottom), 8..15 are L1..L8 (left side,
 * top to bottom). ledmap.c holds the maps from the index to the output.
 */
#define NLEDS		16
#define NROWS		8

/* board.c */
void board_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
void system_reset(void);

/* guard.c: the start guard */
void guard_boot(void);			/* first at start: watchdog, failed start count */
void guard_report(void);		/* error log entries about the last start */
void guard_poll(void);			/* main loop: the USB rule */
void guard_usb_host(void);		/* SOF packets after a bus reset: a host runs */
void guard_usb_configured(void);
void guard_kick(void);
void guard_reboot(void);		/* REBOOT: a planned reset */
void guard_request_bootloader(void);	/* IMGUPD: mailbox "UPG", reset */
uint32_t guard_reset_flags(void);
uint32_t guard_start_count(void);	/* this start since power-on */
uint32_t guard_fails(void);		/* failed starts in a row */

/* errlog.c */
#define ERR_TLC_INIT		119	/* LED driver chip did not start. cause: chip | I2C error flags << 8 */
#define ERR_I2C			120
#define ERR_WATCHDOG		121	/* the last start ended in a reset that was not planned */
#define ERR_GUARD		122	/* cause: failed starts in a row */
#define ERR_USB			123	/* cause: the IN endpoint that was stuck */
#define ERR_FAULT		124	/* the last start ended in a hard fault */
#define ERR_NOCONFIG		125	/* the last start: a host ran, no USB configuration */
#define ERR_TLC_START		126	/* not all LED driver chips started: what each step found */
void errlog_add(uint16_t subsystem, uint16_t cause);
int errlog_count(void);
void errlog_clear(void);
/* calls fn for each entry: index, time ms, subsystem, cause */
void errlog_each(void (*fn)(int, uint32_t, uint16_t, uint16_t));

/* i2c_tlc.c: the three TLC59116 drivers, one per color */
bool tlc_init(void);			/* probe, reset pulses, bus recovery: see i2c_tlc.c */
bool tlc_ready(int color);
/* I2C error flags of a failed transfer */
#define TLC_E_AF		0x01	/* no ACK */
#define TLC_E_ARLO		0x02	/* arbitration lost */
#define TLC_E_BERR		0x04	/* bus error */
#define TLC_E_TIMEOUT		0x08	/* no flag within the time limit */
#define TLC_E_BUSY		0x10	/* the bus stayed busy before the START */
/* pin levels: bit set = high */
#define TLC_PIN_SDA		0x01
#define TLC_PIN_SCL		0x02
#define TLC_PIN_RESET		0x04
#define TLC_PINS_ALL		0x07
#define TLC_PINS_NONE		0xFF	/* not read */
#define TLC_ADDR_ALLCALL	0x68
#define TLC_ADDR_SWRST		0x6B
#define TLC_NREGS		0x1F	/* registers 0x00..0x1E, EFLAG2 is the last */
#define TLC_REG_IREF		0x1C
#define TLC_REG_EFLAG1		0x1D
enum { TLC_PHASE_NONE, TLC_PHASE_PROBE, TLC_PHASE_RESET, TLC_PHASE_RECOVERY };
struct tlc_chip_info {
	uint8_t phase;			/* the start step that made the chip ready */
	uint8_t last_err;		/* TLC_E flags of the last failed transfer */
	uint16_t af, arlo, berr, timeout, busy;	/* failed transfers since the init, by flag */
};
struct tlc_start_info {
	uint8_t pins_power;		/* at the first init, before RESET is driven */
	uint8_t pins_idle;		/* at the init, before the probe */
	uint8_t pins_reset;		/* after the first reset pulse, or TLC_PINS_NONE */
	uint8_t probe, reset, recovery;	/* bit c: chip c answered in that step */
	uint8_t probe_rounds, reset_tries;
	bool recovery_ran;
	bool stuck_after_reset;		/* SDA or SCL low after a reset pulse */
	bool bus_free;			/* SDA and SCL high at the end of the init */
	uint32_t ms;			/* time of the init */
};
const struct tlc_chip_info *tlc_chip_info(int color);
const struct tlc_start_info *tlc_start_info(void);
const char *tlc_state_name(int color);	/* "ok", "BAD(nack)" or "BAD(bus)" */
uint8_t tlc_probe(uint8_t addr);	/* I2CSCAN: 0 = ACK, else TLC_E flags */
uint8_t tlc_pins(void);			/* SDA, SCL and RESET now: TLC_PIN bits */
uint8_t tlc_pullups(void);		/* TLC_PIN bit set: the line has a pull-up */
void tlc_bus_slow(bool on);		/* I2CSCAN SLOW: the bus at 10 kHz, or 100 kHz again */
#define TLC_RISE_NEVER		0xFFFFFFFFU	/* the line did not rise within 100 us */
struct tlc_pintest {
	uint32_t sda_ext, sda_int;	/* ns to rise: board pull-up only, internal too */
	uint32_t scl_ext, scl_int;
	uint32_t step_ns;		/* the time between two samples */
	uint32_t floor_ns;		/* the same test on a driven edge: the latency */
	uint8_t rst_low, rst_high;	/* RESET read back while driven low, high */
	uint32_t rst_rise;		/* ns to rise, released after a low level */
};
void tlc_pintest(struct tlc_pintest *t);	/* PINTEST: resets the chips */
bool tlc_read_all(int color, uint8_t regs[TLC_NREGS]);	/* from the chip */
#ifdef TSX_QEMU
void tlc_qemu_chips(uint8_t mask);	/* TEST CHIPS: bit c = chip c answers */
#endif
bool tlc_write(int color, uint8_t reg, const uint8_t *data, size_t n);
bool tlc_read(int color, uint8_t reg, uint8_t *data, size_t n);
bool tlc_set_group_pwm(int color, uint8_t duty);
/* PWM0..PWM15 and GRPPWM are 17 registers in a row, written in one transfer */
#define TLC_REG_PWM0		0x02
#define TLC_REG_GRPPWM		0x12
#define TLC_DIM_REGS		17
void tlc_get_dim(int color, uint8_t *pwm, uint8_t *grp);	/* PWM0 and GRPPWM */
uint8_t tlc_get_pwm(int color, int out);
bool tlc_set_group_blink(int color, bool blink, uint8_t freq);
bool tlc_set_out_mode(int color, int out, int mode);	/* out 0..15 or -1 = all */
bool tlc_set_pwm(int color, int out, uint8_t duty);
int tlc_get_out_mode(int color, int out);
void tlc_reset_pulse(void);

/* ledmap.c: the LED maps and the board variant pins */
enum ledmap_source { LEDMAP_DEFAULT, LEDMAP_PANEL, LEDMAP_CONSOLE };
void ledmap_init(void);			/* read the variant pins, take the default map */
unsigned ledmap_variant(void);		/* the value of the pins, 0..7: information only */
const char *ledmap_name(void);		/* the map in use */
const char *ledmap_source(void);	/* "default", "panel" or "console" */
const uint8_t *ledmap_outputs(void);	/* the map in use: output of LED index 0..15 */
void ledmap_default(void);		/* the default map (TSW-1060-LB), source "default" */
bool ledmap_select(const char *name, enum ledmap_source src);	/* false: no map with this name */
void ledmap_each(void (*fn)(const char *));	/* the name of each map */
/* the console command LEDMAP [NAME [PANEL] | DEFAULT] */
enum ledmap_cmd { LEDMAP_SHOW, LEDMAP_DONE, LEDMAP_NO_MAP, LEDMAP_USAGE };
enum ledmap_cmd ledmap_command(int argc, char *const *argv);

/* leds.c: the LED engine */
struct led_state {
	uint8_t level[NCOLORS];		/* host level 0..100 (analog join) */
	bool control[NCOLORS];		/* host switch (digital join) */
	uint16_t blink_100ms[NCOLORS];	/* stock blink time, 100 ms units */
	uint16_t duty[NCOLORS];		/* highest LED duty sent, 0..65535 of full */
	uint16_t led_duty[NLEDS][NCOLORS];	/* duty sent for each LED */
	uint8_t pattern[NLEDS][NCOLORS];	/* LED pattern levels 0..100 */
	bool pattern_on;		/* the pattern, not the host color, is the base */
	uint16_t limited;		/* bit i: the per-LED limit scales LED i */
};
void leds_init(void);
void leds_tick(void);			/* run every ms from the main loop */
void leds_set_level(int color, unsigned level);
void leds_set_control(int color, bool on);
void leds_set_blink_time(int color, unsigned n100ms);
const struct led_state *leds_state(void);
int leds_output(int led);		/* TLC59116 output of LED index 0..15 */
void leds_base_level(int led, uint8_t rgb[3]);	/* pattern or steady host level */
/* the LED pattern: set LEDs first..last, or go back to the host color */
void leds_pattern_set(int first, int last, const uint8_t rgb[3]);
void leds_pattern_clear(void);
void leds_resync(void);			/* after TLCRESET: write all registers again */
uint32_t leds_grp_changes(int color);	/* GRPPWM changes written by the engine */
void leds_freeze(bool on);		/* FX FREEZE ON|OFF */
bool leds_frozen(void);
void leds_step(uint32_t ms);		/* FX STEP MS */
/* effects */
void leds_fx_off(void);
void leds_fx_fade(const uint8_t rgb[3], uint32_t ms);
void leds_fx_blink(const uint8_t rgb[3], uint32_t on_ms, uint32_t off_ms);
void leds_fx_breathe(const uint8_t rgb[3], uint32_t period_ms);
void leds_fx_rainbow(uint32_t period_ms, uint8_t level);
void leds_fx_chase(const uint8_t rgb[3], uint32_t period_ms);
void leds_fx_fill(const uint8_t rgb[3], unsigned percent);
void leds_fx_spectrum(uint32_t period_ms, uint8_t level, bool ring);	/* ring or rows */
void leds_fx_split(const uint8_t right[3], const uint8_t left[3]);
void leds_set_smooth(uint32_t ms);
uint32_t leds_get_smooth(void);
const char *leds_fx_name(void);
void leds_direct(const uint8_t rgb[3]);	/* SELFTEST: duty straight to the chips */
void leds_direct_off(void);
void leds_set_cap(unsigned percent);
unsigned leds_get_cap(void);

/* usb_dev.c */
void usb_init(void);
void usb_poll(void);
void usb_flush(uint32_t ms);		/* before a reset: send the console text (at most ms), then wait 20 ms */
bool usb_configured(void);
void usb_debug_state(uint32_t v[7]);	/* DIEPCTL1, DIEPINT1, DIEPTSIZ1, GINTSTS, DAINT, console head, tail */
/* console (interface 0) text out, Cresnet (interface 1) packets out */
void console_write(const char *s);
void console_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool cresnet_send(const uint8_t *pkt, size_t n);
/* called from usb_dev.c with received data */
void console_rx(const uint8_t *data, size_t n);
void cresnet_rx(const uint8_t *data, size_t n);

/* console.c */
void console_init(void);
void console_poll(void);

/* cresnet.c */
void cresnet_init(void);
void cresnet_poll(void);

/* qemu_uart.c (TSX_QEMU only) */
#ifdef TSX_QEMU
void qemu_uart_init(void);
void qemu_uart_write(const char *s);
int qemu_uart_read(void);
#endif

#endif
