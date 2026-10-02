#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/tsx-upg.py. With TSX_STOCK_UPG set to the stock
statussign .upg file (never in the repo), the stock image is checked and
repacked byte for byte."""
import importlib.util
import os
import struct
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('tsx_upg', os.path.join(HERE, '..', 'tools', 'tsx-upg.py'))
upg = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upg)


def synthetic_image(size=0x400):
    img = bytearray(b'\xff' * size)
    struct.pack_into('<2I', img, 0, 0x20001000, upg.APP_BASE + 0x200 | 1)
    struct.pack_into('<4I', img, upg.HEADER_OFF, 0, 0, 0, 0)
    img[upg.HEADER_OFF + 16:upg.HEADER_OFF + 28] = b'CSIGN\0\0\0\0\0\0\0'
    struct.pack_into('<4H', img, upg.HEADER_OFF + 28, 0, 1, 0, upg.PRODUCT)
    for i in range(0x200, size):
        img[i] = i & 0xFF
    return bytes(img)


class Crc(unittest.TestCase):
    def test_known_vector(self):
        self.assertEqual(upg.crc16_xmodem(b'123456789'), 0x31C3)

    def test_empty(self):
        self.assertEqual(upg.crc16_xmodem(b''), 0)


class Records(unittest.TestCase):
    def test_roundtrip(self):
        line = upg.make_record(0x08020000, bytes(range(16)))
        t, a, d = upg.parse_record(line)
        self.assertEqual((t, a, d), (3, 0x08020000, bytes(range(16))))

    def test_bad_checksum(self):
        line = upg.make_record(0x08020000, b'\x01\x02')
        bad = line[:-2] + ('00' if line[-2:] != '00' else '01')
        with self.assertRaises(upg.UpgError):
            upg.parse_record(bad)

    def test_tag_record(self):
        self.assertEqual(upg.make_record(upg.TAG_ADDR, b'DM\xe5'), 'S308BAD0ADD0444DE57A')


class Pack(unittest.TestCase):
    def test_pack_verify_unpack(self):
        img = synthetic_image()
        filled = upg.fill_header(img, upg.APP_BASE, (0, 1, 2), upg.PRODUCT)
        checks = upg.check_image(filled, upg.APP_BASE, [upg.PRODUCT])
        self.assertTrue(all(ok for _, ok, _ in checks), checks)
        f = upg.header_fields(filled, upg.APP_BASE)
        self.assertEqual((f['major'], f['minor'], f['build']), (0, 1, 2))
        self.assertEqual(f['end'], upg.APP_BASE + len(img) - 1)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, 't.upg')
            upg.write_upg(path, filled, upg.APP_BASE, [upg.PRODUCT])
            codes, recs = upg.read_upg(path)
            base, back = upg.image_from_records(recs)
            self.assertEqual(codes, [upg.PRODUCT])
            self.assertEqual(base, upg.APP_BASE)
            self.assertEqual(back, filled)
            with open(path, 'rb') as fh:
                text = fh.read()
            self.assertTrue(text.startswith(b'S308BAD0ADD0444DE57A\r\n'))
            self.assertTrue(text.endswith(b'\r\n'))
            self.assertNotIn(b'S7', text[:2])

    def test_bad_crc_is_found(self):
        img = bytearray(upg.fill_header(synthetic_image(), upg.APP_BASE))
        img[-1] ^= 0xFF
        checks = dict((n, ok) for n, ok, _ in upg.check_image(bytes(img), upg.APP_BASE))
        self.assertFalse(checks['crc'])

    def test_wrong_base_is_found(self):
        img = upg.fill_header(synthetic_image(), 0x08010000)
        checks = dict((n, ok) for n, ok, _ in upg.check_image(img, 0x08010000))
        self.assertFalse(checks['base'])

    def test_cli_pack_refuses_without_header(self):
        with tempfile.TemporaryDirectory() as d:
            b = os.path.join(d, 'x.bin')
            with open(b, 'wb') as fh:
                fh.write(b'\x00' * 0x400)
            rc = upg.main(['pack', b, os.path.join(d, 'x.upg')])
            self.assertEqual(rc, 2)
            self.assertFalse(os.path.exists(os.path.join(d, 'x.upg')))


@unittest.skipUnless(os.environ.get('TSX_STOCK_UPG'), 'TSX_STOCK_UPG not set')
class Stock(unittest.TestCase):
    def test_stock_checks_and_repack(self):
        path = os.environ['TSX_STOCK_UPG']
        codes, recs = upg.read_upg(path)
        base, img = upg.image_from_records(recs)
        checks = upg.check_image(img, base, codes)
        self.assertTrue(all(ok for _, ok, _ in checks), checks)
        f = upg.header_fields(img, base)
        self.assertEqual((f['major'], f['minor'], f['build']), (1, 3443, 18))
        self.assertEqual(f['crc'], 0x0AE0)
        refilled = upg.fill_header(img, base)
        self.assertEqual(refilled, img)
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, 'repack.upg')
            upg.write_upg(out, img, base, codes)
            with open(out, 'rb') as a, open(path, 'rb') as b:
                self.assertEqual(a.read(), b.read())


if __name__ == '__main__':
    unittest.main()
