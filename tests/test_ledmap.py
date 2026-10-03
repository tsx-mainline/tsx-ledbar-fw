#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Checks the LED maps: the table ledmaps[] in fw/src/ledmap.c, the map
"outputs", and the tables in docs/leds.md.

MAPS holds the expected map of each tested LED bar model, by name. The
first entry is the default map. The map of the TSW-1060-LB is the
physical order of that bar, top to bottom: the right side is driven by
outputs 15, 6, 0, 1, 2, 3, 4, 5 and the left side by outputs 14, 13, 12,
11, 10, 9, 8, 7. Output n of the red, green and blue chip drives the same
LED.

To add a map for a tested bar model: add the entry to ledmaps[], to MAPS
and to the table of maps in docs/leds.md (section "Board variant and LED
map")."""
import os
import re
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
RIGHT = [15, 6, 0, 1, 2, 3, 4, 5]
LEFT = [14, 13, 12, 11, 10, 9, 8, 7]
# the tested LED bar models: (map name, outputs of LED index 0..15), the default first
MAPS = [
    ('TSW-1060-LB', RIGHT + LEFT),
]
DEFAULT = 'TSW-1060-LB'
KEYWORDS = ('DEFAULT', 'PANEL')


def read(path):
    with open(os.path.join(ROOT, path)) as f:
        return f.read()


def numbers(text):
    return [int(x) for x in re.findall(r'\b\d+\b', re.sub(r'/\*.*?\*/', '', text, flags=re.S))]


def firmware_maps():
    """ledmaps[] of fw/src/ledmap.c: [(name, outputs)] in table order"""
    src = read('fw/src/ledmap.c')
    block = re.search(r'static const struct ledmap ledmaps\[\] = \{(.*?)\n\};', src, re.S)
    if not block:
        return None
    found = re.findall(r'\{\s*"([^"]+)",\s*\{([^}]*)\}\s*\}', block.group(1))
    return [(name, numbers(outs)) for name, outs in found]


class LedMap(unittest.TestCase):
    def test_firmware_table(self):
        maps = firmware_maps()
        self.assertIsNotNone(maps, 'ledmaps[] not found in fw/src/ledmap.c')
        self.assertEqual(maps, MAPS)

    def test_default_map(self):
        """the first entry is the default map, the map of the TSW-1060-LB"""
        src = read('fw/src/ledmap.c')
        self.assertRegex(src, r'#define DEFAULT_MAP\s+\(&ledmaps\[0\]\)')
        self.assertEqual(MAPS[0][0], DEFAULT)
        self.assertEqual(MAPS[0][1], RIGHT + LEFT)

    def test_names_and_outputs(self):
        names = [n for n, _ in MAPS] + ['outputs']
        self.assertEqual(len(names), len(set(n.lower() for n in names)), 'two maps with one name')
        for name, outs in MAPS:
            self.assertNotRegex(name, r'\s', 'map %s: LEDMAP takes the name as one word' % name)
            self.assertNotIn(name.upper(), KEYWORDS, 'map %s: the name is a LEDMAP keyword' % name)
            self.assertNotRegex(name, r'^[0-7]$', 'map %s: a variant value is not a map name' % name)
            self.assertEqual(sorted(outs), list(range(16)), 'map %s: each output once' % name)

    def test_outputs_map(self):
        src = read('fw/src/ledmap.c')
        m = re.search(r'static const struct ledmap plain = \{\s*"([^"]+)",\s*\{([^}]*)\}', src)
        self.assertIsNotNone(m, 'the map outputs (plain) not found')
        self.assertEqual(m.group(1), 'outputs')
        self.assertEqual(numbers(m.group(2)), list(range(16)))

    def test_doc_led_table(self):
        """the LED table of docs/leds.md is the default map"""
        rows = re.findall(r'^\| (\d+) \| ([RL][1-8]) \| (\d+) \|', read('docs/leds.md'), re.M)
        self.assertEqual([int(r[0]) for r in rows], list(range(16)))
        self.assertEqual([r[1] for r in rows], ['R%d' % n for n in range(1, 9)] + ['L%d' % n for n in range(1, 9)])
        self.assertEqual([int(r[2]) for r in rows], MAPS[0][1])

    def test_doc_map_table(self):
        """the table of maps in docs/leds.md lists each map. Only bar models are tested."""
        doc = read('docs/leds.md')
        self.assertTrue('\n## Board variant and LED map\n' in doc, 'no section "Board variant and LED map"')
        rows = re.findall(r'^\| `([^`]+)` \| ([^|]+) \| (yes|no) \|', doc, re.M)
        self.assertEqual([name for name, _, _ in rows], [n for n, _ in MAPS] + ['outputs'])
        self.assertEqual([name for name, _, t in rows if t == 'yes'], [n for n, _ in MAPS])
        self.assertIn('default', dict((n, u) for n, u, _ in rows)[DEFAULT])


if __name__ == '__main__':
    unittest.main()
