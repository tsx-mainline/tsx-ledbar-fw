#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/tsx-ledbar-flash without a bar: the record parsing, the
packets, and the upload to a fake bootloader."""
import contextlib
import importlib.machinery
import importlib.util
import io
import os
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.join(HERE, '..', 'tools')


def load(name, path):
    loader = importlib.machinery.SourceFileLoader(name, path)
    spec = importlib.util.spec_from_loader(name, loader)
    m = importlib.util.module_from_spec(spec)
    loader.exec_module(m)
    return m


flash = load('tsx_ledbar_flash', os.path.join(TOOLS, 'tsx-ledbar-flash'))
upg = load('tsx_upg', os.path.join(TOOLS, 'tsx-upg.py'))
BASE = upg.APP_BASE


def ready(log2):
    return bytes([0x02, 0x03, 0x04, 0x01, log2])


class FakeBootloader:
    """A bar in bootloader mode. It sends the ready packet first and an
    accept packet after each end of block, or an abort after end of block
    number abort_at. It keeps every packet it gets."""

    def __init__(self, log2=15, abort_at=None):
        self.answers = [ready(log2)]
        self.got = []
        self.gone = False
        self.ends = 0
        self.abort_at = abort_at

    def write(self, data):
        self.got.append(bytes(data))
        if bytes(data) == flash.PKT_END_BLOCK:
            self.ends += 1
            self.answers.append(bytes([0x02, 0x02, 0x04, 0x06]) if self.ends == self.abort_at else ready(15))
        return len(data)

    def read(self, timeout_ms=200, n=64):
        return self.answers.pop(0) if self.answers else b''

    def drain(self, ms):
        out = b''.join(self.answers)
        self.answers = []
        return out


def quiet(fn, *args):
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return fn(*args)


def write_upg(d, records, name='t.upg'):
    """records: [(addr, data)] after the tag record"""
    path = os.path.join(d, name)
    with open(path, 'w', newline='') as f:
        f.write(upg.make_record(upg.TAG_ADDR, b'DM\xe5') + '\r\n')
        for a, data in records:
            f.write(upg.make_record(a, data) + '\r\n')
    return path


def blocks_of(got):
    """the data record addresses of each block, from the packets sent"""
    out, cur = [], []
    for p in got[2:]:   # after the tag record and its end of block
        if p == flash.PKT_END_BLOCK:
            out.append(cur)
            cur = []
        elif p == flash.PKT_END_UPDATE:
            out.append(cur)
        else:
            cur.append(int.from_bytes(p[3:7], 'big'))
    return out


class Packets(unittest.TestCase):
    def test_tag_packet(self):
        body = bytes.fromhex('S308BAD0ADD0444DE57A'[2:])[1:]
        self.assertEqual(flash.srec_packet(body), bytes.fromhex('02090DBAD0ADD0444DE57A'))

    def test_read_records(self):
        with tempfile.TemporaryDirectory() as d:
            path = write_upg(d, [(BASE + 16 * i, bytes(range(16))) for i in range(4)])
            recs = flash.read_records(path, upg)
        self.assertEqual([a for a, _ in recs], [upg.TAG_ADDR] + [BASE + 16 * i for i in range(4)])
        self.assertTrue(all(len(b) == 4 + 16 + 1 for _, b in recs[1:]))
        self.assertEqual(recs[1][1][:4], BASE.to_bytes(4, 'big'))

    def test_record_length_limit(self):
        with tempfile.TemporaryDirectory() as d:
            ok = write_upg(d, [(BASE, bytes(56))], 'ok.upg')
            long = write_upg(d, [(BASE, bytes(57))], 'long.upg')
            recs = flash.read_records(ok, upg)
            self.assertEqual(len(flash.srec_packet(recs[1][1])), 64)
            with self.assertRaises(upg.UpgError):
                flash.read_records(long, upg)


class Upload(unittest.TestCase):
    def records(self, addrs):
        tag = bytes.fromhex(upg.make_record(upg.TAG_ADDR, b'DM\xe5')[2:])[1:]
        return [(upg.TAG_ADDR, tag)] + [
            (a, bytes.fromhex(upg.make_record(a, bytes(16))[2:])[1:]) for a in addrs]

    def test_sequence(self):
        bar = FakeBootloader()
        recs = self.records([BASE + 16 * i for i in range(8)])
        quiet(flash.upload, bar, recs)
        self.assertEqual(bar.got[0], flash.srec_packet(recs[0][1]), 'the tag record goes first')
        self.assertEqual(bar.got[1], flash.PKT_END_BLOCK, 'end of block after the tag record')
        self.assertEqual(bar.got[-1], flash.PKT_END_UPDATE, 'end of update last')
        self.assertEqual(len(bar.got), 2 + 8 + 1)

    def test_block_boundary(self):
        # block size 64: a record at exactly block start + 64 stays in the block
        bar = FakeBootloader(log2=6)
        addrs = [BASE + 16 * i for i in range(12)]
        quiet(flash.upload, bar, self.records(addrs))
        self.assertEqual(blocks_of(bar.got), [
            [BASE + x for x in (0, 16, 32, 48, 64)],
            [BASE + x for x in (80, 96, 112, 128, 144)],
            [BASE + x for x in (160, 176)],
        ])

    def test_gap_starts_a_block(self):
        bar = FakeBootloader()
        quiet(flash.upload, bar, self.records([BASE, BASE + 16, BASE + 0x9000]))
        self.assertEqual(blocks_of(bar.got), [[BASE, BASE + 16], [BASE + 0x9000]])

    def test_32k_blocks(self):
        bar = FakeBootloader()
        addrs = [BASE + 16 * i for i in range(0x18000 // 16)]
        quiet(flash.upload, bar, self.records(addrs))
        b = blocks_of(bar.got)
        self.assertEqual([x[0] for x in b], [BASE, BASE + 0x8010, BASE + 0x10020])
        self.assertEqual(b[0][-1], BASE + 0x8000)

    def test_abort(self):
        bar = FakeBootloader(log2=6, abort_at=2)
        with self.assertRaises(SystemExit):
            quiet(flash.upload, bar, self.records([BASE + 16 * i for i in range(12)]))
        self.assertNotIn(flash.PKT_END_UPDATE, bar.got)

    def test_no_ready_packet(self):
        bar = FakeBootloader()
        bar.answers = []
        with self.assertRaises(SystemExit):
            quiet(flash.wait_ready, bar, 0.3)


if __name__ == '__main__':
    unittest.main()
