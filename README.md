# LED bar firmware: install and tools

tsx-ledbar-fw is open firmware for the Crestron USB LED bar, with the tools that load it. License: GPL-2.0-or-later.

The firmware is tested on the TSW-1060-LB LED bar. The firmware has the LED map of the TSW-1060-LB and uses it by default. It also has the measured map of the TSW-1070-LB. Other LED bar models are not tested. Another bar model can have a different map. The bar cannot tell its model. The panel service `tsx-ledbard` selects the map with the console command `LEDMAP`: see [Board variant and LED map](docs/leds.md#board-variant-and-led-map). On another bar model, all functions work, but an LED can light at the wrong position. `STATUS` shows the map.

The bar has an STM32F205RC and three TLC59116 LED drivers (red, green, blue, 16 outputs each) for 16 RGB LEDs, 8 on each side. The stock firmware stays the default. This firmware is optional. It adds fades, blink, breathe, rainbow and zone effects that run on the bar, and control of each LED. It keeps the stock USB vendor and product IDs, the product string, the Cresnet joins and the console commands. The manufacturer string is "tsx-mainline", and the firmware name is `TSX-LEDBAR`. The firmware refuses the stock commands that write the LED driver registers directly, because these commands skip the power limits. The mainline driver `leds-crestron-stm32` and the `tsx-ledbar` tools work with this firmware without changes.

## Install the firmware

Do these steps on the panel, as root.

1. Install the package: `apk add tsx-ledbar-fw`.
2. Run `tsx-ledbar-fw-install`.
3. Wait for the command to end. It stops the `tsx-ledbar` service, loads the image, and starts the service again.
4. Run `tsx-ledbar-fw-install --check`. The command exits with 0 when the bar runs the packaged version.

After this, a package upgrade loads the new image into the bar. The install writes the marker `/data/tsx/ledbar-fw.installed`, and the upgrade script looks for it. The load restarts `tsx-esphome` and `tsx-voice` when they run, because these services read the firmware name (the effect list) only at start. A failed restart does not fail the load.

## Load an image by hand

1. Run `tsx-ledbar-flash info` to show the mode, the USB strings and the version.
2. Run `tsx-ledbar-flash flash FILE.upg`.

The command checks the image first (record checksums and lengths, tag, header, CRC, address range). It stops the `tsx-ledbar` service, sends the prepare packet, waits for the bootloader, and sends the image as Cresnet packets ([update protocol](docs/update-protocol.md)). It then waits for the new application and starts the service again. A load takes a few seconds. A bar that waits in bootloader mode may not send its ready packet. Then the command sends the prepare packet to the bootloader, and the bootloader answers it ([update protocol](docs/update-protocol.md#the-ready-packet)). If the bootloader does not take the prepare packet, the command tries three times. It then stops, and the bar keeps its image. Unplug the LED bar and plug it in again, or run the command again. The bootloader keeps an image only when its CRC is good. An interrupted load leaves the bootloader in place. Run the command again.

## Go back to the stock firmware

The stock image is Crestron software. It is not in this repo or in a package. The mainline installer keeps a copy on the panel in `/data/tsx/vendor/`.

1. Run `tsx-ledbar-fw-uninstall`. If the stock image is in another place, give its path: `tsx-ledbar-fw-uninstall PATH/statussign_VERSION.upg`.
2. The command checks the image, loads it, restarts `tsx-esphome` and `tsx-voice` when they run, and removes the marker. Package upgrades then leave the bar alone.

## Recover a bar in bootloader mode

A bar in the bootloader (USB `14be:001a`) has no application, and its LEDs stay dark. This happens after an interrupted load. It also happens when the start guard hands the bar to the bootloader.

The `tsx-ledbard` service of the panel image loads an image by itself. It looks for the bar when the service starts and when a bar appears later. When it finds the bar in bootloader mode, it runs `tsx-ledbar-fw-install --recover`. It tries this once for each start of the service. If the load fails, it logs the failure and does not try again. Restart the `tsx-ledbar` service for one more try.

The command loads the image that the panel is set up for. It takes the first of these:

1. The packaged image, when the marker `/data/tsx/ledbar-fw.installed` exists.
2. The newest stock image in `/data/tsx/vendor/`.
3. The packaged image.

A panel that ran `tsx-ledbar-fw-uninstall` has no marker. It gets the stock image, not the open firmware.

`tsx-ledbar-fw-install --recover-image` prints the image and exits with 3 when there is none. It loads nothing. `--recover` does nothing when the bar runs its application. It keeps the `tsx-ledbar` service running, because the service calls it. It does not write or remove the marker. It restarts `tsx-esphome` and `tsx-voice` when they run, as a load does.

## Tools

| Command | Function |
| --- | --- |
| `tsx-ledbar-fw-install [FILE.upg]` | Load the packaged image or another image. Write the marker |
| `tsx-ledbar-fw-install --check` | Exit with 0 when the bar runs the packaged version. Else exit with 1 and tell what the bar runs: another version, bootloader mode, or no bar |
| `tsx-ledbar-fw-install --recover-image` | Print the image that `--recover` loads. Exit with 3 when there is none |
| `tsx-ledbar-fw-install --recover` | Load that image into a bar in bootloader mode. Keep the `tsx-ledbar` service running and the marker as it is. `tsx-ledbard` calls it |
| `tsx-ledbar-fw-uninstall [FILE.upg]` | Load the stock image and remove the marker |
| `tsx-ledbar-flash info` | Show the mode, the USB strings, the version and the LED map line |
| `tsx-ledbar-flash flash FILE.upg` | Check the file, then load it |
| `tsx-ledbar-flash console CMD...` | Send one application console command |
| `tsx-ledbar-flash enter-bootloader` | Send the prepare packet only. The `tsx-ledbar` service stays stopped: start it after the load |
| `tsx-ledbar-flash btl-read [MS]` | Read bootloader packets for MS milliseconds |
| `tsx-ledbar-flash port-reset` | Reset the USB bus of the bar port: disable it for 3 seconds, then enable it |
| `tools/tsx-upg.py` | Pack, unpack, `info` and `verify` for `.upg` images (S-record container with the application header) |

`port-reset` does not cut the power of the bar, because the panel keeps VBUS on. A bar that does not answer on USB can return to its application after it. A real power cycle needs a power cycle of the panel.

The package installs the tools in `/usr/local/lib/tsx-ledbar-fw/` and the commands in `/usr/local/sbin/`. The packaged image is `/usr/share/tsx-ledbar-fw/tsx-ledbar.upg`.

## Console

The console is USB interface 0. It takes one command per line. Use `tsx-ledbar-flash console CMD...` from the panel. `HELP` lists the commands. `CAPS` lists the features of the firmware.

| Command | Function |
| --- | --- |
| `VER` | Firmware name and version |
| `CAPS` | Features of this firmware |
| `STATUS` | Levels, 16-bit duties, PWM and group values of each chip, effect, chip state, uptime, start number, failed starts, reset flags, board variant value and LED map. The last two lines show the start of the LED chips and the bus pins: see [LED chips](#led-chips) |
| `LEDMAP [NAME [PANEL] \| DEFAULT]` | Show the LED map and the list of maps, or use another map until the next start: see [Board variant and LED map](docs/leds.md#board-variant-and-led-map) |
| `ERRLOG`, `CLEARERR` | Show or clear the [error log](#error-log) |
| `REBOOT` | Restart the application |
| `IMGUPD` | Restart into the bootloader (USB update mode) |
| `TLCOUTMODE COLOR NUM\|ALL` | Show the output mode of a TLC59116 output. `ALL` shows output 0 |
| `TLCOUTMODE COLOR NUM\|ALL MODE`, `TLCGROUPMODE`, `TLCBRIGHTNESS` | Refused. In the stock firmware these commands write the LED driver registers directly, past the power limits. Use `LED SET` and `FX` |
| `TLCRESET` | Reset and initialize the LED driver chips |
| `TLCREGS COLOR` | Read the registers of one LED driver chip, and show the number of group PWM changes since the start. The third line shows IREF and the error flags EFLAG1 and EFLAG2 |
| `I2CSCAN [RESET] [SLOW]` | Probe the I2C addresses 0x60 to 0x6F and show the answers, the I2C errors of each chip, the registers of each chip that answers, the pins and a pull-up test: see [LED chips](#led-chips). `RESET` pulses the reset line first and then starts the chips again. `SLOW` probes at 10 kHz instead of 100 kHz |
| `PINTEST` | Measure the rise time of SDA and SCL, and test the reset line: see [LED chips](#led-chips). The test resets the chips and then starts them again |
| `SELFTEST LED ON\|OFF [COLOR\|ALL] [PERCENT]` | Light the LEDs for a test |
| `LED COLOR LEVEL\|CONTROL\|BLINK VALUE` | Set the host joins by hand |
| `LED SET`, `LED SIDE`, `LED GET`, `LED CLEAR` | Control of each LED: see [LED control](docs/leds.md) |
| `FX ...` | Effects, in the next table |

### Effects

Time values are in milliseconds. Colors R G B are levels from 0 to 100.

| Command | Effect |
| --- | --- |
| `FX OFF` | End the effect. Ramp back to the LED pattern when one is set, else to the host color |
| `FX FADE R G B MS` | Fade to a color in MS |
| `FX BLINK R G B ON_MS OFF_MS` | Blink a color |
| `FX BREATHE R G B MS` | Fade a color up and down, one cycle in MS |
| `FX RAINBOW MS [LEVEL]` | Cycle the hue over the whole bar, one turn in MS |
| `FX CHASE R G B MS` | A dot runs down both sides in step |
| `FX FILL R G B PERCENT` | A level bar from the bottom up on both sides |
| `FX SPECTRUM MS [LEVEL] [RING\|ROWS]` | The hue circle around the bar (`RING`, the default) or down the rows (`ROWS`) |
| `FX SPLIT R G B R G B` | One color on the right side, one on the left side |
| `FX SMOOTH MS` | Ramp every host change over MS (0 to 60000). A hue sweep from Home Assistant then looks smooth at a low update rate |
| `FX CAP PERCENT` | Power limit, 10 to 150, default 110: see [LED control](docs/leds.md). The stock firmware never goes above 110. A higher value lets the bar draw more current from the panel |
| `FX FREEZE ON\|OFF` | Stop or start the clock of the effect, so the bar holds its current color |
| `FX STEP MS` | Move the clock of the effect forward by MS |

## Cresnet joins

The Cresnet interface is USB interface 1.

| Join | Function |
| --- | --- |
| Analog 3 to 5 | Levels of red, green and blue |
| Digital 0 to 2 | Switch the colors |
| Analog 0 to 2 | Blink time in 100 ms steps |

A host join drops the LED pattern, ends a running effect and ramps the bar to the host color, also when the join keeps that color.

## Firmware behavior

| Part | Behavior |
| --- | --- |
| Start guard | The independent watchdog runs from the first instruction (4 s). After three failed starts in a row, the guard hands the bar to the bootloader for an update: see [Start guard](#start-guard) |
| LED map | The firmware starts with the map of the TSW-1060-LB. The panel service `tsx-ledbard` sends the map of the panel with `LEDMAP` after each plug-in and each start of the bar: see [Board variant and LED map](docs/leds.md#board-variant-and-led-map) |
| Board variant | At start the firmware reads the variant pins PB13 to PB15 (value 0 to 7). `STATUS` shows the value. The value does not change the behavior |
| LED chips | The init first probes the chips with no reset pulse. When a chip is missing, it pulses the reset line and tries again, then it frees the bus. Each chip that answers stays in use: see [LED chips](#led-chips) |
| LED engine | Runs every 10 ms. Keeps a 16-bit duty for each of the 16 LEDs and each color. A level (0 to 100) maps to a duty with the CIE 1976 lightness curve, so fades do not dwell at the bottom. Effects and fades dim a color in light, so it keeps its hue: see [LED control](docs/leds.md) |
| Power limit | `FX CAP`: see [LED control](docs/leds.md) |
| Chip brightness | The product of the PWM of the output and the group PWM. The lowest step is 1/65025 of full brightness. The group PWM goes up when the light of the chip needs it, and it goes down only while the chip is dark: see [LED control](docs/leds.md). One transfer writes the registers that changed |
| Image | Linked at 0x08020000. The header holds the CRC-16 of the image, the version and the product code 0xE5 |

### LED chips

The three TLC59116 LED driver chips share the I2C bus (SDA, SCL) and one reset line. The init starts them in three steps:

1. Probe: the firmware writes the registers of each chip with no reset pulse, as the stock firmware does. When some chips answer, the driver supply is on. The firmware then asks the other chips again, up to five times, 50 ms apart, still with no pulse.
2. Reset: when a chip is still missing, the firmware pulses the reset line, waits 50 ms and starts all chips again. A pulse clears every chip, so every chip gets its registers again. This step runs up to five times. It stops early when SDA or SCL stays low after a pulse, or when a pulse costs a chip that answered before.
3. Recovery: when a chip is still missing, the firmware frees the bus (up to 18 SCL clocks and a STOP) and starts the missing chips again with no pulse.

A bar whose driver supply comes up late gives no answer to the probe. The reset step then gives the supply up to five times 60 ms. A good bar answers the first probe, and the init takes about 7 ms.

Each chip that answers stays in use. The other colors stay dark: the LED engine, the effects, `LED GET` and `SELFTEST` work with the chips that answer. `LED GET` and `SELFTEST` name a chip that is not in use. `TLCOUTMODE` and `TLCREGS` answer "LED driver not initialized!" for it, as the stock firmware does.

`STATUS` shows the result in three lines:

```
chips ok BAD(nack) ok uptime ...
start probe red blue (5 rounds), reset red blue (5 pulses), recovery none, 612 ms
pins power-up SDA 1 SCL 1 RESET 1, idle SDA 1 SCL 1 RESET 1, after reset SDA 1 SCL 1 RESET 1, now SDA 1 SCL 1 RESET 1
```

- `chips`: the state of the red, green and blue chip. `ok`: in use. `BAD(nack)`: the last transfer got no ACK. The chip is absent, has no supply, or does not see the bus. `BAD(bus)`: the last transfer saw an arbitration loss, a bus error, a timeout or a busy bus. A line is held low, or a chip answers wrong.
- `start`: the chips that answered in each step, the number of probe rounds and reset pulses, and the time of the init. "not needed" means that the step did not run.
- `pins`: the levels of SDA, SCL and RESET at power-up, at the init, after the first reset pulse, and now. The firmware reads SDA and SCL as GPIO inputs with the pull-ups of the bus. RESET is an output, so a 0 there means a short to ground. A 0 on SDA or SCL means that the line is held low.

`I2CSCAN` probes each address from 0x60 to 0x6F with a START, the address and a STOP. It sends no data, so the software reset address 0x6B resets nothing. The answer shows:

- The addresses that answer. Red, green and blue are at 0x60, 0x61 and 0x62.
- 0x68 (all-call): only a chip before its init or after a reset answers here, because the init turns the all-call address off.
- 0x6B (software reset): every chip with a supply answers here. No answer at 0x6B means that no chip sees the bus.
- An answer at another address: no chip has this address. Look at the address pins of the chips.
- The I2C errors of each chip since the init: AF, ARLO, BERR, timeout and busy bus, and the flags of the last failed transfer.
- MODE1, MODE2, IREF, EFLAG1 and EFLAG2 of each chip that answers.
- The `start` and `pins` lines of `STATUS`.
- A pull-up test: the firmware drives SDA and SCL low, releases them with no internal pull-up, and reads them again. "rises" means that the line has a pull-up resistor. "stays low" means that it has none. The firmware tests SDA while SCL is low, so the chips see no START or STOP.

`I2CSCAN RESET` first pulses the reset line and shows the pins after the pulse. After the scan, it starts the chips again with the three steps. `I2CSCAN SLOW` probes at 10 kHz. A chip that answers at 10 kHz but not at 100 kHz points to weak pull-ups or a long, loaded bus.

`PINTEST` measures the lines with no I2C transfer:

- SDA and SCL: the firmware holds the line low, releases it, and reads the port 64 times in a row (a step of about 35 ns). The first high sample gives the rise time. The test runs with the pull-up resistors of the board only, then with the internal pull-up too. It runs 8 times and keeps the shortest time. A line that is still low after the 64 samples gets a slower count, up to 100 us. A longer bus, more chips and weaker pull-ups give a longer rise time. "stays low for 100 us" means that the line has no pull-up, or that something holds it low.
- The first line also shows the latency of the test: the same measurement on an edge that the firmware drives. A rise time near this value means a very light load on the line.
- RESET: the firmware reads the line back while it drives it low and high. Then it releases the line after a low level and measures the time until it reads high. "stays low for 100 us" means that the reset line has no pull-up.

Compare the values with the values of a good bar on the same controller. On a good bar, the lines reach the three chips, so SDA and SCL have a larger load.

### Start guard

The bootloader starts the application at once, with no window for an update. An application that hangs, or that cannot come up on USB, would block the update path. The start guard prevents this.

The independent watchdog runs from the first instruction, with a period of 4 s. The guard counts the failed starts in a row. After three failed starts in a row, it writes "UPG" into the bootloader mailbox and resets. The bootloader then stays in its USB update mode, and `tsx-ledbar-fw-install` or `tsx-ledbar-flash flash` can load an image.

One of these events makes a start a failed start:

- The watchdog, or another reset that the firmware did not plan.
- A hard fault.
- A USB host runs, and USB does not reach "configured" within 60 s. A host runs when it resets the bus and then sends SOF packets. A bus reset alone does not count, because a host that powers up or goes down can give a short reset.

A start without a USB host is not a failed start. The bar keeps its light when the panel stops in U-Boot, runs an installer, or hangs in a kernel panic. A planned reset (`REBOOT`, `IMGUPD`, the prepare packet) is not a failed start. The count goes to 0 when USB reaches "configured", and when a start runs for 60 s without a host.

The guard cannot find a firmware that never connects to the USB bus. Such a firmware gets no bus reset, so the guard cannot tell it from a bar without a host. The panel service `tsx-ledbard` finds a bar in bootloader mode and loads an image: see [Recover a bar in bootloader mode](#recover-a-bar-in-bootloader-mode).

`STATUS` shows the number of the start since power-on (`start`) and the failed starts in a row (`fails`).

### Error log

`ERRLOG` shows one line for each entry: the time in ms after the start, the subsystem and a cause.

| Subsystem | Meaning |
| --- | --- |
| 119 | A LED driver chip did not start at the init. Cause: the chip (0 red, 1 green, 2 blue) in the low byte. The high byte holds the I2C error flags of its last transfer: 0x01 no ACK (AF), 0x02 arbitration lost (ARLO), 0x04 bus error (BERR), 0x08 timeout, 0x10 bus busy |
| 120 | An I2C transfer failed. Cause: chip x 256 + register |
| 121 | The last start ended in a reset that the firmware did not plan. Cause 0: the watchdog flag is set. Cause 1: no watchdog flag |
| 122 | Failed starts in a row. Cause: the count |
| 123 | A USB IN endpoint did not send, and the firmware reset it. Cause: the endpoint |
| 124 | The last start ended in a hard fault |
| 125 | In the last start a USB host ran, but USB did not reach "configured" within 60 s |
| 126 | Not all LED driver chips started. Cause: bits 0 to 2 are the chips that answered the probe, bits 4 to 6 the chips after the last reset pulse, bits 8 to 10 the chips that the bus recovery started. Bit 12: SDA or SCL was low after a reset pulse. Bit 13: the bus was not free at the end |

## Build

Needs `arm-none-eabi-gcc` and a libopencm3 tree built for `stm32/f2` (`make TARGETS=stm32/f2` in the libopencm3 directory).

1. Run `cd fw`.
2. Run `make LIBOPENCM3_DIR=/path/to/libopencm3`. The result is `build/tsx-ledbar.upg`, the file that the bar bootloader takes.
3. For the QEMU build, run `make LIBOPENCM3_DIR=/path/to/libopencm3 qemu`. The result is `build/qemu/tsx-ledbar-qemu.elf`.

## Repo layout

| Path | Contents |
| --- | --- |
| `fw/` | The firmware (libopencm3, `make`) |
| `tools/` | `tsx-upg.py` and `tsx-ledbar-flash` |
| `panel/` | The install, uninstall and post-upgrade scripts of the package |
| `tests/test_upg.py` | Tests of the container tool |
| `tests/test_flash.py` | Tests of `tsx-ledbar-flash` with a fake bootloader: records, packets, blocks, a bootloader that does not take a packet |
| `tests/test_panel.py` | Tests of `tsx-ledbar-fw-install --check`, `--recover-image` and `--recover` with a fake sysfs tree, a fake flasher and a fake `rc-service` |
| `tests/test_ledmap.py` | Checks the LED maps in the firmware, in the test and in `docs/leds.md`: the default map, the map `outputs`, the names |
| `tests/test_fw_host.py` | Builds and runs the C tests in `tests/host/` with the host compiler: firmware files with fake registers (`tests/host/fake/`). The I2C test has a model of the bus and the chips: absent chips, a chip that needs a reset pulse, a late driver supply, SDA held low after a reset pulse or for good, and a shorted reset line |
| `qemu/run-test.py ELF` | Runs the QEMU build on the netduino2 machine. Checks the console, the LED engine, the start guard, `LEDMAP`, `I2CSCAN`, a bar with a subset of the LED chips, and a start with each variant value. The QEMU build has the command `TEST` for the guard events and a USB time of 20 s. `TEST CHIPS MASK` sets the chips that answer (bit 0 red, bit 1 green, bit 2 blue) and starts the chips again. The QEMU build reads the variant value from RAM, because the machine has no GPIO model |
| `qemu/test-leds.py ELF` | QEMU tests of the LEDs: maps (the default map, `LEDMAP`, a start with another variant value), pattern, power limits, dimming, effects, group PWM rules |
| `docs/` | [LED control](docs/leds.md) and the [update protocol](docs/update-protocol.md) |
