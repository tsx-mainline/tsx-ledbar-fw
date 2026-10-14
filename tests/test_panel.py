#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of "tsx-ledbar-fw-install", the post-upgrade hook and the uninstall
script (panel/) with a fake sysfs tree and a packed test image. --check,
--status and --recover-image need no root. The loads run with a fake "id", a
fake flasher and a fake rc-service."""
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
UNINSTALL = os.path.join(HERE, '..', 'panel', 'tsx-ledbar-fw-uninstall')
HOOK = os.path.join(HERE, '..', 'panel', 'tsx-ledbar-fw.post-upgrade')
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


@unittest.skipUnless(shutil.which('sh') and shutil.which('python3'), 'needs sh and python3')
class PanelBase(unittest.TestCase):
    """A fake panel: files, a fake flasher, a fake id and a fake rc-service."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = self.tmp.name
        self.image = os.path.join(d, 'tsx-ledbar.upg')
        with open(self.image, 'w') as f:
            f.write('packaged\n')
        self.marker = os.path.join(d, 'ledbar-fw.installed')
        self.pending = os.path.join(d, 'ledbar-fw.pending')
        self.leds = os.path.join(d, 'leds')
        self.stock_dir = os.path.join(d, 'vendor')
        self.usb = os.path.join(d, 'usb')
        self.bin = os.path.join(d, 'bin')
        self.calls = os.path.join(d, 'calls')
        self.flash_rc = os.path.join(d, 'flash_rc')
        for p in (self.stock_dir, self.usb, self.bin):
            os.makedirs(p)
        # the hook and the scripts find the install script by its name
        os.symlink(os.path.abspath(INSTALL), os.path.join(self.bin, 'tsx-ledbar-fw-install'))
        self.fake('id', 'echo ${FAKE_UID:-0}')
        # The flasher records its arguments. The exit code comes from a file.
        self.fake('flash', 'echo "flash $*" >> %s\ncat %s 2>/dev/null | { read -r r; exit ${r:-0}; }'
                  % (self.calls, self.flash_rc))
        # All services run, so each restart is recorded.
        self.fake('rc-service', 'echo "rc-service $*" >> %s\nexit 0' % self.calls)

    def tearDown(self):
        self.tmp.cleanup()

    def fake(self, name, body):
        p = os.path.join(self.bin, name)
        with open(p, 'w') as f:
            f.write('#!/bin/sh\n' + body + '\n')
        os.chmod(p, 0o755)

    def stock(self, *names):
        for n in names:
            with open(os.path.join(self.stock_dir, n), 'w') as f:
                f.write('stock\n')

    def set_marker(self):
        with open(self.marker, 'w') as f:
            f.write('2026-10-03 10:00:00\n')

    def usb_device(self, pid):
        d = os.path.join(self.usb, '1-1')
        os.makedirs(d, exist_ok=True)
        for k, v in (('idVendor', '14be'), ('idProduct', pid)):
            with open(os.path.join(d, k), 'w') as f:
                f.write(v + '\n')

    def env(self, uid='0'):
        return dict(os.environ, TSX_LEDBAR_FW_IMAGE=self.image, TSX_LEDBAR_FW_MARKER=self.marker,
                    TSX_LEDBAR_FW_PENDING=self.pending, TSX_LEDBAR_FW_LIB=TOOLS, TSX_LEDBAR_LEDS=self.leds,
                    TSX_LEDBAR_STOCK_DIR=self.stock_dir, TSX_LEDBAR_USB=self.usb,
                    TSX_LEDBAR_FLASH=os.path.join(self.bin, 'flash'), FAKE_UID=uid,
                    PATH=self.bin + os.pathsep + os.environ['PATH'])

    def run_script(self, script, *args, uid='0'):
        r = subprocess.run(['sh', script] + list(args), env=self.env(uid), capture_output=True,
                           text=True, timeout=60)
        return r.returncode, r.stdout, r.stderr

    def run_install(self, *args, uid='0'):
        return self.run_script(INSTALL, *args, uid=uid)

    def bar_firmware(self, name):
        os.makedirs(self.leds, exist_ok=True)
        with open(os.path.join(self.leds, 'firmware'), 'w') as f:
            f.write(name + '\n')

    def calls_made(self):
        try:
            with open(self.calls) as f:
                return f.read().splitlines()
        except OSError:
            return []


