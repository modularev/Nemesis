#!/usr/bin/env python3
"""Saves the Nemesis display as a PNG: asks the firmware for SCREEN (the
SSD1306 framebuffer, 128 x 32) over USB serial and writes it enlarged.

usage: screen.py [out.png] [-p port] [-s scale]
"""
import argparse, struct, sys, time, zlib
import serial

ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
ap.add_argument('out', nargs='?', default='screen.png')
ap.add_argument('-p', '--port', default='/dev/cu.usbmodem103226001')
ap.add_argument('-s', '--scale', type=int, default=4)
a = ap.parse_args()

s = serial.Serial(a.port, timeout=0.1)
time.sleep(0.2); s.read(100000)
s.write(b'SCREEN\n')
line, t = '', time.time()
while time.time() - t < 3 and not line:
    for l in s.read(20000).decode(errors='replace').splitlines():
        if l.startswith('SCREEN '):
            line = l[7:].strip()
if not line or line == 'none':
    sys.exit('no screen (rev1 has no display, or no answer)')
buf = bytes.fromhex(line)
W, H, k = 128, 32, a.scale
px = [[(buf[x + 128 * (y // 8)] >> (y % 8)) & 1 for x in range(W)] for y in range(H)]
rows = b''
for y in range(H * k):
    r = bytes(255 if px[y // k][x // k] else 0 for x in range(W * k))
    rows += b'\0' + r
def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W * k, H * k, 8, 0, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
open(a.out, 'wb').write(png)
print('saved', a.out)
