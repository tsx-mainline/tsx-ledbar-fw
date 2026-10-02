#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tsx-upg: pack, unpack and check application images of the xx60 LED bar.

The bar bootloader takes an application image as Motorola S-record text
(S3 records, CR LF line ends, no S0 and no S7 record). The first record
is a tag at the address 0xBAD0ADD0 with the data "DM" plus a list of
product codes. The application starts at 0x08020000. Its vector table is
0x184 bytes long, and a header follows it:

  +0x00 u32  CRC-16/XMODEM of the bytes start..end
  +0x04 u32  address of the CRC word (base + 0x184)
  +0x08 u32  start of the CRC range (base + 0x188)
  +0x0C u32  end of the CRC range (last byte, inclusive)
  +0x10 char[12] "CSIGN"
  +0x1C u16  version major, +0x1E u16 minor, +0x20 u16 build
  +0x22 u16  product code (0xE5 for the LED bar)

Commands:
  info FILE.upg               print the records, the header and the checks
  verify FILE.upg             exit 1 when a check fails
  unpack FILE.upg OUT.bin     write the flash bytes (gaps filled with 0xFF)
  pack IN.bin OUT.upg         fill the header, write the S-record text
      [--base 0x08020000] [--product 0xE5] [--version 0.1.0]
"""
import argparse
import struct
import sys

TAG_ADDR = 0xBAD0ADD0
APP_BASE = 0x08020000
APP_END = 0x08040000
HEADER_OFF = 0x184
HEADER_LEN = 0x24
PRODUCT = 0xE5
LINE_DATA = 16


def crc16_xmodem(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


class UpgError(Exception):
    pass


def parse_record(line):
    """Return (type, address, data) of one S-record line. Checks the checksum."""
    if len(line) < 10 or line[0] != 'S' or line[1] not in '123':
        raise UpgError('not an S1, S2 or S3 record: %r' % line[:12])
    alen = {'1': 2, '2': 3, '3': 4}[line[1]]
    try:
        raw = bytes.fromhex(line[2:])
    except ValueError:
        raise UpgError('bad hex in record %r' % line[:12])
    count = raw[0]
    if count != len(raw) - 1:
        raise UpgError('record length %d does not match count %d' % (len(raw) - 1, count))
    if sum(raw) & 0xFF != 0xFF:
        raise UpgError('bad checksum in record %r' % line[:12])
    addr = int.from_bytes(raw[1:1 + alen], 'big')
    data = raw[1 + alen:-1]
    return int(line[1]), addr, data


def make_record(addr, data):
    body = bytes([len(data) + 5]) + addr.to_bytes(4, 'big') + bytes(data)
    cs = 0xFF - (sum(body) & 0xFF)
    return 'S3' + (body + bytes([cs])).hex().upper()


def read_upg(path):
    """Return (product_codes, [(addr, data)...]) of a .upg file."""
    with open(path, 'rb') as f:
        text = f.read()
    try:
        text = text.decode('ascii')
    except UnicodeDecodeError:
        raise UpgError('not an ASCII file')
    lines = [l for l in text.replace('\r', '\n').split('\n') if l]
    if not lines:
        raise UpgError('empty file')
    recs = [parse_record(l) for l in lines]
    t, addr, data = recs[0]
    if addr != TAG_ADDR or data[:2] != b'DM':
        raise UpgError('first record is not the application tag')
    codes = list(data[2:])
    return codes, [(a, d) for _, a, d in recs[1:]]


def image_from_records(recs):
    """Return (base, bytes) of the flash content. Gaps are 0xFF."""
    if not recs:
        raise UpgError('no data records')
    lo = min(a for a, d in recs)
    hi = max(a + len(d) for a, d in recs)
    img = bytearray(b'\xff' * (hi - lo))
    for a, d in recs:
        img[a - lo:a - lo + len(d)] = d
    return lo, bytes(img)


def header_fields(img, base):
    h = img[HEADER_OFF:HEADER_OFF + HEADER_LEN]
    if len(h) < HEADER_LEN:
        raise UpgError('image too short for the header')
    crc, crc_ptr, start, end = struct.unpack('<4I', h[:16])
    tag = h[16:28]
    major, minor, build, product = struct.unpack('<4H', h[28:36])
    return dict(crc=crc, crc_ptr=crc_ptr, start=start, end=end, tag=tag,
                major=major, minor=minor, build=build, product=product)


def check_image(img, base, codes=None):
    """Return a list of (name, ok, text) checks."""
    out = []
    out.append(('base', base == APP_BASE, '0x%08X' % base))
    out.append(('range', base >= APP_BASE and base + len(img) <= APP_END,
                '0x%08X..0x%08X (%d bytes)' % (base, base + len(img) - 1, len(img))))
    sp, pc = struct.unpack('<2I', img[:8])
    out.append(('stack pointer', 0x20000000 < sp <= 0x20018000, '0x%08X' % sp))
    out.append(('reset vector', base <= pc < base + len(img) and pc & 1, '0x%08X' % pc))
    if codes is not None:
        out.append(('product tag', PRODUCT in codes, ' '.join('0x%02X' % c for c in codes)))
    try:
        f = header_fields(img, base)
    except UpgError as e:
        out.append(('header', False, str(e)))
        return out
    out.append(('header tag', f['tag'][:5] == b'CSIGN', repr(f['tag'].rstrip(b'\0'))))
    out.append(('crc pointer', f['crc_ptr'] == base + HEADER_OFF, '0x%08X' % f['crc_ptr']))
    out.append(('crc start', f['start'] == base + HEADER_OFF + 4, '0x%08X' % f['start']))
    end_ok = f['start'] < f['end'] < base + len(img)
    out.append(('crc end', end_ok, '0x%08X (image end 0x%08X)' % (f['end'], base + len(img) - 1)))
    if end_ok and f['start'] >= base:
        calc = crc16_xmodem(img[f['start'] - base:f['end'] - base + 1])
        out.append(('crc', calc == f['crc'], 'header 0x%04X, computed 0x%04X' % (f['crc'], calc)))
    out.append(('product', f['product'] == PRODUCT, '0x%02X' % f['product']))
    out.append(('version', True, '%d.%d.%d' % (f['major'], f['minor'], f['build'])))
    return out


def fill_header(img, base, version=None, product=None):
    """Return the image with the CRC range and the CRC filled in."""
    img = bytearray(img)
    f = header_fields(img, base)
    if f['tag'][:5] != b'CSIGN':
        raise UpgError('no CSIGN header at 0x%08X' % (base + HEADER_OFF))
    start = base + HEADER_OFF + 4
    end = base + len(img) - 1
    if version is not None:
        major, minor, build = version
        struct.pack_into('<3H', img, HEADER_OFF + 0x1C, major, minor, build)
    if product is not None:
        struct.pack_into('<H', img, HEADER_OFF + 0x22, product)
    struct.pack_into('<3I', img, HEADER_OFF + 4, base + HEADER_OFF, start, end)
    crc = crc16_xmodem(img[start - base:end - base + 1])
    struct.pack_into('<I', img, HEADER_OFF, crc)
    return bytes(img)


def write_upg(path, img, base, codes):
    lines = [make_record(TAG_ADDR, b'DM' + bytes(codes))]
    for off in range(0, len(img), LINE_DATA):
        lines.append(make_record(base + off, img[off:off + LINE_DATA]))
    with open(path, 'w', newline='') as f:
        for l in lines:
            f.write(l + '\r\n')
    return len(lines)


def parse_version(s):
    parts = s.split('.')
    if len(parts) != 3:
        raise argparse.ArgumentTypeError('version is MAJOR.MINOR.BUILD')
    return tuple(int(p) for p in parts)


def cmd_info(args, verify_only=False):
    codes, recs = read_upg(args.upg)
    base, img = image_from_records(recs)
    checks = check_image(img, base, codes)
    if not verify_only:
        print('%s: %d records, %d data bytes' % (args.upg, len(recs) + 1, sum(len(d) for a, d in recs)))
    bad = 0
    for name, ok, text in checks:
        bad += not ok
        if not verify_only or not ok:
            print('  %-14s %s  %s' % (name, 'ok ' if ok else 'BAD', text))
    if verify_only:
        print('%s: %s' % (args.upg, 'ok' if not bad else '%d checks failed' % bad))
    return 1 if bad else 0


def cmd_unpack(args):
    codes, recs = read_upg(args.upg)
    base, img = image_from_records(recs)
    with open(args.bin, 'wb') as f:
        f.write(img)
    print('%s: %d bytes from 0x%08X, product codes %s' % (
        args.bin, len(img), base, ' '.join('0x%02X' % c for c in codes)))
    return 0


def cmd_pack(args):
    with open(args.bin, 'rb') as f:
        img = f.read()
    if len(img) % 4:
        img += b'\xff' * (4 - len(img) % 4)
    img = fill_header(img, args.base, args.version, args.product)
    checks = check_image(img, args.base, [args.product])
    bad = [c for c in checks if not c[1]]
    if bad:
        for name, ok, text in bad:
            print('  %-14s BAD  %s' % (name, text), file=sys.stderr)
        raise UpgError('image checks failed, nothing written')
    n = write_upg(args.upg, img, args.base, [args.product])
    f = header_fields(img, args.base)
    print('%s: %d records, %d bytes, version %d.%d.%d, crc 0x%04X' % (
        args.upg, n, len(img), f['major'], f['minor'], f['build'], f['crc']))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = p.add_subparsers(dest='cmd', required=True)
    a = sub.add_parser('info'); a.add_argument('upg')
    a = sub.add_parser('verify'); a.add_argument('upg')
    a = sub.add_parser('unpack'); a.add_argument('upg'); a.add_argument('bin')
    a = sub.add_parser('pack'); a.add_argument('bin'); a.add_argument('upg')
    a.add_argument('--base', type=lambda s: int(s, 0), default=APP_BASE)
    a.add_argument('--product', type=lambda s: int(s, 0), default=PRODUCT)
    a.add_argument('--version', type=parse_version, default=None)
    args = p.parse_args(argv)
    try:
        if args.cmd == 'info':
            return cmd_info(args)
        if args.cmd == 'verify':
            return cmd_info(args, verify_only=True)
        if args.cmd == 'unpack':
            return cmd_unpack(args)
        if args.cmd == 'pack':
            return cmd_pack(args)
    except (UpgError, OSError) as e:
        print('tsx-upg: %s' % e, file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
