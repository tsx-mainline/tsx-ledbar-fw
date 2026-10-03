/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef TSX_H
#define TSX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TSX_VERSION_MAJOR	0
#define TSX_VERSION_MINOR	1
#define TSX_VERSION_BUILD	3
#define TSX_VERSION_STR		"0.1.3"
/* USB string 4 and the VER command: the host tools look for "TSX-LEDBAR" */
#define TSX_FW_NAME		"TSX-LEDBAR [v" TSX_VERSION_STR "]"
#define TSX_PRODUCT_CODE	0xE5

enum { RED, GREEN, BLUE, NCOLORS };

/*
 * The bar has 16 RGB LEDs, 8 on each side. Output n of the red, green and
 * blue chip drives the same LED. The LED index counts by position: 0..7
 * are R1..R8 (right side, top to bottom), 8..15 are L1..L8 (left side,
 * top to bottom). leds.c holds the map from the index to the output.
 */
#define NLEDS		16
#define NROWS		8

/* board.c */
void board_init(void);
uint32_t millis(void);
void delay_ms(uint32_t ms);
void system_reset(void);

/* guard.c */
void guard_boot(void);
void guard_usb_configured(void);
void guard_kick(void);
void guard_request_bootloader(void);	/* IMGUPD: mailbox "UPG", reset */
uint32_t guard_reset_flags(void);
uint32_t guard_start_count(void);

/* errlog.c */
#define ERR_TLC_INIT		119	/* LED driver chip did not answer */
#define ERR_I2C			120
#define ERR_WATCHDOG		121
#define ERR_GUARD		122
#define ERR_USB			123
void errlog_add(uint16_t subsystem, uint16_t cause);
int errlog_count(void);
void errlog_clear(void);
/* calls fn for each entry: index, time ms, subsystem, cause */
void errlog_each(void (*fn)(int, uint32_t, uint16_t, uint16_t));

/* i2c_tlc.c: the three TLC59116 drivers, one per color */
bool tlc_init(void);			/* reset pulse and register init, all chips */
bool tlc_ready(int color);
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
