#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the QEMU build of the firmware on the netduino2 machine (an
STM32F205) and check the console, the LED engine and the start guard.

  qemu/run-test.py fw/build/qemu/tsx-ledbar-qemu.elf

The QEMU build has no USB and no I2C model: the console runs on USART1
and the LED driver registers are kept in RAM. TRACE ON prints one line
"led MS R G B" (duties) after each engine tick.
"""
import os
import re
import select
import subprocess
import sys
import time

QEMU = os.environ.get('QEMU', 'qemu-system-arm')
failures = 0


def check(cond, what):
    global failures
    print('%s %s' % ('ok  ' if cond else 'FAIL', what), flush=True)
    if not cond:
        failures += 1


class Machine:
    def __init__(self, elf):
        self.p = subprocess.Popen(
            [QEMU, '-M', 'netduino2', '-nographic', '-monitor', 'none', '-serial', 'stdio',
             '-kernel', elf], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.buf = b''

    def read_for(self, s):
        end = time.monotonic() + s
        while True:
            left = end - time.monotonic()
            if left <= 0:
                break
            r, _, _ = select.select([self.p.stdout], [], [], left)
            if not r:
                break
            d = os.read(self.p.stdout.fileno(), 4096)
            if not d:
                break
            self.buf += d
        out = self.buf.decode('latin-1')
        self.buf = b''
        return out

    def wait_for(self, pattern, timeout=10):
        end = time.monotonic() + timeout
        text = ''
        while time.monotonic() < end:
            text += self.read_for(0.2)
            m = re.search(pattern, text)
            if m:
                return text, m
        return text, None

    def send(self, line):
        self.p.stdin.write((line + '\r\n').encode())
        self.p.stdin.flush()

    def cmd(self, line, settle=0.4):
        self.send(line)
        return self.read_for(settle)

    def close(self):
        self.p.kill()
        self.p.wait()


def leds_in(text):
    # complete lines only: a read can end in the middle of a trace line
    return [tuple(int(x) for x in m) for m in re.findall(r'led (\d+) (\d+) (\d+) (\d+)\r?\n', text)]


START = r'qemu start (\d+) mailbox 0x([0-9A-F]+) fails (\d+)'


def restart(m, what, timeout=15):
    """wait for the banner of the next start: (start, mailbox, fails) or None"""
    text, hit = m.wait_for(START, timeout)
    if hit is None:
        print(text)
        return None
    return int(hit.group(1)), hit.group(2), int(hit.group(3))


def errlog(m):
    out = m.cmd('ERRLOG')
    return out, [(int(a), int(b, 16)) for a, b in re.findall(r'subsystem (\d+) cause 0x([0-9A-F]+)', out)]


def test_guard(m):
    """The start guard (fw/src/guard.c). QEMU has no USB and no watchdog
    model. TEST HOST, TEST CONFIG, TEST FAULT and TEST RESET make the
    events. The QEMU build uses a USB time of 20000 ms."""
    # planned resets are not failed starts
    for n in (2, 3, 4):
        m.send('REBOOT')
        r = restart(m, 'REBOOT')
        check(r is not None and r[0] == n and r[2] == 0 and r[1] != '00475055',
              'REBOOT: start %d, no failed start, no handover: %s' % (n, r))
    # a start without a host runs on: no reset, no failed start
    out = m.read_for(4.0)
    check('qemu start' not in out, 'no host for longer than the USB time: the firmware does not reset')
    out = m.cmd('STATUS')
    up = re.search(r'uptime (\d+) ms start (\d+) fails (\d+)', out)
    check(up is not None and int(up.group(1)) > 20000 and up.group(2) == '4' and up.group(3) == '0',
          'STATUS after the USB time without a host: %s' % (up.group(0) if up else out.strip()))

    # a host runs, no configuration: a failed start after the USB time
    for n in (1, 2, 3):
        check('test HOST' in m.cmd('TEST HOST', 0.2), 'TEST HOST %d' % n)
        r = restart(m, 'USB rule', 20)
        check(r is not None and r[2] == n and r[1] != '00475055',
              'a host ran without a USB configuration: reset, failed start %d: %s' % (n, r))
        if n == 1:
            out, log = errlog(m)
            check((125, 0) in log and (122, 1) in log,
                  'ERRLOG shows the USB rule (125) and the count (122, 1): %s' % log)
    m.cmd('TEST HOST', 0.2)
    r = restart(m, 'handover', 20)
    check(r is not None and r[1] == '00475055' and r[2] == 0,
          'the fourth failed start hands over: mailbox UPG, count 0: %s' % (r,))
    out, log = errlog(m)
    check(log == [], 'ERRLOG is empty after the handover: %s' % log)

    # a reset that the firmware did not plan (the watchdog on the bar) is a failed start
    m.send('TEST RESET')
    r = restart(m, 'unplanned reset')
    check(r is not None and r[2] == 1, 'an unplanned reset is a failed start: %s' % (r,))
    out, log = errlog(m)
    check((121, 1) in log and (122, 1) in log, 'ERRLOG shows the unplanned reset (121) and the count: %s' % log)
    out = m.cmd('TEST CONFIG')
    check(re.search(r'start \d+ fails 0', m.cmd('STATUS')) is not None, 'USB configured sets the count to 0')

    # a hard fault resets at once and is a failed start
    m.send('TEST FAULT')
    r = restart(m, 'hard fault')
    check(r is not None and r[2] == 1, 'a hard fault is a failed start: %s' % (r,))
    out, log = errlog(m)
    check((124, 0) in log, 'ERRLOG shows the hard fault (124): %s' % log)
    m.send('REBOOT')
    r = restart(m, 'REBOOT after a failed start')
    check(r is not None and r[2] == 1, 'REBOOT keeps the count of failed starts: %s' % (r,))
    m.read_for(3.5)
    out = m.cmd('STATUS')
    check(re.search(r'start \d+ fails 0', out) is not None and 'qemu start' not in out,
          'a start that runs the USB time without a host sets the count to 0')
    # a host after that time still starts the USB rule
    m.cmd('TEST HOST', 0.2)
    r = restart(m, 'late host', 20)
    check(r is not None and r[2] == 1, 'a host after the USB time without a configuration: failed start 1: %s' % (r,))

    m.send('IMGUPD')
    r = restart(m, 'IMGUPD')
    check(r is not None and r[1] == '00475055' and r[2] == 1, 'IMGUPD writes UPG and resets, the count stays: %s' % (r,))


def main():
    elf = sys.argv[1]
    m = Machine(elf)
    try:
        text, hit = m.wait_for(r'tsx-ledbar ' + START, 15)
        check(hit is not None, 'firmware starts and prints its banner')
        if not hit:
            print(text)
            return 1
        check(hit.group(1) == '1' and hit.group(3) == '0', 'first start is start 1, no failed start')

        out = m.cmd('VER')
        check('TSX-LEDBAR [v' in out, 'VER answers with the firmware name: %s' % out.strip())
        out = m.cmd('CAPS')
        check('fade' in out and 'rainbow' in out, 'CAPS lists the effects')
        out = m.cmd('STATUS')
        check('chips ok ok ok' in out, 'STATUS shows the three chips ready')
        out = m.cmd('TLCOUTMODE RED 0')
        check('output mode is:3' in out, 'TLCOUTMODE answers like the stock firmware: %s' % out.strip())
        out = m.cmd('TLCOUTMODE BLUE ALL')
        check('BLUE -1 output mode is:3' in out, 'TLCOUTMODE COLOR ALL reads the mode: %s' % out.strip())
        # the raw TLC write commands would skip the power limits: refused
        regs = [m.cmd('TLCREGS %s' % c) for c in ('RED', 'GREEN', 'BLUE')]
        for line in ('TLCOUTMODE RED ALL 1', 'TLCOUTMODE GREEN 3 2', 'TLCOUTMODE BLUE 0 0',
                     'TLCGROUPMODE BLUE 1', 'TLCGROUPMODE RED 0',
                     'TLCBRIGHTNESS RED 5 100', 'TLCBRIGHTNESS GREEN GROUP 100'):
            out = m.cmd(line)
            check('refused' in out, '%s is refused: %s' % (line, out.strip()))
        after = [m.cmd('TLCREGS %s' % c) for c in ('RED', 'GREEN', 'BLUE')]
        check(after == regs and all('mode 80 00' in r and 'ledout FF FF FF FF' in r for r in after),
              'the refused commands leave the chip registers as they were')
        out = m.cmd('NOSUCH')
        check('unknown command' in out, 'unknown command is reported')
        out = m.cmd('ERRLOG')
        check(out.strip().startswith('0 errors'), 'ERRLOG starts empty: %s' % out.strip())

        # host joins: red on at 100 -> duty 255
        m.cmd('TRACE ON')
        m.cmd('LED RED LEVEL 100')
        out = m.cmd('LED RED CONTROL 1', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (65535, 0, 0), 'red at level 100 gives duty 65535 0 0: %s' % (leds[-1:],))

        # the power cap: red and green at 100 -> both scaled to 36044 (110 % of one channel)
        m.cmd('LED GREEN LEVEL 100')
        out = m.cmd('LED GREEN CONTROL 1', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (36044, 36044, 0), 'power cap keeps red+green at 36044 36044 0: %s' % (leds[-1:],))

        # a smoothed host change ramps the blue channel up over about 500 ms
        m.cmd('FX SMOOTH 500')
        m.cmd('LED BLUE LEVEL 100')
        out = m.cmd('LED BLUE CONTROL 1', 1.5)
        leds = leds_in(out)
        blues = [l[3] for l in leds]
        check(len(blues) >= 10 and blues == sorted(blues), 'smooth ramp: %d steps, rising' % len(blues))
        check(leds and leds[-1][1:] == (24029, 24029, 24029), 'three colors at 100 end at 24029 each (cap): %s' % (leds[-1:],))

        # a fade effect to black over 300 ms, then the host color comes back with FX OFF
        m.cmd('FX SMOOTH 0')
        out = m.cmd('FX FADE 0 0 0 300', 1.0)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (0, 0, 0) and len(leds) >= 10, 'FX FADE reaches black in steps: %d steps' % len(leds))
        out = m.cmd('FX OFF', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (24029, 24029, 24029), 'FX OFF restores the host color: %s' % (leds[-1:],))

        # a host join that keeps the host color also ends a fade, and the bar
        # goes back to the host color (0.1.1 kept the fade color)
        m.cmd('FX FADE 0 0 0 300', 1.0)
        out = m.cmd('LED RED LEVEL 100', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (24029, 24029, 24029),
              'a join with the same host color ends FX FADE and restores the host color: %s' % (leds[-1:],))
        check('fx off' in m.cmd('FX'), 'the join ends the fade effect')
        m.cmd('FX SMOOTH 300')
        m.cmd('FX FADE 0 0 0 0', 0.5)
        out = m.cmd('LED RED CONTROL 1', 1.0)
        leds = leds_in(out)
        reds = [l[1] for l in leds]
        check(len(set(reds)) >= 10 and reds == sorted(reds) and leds[-1][1:] == (24029, 24029, 24029),
              'with FX SMOOTH the same join ramps back to the host color: %d steps, end %s' % (len(set(reds)), leds[-1:]))
        m.cmd('FX SMOOTH 0')

        out = m.cmd('FX RAINBOW 1000 50', 1.2)
        leds = leds_in(out)
        check(len(leds) >= 20 and len(set(l[1:] for l in leds)) >= 20, 'rainbow changes the color: %d steps' % len(leds))
        out = m.cmd('FX BLINK 100 0 0 200 200', 1.0)
        leds = leds_in(out)
        check((65535, 0, 0) in [l[1:] for l in leds] and (0, 0, 0) in [l[1:] for l in leds], 'blink toggles red')
        out = m.cmd('LED RED LEVEL 10', 0.8)
        check('fx off' in m.cmd('FX'), 'a host join ends the effect')
        check(leds_in(out) and leds_in(out)[-1][1:] == (403, 35842, 35842), 'host color after the join, capped: %s' % (leds_in(out)[-1:],))

        # breathe: no jump and no long dwell at the bottom of the wave
        m.cmd('LED RED CONTROL 0'); m.cmd('LED GREEN CONTROL 0'); m.cmd('LED BLUE CONTROL 0', 0.5)
        out = m.cmd('FX BREATHE 100 0 0 2000', 4.5)
        reds = [l[1] for l in leds_in(out)]
        low = [r for r in reds if 0 < r < 1000]
        first = min([r for r in reds if r > 0], default=None)
        steps = [abs(b - a) for a, b in zip(reds, reds[1:]) if min(a, b) < 1000]
        check(first is not None and first < 50, 'breathe: first step above off is tiny (%s of 65535)' % first)
        check(steps and max(steps) < 300, 'breathe: steps near the bottom stay small (max %s)' % (max(steps) if steps else None))
        # The QEMU clock runs faster than the wall clock, so the trace holds
        # more than two periods. When a tick falls on the period start, each
        # period has one tick at zero. A dwell gives more.
        trace = leds_in(out)
        periods = (trace[-1][0] - trace[0][0]) // 2000 if trace else 0
        zeros = sum(1 for r in reds if r == 0)
        check(trace and zeros <= periods + 1,
              'breathe: at most one tick at zero per period (%d in %d periods)' % (zeros, periods))
        check(len(low) >= 4, 'breathe: several distinct ticks below 1000 (%d)' % len(low))
        out = m.cmd('STATUS')
        check(re.search(r'pwm:grp \d+:\d+', out) is not None, 'STATUS shows the PWM and group values')
        m.cmd('FX OFF')
        m.cmd('TRACE OFF')

        test_guard(m)
    finally:
        m.close()
    print('%s: %d failures' % (os.path.basename(elf), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
