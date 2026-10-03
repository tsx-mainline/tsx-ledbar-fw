#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of the 16 LEDs (0.1.3): the LED map, the LED pattern commands,
the per-LED limit and the cap over all LEDs, the per-output dimming
values, the zone effects, and the one-color behavior of 0.1.2.

  qemu/test-leds.py NEW.elf [REF.elf]

NEW.elf is the QEMU build under test. With REF.elf (the QEMU build of
0.1.2), the script also sets the same host colors on both builds and
compares the duties and the PWM and group values.

TRACE PIX prints one line "pix MS" with the 48 duties (R1 red, green,
blue, R2 red, ... L8 blue) after each engine tick. TRACE REGS also prints
one line "reg MS" with PWM0..15 and GRPPWM of the red, green and blue
chip, as the engine wrote them in that tick.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('run_test', os.path.join(HERE, 'run-test.py'))
rt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rt)
Machine, leds_in = rt.Machine, rt.leds_in

NAMES = ['R%d' % n for n in range(1, 9)] + ['L%d' % n for n in range(1, 9)]
OUTPUTS = [15, 6, 0, 1, 2, 3, 4, 5, 14, 13, 12, 11, 10, 9, 8, 7]
RING = list(range(8)) + list(range(15, 7, -1))  # R1..R8, L8..L1: clockwise from the front
CIE = [0, 73, 145, 218, 290, 363, 435, 508, 580, 656,
       738, 826, 922, 1024, 1134, 1251, 1376, 1509, 1650, 1800,
       1959, 2127, 2304, 2491, 2687, 2894, 3111, 3338, 3576, 3826,
       4087, 4359, 4643, 4940, 5248, 5569, 5903, 6251, 6611, 6985,
       7373, 7775, 8192, 8623, 9069, 9530, 10006, 10498, 11006, 11530,
       12071, 12628, 13202, 13793, 14401, 15027, 15671, 16333, 17014, 17713,
       18431, 19168, 19924, 20700, 21497, 22313, 23149, 24007, 24885, 25784,
       26705, 27648, 28612, 29598, 30607, 31639, 32694, 33771, 34872, 35997,
       37146, 38319, 39516, 40738, 41986, 43258, 44555, 45879, 47228, 48603,
       50005, 51434, 52890, 54372, 55883, 57421, 58987, 60581, 62203, 63855,
       65535]

check = rt.check


def led_limit(percent=110):
    return percent * 65535 // 100


def cap_total(percent=110):
    return 16 * led_limit(percent)


