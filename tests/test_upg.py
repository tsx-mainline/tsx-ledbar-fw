#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/tsx-upg.py. With TSX_STOCK_UPG set to the stock
statussign .upg file (never in the repo), the stock image is checked and
repacked byte for byte."""
import contextlib
import importlib.util
import io
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


def s1_record(addr, data):
    body = bytes([len(data) + 3]) + addr.to_bytes(2, 'big') + bytes(data)
    return 'S1' + (body + bytes([0xFF - (sum(body) & 0xFF)])).hex().upper()


def write_lines(d, lines, name='h.upg'):
    path = os.path.join(d, name)
    with open(path, 'w', newline='') as f:
        for l in lines:
            f.write(l + '\r\n')
    return path


TAG = upg.make_record(upg.TAG_ADDR, b'DM\xe5')


class Hostile(unittest.TestCase):
    """Bad or hostile files end in UpgError or a failed check, never in a
    traceback or a large allocation."""

    def test_short_line(self):
        with self.assertRaises(upg.UpgError):
            upg.parse_record('S3')

    def test_non_hex(self):
        with self.assertRaises(upg.UpgError):
            upg.parse_record('S30508020000ZZ')

    def test_odd_hex(self):
        with self.assertRaises(upg.UpgError):
            upg.parse_record(upg.make_record(upg.APP_BASE, b'\x01')[:-1])

    def test_count_mismatch(self):
        line = upg.make_record(upg.APP_BASE, b'\x01\x02')
        with self.assertRaises(upg.UpgError):
            upg.parse_record(line[:2] + '09' + line[4:])

    def test_s1_record_refused(self):
        self.assertEqual(upg.parse_record(s1_record(0x1000, b'\x01'))[0], 1)
        with tempfile.TemporaryDirectory() as d:
            path = write_lines(d, [TAG, s1_record(0x1000, b'\x01\x02')])
            with self.assertRaises(upg.UpgError):
                upg.read_upg(path)

    def test_record_outside_flash(self):
        good = (upg.APP_BASE, bytes(16))
        for a in (0, 0x07FFFFF0, 0x08040000, 0xFFFFFFF0, upg.FLASH_END - 8):
            with self.assertRaises(upg.UpgError, msg='0x%08X' % a):
                upg.image_from_records([good, (a, bytes(16))])

    def test_overlapping_records(self):
        with self.assertRaises(upg.UpgError):
            upg.image_from_records([(upg.APP_BASE, bytes(16)), (upg.APP_BASE + 8, bytes(16))])
        base, img = upg.image_from_records([(upg.APP_BASE + 16, b'\x02' * 16), (upg.APP_BASE, b'\x01' * 16)])
        self.assertEqual((base, img), (upg.APP_BASE, b'\x01' * 16 + b'\x02' * 16))

    def test_short_image(self):
        checks = upg.check_image(b'\x00\x00\x00\x00', upg.APP_BASE, [upg.PRODUCT])
        self.assertIn(('vectors', False), [(n, ok) for n, ok, _ in checks])

    def test_cli_hostile_files(self):
        with tempfile.TemporaryDirectory() as d:
            far = write_lines(d, [TAG, upg.make_record(upg.APP_BASE, bytes(16)),
                                  upg.make_record(0xFFFFFFF0, bytes(16))], 'far.upg')
            short = write_lines(d, [TAG, upg.make_record(upg.APP_BASE, bytes(4))], 'short.upg')
            junk = os.path.join(d, 'junk.upg')
            with open(junk, 'wb') as f:
                f.write(b'\xff\xfe binary')
            err = io.StringIO()
            with contextlib.redirect_stderr(err), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(upg.main(['info', far]), 2)
                self.assertEqual(upg.main(['verify', far]), 2)
                self.assertEqual(upg.main(['verify', short]), 1)
                self.assertEqual(upg.main(['info', junk]), 2)
            self.assertIn('outside the flash', err.getvalue())


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
