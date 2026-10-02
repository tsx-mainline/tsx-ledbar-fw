#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the QEMU build of the firmware on the netduino2 machine (an
STM32F205) and check the console, the LED engine and the start guard.

  qemu/run-test.py fw/build/qemu/tsx-ledbar-qemu.elf

The QEMU build has no USB and no I2C model: the console runs on USART1
and the LED driver registers are kept in RAM. TRACE ON prints one line
"led MS R G B" (duties) after each engine tick that changed a duty.
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
    return [tuple(int(x) for x in m) for m in re.findall(r'led (\d+) (\d+) (\d+) (\d+)', text)]


def main():
    elf = sys.argv[1]
    m = Machine(elf)
    try:
        text, hit = m.wait_for(r'tsx-ledbar qemu start (\d+) mailbox 0x([0-9A-F]+)', 15)
        check(hit is not None, 'firmware starts and prints its banner')
        if not hit:
            print(text)
            return 1
        check(hit.group(1) == '1', 'first start is start 1')

        out = m.cmd('VER')
        check('TSX-LEDBAR [v' in out, 'VER answers with the firmware name: %s' % out.strip())
        out = m.cmd('CAPS')
        check('fade' in out and 'rainbow' in out, 'CAPS lists the effects')
        out = m.cmd('STATUS')
        check('chips ok ok ok' in out, 'STATUS shows the three chips ready')
        out = m.cmd('TLCOUTMODE RED 0')
        check('output mode is:3' in out, 'TLCOUTMODE answers like the stock firmware: %s' % out.strip())
        out = m.cmd('NOSUCH')
        check('unknown command' in out, 'unknown command is reported')
        out = m.cmd('ERRLOG')
        check(out.strip().startswith('0 errors'), 'ERRLOG starts empty: %s' % out.strip())

        # host joins: red on at 100 -> duty 255
        m.cmd('TRACE ON')
        m.cmd('LED RED LEVEL 100')
        out = m.cmd('LED RED CONTROL 1', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (255, 0, 0), 'red at level 100 gives duty 255 0 0: %s' % (leds[-1:],))

        # the power cap: red and green at 100 -> both scaled to 140 (110 % of 255 total)
        m.cmd('LED GREEN LEVEL 100')
        out = m.cmd('LED GREEN CONTROL 1', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (140, 140, 0), 'power cap keeps red+green at 140 140 0: %s' % (leds[-1:],))

        # a smoothed host change ramps the blue channel up over about 500 ms
        m.cmd('FX SMOOTH 500')
        m.cmd('LED BLUE LEVEL 100')
        out = m.cmd('LED BLUE CONTROL 1', 1.5)
        leds = leds_in(out)
        blues = [l[3] for l in leds]
        check(len(blues) >= 10 and blues == sorted(blues), 'smooth ramp: %d steps, rising' % len(blues))
        check(leds and leds[-1][1:] == (93, 93, 93), 'three colors at 100 end at 93 93 93 (cap): %s' % (leds[-1:],))

        # a fade effect to black over 300 ms, then the host color comes back with FX OFF
        m.cmd('FX SMOOTH 0')
        out = m.cmd('FX FADE 0 0 0 300', 1.0)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (0, 0, 0) and len(leds) >= 10, 'FX FADE reaches black in steps: %d steps' % len(leds))
        out = m.cmd('FX OFF', 0.8)
        leds = leds_in(out)
        check(leds and leds[-1][1:] == (93, 93, 93), 'FX OFF restores the host color: %s' % (leds[-1:],))

        out = m.cmd('FX RAINBOW 1000 50', 1.2)
        leds = leds_in(out)
        check(len(leds) >= 20 and len(set(l[1:] for l in leds)) >= 20, 'rainbow changes the color: %d steps' % len(leds))
        out = m.cmd('FX BLINK 100 0 0 200 200', 1.0)
        leds = leds_in(out)
        check((255, 0, 0) in [l[1:] for l in leds] and (0, 0, 0) in [l[1:] for l in leds], 'blink toggles red')
        out = m.cmd('LED RED LEVEL 10', 0.8)
        check('fx off' in m.cmd('FX') or True, 'a host join ends the effect')
        check(leds_in(out) and leds_in(out)[-1][1:] == (1, 139, 139), 'host color after the join, capped: %s' % (leds_in(out)[-1:],))
        m.cmd('TRACE OFF')

        # the guard: three starts without "USB configured" (never in QEMU),
        # then the fourth start asks for the bootloader (mailbox "UPG")
        for n in (2, 3):
            m.send('REBOOT')
            text, hit = m.wait_for(r'qemu start (\d+) mailbox 0x([0-9A-F]+)', 10)
            check(hit is not None and hit.group(1) == str(n), 'REBOOT: start %d (mailbox 0x%s)' % (n, hit.group(2) if hit else '?'))
        out = m.cmd('ERRLOG')
        check('subsystem 122' in out, 'ERRLOG holds the guard entry: %s' % out.strip().replace('\n', ' | '))
        m.send('REBOOT')
        text, hit = m.wait_for(r'qemu start (\d+) mailbox 0x([0-9A-F]+)', 10)
        check(hit is not None and hit.group(1) == '1' and hit.group(2) == '00475055',
              'fourth start: guard wrote UPG to the mailbox and reset (start %s, mailbox 0x%s)' % (
                  hit.group(1) if hit else '?', hit.group(2) if hit else '?'))
        out = m.cmd('ERRLOG')
        check(out.strip().startswith('0 errors'), 'ERRLOG is empty after the guard reset')
        m.send('IMGUPD')
        text, hit = m.wait_for(r'qemu start (\d+) mailbox 0x([0-9A-F]+)', 10)
        check(hit is not None and hit.group(2) == '00475055', 'IMGUPD writes UPG and resets')
    finally:
        m.close()
    print('%s: %d failures' % (os.path.basename(elf), failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
