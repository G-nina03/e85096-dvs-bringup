#!/usr/bin/env python3
"""Keep /dev/fb0 open and unblanked (E850-96 HDMI).

Closing the last fd of the Exynos DECON fb runs decon_release -> decon_disable, which turns
DECON/DSIM and the panel power off. The text console (fbcon, tty1) draws on fb0 without
holding a file open, so this process keeps it on.

    fb-hold.py           unblank and hold
    fb-hold.py --bars    also draw colour bars first (HDMI test without fbcon)
"""
import fcntl
import mmap
import os
import struct
import sys
import time

FBIOGET_VSCREENINFO, FBIOPAN_DISPLAY, FBIOBLANK = 0x4600, 0x4606, 0x4611

fd = os.open('/dev/fb0', os.O_RDWR)
fcntl.ioctl(fd, FBIOBLANK, 0)                    # FB_BLANK_UNBLANK
var = bytearray(160)
fcntl.ioctl(fd, FBIOGET_VSCREENINFO, var)
xres, yres, xv, yv = struct.unpack_from('4I', var)

if '--bars' in sys.argv:
    import numpy as np
    stride = int(open('/sys/class/graphics/fb0/stride').read())
    bars = [(255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
            (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0)]
    img = np.zeros((yres, xres, 4), np.uint8)
    img[..., 3] = 255
    for i, (r, g, b) in enumerate(bars):
        img[:, i * xres // 8:(i + 1) * xres // 8, :3] = (b, g, r)    # XRGB8888 in memory: B G R X
    img[:8, :, :3] = img[-8:, :, :3] = img[:, :8, :3] = img[:, -8:, :3] = 255
    m = mmap.mmap(fd, stride * yv, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    for y0 in (0, yres):                          # both buffers
        m[y0 * stride:(y0 + yres) * stride] = img.tobytes()
    struct.pack_into('II', var, 16, 0, 0)         # xoffset, yoffset
# fbcon's pan while DECON was off was dropped ("UNBLANK missed"): pan now so DECON scans fb0 out
fcntl.ioctl(fd, FBIOPAN_DISPLAY, var)

print('fb0 %dx%d unblanked%s; holding it open' % (xres, yres, ', bars drawn' if '--bars' in sys.argv else ''),
      flush=True)
while True:
    time.sleep(3600)