@unittest.skipUnless(shutil.which('sh') and shutil.which('python3'), 'needs sh and python3')
class InstallRecover(PanelBase):
    """--recover-image and --recover: the image for a bar in bootloader mode."""

    # --recover-image

    def test_image_marker(self):
        self.set_marker()
        self.stock('statussign_1.3443.00018.upg')
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (0, self.image))

    def test_image_stock_without_marker(self):
        self.stock('statussign_1.3443.00018.upg')
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (0, os.path.join(self.stock_dir, 'statussign_1.3443.00018.upg')))

    def test_image_newest_stock(self):
        self.stock('statussign_1.3443.00018.upg', 'statussign_1.3443.00100.upg', 'statussign_1.3443.00009.upg')
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (0, os.path.join(self.stock_dir, 'statussign_1.3443.00100.upg')))

    def test_image_packaged_without_marker_and_stock(self):
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (0, self.image))

    def test_image_stock_when_packaged_image_is_gone(self):
        self.set_marker()
        os.remove(self.image)
        self.stock('statussign_1.3443.00018.upg')
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (0, os.path.join(self.stock_dir, 'statussign_1.3443.00018.upg')))

    def test_no_image(self):
        os.remove(self.image)
        rc, out, _ = self.run_install('--recover-image')
        self.assertEqual((rc, out.strip()), (3, ''))

    # --recover

    def test_recover_packaged_image(self):
        self.set_marker()
        self.usb_device('001a')
        rc, out, err = self.run_install('--recover')
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls_made()[0], 'flash --keep-service flash %s' % self.image)
        self.assertIn('rc-service tsx-esphome restart', self.calls_made())
        self.assertTrue(os.path.exists(self.marker), 'the marker stays')
        self.assertIn('recovered', out)

    def test_recover_stock_image_keeps_no_marker(self):
        self.stock('statussign_1.3443.00018.upg')
        self.usb_device('001a')
        rc, out, err = self.run_install('--recover')
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls_made()[0], 'flash --keep-service flash %s'
                         % os.path.join(self.stock_dir, 'statussign_1.3443.00018.upg'))
        self.assertFalse(os.path.exists(self.marker), 'recover writes no marker')

    def test_recover_bar_in_application(self):
        self.set_marker()
        self.usb_device('001b')
        rc, out, _ = self.run_install('--recover')
        self.assertEqual(rc, 0)
        self.assertEqual(self.calls_made(), [])
        self.assertIn('not in bootloader mode', out)

    def test_recover_no_bar(self):
        rc, out, _ = self.run_install('--recover')
        self.assertEqual((rc, self.calls_made()), (0, []))

    def test_recover_no_image(self):
        os.remove(self.image)
        self.usb_device('001a')
        rc, _, err = self.run_install('--recover')
        self.assertEqual(rc, 1)
        self.assertIn('no image to load', err)
        self.assertEqual(self.calls_made(), [])

    def test_recover_flash_fails(self):
        self.set_marker()
        self.usb_device('001a')
        with open(self.flash_rc, 'w') as f:
            f.write('1\n')
        rc, _, err = self.run_install('--recover')
        self.assertEqual(rc, 1)
        self.assertIn('flash failed', err)
        self.assertEqual(self.calls_made(), ['flash --keep-service flash %s' % self.image])
        self.assertTrue(os.path.exists(self.marker), 'a failed recover keeps the marker')

    def test_recover_needs_root(self):
        self.usb_device('001a')
        rc, _, err = self.run_install('--recover', uid='1000')
        self.assertEqual(rc, 1)
        self.assertIn('run as root', err)
        self.assertEqual(self.calls_made(), [])


