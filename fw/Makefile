# SPDX-License-Identifier: GPL-2.0-or-later
# tsx-ledbar firmware. Needs arm-none-eabi-gcc and a libopencm3 build for
# stm32/f2 (make TARGETS=stm32/f2 in the libopencm3 tree).
#
#   make LIBOPENCM3_DIR=/path/to/libopencm3          build/tsx-ledbar.upg
#   make LIBOPENCM3_DIR=... qemu                      build/qemu/tsx-ledbar-qemu.elf
#
# The firmware image goes to the bar with tools/tsx-ledbar-flash.

LIBOPENCM3_DIR ?= ../libopencm3
PREFIX ?= arm-none-eabi-
CC = $(PREFIX)gcc
OBJCOPY = $(PREFIX)objcopy
SIZE = $(PREFIX)size
PYTHON ?= python3
UPG = $(PYTHON) ../tools/tsx-upg.py

VERSION := $(shell sed -n 's/^#define TSX_VERSION_STR[[:space:]]*"\([^"]*\)".*/\1/p' src/tsx.h)

SRCS = main.c board.c guard.c header.c errlog.c i2c_tlc.c leds.c ledmap.c usb_dev.c console.c cresnet.c qemu_uart.c

CFLAGS = -Os -g -std=gnu11 -Wall -Wextra -Wshadow -Wundef -Wredundant-decls \
	-mcpu=cortex-m3 -mthumb -msoft-float -DSTM32F2 \
	-ffunction-sections -fdata-sections -fno-common -MMD \
	-I$(LIBOPENCM3_DIR)/include
LDFLAGS = -mcpu=cortex-m3 -mthumb -msoft-float -nostartfiles \
	--specs=nano.specs --specs=nosys.specs \
	-Wl,--gc-sections -L$(LIBOPENCM3_DIR)/lib
LDLIBS = -lopencm3_stm32f2 -lc -lgcc -lnosys

B = build
OBJS = $(addprefix $(B)/, $(SRCS:.c=.o))
QB = build/qemu
QOBJS = $(addprefix $(QB)/, $(SRCS:.c=.o))

all: $(B)/tsx-ledbar.upg

$(B)/%.o: src/%.c | $(B)
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/tsx-ledbar.elf: $(OBJS) ld/app.ld
	$(CC) $(LDFLAGS) -T ld/app.ld -Wl,-Map=$(B)/tsx-ledbar.map -o $@ $(OBJS) $(LDLIBS)
	$(SIZE) $@

$(B)/tsx-ledbar.bin: $(B)/tsx-ledbar.elf
	$(OBJCOPY) -O binary $< $@

$(B)/tsx-ledbar.upg: $(B)/tsx-ledbar.bin
	$(UPG) pack --version $(VERSION) $< $@
	$(UPG) verify $@

qemu: $(QB)/tsx-ledbar-qemu.elf

$(QB)/%.o: src/%.c | $(QB)
	$(CC) $(CFLAGS) -DTSX_QEMU -c -o $@ $<

$(QB)/tsx-ledbar-qemu.elf: $(QOBJS) ld/app.ld
	$(CC) $(LDFLAGS) -T ld/app.ld -Wl,--defsym=TSX_ROM_ORIGIN=0x08000000 -o $@ $(QOBJS) $(LDLIBS)
	$(SIZE) $@

$(B) $(QB):
	mkdir -p $@

clean:
	rm -rf $(B)

-include $(OBJS:.o=.d) $(QOBJS:.o=.d)

.PHONY: all qemu clean
