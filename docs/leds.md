# LED control

The console (USB interface 0) is the only path for the LED features on this page. The Cresnet joins and the kernel driver work as with the stock firmware.

## LED names and the map

The bar has 16 RGB LEDs, 8 on each side. Each LED has one output on each of
the red, green and blue TLC59116 chips. Output n of the three chips drives
the same LED. A LED map gives the output of each LED. The table below is
the map of the TSW-1060-LB. It is the default map. Another LED bar model
can have a different map: see
[Board variant and LED map](#board-variant-and-led-map). The console names
a LED by its position:

- `R1` to `R8`: the right side, from top to bottom.
- `L1` to `L8`: the left side, from top to bottom.
- The index `0` to `15`: `R1` to `R8` are 0 to 7, `L1` to `L8` are 8 to 15.

| Index | Name | Output |
| --- | --- | --- |
| 0 | R1 | 15 |
| 1 | R2 | 6 |
| 2 | R3 | 0 |
| 3 | R4 | 1 |
| 4 | R5 | 2 |
| 5 | R6 | 3 |
| 6 | R7 | 4 |
| 7 | R8 | 5 |
| 8 | L1 | 14 |
| 9 | L2 | 13 |
| 10 | L3 | 12 |
| 11 | L4 | 11 |
| 12 | L5 | 10 |
| 13 | L6 | 9 |
| 14 | L7 | 8 |
| 15 | L8 | 7 |

The entry `TSW-1060-LB` in the table `ledmaps` in `fw/src/ledmap.c` holds
this map. `tests/test_ledmap.py` checks that the firmware table and this
table agree.

A LED selection (`LEDS` below) is one of:

- one LED: `R3`, `L1`, `5`
- a range by index: `R1-R4`, `8-11`, `R7-L2` (indexes 6 to 9)
- a side: `R` or `L`
- `ALL`

The ring is the order of the LEDs around the bar, clockwise seen from the
front: `R1` to `R8` down the right side, then `L8` to `L1` up the left
side, then `R1` again. The table `ring_led` in `fw/src/leds.c` holds this
order. `FX SPECTRUM` uses it.

## Board variant and LED map

The bar cannot tell its model. So the panel tells the bar which LED map to
use. The firmware has these maps:

| Map | Use | Tested |
| --- | --- | --- |
| `TSW-1060-LB` | The TSW-1060-LB. This is the default map | yes |
| `outputs` | LED index n is output n. Use it to find the map of another bar model | no |

The firmware selects the map in this order:

1. At start, the firmware uses the default map, `TSW-1060-LB`.
2. The panel service `tsx-ledbard` sends `LEDMAP NAME PANEL` after each
   plug-in and after each start of the bar, before the first color. It
   selects the map from the panel model or from its configuration. Without
   a map for the panel model, it sends `LEDMAP DEFAULT`.
3. A user can send `LEDMAP NAME` on the console at any time.

The firmware does not save the map. The next start (`REBOOT`, a reset or a
power cycle) uses the default map again. The panel service then sends the
map again.

### Board variant pins

The controller board has three variant pins: PB13, PB14 and PB15.
Resistors on the board set their levels. At start the firmware sets the
three pins as inputs with no pull-up or pull-down and reads them once. The
stock firmware does the same. PB13 is bit 0, PB14 is bit 1 and PB15 is
bit 2. So the value is 0 to 7.

The value does not select the map, and it does not change the behavior of
the firmware. `STATUS` shows it for information. The TSW-1060-LB reads
value 1. The stock firmware has one name for all LED bars, and it uses
that name for value 1. So the value cannot tell one LED bar model from
another. In the stock firmware, value 0 is a sign board with no TLC59116
chips, and the values 2 to 7 are `TS-ROOM` boards.

### Read the map

The last line of `STATUS` shows the value and the map, for example:

```
variant 1 map TSW-1060-LB panel
```

The words are the value of the variant pins, the map in use, and the
source of the map:

| Source | The map came from |
| --- | --- |
| `default` | The start of the firmware, or `LEDMAP DEFAULT` |
| `panel` | `LEDMAP NAME PANEL`, from the panel service |
| `console` | `LEDMAP NAME`, from a user |

`LEDMAP` with no argument shows the same line and the list of maps.
`tsx-ledbar-flash info` shows the line after `board`.

### Select a map

| Command | Action |
| --- | --- |
| `LEDMAP` | Show the map line and the list of maps |
| `LEDMAP NAME` | Use the map NAME until the next start. The source is `console` |
| `LEDMAP NAME PANEL` | Use the map NAME until the next start. The source is `panel` |
| `LEDMAP DEFAULT` | Use the default map. The source is `default` |

The map names are not case-sensitive. A name with no map changes nothing.
The answer is then `no LED map NAME` and the list of maps.

The new map applies at the next tick of the LED engine. The LED pattern
and a running effect stay. They show on the outputs of the new map. The
power limits do not depend on the map.

### Add a map for another bar model

Add a map only for a bar model that you measured and tested. Find and
check the map on that bar with the steps below. Use the name of the bar model as the name of the map.

1. Load this firmware into the bar.
2. Run `LEDMAP outputs`.
3. Run `LED SET ALL 0 0 0`.
4. For each output n from 0 to 15, run `LED SET n 100 0 0`. Write down
   the position of the red LED. Then run `LED SET n 0 0 0`.
5. Write the 16 outputs in the order of their positions: the right side
   from top to bottom, then the left side from top to bottom.
6. Add one entry to `ledmaps` in `fw/src/ledmap.c`:
   `{ "MODEL", { the 16 outputs } }`. Add it after the first entry. The
   first entry is the default map.
7. Add the same map to `MAPS` in `tests/test_ledmap.py`. Add a row to the
   table of maps above.
8. Run `tests/test_ledmap.py`. It checks that each output is in the map
   once, and that the firmware, the test and this page agree.
9. Build the firmware and load it into the bar.
10. Run `LEDMAP MODEL`.
11. Run `LED SET R1 100 0 0`. Make sure that the top LED on the right side
    lights.
12. Run `LED SET L8 0 0 100`. Make sure that the bottom LED on the left
    side lights.
13. Run `LED CLEAR`.

To use the map on a panel, set `LEDMAP=MODEL` in `/etc/tsx/ledbar.conf`,
or add the panel model to `tsx-ledbard`.

## Base color, pattern and effects

The bar shows a base color. An effect can run on top of the base.

- The base is the host color (Cresnet joins, the same color on all 16
  LEDs) or the LED pattern (a level for each LED and color).
- The first `LED SET` or `LED SIDE` copies the steady host color into the
  pattern. Then it changes the selected LEDs. The other LEDs keep their
  color.
- `LED CLEAR` drops the pattern. The bar shows the host color again.
- A host join drops the pattern and ends the effect. The bar ramps to the
  host color, also when the join keeps that color.
- A `LED SET`, `LED SIDE` or `LED CLEAR` command ends the effect.
- `FX OFF` ramps back to the base: the pattern when one is set, otherwise
  the host color.
- `FX SMOOTH MS` sets the ramp time for changes of the base and for
  `FX FILL` and `FX SPLIT`.

## Color and fades

A level 0 to 100 is a lightness. The engine maps it to a duty 0 to 65535
with the CIE 1976 lightness curve. The curve is linear near zero, so a
fade does not stay long at the bottom.

An effect dims a color in light, not in level. All three duties of a LED
get the same factor, so the duty ratios stay the same and the color keeps
its hue. The factor follows the lightness curve, so the fade looks even.
`BREATHE`, the top row of `FILL`, and LEVEL of `RAINBOW` and `SPECTRUM`
work this way. If each color went through the curve on its own, the
ratios would change with the level. For example, 84 38 100 would turn
green and pink near zero.

A ramp (`FX FADE`, `FX OFF`, the smooth time of `FX SMOOTH`, `FX FILL` and
`FX SPLIT`) moves each LED on a straight line between the duties of its
start and its end. The lightness of the sum of the three duties changes
linearly over the ramp time. In a fade from or to black, the three colors
of a LED go dark together.

The host joins and `LED SET` set a level for each color. So a dim host
color has other duty ratios than the same color at full level. For
example, 17 8 20 is a little greener and redder than 84 38 100.

## Power limits

The stock firmware lights all 16 LEDs with the same color. It keeps the
sum of the red, green and blue duty at or below 110 % of one full channel.
So no LED ever gets more than 110 % of one channel. This firmware keeps
that rule for each LED.

The stock commands `TLCOUTMODE COLOR NUM MODE`, `TLCGROUPMODE` and
`TLCBRIGHTNESS` write the LED driver registers directly. They would skip
both limits: LEDOUT mode 1 is fully on, and group blinking runs on PWMx
alone. This firmware refuses them. Only the LED engine and `TLCRESET`
write the chip registers.

One setting, `FX CAP PERCENT` (default 110, range 10 to 150), controls two
limits. A change of `FX CAP` raises or lowers both.

1. The per-LED limit: for each LED, the sum of its red, green and blue
   duty stays at or below 65535 x PERCENT / 100. When the sum is above
   this limit, the three duties of that LED scale by the same factor. The
   LED keeps its hue.
2. The bar cap: the sum of the duties of all 48 outputs stays at or below
   16 x 65535 x PERCENT / 100. When the sum is above the cap, every duty
   scales by the same factor.

The engine applies the per-LED limit first and the bar cap after it.
After the per-LED limit, the sum of the 48 duties is never above the bar
cap, so the bar cap is only a second guard.

With one color on the whole bar, the per-LED limit and the bar cap give
the same duties. For example, white at
100 100 100 gives 24029 on each output, at both limits.

The per-LED limit also applies to one LED alone. One white LED at
100 100 100 gets 24029 on each color, about 37 % of full, not 65535. One
red LED at 100 0 0 stays at 65535, because 65535 is below the limit
72088. Each LED has its own limit. Two LEDs do not share one.

`CHASE`, `FILL`, `BREATHE`, `RAINBOW` and `SPECTRUM` apply the per-LED
limit to their full color before they share or dim it. So the moving dot
of `CHASE` keeps the same light, and the top row of `FILL` keeps its part
of a full row. A white chase dot at 100 100 100 shows about 37 % on each
color. A `BREATHE` of 84 38 100 (174 % of one channel at full) peaks at
the limit and does not stay at the limit for a part of each period.
`SPLIT` and the ends of a ramp go through the per-LED limit LED by LED.

## Dimming

Brightness on a TLC59116 output in LEDOUT mode 3 is PWMx x GRPPWM, in
steps of 1/65025 of full brightness. Each chip has one GRPPWM and one PWMx
per output. PWMx runs at 97 kHz. GRPPWM sets the length of a 190 Hz
window that runs free on each chip. A higher GRPPWM is safe at any time.
A lower GRPPWM that comes while the window is open lets the window run to
the end of its period. A lit output then flashes too bright for up to
5 ms. So the engine writes a lower GRPPWM only together with PWMx 0.

For each chip and each tick, the engine:

1. Converts each duty d (0 to 65535) to a target t = d x 65025 / 65535,
   at least 1 for a lit output.
2. Calculates need = ceil(tmax / 255). This is the smallest GRPPWM that
   lets the brightest output reach its target with PWMx 255 or less.
3. When need is more than GRPPWM, sets GRPPWM to 1.5 x tmax / 255 (at
   most 255).
4. When GRPPWM is 4 x need or more (the steps are coarse):
   - If the chip was dark at the last write, or its light did not fall
     for 3 ticks, it writes the lower GRPPWM with PWMx 0. The chip is dark
     for this tick and lights on the next tick.
   - Else (the light falls) it keeps GRPPWM. An output below half a step
     goes dark.
5. Else keeps GRPPWM.
6. Sets PWMx to t / GRPPWM, rounded, at least 1 for a lit output (except
   in the falling case of step 4), 0 for a dark output.

A chip that goes dark this way while its light falls stays dark until
its light rises again, or stops falling for 3 ticks. It lowers GRPPWM
while it is dark. Chips that come out of the dark light on the same tick.
When an output rounds to 0 and the other colors of that LED are at their
last steps (PWMx 2 or less), the LED goes dark as a whole. So the three
colors go dark together and light together.

The result:

- `CHASE`, `SPECTRUM`, `FILL`, `SPLIT` and a steady color run on a
  constant GRPPWM after their first ticks. GRPPWM stays the same while
  the light of a chip changes inside a range of 2.
- A `BREATHE` or a fade down keeps GRPPWM on the way down. Near the
  bottom its steps are coarse, and it goes dark a little before the
  bottom.
- On the way up it starts at GRPPWM 1 with steps of 1/65025 and raises
  GRPPWM as the light grows.
- A chip with no lit output gets PWMx 0, and one tick later GRPPWM 1.

The engine keeps the last register values it wrote. When a tick changes
them, one I2C transfer writes the registers from the first changed one to
the last changed one. A tick with no change writes nothing.
`TLCREGS COLOR` reads the registers from the chip and shows how many
GRPPWM changes the engine wrote since the start.

## LED commands

| Command | Action |
| --- | --- |
| `LED SET LEDS R G B` | set the pattern level (0 to 100) of the selected LEDs |
| `LED SIDE R\|L R G B` | the same for one side |
| `LED GET [LEDS]` | show each LED: index, name, output, base level, duty and PWMx of each color |
| `LED CLEAR` | drop the pattern, show the host color |
| `FX CHASE R G B MS` | a dot runs down both sides in step, MS for one run from top to bottom |
| `FX FILL R G B PERCENT` | a level bar from the bottom up on both sides, PERCENT 0 to 100 |
| `FX SPECTRUM MS [LEVEL] [RING\|ROWS]` | the hue circle around the bar (`RING`, the default) or along the 8 rows of each side (`ROWS`), one turn in MS |
| `FX SPLIT R G B R G B` | the first color on the right side, the second on the left side |
| `FX FREEZE ON\|OFF` | stop or start the clock of the effect and of a ramp, so the bar holds its current color |
| `FX STEP MS` | move the clock of the effect forward by MS, also while it is stopped |

The first line of `LED GET` shows the base (`host` or `pattern`), the
effect and the GRPPWM value of each chip. `STATUS` shows the base in the
line `leds host` or `leds pattern`. In `STATUS`, `duty` is the highest LED
duty of each color. `cap` in `STATUS` is the `FX CAP` value of both
limits.

The word `limit` shows when the per-LED limit is active. It is at the end
of the first line of `LED GET` and after the base in `STATUS` (for example
`leds pattern limit`) when the limit scales one LED or more. It is at the
end of the line of each LED that the limit scales. During `CHASE`,
`FILL`, `BREATHE`, `RAINBOW`, `SPECTRUM` and a ramp, it marks each lit LED
when the limit scales the full color of that LED.

Effect details:

- `CHASE` shares the light of the dot between two rows by duty, not by
  level, so the light of the dot stays the same while it moves. After the
  bottom row the dot starts again at the top.
- `FILL` lights PERCENT x 8 / 100 rows from the bottom. The top row of the
  bar gets the part of a row that is left, as a part of the lightness,
  dimmed in light.
- `SPECTRUM RING` (the default) puts the 16 LEDs 1/16 of the hue circle
  apart in ring order. The colors turn clockwise seen from the front, one
  LED in MS / 16, one full turn in MS.
- `SPECTRUM ROWS` puts the rows 1/8 of the hue circle apart. Both sides
  show the same colors, and the colors move down.
- For `SPECTRUM` and `RAINBOW`, LEVEL (default 100) is the lightness of
  the color. The engine makes the color at full level, applies the
  per-LED limit, and dims it in light.
  LEVEL and the mode can each be left out: `FX SPECTRUM 4000 ROWS` is
  valid.

`CAPS` shows the words `leds16 chase fill spectrum split`, and `ledmap`
for the `LEDMAP` command.
