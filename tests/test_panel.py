#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of "tsx-ledbar-fw-install --check" (panel/) with a fake sysfs
tree and a packed test image. --check needs no root."""
import importlib.util
import os
import shutil
import struct
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'tools')
INSTALL = os.path.join(HERE, '..', 'panel', 'tsx-ledbar-fw-install')
spec = importlib.util.spec_from_file_location('tsx_upg', os.path.join(TOOLS, 'tsx-upg.py'))
upg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upg)


def make_image(version):
    img = bytearray(b'\xff' * 0x400)
    struct.pack_into('<2I', img, 0, 0x20001000, upg.APP_BASE + 0x200 | 1)
    img[upg.HEADER_OFF + 16:upg.HEADER_OFF + 28] = b'CSIGN\0\0\0\0\0\0\0'
    struct.pack_into('<4H', img, upg.HEADER_OFF + 28, 0, 0, 0, upg.PRODUCT)
    return upg.fill_header(bytes(img), upg.APP_BASE, version, upg.PRODUCT)


@unittest.skipUnless(shutil.which('sh') and shutil.which('python3'), 'needs sh and python3')
class InstallCheck(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.image = os.path.join(d, 'tsx-ledbar.upg')
        upg.write_upg(self.image, make_image((0, 1, 9)), upg.APP_BASE, [upg.PRODUCT])
        self.leds = os.path.join(d, 'leds')
        self.usb = os.path.join(d, 'usb')
        os.makedirs(self.usb)

    def tearDown(self):
        self.tmp.cleanup()

    def bar_firmware(self, name):
        os.makedirs(self.leds, exist_ok=True)
        with open(os.path.join(self.leds, 'firmware'), 'w') as f:
            f.write(name + '\n')

    def usb_device(self, pid):
        d = os.path.join(self.usb, '1-1')
        os.makedirs(d, exist_ok=True)
        for k, v in (('idVendor', '14be'), ('idProduct', pid)):
            with open(os.path.join(d, k), 'w') as f:
                f.write(v + '\n')

    def check(self, image=None):
        env = dict(os.environ, TSX_LEDBAR_FW_IMAGE=image or self.image, TSX_LEDBAR_FW_LIB=TOOLS,
                   TSX_LEDBAR_LEDS=self.leds, TSX_LEDBAR_USB=self.usb)
        r = subprocess.run(['sh', INSTALL, '--check'], env=env, capture_output=True, text=True, timeout=60)
        return r.returncode, r.stdout + r.stderr

    def test_packaged_version(self):
        self.bar_firmware('TSX-LEDBAR [v0.1.9]')
        rc, out = self.check()
        self.assertEqual((rc, out.strip()), (0, 'bar runs tsx-ledbar 0.1.9'))

    def test_other_version(self):
        self.bar_firmware('TSW-XX60-LB [v1.3443.00018]')
        rc, out = self.check()
        self.assertEqual(rc, 1)
        self.assertIn('package has 0.1.9', out)

    def test_no_bar(self):
        rc, out = self.check()
        self.assertEqual(rc, 1)
        self.assertIn('no LED bar found', out)

    def test_bar_in_bootloader(self):
        self.usb_device('001a')
        rc, out = self.check()
        self.assertEqual(rc, 1)
        self.assertIn('bootloader mode', out)

    def test_application_without_driver(self):
        self.usb_device('001b')
        rc, out = self.check()
        self.assertEqual(rc, 1)
        self.assertIn('no LED bar found', out)

    def test_broken_image(self):
        self.bar_firmware('TSX-LEDBAR [v0.1.9]')
        bad = os.path.join(self.tmp.name, 'bad.upg')
        with open(self.image, newline='') as f, open(bad, 'w', newline='') as g:
            lines = f.read().split('\r\n')
            lines[5] = lines[5][:-2] + ('00' if lines[5][-2:] != '00' else '01')   # bad checksum
            g.write('\r\n'.join(lines))
        rc, out = self.check(bad)
        self.assertEqual(rc, 1)
        self.assertIn('cannot read the version', out)


if __name__ == '__main__':
    unittest.main()
