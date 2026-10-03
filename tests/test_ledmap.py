#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the LED maps: the table ledmaps[] in fw/src/ledmap.c, the map
"outputs" of an unknown variant value, and the tables in docs/leds.md.

MAPS holds the expected map of each known variant value. The map of value
1 is the physical order of the TSW-1060-LB, top to bottom: the right side
is driven by outputs 15, 6, 0, 1, 2, 3, 4, 5 and the left side by outputs
14, 13, 12, 11, 10, 9, 8, 7. Output n of the red, green and blue chip
drives the same LED.

To add a map: add the entry to ledmaps[], to MAPS and to the table of
values in docs/leds.md (section "Board variant and LED map")."""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
RIGHT = [15, 6, 0, 1, 2, 3, 4, 5]
LEFT = [14, 13, 12, 11, 10, 9, 8, 7]
# variant value: (map name, outputs of LED index 0..15)
MAPS = {
    1: ('TSW-1060-LB', RIGHT + LEFT),
}
TESTED = {1}


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


def numbers(text):
    return [int(x) for x in re.findall(r'\b\d+\b', re.sub(r'/\*.*?\*/', '', text, flags=re.S))]


def firmware_maps():
    """ledmaps[] of fw/src/ledmap.c: {value: (name, outputs)}, and the list of values in order"""
    src = read('fw/src/ledmap.c')
    block = re.search(r'static const struct ledmap ledmaps\[\] = \{(.*?)\n\};', src, re.S)
    if not block:
        return None, None
    found = re.findall(r'\{\s*(\d+),\s*"([^"]+)",\s*\{([^}]*)\}\s*\}', block.group(1))
    return {int(v): (name, numbers(outs)) for v, name, outs in found}, [int(v) for v, _, _ in found]


class LedMap(unittest.TestCase):
    def test_firmware_table(self):
        maps, order = firmware_maps()
        self.assertIsNotNone(maps, 'ledmaps[] not found in fw/src/ledmap.c')
        self.assertEqual(len(order), len(set(order)), 'a variant value has two maps: %s' % order)
        self.assertEqual(maps, MAPS)

    def test_values_and_outputs(self):
        names = [n for n, _ in MAPS.values()] + ['outputs']
        self.assertEqual(len(names), len(set(n.lower() for n in names)), 'two maps with one name')
        for value, (name, outs) in MAPS.items():
            self.assertIn(value, range(8), 'map %s: the variant value is 0..7' % name)
            self.assertNotRegex(name, r'^[0-7]$|\s', 'map %s: LEDMAP takes the name as one word' % name)
            self.assertEqual(sorted(outs), list(range(16)), 'map %s: each output once' % name)
        self.assertEqual(MAPS[1][1], RIGHT + LEFT)

    def test_unknown_value_map(self):
        src = read('fw/src/ledmap.c')
        m = re.search(r'static const struct ledmap plain = \{\s*0xFF,\s*"([^"]+)",\s*\{([^}]*)\}', src)
        self.assertIsNotNone(m, 'the map of an unknown value (plain) not found')
        self.assertEqual(m.group(1), 'outputs')
        self.assertEqual(numbers(m.group(2)), list(range(16)))

    def test_doc_led_table(self):
        """the LED table of docs/leds.md is the map of value 1"""
        rows = re.findall(r'^\| (\d+) \| ([RL][1-8]) \| (\d+) \|', read('docs/leds.md'), re.M)
        self.assertEqual([int(r[0]) for r in rows], list(range(16)))
        self.assertEqual([r[1] for r in rows], ['R%d' % n for n in range(1, 9)] + ['L%d' % n for n in range(1, 9)])
        self.assertEqual([int(r[2]) for r in rows], MAPS[1][1])

    def test_doc_value_table(self):
        """the table of values in docs/leds.md lists each map, and only the tested ones as tested"""
        doc = read('docs/leds.md')
        self.assertTrue('\n## Board variant and LED map\n' in doc, 'no section "Board variant and LED map"')
        rows = re.findall(r'^\| ([0-7]) \| `([^`]+)` \| (yes|no) \|', doc, re.M)
        self.assertEqual({int(v): name for v, name, _ in rows}, {v: n for v, (n, _) in MAPS.items()})
        self.assertEqual({int(v) for v, _, t in rows if t == 'yes'}, TESTED)
        self.assertTrue(re.search(r'^\| Other \| `outputs` \| no \|', doc, re.M), 'no row for an unknown value')


if __name__ == '__main__':
    unittest.main()
