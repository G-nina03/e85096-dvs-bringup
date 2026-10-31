#!/usr/bin/env python3
"""Draw colour bars and a white border on /dev/fb0 (E850-96 HDMI test).

    fb-test.py [/dev/fbN]
Reads the geometry from sysfs, so it works for any resolution and 16/24/32 bpp.
"""
import mmap
import os
import sys

import numpy as np

dev = sys.argv[1] if len(sys.argv) > 1 else '/dev/fb0'
name = dev.rsplit('/', 1)[-1]
sysfs = '/sys/class/graphics/' + name
w, h = map(int, open(sysfs + '/virtual_size').read().strip().split(','))
bpp = int(open(sysfs + '/bits_per_pixel').read())
stride = int(open(sysfs + '/stride').read())
print(f'{dev}: {w}x{h} (virtual), {bpp} bpp, stride {stride}')

bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
rgb = np.zeros((h, w, 3), np.uint8)
for i, c in enumerate(bars):
    rgb[:, i * w // 8:(i + 1) * w // 8] = c
rgb[:8], rgb[-8:], rgb[:, :8], rgb[:, -8:] = 255, 255, 255, 255

if bpp == 32:      # assume XRGB8888 (B, G, R, X in memory)
    px = np.dstack([rgb[..., 2], rgb[..., 1], rgb[..., 0], np.full((h, w), 255, np.uint8)])
elif bpp == 24:
    px = rgb[..., ::-1]
else:              # RGB565
    v = ((rgb[..., 0].astype(np.uint16) >> 3) << 11) | ((rgb[..., 1].astype(np.uint16) >> 2) << 5) | \
        (rgb[..., 2].astype(np.uint16) >> 3)
    px = v.view(np.uint8).reshape(h, w, 2)
rows = px.reshape(h, -1)
pad = stride - rows.shape[1]
if pad > 0:
    rows = np.hstack([rows, np.zeros((h, pad), np.uint8)])
# the Exynos DECON fb has no write(); map it instead (both buffers of the virtual 1920x2160)
fd = os.open(dev, os.O_RDWR)
size = stride * h
m = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
m[:size] = rows.tobytes()
m.close()
os.close(fd)
print('colour bars written')
