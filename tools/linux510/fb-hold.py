#!/usr/bin/env python3
"""Keep /dev/fb0 open (closing it turns DECON/DSIM off), unblank it, draw colour bars, pan."""
import fcntl, mmap, os, struct, sys, time
import numpy as np
fd = os.open('/dev/fb0', os.O_RDWR)
fcntl.ioctl(fd, 0x4611, 0)                      # FBIOBLANK, FB_BLANK_UNBLANK
var = bytearray(160); fcntl.ioctl(fd, 0x4600, var)   # FBIOGET_VSCREENINFO
xres, yres, xv, yv = struct.unpack_from('4I', var)
stride = int(open('/sys/class/graphics/fb0/stride').read())
bars = [(255,255,255),(255,255,0),(0,255,255),(0,255,0),(255,0,255),(255,0,0),(0,0,255),(0,0,0)]
img = np.zeros((yres, xres, 4), np.uint8); img[..., 3] = 255
for i, (r, g, b) in enumerate(bars):
    img[:, i*xres//8:(i+1)*xres//8, :3] = (b, g, r)
img[:8, :, :3] = img[-8:, :, :3] = img[:, :8, :3] = img[:, -8:, :3] = 255
m = mmap.mmap(fd, stride * yv, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
for y0 in (0, yres):                            # both buffers
    m[y0*stride:(y0+yres)*stride] = img.tobytes()
struct.pack_into('II', var, 16, 0, 0)           # xoffset, yoffset
fcntl.ioctl(fd, 0x4606, var)                    # FBIOPAN_DISPLAY
print('fb0 %dx%d unblanked, bars drawn; holding it open' % (xres, yres), flush=True)
while True:
    time.sleep(3600)
