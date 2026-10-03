#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the LED map: the table in fw/src/leds.c and the table in
docs/leds.md must both give the physical order of the bar.

Physical order, top to bottom: the right side is driven by outputs
15, 6, 0, 1, 2, 3, 4, 5 and the left side by outputs 14, 13, 12, 11, 10,
9, 8, 7. Output n of the red, green and blue chip drives the same LED."""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
RIGHT = [15, 6, 0, 1, 2, 3, 4, 5]
LEFT = [14, 13, 12, 11, 10, 9, 8, 7]


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


class LedMap(unittest.TestCase):
    def test_firmware_table(self):
        m = re.search(r'led_out\[NLEDS\] = \{([^}]*)\}', read('fw/src/leds.c'))
        self.assertIsNotNone(m)
        table = [int(x) for x in re.findall(r'\b\d+\b', re.sub(r'/\*.*?\*/', '', m.group(1)))]
        self.assertEqual(table, RIGHT + LEFT)

    def test_every_output_once(self):
        self.assertEqual(sorted(RIGHT + LEFT), list(range(16)))

    def test_doc_table(self):
        rows = re.findall(r'^\| (\d+) \| ([RL][1-8]) \| (\d+) \|', read('docs/leds.md'), re.M)
        self.assertEqual([int(r[0]) for r in rows], list(range(16)))
        self.assertEqual([r[1] for r in rows], ['R%d' % n for n in range(1, 9)] + ['L%d' % n for n in range(1, 9)])
        self.assertEqual([int(r[2]) for r in rows], RIGHT + LEFT)


if __name__ == '__main__':
    unittest.main()