def limit_model(d, percent=110):
    """the per-LED limit: one factor for the three duties of one LED"""
    lim, s = led_limit(percent), sum(d)
    return tuple(x * lim // s for x in d) if s > lim else tuple(d)


def bar_cap_model(px, percent=110):
    """the cap over all LEDs alone: one factor for all 48 duties"""
    cap, s = cap_total(percent), sum(map(sum, px))
    return [tuple(x * cap // s for x in p) for p in px] if s > cap else list(px)


def level_of(d):
    """the level 0..100 (a float) of a duty, the inverse of the CIE table"""
    for i in range(100):
        if d < CIE[i + 1]:
            return i + (d - CIE[i]) / (CIE[i + 1] - CIE[i])
    return 100.0


def hue_est(duty):
    """the hue 0..1535 of one LED (6 x 256, as the firmware), None when dark"""
    r, g, b = (level_of(x) for x in duty)
    mx, mn = max(r, g, b), min(r, g, b)
    if mx == 0 or mx == mn:
        return None
    if mx == r:
        h = (g - b) / (mx - mn)
    elif mx == g:
        h = 2 + (b - r) / (mx - mn)
    else:
        h = 4 + (r - g) / (mx - mn)
    return h * 256 % 1536


def hue_near(a, b, tol):
    return a is not None and b is not None and min((a - b) % 1536, (b - a) % 1536) <= tol


def pix_in(text):
    """complete "pix" lines: a list of (ms, [(r, g, b) x 16])"""
    out = []
    for m in re.finditer(r'pix (\d+)((?: \d+){48})\r?\n', text):
        v = [int(x) for x in m.group(2).split()]
        out.append((int(m.group(1)), [tuple(v[3 * i:3 * i + 3]) for i in range(16)]))
    return out


def last_pix(m, line, settle=0.6):
    p = pix_in(m.cmd(line, settle))
    return p[-1][1] if p else None


def led_get(m, arg=''):
    """LED GET: header (mode, fx, grp) and a dict index -> row"""
    out = m.cmd(('LED GET ' + arg).strip())
    head = re.search(r'leds (\w+) fx (\w+) grp (\d+) (\d+) (\d+)', out)
    rows = {}
    for r in re.finditer(r'(\d+) ([RL][1-8]) out (\d+) level (\d+) (\d+) (\d+) duty (\d+) (\d+) (\d+) '
                         r'pwm (\d+) (\d+) (\d+)( limit)?\r?\n', out):
        v = [int(x) for x in r.groups()[2:12]]
        rows[int(r.group(1))] = {'name': r.group(2), 'out': v[0], 'level': tuple(v[1:4]),
                                 'duty': tuple(v[4:7]), 'pwm': tuple(v[7:10]), 'limit': bool(r.group(13))}
    if head:
        return head.group(1), head.group(2), tuple(int(x) for x in head.groups()[2:]), rows
    return None, None, None, rows


GRP_LOWER = 4   # the GRPPWM rules of leds.c: raise to 1.5 x need, lower by 1/8 per tick from 4 x need


def targets(duties):
    """the targets t (1/65025) of one chip by output, duties by LED index"""
    t = [0] * 16
    for i, d in enumerate(duties):
        if d:
            t[OUTPUTS[i]] = max(1, (d * 65025 + 32767) // 65535)
    return t


def pwm_for(t, g):
    return [0 if x == 0 else max(1, min(255, (x + g // 2) // g)) for x in t]


def grp_for(need):
    return min(255, (3 * need + 1) // 2)


def chip_check(duties, pwm, g, settled=True):
    """'' when PWMx (by output) and GRPPWM g of one chip follow the rules
    for these duties (by LED index), else the reason. A tick that is not
    settled can still lower GRPPWM in steps of 1/8, so GRPPWM can be
    4 x need or more."""
    t = targets(duties)
    tmax = max(t)
    if tmax == 0:
        return '' if not any(pwm) else 'dark chip with PWMx %s' % pwm
    need = (tmax + 254) // 255
    if g < need or (settled and g >= GRP_LOWER * need):
        return 'grp %d outside %d..%d' % (g, need, GRP_LOWER * need - 1)
    want = pwm_for(t, g)
    return '' if list(pwm) == want else 'pwm %s, want %s' % (list(pwm), want)


def limit_flags(m):
    """the limit flag of LED GET (header) and of STATUS, and the limited LEDs"""
    out = m.cmd('LED GET')
    head = re.search(r'leds \w+ fx \w+ grp \d+ \d+ \d+( limit)?\r?\n', out)
    rows = re.findall(r'^(\d+) [RL][1-8] out .* limit\r?$', out, re.M)
    status = re.search(r'leds \w+( limit)? \(LED GET', m.cmd('STATUS'))
    return (bool(head and head.group(1)), bool(status and status.group(1)), sorted(int(x) for x in rows))


def regs_match_model(m, what):
    mode, fx, grp, rows = led_get(m)
    ok = len(rows) == 16 and grp is not None
    detail = ''
    for c in range(3):
        if not ok:
            break
        duties = [rows[i]['duty'][c] for i in range(16)]
        pwm = [0] * 16
        for i in range(16):
            pwm[OUTPUTS[i]] = rows[i]['pwm'][c]
        detail = chip_check(duties, pwm, grp[c])
        if detail:
            ok, detail = False, 'color %d: %s' % (c, detail)
    check(ok, '%s: PWMx and GRPPWM follow the rules %s' % (what, detail))
    return rows, grp


def all_off(m):
    for c in ('RED', 'GREEN', 'BLUE'):
        m.cmd('LED %s CONTROL 0' % c, 0.1)
    m.cmd('FX OFF', 0.1)
    m.cmd('LED CLEAR', 0.4)


def test_features(m):
    out = m.cmd('VER')
    check('[v0.1.3]' in out, 'VER is 0.1.3: %s' % out.strip())
    out = m.cmd('CAPS')
    check(all(w in out.split() for w in ('leds16', 'chase', 'fill', 'spectrum', 'split')),
          'CAPS lists leds16 chase fill spectrum split: %s' % out.strip())

    # the map
    mode, fx, grp, rows = led_get(m)
    check([rows.get(i, {}).get('name') for i in range(16)] == NAMES,
          'LED GET: index 0..15 is R1..R8 then L1..L8')
    check([rows.get(i, {}).get('out') for i in range(16)] == OUTPUTS,
          'LED GET: outputs %s' % [rows.get(i, {}).get('out') for i in range(16)])
    _, _, _, rows = led_get(m, 'R3')
    check(list(rows) == [2] and rows[2]['out'] == 0, 'LED GET R3: index 2, output 0')
    _, _, _, rows = led_get(m, 'L2-L1')
    check(list(rows) == [8, 9] and rows[9]['out'] == 13, 'LED GET L2-L1: L1 and L2, L2 on output 13')
    check('usage' in m.cmd('LED SET R9 1 2 3'), 'LED SET R9 is refused')
    check('usage' in m.cmd('LED SET 16 1 2 3'), 'LED SET 16 is refused')
    check('usage' in m.cmd('LED SIDE ALL 1 2 3'), 'LED SIDE takes only R or L')
    check('usage' in m.cmd('LED SET R1 101 0 0'), 'LED SET refuses a level above 100')

    m.cmd('TRACE PIX')
    # one LED alone
    px = last_pix(m, 'LED SET R3 100 0 0')
    want = [(0, 0, 0)] * 16
    want[2] = (65535, 0, 0)
    check(px == want, 'LED SET R3 100 0 0 lights only R3 red: %s' % (px and px[:4]))
    mode, fx, grp, rows = led_get(m)
    check(mode == 'pattern' and rows[2]['level'] == (100, 0, 0), 'LED GET shows the pattern and R3 at 100 0 0')
    check(grp and grp[0] == 255 and rows[2]['pwm'][0] == 255 and
          all(rows[i]['pwm'][0] == 0 for i in range(16) if i != 2),
          'red chip: output 0 PWM 255, the other outputs PWM 0, GRPPWM 255')
    regs_match_model(m, 'one LED')

    # fine steps for the darkest LED
    m.cmd('LED SET ALL 0 0 0', 0.3)
    px = last_pix(m, 'LED SET R1 1 0 0')
    check(px and px[0] == (73, 0, 0), 'R1 at level 1 gives duty 73: %s' % (px and px[0],))
    _, _, grp, rows = led_get(m)
    check(grp and grp[0] == 1 and rows[0]['pwm'][0] == 72, 'a single dim LED: GRPPWM 1, PWM 72 (step 1/65025)')
    m.cmd('LED SET R2 2 0 0', 0.4)
    _, _, grp, rows = led_get(m)
    check(grp and grp[0] == 1 and rows[0]['pwm'][0] == 72 and rows[1]['pwm'][0] == 144,
          'two dim LEDs keep GRPPWM 1: PWM 72 and 144')
    m.cmd('LED SET R1 100 0 0', 0.4)
    m.cmd('LED SET R2 10 0 0', 0.4)
    rows, grp = regs_match_model(m, 'bright R1 and R2 at level 10')
    check(rows and rows[1]['duty'][0] == 738 and grp[0] == 255 and rows[1]['pwm'][0] == 3,
          'R2 at level 10 next to R1 at 100: duty 738, PWM 3 x GRPPWM 255')
    m.cmd('LED SET ALL 0 0 0', 0.3)
    m.cmd('LED SET R4-R6 0 30 60', 0.3)
    m.cmd('LED SET L5 0 7 90', 0.4)
    regs_match_model(m, 'a mixed pattern')

    # ranges and sides
    m.cmd('LED SET ALL 0 0 0', 0.3)
    m.cmd('LED SET R7-L2 0 100 0', 0.3)
    px = last_pix(m, 'LED SIDE L 0 0 100')
    want = [(0, 0, 0)] * 6 + [(0, 65535, 0)] * 2 + [(0, 0, 65535)] * 8
    check(px == want, 'LED SET R7-L2 then LED SIDE L: R7 R8 green, left side blue')
    px = last_pix(m, 'LED SET 4 100 0 0')
    check(px and px[4] == (65535, 0, 0) and px[3] == (0, 0, 0), 'LED SET 4 sets R5')

    # the cap over all LEDs
    px = last_pix(m, 'LED SET ALL 100 100 100')
    check(px and all(p == (24029, 24029, 24029) for p in px) and sum(map(sum, px)) <= cap_total(),
          'all 16 LEDs white: 24029 each, as one white bar in 0.1.2')
    m.cmd('LED SET ALL 0 0 0', 0.3)
    px = last_pix(m, 'LED SIDE R 100 100 100')
    check(px and all(p == (24029, 24029, 24029) for p in px[:8]) and all(p == (0, 0, 0) for p in px[8:]),
          'right side white: the per-LED limit gives 24029 each (sum %d, cap %d)' % (sum(map(sum, px or [])), cap_total()))
    px = last_pix(m, 'LED SIDE R 100 0 0')
    check(px and all(p == (65535, 0, 0) for p in px[:8]), 'right side red is under the cap: 65535 each')
    px = last_pix(m, 'FX CAP 40')
    s = sum(map(sum, px or []))
    check(px and s <= cap_total(40) and px[:8] == [(led_limit(40), 0, 0)] * 8,
          'FX CAP 40 scales the pattern: each LED at the limit %d, sum %d, cap %d' % (led_limit(40), s, cap_total(40)))
    m.cmd('FX CAP 110', 0.3)
    m.cmd('LED SET ALL 100 100 100', 0.2)
    m.cmd('LED SET L1 0 0 0', 0.2)
    px = last_pix(m, 'LED SET R3 100 0 100')
    s = sum(map(sum, px or []))
    want = [limit_model((CIE[100],) * 3)] * 16
    want[2], want[8] = limit_model((CIE[100], 0, CIE[100])), (0, 0, 0)
    check(px == want and s <= cap_total(),
          'a mixed pattern: each LED at the per-LED limit, sum %d under the cap %d' % (s, cap_total()))

    # the base: host color, pattern, LED CLEAR, host join, FX OFF
    all_off(m)
    m.cmd('LED RED LEVEL 50', 0.1)
    m.cmd('LED RED CONTROL 1', 0.4)
    px = last_pix(m, 'LED SET R1 0 0 100')
    check(px and px[0] == (0, 0, 65535) and all(p == (12071, 0, 0) for p in px[1:]),
          'the first LED SET copies the host color into the other LEDs')
    px = last_pix(m, 'LED CLEAR')
    check(px and all(p == (12071, 0, 0) for p in px), 'LED CLEAR shows the host color again')
    m.cmd('LED SET R1 0 0 100', 0.3)
    px = last_pix(m, 'LED RED LEVEL 60')
    out = m.cmd('STATUS')
    check(px and all(p == (18431, 0, 0) for p in px) and 'leds host' in out,
          'a host join drops the pattern: all LEDs red 60')
    m.cmd('LED SET R1 0 0 100', 0.3)
    m.cmd('FX BLINK 0 100 0 100 100', 0.5)
    px = last_pix(m, 'FX OFF')
    check(px and px[0] == (0, 0, 65535) and all(p == (18431, 0, 0) for p in px[1:]),
          'FX OFF goes back to the pattern')
    m.cmd('FX RAINBOW 1000', 0.4)
    m.cmd('LED SET R2 0 100 0', 0.4)
    check('fx off' in m.cmd('FX'), 'a LED command ends an effect')

    # the effects
    all_off(m)
    out = m.cmd('FX CHASE 100 0 0 800', 2.0)
    check('fx chase' in out, 'FX CHASE starts')
    pix = pix_in(out)[5:]
    sides = all(p[:8] == p[8:] for _, p in pix)
    lit_ok = True
    light = []
    centers = []
    for _, p in pix:
        lit = [r for r in range(8) if p[r][0]]
        if len(lit) > 2 or (len(lit) == 2 and (lit[1] - lit[0]) % 8 not in (1, 7)):
            lit_ok = False
        light.append(sum(p[r][0] for r in range(8)))
        centers.append(max(range(8), key=lambda r: p[r][0]))
    moves = [(b - a) % 8 for a, b in zip(centers, centers[1:])]
    check(len(pix) > 50 and sides, 'CHASE: both sides in step (%d ticks)' % len(pix))
    check(pix and lit_ok, 'CHASE: one dot, at most two next rows lit')
    check(light and min(light) > 0.97 * 65535 and max(light) <= 65535,
          'CHASE: the light of the dot stays the same (%s..%s)' % (min(light or [0]), max(light or [0])))
    check(moves and set(moves) <= {0, 1} and len(set(centers)) == 8, 'CHASE: the dot runs down over all 8 rows')

    out = m.cmd('FX CHASE 100 100 100 800', 2.0)
    pix = pix_in(out.split('fx chase', 1)[-1])[5:]  # the red dot runs until the command
    full = limit_model((CIE[100],) * 3)[0]
    light = [sum(p[r][c] for r in range(8)) for _, p in pix for c in range(3)]
    check(light and min(light) > 0.97 * full and max(light) <= full,
          'CHASE white 100: the dot keeps %d per color, %d %% of full (%s..%s)' % (
              full, round(full * 100 / 65535), min(light or [0]), max(light or [0])))
    check(pix and all(sum(q) <= led_limit() for _, p in pix for q in p), 'CHASE white: no LED above the per-LED limit')
    flags = limit_flags(m)
    check(flags[0] and flags[1], 'CHASE white: LED GET and STATUS show the limit')

    px = last_pix(m, 'FX FILL 0 100 0 50')
    want = [(0, 0, 0)] * 4 + [(0, 65535, 0)] * 4
    check(px == want + want, 'FILL 50: the bottom 4 rows on both sides')
    px = last_pix(m, 'FX FILL 0 100 0 55')
    check(px and px[3][1] == CIE[39] + (CIE[40] - CIE[39]) * 216 // 256 and px[4:8] == [(0, 65535, 0)] * 4 and px[2] == (0, 0, 0),
          'FILL 55: 4 rows and a part of row 4 (%s)' % (px and px[3],))
    px = last_pix(m, 'FX FILL 0 100 0 100')
    check(px == [(0, 65535, 0)] * 16, 'FILL 100: all rows')
    px = last_pix(m, 'FX FILL 0 100 0 0')
    check(px == [(0, 0, 0)] * 16, 'FILL 0: all rows off')
    m.cmd('FX SMOOTH 300', 0.2)
    out = m.cmd('FX FILL 0 100 0 100', 1.0)
    steps = len(set(p[0][1] for _, p in pix_in(out)))
    check(steps >= 10, 'FILL ramps over the smooth time (%d steps)' % steps)
    m.cmd('FX SMOOTH 0', 0.2)

    out = m.cmd('FX SPECTRUM 1600 ROWS', 2.0)
    pix = pix_in(out.split('fx spectrum', 1)[-1])[5:]
    check(len(pix) > 50 and all(p[:8] == p[8:] for _, p in pix), 'SPECTRUM ROWS: both sides the same')
    check(pix and min(len(set(p[:8])) for _, p in pix) == 8, 'SPECTRUM ROWS: 8 different colors on a side')
    check(pix and all(sum(map(sum, p)) <= cap_total() for _, p in pix), 'SPECTRUM ROWS: under the cap')
    check(pix and all(sum(q) <= led_limit() for _, p in pix for q in p), 'SPECTRUM ROWS: no LED above the per-LED limit')
    down = up = 0
    for i, (t, p) in enumerate(pix[:len(pix) // 2]):
        later = min(pix, key=lambda s: abs(s[0] - (t + 200)))
        if abs(later[0] - (t + 200)) > 10:
            continue
        d = lambda a, b: max(abs(x - y) for x, y in zip(a, b))
        down += d(later[1][1], p[0]) < 4000
        up += d(later[1][0], p[1]) < 4000
    check(down > 20 and up < down // 4, 'SPECTRUM ROWS: the colors move down one row in 200 ms (%d down, %d up)' % (down, up))

    # SPECTRUM RING: the hue circle around the bar, clockwise from the front
    check('usage' in m.cmd('FX SPECTRUM 1600 40 UP'), 'FX SPECTRUM refuses a mode other than RING or ROWS')
    out = m.cmd('FX SPECTRUM 1600 40', 2.0)
    check('fx spectrum' in out, 'FX SPECTRUM MS LEVEL starts, RING is the default')
    pix = pix_in(out.split('fx spectrum', 1)[-1])[5:]
    steps = []
    for _, p in pix:
        h = [hue_est(p[i]) for i in RING]
        steps += [(h[(k + 1) % 16] - h[k]) % 1536 if None not in h else -1 for k in range(16)]
    check(len(pix) > 50 and steps and all(abs(x - 96) <= 12 for x in steps),
          'SPECTRUM RING: in ring order each LED is 1/16 of the hue circle after the LED before (%d..%d of 1536)' % (
              round(min(steps or [0])), round(max(steps or [0]))))
    cw = ccw = 0
    for t, p in pix[:len(pix) // 2]:
        later = min(pix, key=lambda q: abs(q[0] - (t + 100)))
        if abs(later[0] - (t + 100)) > 5:
            continue
        h0 = [hue_est(p[i]) for i in RING]
        h1 = [hue_est(later[1][i]) for i in RING]
        cw += all(hue_near(h1[(k + 1) % 16], h0[k], 24) for k in range(16))
        ccw += all(hue_near(h1[(k - 1) % 16], h0[k], 24) for k in range(16))
    check(cw > 20 and ccw == 0,
          'SPECTRUM RING: the colors turn clockwise, one LED in 100 ms (%d clockwise, %d counterclockwise)' % (cw, ccw))
    out = m.cmd('FX SPECTRUM 1600 RING', 1.5)
    pix = pix_in(out.split('fx spectrum', 1)[-1])[5:]
    check(len(pix) > 50 and all(len(set(p)) == 16 for _, p in pix), 'SPECTRUM RING at 100: 16 different colors')
    check(pix and all(sum(q) <= led_limit() for _, p in pix for q in p) and
          all(sum(map(sum, p)) <= cap_total() for _, p in pix),
          'SPECTRUM RING at 100: no LED above the per-LED limit, the bar under the cap')
    flags = limit_flags(m)
    check(flags[0] and flags[1], 'SPECTRUM RING at 100: LED GET and STATUS show the limit')
    out = m.cmd('FX CAP 50', 1.0)
    pix = pix_in(out.split('fx spectrum', 1)[-1])[5:]
    check(len(pix) > 20 and all(sum(q) <= led_limit(50) for _, p in pix for q in p),
          'SPECTRUM RING with FX CAP 50: no LED above %d' % led_limit(50))
    m.cmd('FX CAP 110', 0.2)

    px = last_pix(m, 'FX SPLIT 100 0 0 0 0 100')
    check(px == [(65535, 0, 0)] * 8 + [(0, 0, 65535)] * 8, 'SPLIT: right side red, left side blue')
    px = last_pix(m, 'FX SPLIT 100 100 100 100 0 0')
    check(px == [(24029, 24029, 24029)] * 8 + [(65535, 0, 0)] * 8,
          'SPLIT white and red: the right side at the per-LED limit, the left side red at full')

    px = last_pix(m, 'FX FILL 100 100 100 55')
    check(px and px[4:8] == [px[7]] * 4 and all(abs(x - 24029) <= 300 for x in px[7]) and
          px[3][0] == px[3][1] == px[3][2] and 0 < px[3][0] < 0.9 * px[7][0] and px[2] == (0, 0, 0),
          'FILL white 55: full rows at the per-LED limit %s, row 4 keeps its part %s' % (px and px[7], px and px[3]))

    # a host join ends each new effect and returns all LEDs to the host color
    m.cmd('LED RED CONTROL 1', 0.2)
    for fx in ('CHASE 0 0 100 500', 'FILL 0 0 100 30', 'SPECTRUM 800', 'SPLIT 0 100 0 100 0 0'):
        m.cmd('FX ' + fx, 0.4)
        px = last_pix(m, 'LED RED LEVEL 20')
        check(px and all(p == (CIE[20], 0, 0) for p in px) and 'fx off' in m.cmd('FX'),
              'a host join ends FX %s and shows the host color' % fx.split()[0])

    # TLCRESET: the engine writes the registers again
    m.cmd('LED RED LEVEL 100', 0.4)
    m.cmd('TLCRESET', 0.6)
    _, _, grp, rows = led_get(m)
    check(grp and grp[0] == 255 and rows[0]['pwm'][0] == 255, 'after TLCRESET the engine writes the red chip again')
    m.cmd('TRACE OFF')


def test_led_limit(m):
    """the per-LED limit: no LED gets more than FX CAP percent of one channel"""
    m.cmd('TRACE PIX')
    all_off(m)
    m.cmd('LED SET ALL 0 0 0', 0.3)
    flags = limit_flags(m)
    check(flags == (False, False, []), 'all LEDs dark: no limit flag in LED GET and STATUS %s' % (flags,))

    # one white LED at 100
    px = last_pix(m, 'LED SET R3 100 100 100')
    want = [(0, 0, 0)] * 16
    want[2] = limit_model((CIE[100],) * 3)
    check(px == want and want[2] == (24029, 24029, 24029),
          'one white LED at 100: 24029 per color, sum %d at the limit %d (%s)' % (sum(want[2]), led_limit(), px and px[2]))
    flags = limit_flags(m)
    check(flags == (True, True, [2]), 'one white LED: LED GET and STATUS show the limit on R3 %s' % (flags,))

    # two LEDs, only one above the limit
    m.cmd('LED SET R3 0 0 0', 0.3)
    m.cmd('LED SET R1 100 0 0', 0.3)
    px = last_pix(m, 'LED SET L4 100 60 0')
    want = [(0, 0, 0)] * 16
    want[0] = (CIE[100], 0, 0)
    want[11] = limit_model((CIE[100], CIE[60], 0))
    check(px == want, 'two LEDs: R1 red 100 stays at %s, L4 at 100 60 0 scales to %s (%s %s)' % (
        want[0], want[11], px and px[0], px and px[11]))
    check(px and abs(px[11][1] * CIE[100] - px[11][0] * CIE[60]) <= CIE[100],
          'L4 keeps its hue: green to red %d to %d' % (px and px[11][1], px and px[11][0]))
    flags = limit_flags(m)
    check(flags == (True, True, [11]), 'two LEDs: only L4 shows the limit %s' % (flags,))

    # FX CAP 150 raises the limit, FX CAP 50 lowers it
    px = last_pix(m, 'FX CAP 150')
    check(px == [(CIE[100], 0, 0)] + [(0, 0, 0)] * 10 + [(CIE[100], CIE[60], 0)] + [(0, 0, 0)] * 4,
          'FX CAP 150: L4 is under the limit %d and gets its full duty %s' % (led_limit(150), px and px[11]))
    check(limit_flags(m) == (False, False, []), 'FX CAP 150: no limit flag')
    px = last_pix(m, 'LED SET R3 100 100 100')
    check(px and px[2] == limit_model((CIE[100],) * 3, 150) == (32767, 32767, 32767),
          'FX CAP 150: one white LED at 100 gets 32767 per color (%s)' % (px and px[2],))
    px = last_pix(m, 'FX CAP 50')
    want = [(0, 0, 0)] * 16
    want[0] = limit_model((CIE[100], 0, 0), 50)
    want[2] = limit_model((CIE[100],) * 3, 50)
    want[11] = limit_model((CIE[100], CIE[60], 0), 50)
    check(px == want and want[0] == (led_limit(50), 0, 0),
          'FX CAP 50: R1 red scales to %s, R3 white to %s, L4 to %s' % (want[0], want[2], want[11]))
    check(limit_flags(m) == (True, True, [0, 2, 11]), 'FX CAP 50: R1, R3 and L4 show the limit')
    m.cmd('FX CAP 110', 0.3)
    m.cmd('TRACE OFF')


def test_one_color_limit(m):
    """one color on the whole bar: the per-LED limit and the cap over all
    LEDs give the same duties, the duties of 0.1.2"""
    m.cmd('TRACE PIX')
    all_off(m)
    for cap in (110, 150, 50):
        m.cmd('FX CAP %d' % cap, 0.2)
        for rgb in ((100, 100, 100), (100, 100, 0), (100, 50, 20), (30, 30, 30), (0, 0, 100)):
            px = last_pix(m, 'LED SET ALL %d %d %d' % rgb)
            duty = tuple(CIE[x] for x in rgb)
            check(px == bar_cap_model([duty] * 16, cap) == [limit_model(duty, cap)] * 16,
                  'one color %d %d %d at FX CAP %d: %s on all LEDs, as the cap over all LEDs alone' % (
                      rgb + (cap, px and px[0])))
    m.cmd('FX CAP 110', 0.2)
    m.cmd('TRACE OFF')


HOST_STATES = [
    ('RED', 100, 1), ('GREEN', 100, 1), ('BLUE', 100, 1), ('RED', 1, 1), ('GREEN', 37, 1),
    ('BLUE', 0, 1), ('RED', 63, 1), ('GREEN', 5, 1), ('BLUE', 80, 0), ('RED', 100, 0),
]


def one_color_states(m):
    """set host colors one by one, return the STATUS duty and the register check after each"""
    res = []
    for c in ('RED', 'GREEN', 'BLUE'):
        m.cmd('LED %s CONTROL 0' % c, 0.1)
    m.cmd('FX OFF', 0.3)
    for cap in (110, 60):
        m.cmd('FX CAP %d' % cap, 0.2)
        for color, level, on in HOST_STATES:
            m.cmd('LED %s LEVEL %d' % (color, level), 0.1)
            m.cmd('LED %s CONTROL %d' % (color, on), 0.3)
            out = m.cmd('STATUS')
            d = re.search(r'duty (\d+) (\d+) (\d+)', out)
            _, _, grp, rows = led_get(m)
            if d and grp is not None and len(rows) == 16:
                bad = []
                for c in range(3):
                    pwm = [0] * 16
                    for i in range(16):
                        pwm[OUTPUTS[i]] = rows[i]['pwm'][c]
                    bad.append(chip_check([rows[i]['duty'][c] for i in range(16)], pwm, grp[c]))
                res.append((d.groups(), not any(bad)))
            else:
                res.append((None, None))
    m.cmd('FX CAP 110', 0.2)
    return res


def test_one_color(m):
    m.cmd('TRACE PIX')
    all_off(m)
    for color, level, on in HOST_STATES:
        m.cmd('LED %s LEVEL %d' % (color, level), 0.1)
        out = m.cmd('LED %s CONTROL %d' % (color, on), 0.5)
        px = pix_in(out)
        led = leds_in(out)
        same = px and led and all(p == px[-1][1][0] for p in px[-1][1]) and px[-1][1][0] == led[-1][1:]
        _, _, grp, rows = led_get(m)
        regs = grp is not None and len(rows) == 16
        for c in range(3):
            if not regs:
                break
            pwm = [0] * 16
            for i in range(16):
                pwm[OUTPUTS[i]] = rows[i]['pwm'][c]
            regs = not chip_check([rows[i]['duty'][c] for i in range(16)], pwm, grp[c])
            # one color on all outputs: the light of the chip is within half a step of the duty
            t = targets([rows[0]['duty'][c]])[OUTPUTS[0]]
            regs = regs and abs(rows[0]['pwm'][c] * grp[c] - t) <= grp[c] // 2 + 1
        check(same and regs, 'one color %s %d %d: 16 equal duties %s, PWM and group follow the rules' % (
            color, level, on, led and led[-1][1:]))
    m.cmd('TRACE OFF')


def regs_in(text):
    """complete "reg" lines: a dict ms -> [(pwm by output, grp) x 3]"""
    out = {}
    for m in re.finditer(r'reg (\d+)((?: \d+){51})\r?\n', text):
        v = [int(x) for x in m.group(2).split()]
        out[int(m.group(1))] = [(v[17 * c:17 * c + 16], v[17 * c + 16]) for c in range(3)]
    return out


def ticks_in(text):
    """the ticks with both a "pix" and a "reg" line: a list of (ms, pix, regs)"""
    regs = regs_in(text)
    return [(t, p, regs[t]) for t, p in pix_in(text) if t in regs]


def grp_safe(ticks, what):
    """the GRPPWM rules in every tick: a lit chip follows the rules, no lit
    output goes dark for a tick, and GRPPWM goes down only in steps of 1/8
    (at least 1) while it is 4 x need or more"""
    dark = bad = 0
    detail = ''
    for (_, _, r0), (t, p, r) in zip(ticks, ticks[1:]):
        for c in range(3):
            pwm, g = r[c]
            g0 = r0[c][1]
            for i in range(16):
                if p[i][c] and not pwm[OUTPUTS[i]]:
                    dark += 1
                    detail = detail or 'tick %d color %d: LED %d duty %d with PWMx 0' % (t, c, i, p[i][c])
            if not any(pwm):
                continue
            why = chip_check([q[c] for q in p], pwm, g, settled=False)
            tmax = max(targets([q[c] for q in p]))
            need = (tmax + 254) // 255
            if not why and g < g0 and any(r0[c][0]) and g0 - g > (g0 + 7) // 8:
                why = 'grp %d -> %d in one tick, more than 1/8' % (g0, g)
            if not why and g >= GRP_LOWER * need and not (g < g0 and g == g0 - (g0 + 7) // 8):
                why = 'grp %d stays at 4 x need %d or more' % (g, need)
            if why:
                bad += 1
                detail = detail or 'tick %d color %d: %s' % (t, c, why)
    check(len(ticks) > 20 and dark == 0 and bad == 0,
          '%s: PWMx and GRPPWM follow the rules, GRPPWM goes down in steps of 1/8, no dark tick (%d ticks) %s' % (
              what, len(ticks), detail))


def light(r, c, i):
    """the light of LED i, color c on the chip: PWMx x GRPPWM (1/65025)"""
    return r[c][0][OUTPUTS[i]] * r[c][1]


def ramps_smooth(ticks, what, c=0, i=0, need_switch=True):
    """on the down and the up ramp, the light of one LED on the chip moves
    in the same direction as its duty, and a GRPPWM change gives no jump"""
    wrong = jumps = switches = 0
    detail = ''
    for (t0, p0, r0), (t1, p1, r1) in zip(ticks, ticks[1:]):
        d0, d1 = p0[i][c], p1[i][c]
        l0, l1 = light(r0, c, i), light(r1, c, i)
        if (d1 < d0 and l1 > l0) or (d1 > d0 and l1 < l0) or (d1 == d0 and l1 != l0):
            wrong += 1
            detail = detail or 'tick %d: duty %d -> %d, light %d -> %d' % (t1, d0, d1, l0, l1)
        if r0[c][1] != r1[c][1] and d0 and d1:
            switches += 1
            # the change of light at a switch: the change of the target and the rounding of both ticks
            t0x, t1x = targets([d0])[OUTPUTS[0]], targets([d1])[OUTPUTS[0]]
            if abs(l1 - l0) > abs(t1x - t0x) + (r0[c][1] + r1[c][1]) // 2 + 1:
                jumps += 1
                detail = detail or 'tick %d: grp %d -> %d, light %d -> %d for target %d -> %d' % (
                    t1, r0[c][1], r1[c][1], l0, l1, t0x, t1x)
    check(len(ticks) > 20 and wrong == 0,
          '%s: the light of LED %d color %d follows its duty down and up, tick by tick %s' % (what, i, c, detail))
    check((switches > 0 or not need_switch) and jumps == 0,
          '%s: no jump of LED %d color %d at its %d GRPPWM changes %s' % (what, i, c, switches, detail))


def grp_changes(ticks, c):
    return sum(1 for a, b in zip(ticks, ticks[1:]) if a[2][c][1] != b[2][c][1])


def grp_steady(m, line, run_ms, what, settle_runs=1, runs=3, before=None):
    """run an effect for settle_runs + runs periods of run_ms; after the
    settle time GRPPWM must not change on any chip"""
    if before:
        before(m)
    out = m.cmd(line, 0.3)
    end = None
    while True:
        ticks = ticks_in(out)
        if ticks and end is None:
            end = ticks[0][0] + (settle_runs + runs) * run_ms
        if ticks and ticks[-1][0] >= end:
            break
        more = m.read_for(0.5)
        if not more:
            break
        out += more
    # the ticks after the answer of the bar, so after the effect started
    ans = re.search(r'^fx \w+\r?$', out, re.M)
    ticks = ticks_in(out[ans.end():] if ans else out)
    t0 = ticks[0][0] if ticks else 0
    late = [x for x in ticks if x[0] >= t0 + settle_runs * run_ms]
    first = [x for x in ticks if x[0] < t0 + settle_runs * run_ms]
    steady = len(late) > 50 and all(grp_changes(late, c) == 0 for c in range(3))
    grp = late[0][2] and [late[0][2][c][1] for c in range(3)] if late else None
    check(steady, '%s: GRPPWM %s stays the same for %d ticks (%d ms) after the first %d ms' % (
        what, grp, len(late), late[-1][0] - late[0][0] if late else 0, settle_runs * run_ms))
    check(first and all(grp_changes(first, c) <= 40 for c in range(3)),
          '%s: at most 40 GRPPWM changes per chip at the start (%s)' % (
              what, [grp_changes(first, c) for c in range(3)]))
    grp_safe(ticks, what)
    return ticks


def host_white(m):
    all_off(m)
    for c in ('RED', 'GREEN', 'BLUE'):
        m.cmd('LED %s LEVEL 100' % c, 0.05)
        m.cmd('LED %s CONTROL 1' % c, 0.05)
    m.cmd('FX OFF', 0.3)


def host_dim(m):
    all_off(m)
    for c, v in (('RED', 18), ('GREEN', 18)):
        m.cmd('LED %s LEVEL %d' % (c, v), 0.05)
        m.cmd('LED %s CONTROL 1' % c, 0.05)
    m.cmd('FX OFF', 0.3)


def test_grppwm(m):
    """GRPPWM: no change while the light of a chip stays inside a range of 2
    (the one-color flash of CHASE in 0.1.3), no dark tick, a smooth BREATHE
    down and up with no jump at a GRPPWM change, fine steps at the bottom"""
    m.cmd('TRACE REGS')
    for before, start in ((host_white, 'after white at 100'), (all_off, 'from dark'), (host_dim, 'after 18 18 0')):
        ticks = grp_steady(m, 'FX CHASE 84 38 100 1500', 1500, 'CHASE 84 38 100 1500 %s' % start, before=before)
    # the light of the chip follows the duties: the dot keeps its light on the registers too
    lim = led_limit()
    over = [(t, i) for t, p, r in ticks for i in range(16)
            if sum(r[c][0][OUTPUTS[i]] * r[c][1] for c in range(3)) * 65535 // 65025 > lim + 3 * 128]
    check(ticks and not over, 'CHASE 84 38 100: PWMx x GRPPWM of each LED stays under the per-LED limit %s' % over[:3])
    grp_steady(m, 'FX CHASE 100 50 0 800', 800, 'CHASE 100 50 0 800', before=host_white)
    grp_steady(m, 'FX CHASE 100 100 100 800', 800, 'CHASE white 800', before=all_off)
    grp_steady(m, 'FX CHASE 3 1 2 1000', 1000, 'CHASE 3 1 2 (dim) 1000', before=host_white)
    grp_steady(m, 'FX SPECTRUM 1600', 1600, 'SPECTRUM RING 1600', settle_runs=0.5, runs=2, before=all_off)
    grp_steady(m, 'FX SPECTRUM 1600 30 ROWS', 1600, 'SPECTRUM ROWS 1600 30', settle_runs=0.5, runs=2, before=host_white)
    grp_steady(m, 'FX FILL 84 38 100 55', 500, 'FILL 84 38 100 55', settle_runs=0.5, runs=2, before=host_white)
    grp_steady(m, 'FX SPLIT 84 38 100 0 100 0', 500, 'SPLIT', settle_runs=0.5, runs=2, before=all_off)

    # RAINBOW and BREATHE change the envelope: GRPPWM changes, but only by the rules
    all_off(m)
    out = m.cmd('FX RAINBOW 1000', 2.0)
    ticks = ticks_in(out.split('fx rainbow', 1)[-1])
    grp_safe(ticks, 'RAINBOW 1000')
    for c in range(3):
        ramps_smooth(ticks, 'RAINBOW 1000', c)
    for line, name in (('FX SPECTRUM 1600', 'SPECTRUM RING 1600'), ('FX SPECTRUM 1600 40 ROWS', 'SPECTRUM ROWS 1600 40')):
        all_off(m)
        sp = m.cmd(line, 2.0)
        sp = ticks_in(sp.split('fx spectrum', 1)[-1])[10:]
        grp_safe(sp, name)
        for c in range(3):
            for i in (0, 5, 12):
                ramps_smooth(sp, name, c, i, need_switch=False)
    ch = [grp_changes(ticks, c) for c in range(3)]
    check(ticks and max(ch) <= len(ticks) // 4,
          'RAINBOW: GRPPWM changes %s in %d ticks, at most 1 in 4 ticks (0.1.3: most ticks)' % (ch, len(ticks)))

    for rgb in ('100 0 0', '84 38 100', '20 0 10'):
        all_off(m)
        out = m.cmd('FX BREATHE %s 2000' % rgb, 4.5)
        out = out.split('fx breathe', 1)[-1]
        while True:   # at least two periods of complete ticks
            ticks = ticks_in(out)
            if ticks and ticks[-1][0] - ticks[0][0] >= 4000:
                break
            more = m.read_for(0.5)
            if not more:
                break
            out += more
        ticks = ticks_in(out)
        grp_safe(ticks, 'BREATHE %s 2000' % rgb)
        for c in range(3):
            if any(p[0][c] for _, p, _ in ticks):
                ramps_smooth(ticks, 'BREATHE %s 2000' % rgb, c)
        ch = [grp_changes(ticks, c) for c in range(3)]
        check(ticks and max(ch) <= len(ticks) // 4,
              'BREATHE %s: GRPPWM changes %s in %d ticks, at most 1 in 4 ticks' % (rgb, ch, len(ticks)))
        # the bottom of the wave: fine steps on the chip
        low = [(t, r[0][1], max(r[0][0])) for t, p, r in ticks if 0 < max(q[0] for q in p) <= 255]
        check(low and all(g <= 8 for _, g, _ in low),
              'BREATHE %s: below duty 256 the red chip runs GRPPWM 8 or less, steps of GRPPWM/65025 (%s)' % (
                  rgb, sorted(set(g for _, g, _ in low))))
        lit = [max(r[0][0]) * r[0][1] for t, p, r in ticks if any(r[0][0])]
        check(lit and min(lit) < 50, 'BREATHE %s: the lowest lit step on the chip is %d of 65025' % (rgb, min(lit or [0])))

    m.cmd('FX OFF', 0.1)
    m.cmd('TRACE OFF', 0.3)
    out = m.cmd('TLCREGS RED')
    check(re.search(r'RED mode 80 00 grp \d+ freq 0 ledout FF FF FF FF grp changes \d+\r?\nRED pwm( \d+){16}', out) is not None,
          'TLCREGS RED shows the chip registers: %s' % out.strip().replace('\r\n', ' | ')[:160])
    check('usage' in m.cmd('TLCREGS PINK'), 'TLCREGS refuses a bad color')


def main():
    new = sys.argv[1]
    ref = sys.argv[2] if len(sys.argv) > 2 else None
    m = Machine(new)
    try:
        text, hit = m.wait_for(r'tsx-ledbar qemu start (\d+) mailbox', 15)
        check(hit is not None, 'firmware starts')
        if not hit:
            print(text)
            return 1
        test_features(m)
        test_led_limit(m)
        test_one_color_limit(m)
        test_one_color(m)
        test_grppwm(m)
        new_states = one_color_states(m)
    finally:
        m.close()
    if ref:
        r = Machine(ref)
        try:
            text, hit = r.wait_for(r'tsx-ledbar qemu start (\d+) mailbox', 15)
            ref_states = one_color_states(r) if hit else []
        finally:
            r.close()
        same = sum(1 for a, b in zip(new_states, ref_states) if a == b and a[0] and a[1])
        check(len(ref_states) == len(new_states) and same == len(new_states),
              'one color: duties equal to %s and registers by the rules in %d of %d states' % (
                  os.path.basename(ref), same, len(new_states)))
    print('%s: %d failures' % (os.path.basename(new), rt.failures))
    return 1 if rt.failures else 0


if __name__ == '__main__':
    sys.exit(main())
