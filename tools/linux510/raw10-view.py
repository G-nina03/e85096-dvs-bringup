#!/usr/bin/env python3
"""Turn an OV5647 RAW10 frame (from ov5647-cap.py) into an RGB image; save it and show it on HDMI.

    raw10-view.py /tmp/ov5647_2.raw [-W 640] [-H 480] [--no-fb] [-o out.ppm]

Input (Bayer BGGR), either
  --format u16 (default): what the E850 CSIS VC0 DMA writes for RAW10 here - one little-endian
                          16-bit word per pixel, 10 valid bits, stride width * 2
  --format packed:        MIPI CSI-2 packed RAW10 (4 pixels in 5 bytes), stride ALIGN(w*5/4, 16)
Demosaic: one RGB pixel per 2x2 Bayer cell (half resolution), then nearest-neighbour 2x back up,
a simple grey-world white balance and gamma 2.2. Only numpy is needed (no PIL on the board).
"""
import argparse
import fcntl
import mmap
import os
import struct

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('raw')
ap.add_argument('-W', type=int, default=640)
ap.add_argument('-H', type=int, default=480)
ap.add_argument('-o', default=None, help='output .ppm (default: <raw>.ppm)')
ap.add_argument('--no-fb', action='store_true', help='do not draw on /dev/fb0')
ap.add_argument('--format', choices=['u16', 'packed'], default='u16')
ap.add_argument('--gain', type=float, default=0, help='fixed gain instead of auto (99.5 percentile)')
a = ap.parse_args()

w, h = a.W, a.H
buf = np.fromfile(a.raw, np.uint8)
if a.format == 'u16':
    stride = w * 2
    bayer = buf[:stride * h].view('<u2').reshape(h, w) & 0x3ff
else:
    stride = ((w * 5 // 4) + 15) & ~15
    lines = buf[:stride * h].reshape(h, stride)[:, :w * 5 // 4].reshape(h, w // 4, 5).astype(np.uint16)
    px = np.empty((h, w // 4, 4), np.uint16)
    for i in range(4):
        px[..., i] = (lines[..., i] << 2) | ((lines[..., 4] >> (2 * i)) & 3)
    bayer = px.reshape(h, w)
print('%s: %dx%d, stride %d, min %d max %d mean %.1f' % (a.raw, w, h, stride, bayer.min(), bayer.max(),
                                                         bayer.mean()))

b = bayer[0::2, 0::2].astype(np.float32)          # BGGR
g = (bayer[0::2, 1::2].astype(np.float32) + bayer[1::2, 0::2]) / 2
r = bayer[1::2, 1::2].astype(np.float32)
rgb = np.dstack([r, g, b]) - 16.0                  # rough black level (RAW10)
rgb = np.clip(rgb, 0, None)
gain = rgb.reshape(-1, 3).mean(0)
rgb *= gain.mean() / np.maximum(gain, 1e-3)        # grey world
rgb /= (1023.0 / a.gain) if a.gain else max(np.percentile(rgb, 99.5), 1.0)
rgb = (np.clip(rgb, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)
rgb = rgb.repeat(2, 0).repeat(2, 1)                # back to w x h

out = a.o or a.raw.rsplit('.', 1)[0] + '.ppm'
with open(out, 'wb') as f:
    f.write(b'P6\n%d %d\n255\n' % (w, h))
    f.write(rgb.tobytes())
print('wrote', out)

if not a.no_fb and os.path.exists('/dev/fb0'):
    fd = os.open('/dev/fb0', os.O_RDWR)
    var = bytearray(160)
    fcntl.ioctl(fd, 0x4600, var)                   # FBIOGET_VSCREENINFO
    xres, yres, xv, yv = struct.unpack_from('4I', var)
    fstride = int(open('/sys/class/graphics/fb0/stride').read())
    m = mmap.mmap(fd, fstride * yv, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    scale = max(1, min(xres // w, yres // h))      # 640x480 -> 2x on 1920x1080
    img = rgb.repeat(scale, 0).repeat(scale, 1)
    ih, iw = img.shape[:2]
    x0, y0 = (xres - iw) // 2, (yres - ih) // 2
    xrgb = np.zeros((ih, iw, 4), np.uint8)
    xrgb[..., 0], xrgb[..., 1], xrgb[..., 2], xrgb[..., 3] = img[..., 2], img[..., 1], img[..., 0], 255
    yoff = struct.unpack_from('I', var, 20)[0]     # yoffset: the buffer on screen
    for y in range(ih):
        o = (yoff + y0 + y) * fstride + x0 * 4
        m[o:o + iw * 4] = xrgb[y].tobytes()
    m.close()
    os.close(fd)
    print('shown on HDMI (%dx scale) at %d,%d' % (scale, x0, y0))
