#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests of firmware C files. Each tests/host/test_*.c file is built
with the C compiler of the host (CC, default cc), the firmware sources it
tests and the fake libopencm3 headers in tests/host/fake. The build uses
the address and undefined behavior sanitizers. Without a compiler the
tests are skipped."""
import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, 'host')
FAKE = os.path.join(HOST, 'fake')
FW = os.path.join(HERE, '..', 'fw', 'src')
CC = os.environ.get('CC', 'cc')
FLAGS = ['-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Wshadow', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']

# test file: the firmware sources it needs
TESTS = {
    'test_guard.c': ['guard.c', 'errlog.c'],
    'test_i2c.c': ['i2c_tlc.c', 'errlog.c'],
    'test_cresnet.c': ['cresnet.c'],
}


@unittest.skipUnless(shutil.which(CC), 'no C compiler (%s)' % CC)
class FirmwareHost(unittest.TestCase):
    def build_run(self, name):
        with tempfile.TemporaryDirectory() as d:
            exe = os.path.join(d, name[:-2])
            cmd = [CC] + FLAGS + ['-I', FAKE, '-I', FW, '-o', exe, os.path.join(HOST, name)]
            cmd += [os.path.join(FW, s) for s in TESTS[name]]
            r = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, 'build failed:\n' + r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout[-4000:] + r.stderr[-4000:])

    def test_guard(self):
        self.build_run('test_guard.c')

    def test_i2c(self):
        self.build_run('test_i2c.c')

    def test_cresnet(self):
        self.build_run('test_cresnet.c')


if __name__ == '__main__':
    unittest.main()
