#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of tools/tsx-ledbar-flash without a bar: the record parsing, the
packets, and the upload to a fake bootloader."""
import argparse
import contextlib
import errno
import importlib.machinery
import importlib.util
import io
import os
import tempfile
import unittest
from unittest import mock

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
    number abort_at. It answers the prepare packet with a ready packet,
    as the stock bootloader does, unless mute. It keeps every packet it
    gets."""

    def __init__(self, log2=15, abort_at=None, mute=False):
        self.answers = [ready(log2)]
        self.log2 = log2
        self.got = []
        self.gone = False
        self.ends = 0
        self.abort_at = abort_at
        self.mute = mute

    def write(self, data):
        self.got.append(bytes(data))
        if bytes(data) == flash.PKT_PREPARE and not self.mute:
            self.answers.append(ready(self.log2))
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

    def test_ready_packet_no_prepare(self):
        # the ready packet of the bootloader start: no prepare packet
        bar = FakeBootloader()
        with mock.patch.object(flash, 'READY_ASK_S', 0.05):
            self.assertEqual(quiet(flash.wait_ready, bar, 3), 32768)
        self.assertEqual(bar.got, [])

    def test_lost_ready_packet(self):
        # a bar that waits in bootloader mode: its start ready packet is lost
        bar = FakeBootloader()
        bar.answers = []
        with mock.patch.object(flash, 'READY_ASK_S', 0.05):
            self.assertEqual(quiet(flash.wait_ready, bar, 3), 32768)
        self.assertEqual(bar.got, [flash.PKT_PREPARE])

    def test_mute_bootloader_one_prepare(self):
        bar = FakeBootloader(mute=True)
        bar.answers = []
        with mock.patch.object(flash, 'READY_ASK_S', 0.05), self.assertRaises(SystemExit):
            quiet(flash.wait_ready, bar, 0.5)
        self.assertEqual(bar.got, [flash.PKT_PREPARE])

    def test_upload_after_lost_ready_packet(self):
        bar = FakeBootloader()
        bar.answers = []
        recs = self.records([BASE + 16 * i for i in range(8)])
        with mock.patch.object(flash, 'READY_ASK_S', 0.05):
            quiet(flash.upload, bar, recs)
        self.assertEqual(bar.got[0], flash.PKT_PREPARE)
        self.assertEqual(bar.got[1], flash.srec_packet(recs[0][1]))
        self.assertEqual(bar.got[2], flash.PKT_END_BLOCK)
        self.assertEqual(bar.got[-1], flash.PKT_END_UPDATE)
        self.assertEqual(len(bar.got), 1 + 2 + 8 + 1)


class FakeAppBar:
    """A bar in application mode whose interface 1 a kernel driver holds"""
    sysname, devnode, pid, is_app = '1-1', '/dev/bus/usb/001/002', '001b', True

    def __init__(self):
        self.closed = False

    def open(self, iface=0):
        raise OSError(errno.EBUSY, 'Device or resource busy')

    def close(self):
        self.closed = True


class FakeService:
    def __init__(self, keep):
        self.stopped = False
        self.starts = 0

    def stop(self):
        self.stopped = True

    def start(self):
        self.starts += 1
        self.stopped = False


class Fallback(unittest.TestCase):
    def test_claim_busy(self):
        bar = FakeAppBar()
        err = io.StringIO()
        with mock.patch.object(flash, 'LED_RAW', '/nonexistent/raw'), contextlib.redirect_stderr(err):
            with self.assertRaises(SystemExit):
                flash.send_io_packet(bar, flash.PKT_PREPARE)
        self.assertIn('kernel driver holds it', err.getvalue())
        self.assertTrue(bar.closed)

    def run_enter(self, enter):
        services = []

        def make_service(keep):
            services.append(FakeService(keep))
            return services[-1]

        out = io.StringIO()
        with mock.patch.object(flash.Bar, 'find', staticmethod(lambda: FakeAppBar())), \
                mock.patch.object(flash, 'Service', make_service), \
                mock.patch.object(flash, 'enter_bootloader', enter), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            try:
                flash.cmd_enter(argparse.Namespace(keep_service=False))
                code = 0
            except SystemExit as e:
                code = e.code
        return code, services[0], out.getvalue()

    def test_enter_fails_starts_the_service(self):
        def enter(bar, service):
            service.stop()
            flash.die('the bar did not restart after the prepare packet')
        code, service, out = self.run_enter(enter)
        self.assertEqual(code, 1)
        self.assertEqual(service.starts, 1)
        self.assertFalse(service.stopped)

    def test_enter_says_the_service_stays_stopped(self):
        btl = FakeAppBar()
        btl.pid, btl.is_app = '001a', False

        def enter(bar, service):
            service.stop()
            return btl
        code, service, out = self.run_enter(enter)
        self.assertEqual(code, 0)
        self.assertEqual(service.starts, 0)
        self.assertIn('stays stopped', out)
        self.assertIn('rc-service tsx-ledbar start', out)


if __name__ == '__main__':
    unittest.main()