@unittest.skipUnless(shutil.which('sh') and shutil.which('python3'), 'needs sh and python3')
class UpgradeAndLoad(PanelBase):
    """A package upgrade never loads the bar. It records a newer image as
    pending. Only a person runs the load."""

    def setUp(self):
        super().setUp()
        # the packaged image is 0.1.9
        upg.write_upg(self.image, make_image((0, 1, 9)), upg.APP_BASE, [upg.PRODUCT])

    def hook(self):
        return self.run_script(HOOK)

    def read_pending(self):
        with open(self.pending) as f:
            return dict(l.rstrip('\n').split('=', 1) for l in f)

    # the hook

    def test_hook_old_bar_records_pending_and_does_not_load(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        rc, out, err = self.hook()
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls_made(), [], 'the upgrade loads nothing and restarts nothing')
        p = self.read_pending()
        self.assertEqual((p['old'], p['new']), ('tsx-ledbar 0.1.7', 'tsx-ledbar 0.1.9'))
        self.assertIn('date', p)
        lines = out.strip().splitlines()
        self.assertEqual(len(lines), 1, out)
        self.assertIn('new bar firmware (0.1.9) is available', lines[0])
        self.assertIn('Run tsx-ledbar-fw-install', lines[0])

    def test_hook_stock_bar(self):
        self.set_marker()
        self.bar_firmware('TSW-XX60-LB [v1.3443.00018]')
        rc, out, _ = self.hook()
        self.assertEqual(rc, 0)
        self.assertEqual(self.calls_made(), [])
        self.assertEqual(self.read_pending()['old'], '"TSW-XX60-LB [v1.3443.00018]"')

    def test_hook_bar_in_bootloader_and_no_bar(self):
        self.set_marker()
        self.usb_device('001a')
        rc, _, _ = self.hook()
        self.assertEqual((rc, self.read_pending()['old'], self.calls_made()), (0, 'bootloader mode', []))
        os.remove(self.pending)
        shutil.rmtree(self.usb)
        os.makedirs(self.usb)
        rc, _, _ = self.hook()
        self.assertEqual((rc, self.read_pending()['old'], self.calls_made()), (0, 'no bar', []))

    def test_hook_bar_runs_packaged_version(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.9]')
        with open(self.pending, 'w') as f:
            f.write('old=tsx-ledbar 0.1.7\nnew=tsx-ledbar 0.1.9\n')
        rc, out, _ = self.hook()
        self.assertEqual((rc, out, self.calls_made()), (0, '', []))
        self.assertFalse(os.path.exists(self.pending), 'a stale pending file goes away')

    def test_hook_without_marker_does_nothing(self):
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        rc, out, _ = self.hook()
        self.assertEqual((rc, out, self.calls_made()), (0, '', []))
        self.assertFalse(os.path.exists(self.pending))

    def test_hook_broken_image_leaves_the_bar_alone(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        with open(self.image, 'w') as f:
            f.write('not an image\n')
        rc, _, err = self.hook()
        self.assertEqual((rc, self.calls_made()), (0, []))
        self.assertIn('cannot read the version', err)
        self.assertFalse(os.path.exists(self.pending))

    def test_hook_does_not_need_root(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        rc, _, _ = self.run_script(HOOK, uid='1000')
        self.assertEqual((rc, self.calls_made()), (0, []))
        self.assertTrue(os.path.exists(self.pending))

    # --status

    def test_status_pending(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        self.hook()
        rc, out, _ = self.run_install('--status', uid='1000')
        self.assertEqual(rc, 0)
        self.assertIn('packaged image: tsx-ledbar 0.1.9', out)
        self.assertIn('bar: tsx-ledbar 0.1.7', out)
        self.assertIn('marker: installed', out)
        self.assertIn('pending: tsx-ledbar 0.1.7 -> tsx-ledbar 0.1.9', out)
        self.assertIn('Run tsx-ledbar-fw-install', out)
        self.assertEqual(self.calls_made(), [])

    def test_status_nothing_pending(self):
        self.bar_firmware('TSX-LEDBAR [v0.1.9]')
        rc, out, _ = self.run_install('--status')
        self.assertEqual(rc, 0)
        self.assertIn('marker: not installed', out)
        self.assertIn('pending: none', out)

    def test_status_drops_a_stale_pending_file(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.9]')
        with open(self.pending, 'w') as f:
            f.write('old=tsx-ledbar 0.1.7\nnew=tsx-ledbar 0.1.9\n')
        _, out, _ = self.run_install('--status')
        self.assertIn('pending: none', out)
        self.assertFalse(os.path.exists(self.pending))

    # the load by hand

    def test_manual_load_flashes_and_clears_pending(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        self.hook()
        self.assertEqual(self.calls_made(), [])
        rc, out, err = self.run_install()
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls_made()[0], 'flash flash %s' % self.image)
        self.assertIn('rc-service tsx-esphome restart', self.calls_made())
        self.assertFalse(os.path.exists(self.pending))
        self.assertTrue(os.path.exists(self.marker))
        self.assertIn('loading %s: tsx-ledbar 0.1.9 (the bar runs tsx-ledbar 0.1.7)' % self.image, out)

    def test_manual_load_writes_the_marker(self):
        self.assertFalse(os.path.exists(self.marker))
        rc, _, err = self.run_install()
        self.assertEqual(rc, 0, err)
        self.assertTrue(os.path.exists(self.marker))

    def test_failed_manual_load_keeps_pending(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        self.hook()
        with open(self.flash_rc, 'w') as f:
            f.write('1\n')
        rc, _, err = self.run_install()
        self.assertEqual(rc, 1)
        self.assertIn('flash failed', err)
        self.assertTrue(os.path.exists(self.pending))

    def test_manual_load_needs_root(self):
        rc, _, err = self.run_install(uid='1000')
        self.assertEqual(rc, 1)
        self.assertIn('run as root', err)
        self.assertEqual(self.calls_made(), [])

    def test_uninstall_clears_marker_and_pending(self):
        self.set_marker()
        self.bar_firmware('TSX-LEDBAR [v0.1.7]')
        self.hook()
        # any good image stands in for the stock image
        rc, _, err = self.run_script(UNINSTALL, self.image)
        self.assertEqual(rc, 0, err)
        self.assertEqual(self.calls_made()[0], 'flash flash %s' % self.image)
        self.assertFalse(os.path.exists(self.marker))
        self.assertFalse(os.path.exists(self.pending))


if __name__ == '__main__':
    unittest.main()
