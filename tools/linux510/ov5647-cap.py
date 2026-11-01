#!/usr/bin/env python3
"""Capture RAW10 frames from the OV5647 through the Pablo (fimc-is) sensor node.

The Exynos sensor video node (/dev/video102 = exynos-is-ss1 for sensor instance 1) is a
V4L2 MPLANE node (OUTPUT type: it leads the sensor group), but it does not implement ENUM_FMT, so v4l2-ctl refuses it.
This calls the ioctls directly:
  S_INPUT(position 1, vindex 2 = IS_VIDEO_SS1_NUM, leader 1) -> S_CTRL(S_SENSOR_SIZE) ->
  S_FMT(SBGGR10) ->
  REQBUFS(DMABUF, buffers from /dev/dma_heap/system-uncached) -> STREAMON -> QBUF ->
  S_CTRL(S_STREAM on) -> DQBUF x N -> STREAMOFF

    ov5647-cap.py [-W 640] [-H 480] [-n 5] [-o /tmp/ov5647]   -> /tmp/ov5647_<i>.raw
Each file is one plane-0 buffer: packed RAW10, stride ALIGN(width*5/4, 16).
"""
import argparse
import ctypes
import fcntl
import mmap
import os
import select
import struct
import sys
import time

DEV = '/dev/video102'
VIDIOC_S_FMT = 0xc0d05605
VIDIOC_G_FMT = 0xc0d05604
VIDIOC_REQBUFS = 0xc0145608
VIDIOC_QUERYBUF = 0xc0585609
VIDIOC_QBUF = 0xc058560f
VIDIOC_DQBUF = 0xc0585611
VIDIOC_STREAMON = 0x40045612
VIDIOC_STREAMOFF = 0x40045613
VIDIOC_S_INPUT = 0xc0045627
VIDIOC_S_CTRL = 0xc008561c
CID_IS_S_SENSOR_SIZE = 0x009a0000 | 0x1000 | 69   # V4L2_CID_FIMC_IS_BASE + 69
CID_IS_S_STREAM = 0x009a0000 | 0x1000 | 14        # sensor (front) stream on/off: sstream in bits 3:0
# the sensor node is a group *leader*: its vb2 queue is OUTPUT_MPLANE (is-video.c queue_init)
BUF_TYPE = 10                           # V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
PIX_FMT_SBGGR10 = 0x30314742            # 'BG10'
INPUT = (1 << 16) | (2 << 8) | 1        # position 1 (FRONT), vindex 2 (SS1), leader
# The leader's meta plane holds struct camera2_shot_ext; the group checks shot.magicNumber.
# Offsets from the fimc-is.ko DWARF (llvm-dwarfdump): shot_ext.shot @0xb38, shot.magicNumber @0xbc98
SHOT_MAGIC = 0x92345678                 # vendor/default/v8_20_0/is-metadata-vendor.h
SHOT_MAGIC_OFFSET = 0xb38 + 0xbc98      # = 51152 in the 51200-byte meta plane


def log(msg):
    up = open('/proc/uptime').read().split()[0]      # same clock as dmesg, roughly
    print('[%8s] %s' % (up, msg), flush=True)


ap = argparse.ArgumentParser()
ap.add_argument('-W', type=int, default=640)
ap.add_argument('-H', type=int, default=480)
ap.add_argument('-n', type=int, default=5, help='frames to save')
ap.add_argument('-b', type=int, default=4, help='buffers')
ap.add_argument('-o', default='/tmp/ov5647')
ap.add_argument('-s', type=int, default=0,
                help='sensor scenario: 0 normal (with modules/ov5647-pablo/bypass.sh), 1 vision/stand-alone (oopses in this BSP)')
ap.add_argument('-t', type=float, default=5.0, help='DQBUF timeout per frame, s')
a = ap.parse_args()

fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK)
inp = INPUT | (a.s << 26)               # scenario: bits 31:26 (SENSOR_SCN_SHIFT)
fcntl.ioctl(fd, VIDIOC_S_INPUT, struct.pack('I', inp))
log('S_INPUT 0x%x ok' % inp)

# the sensor mode is looked up from device->sensor_width/height (this control), not from S_FMT
fcntl.ioctl(fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_SENSOR_SIZE, (a.W << 16) | a.H))
log('S_CTRL sensor size %dx%d ok' % (a.W, a.H))

# struct v4l2_format: type (u32), pad, union at offset 8 (200 bytes)
# v4l2_pix_format_mplane: w h pixfmt field colorspace, plane_fmt[8]{sizeimage, bpl, u16[6]}, num_planes u8 ...
fmt = bytearray(208)
struct.pack_into('I', fmt, 0, BUF_TYPE)
struct.pack_into('5I', fmt, 8, a.W, a.H, PIX_FMT_SBGGR10, 1, 0)   # field NONE
fcntl.ioctl(fd, VIDIOC_S_FMT, fmt)
w, h, pf = struct.unpack_from('3I', fmt, 8)
nplanes = fmt[8 + 20 + 8 * 20]
planes = [struct.unpack_from('2I', fmt, 8 + 20 + i * 20) for i in range(max(nplanes, 1))]
log('S_FMT %dx%d fourcc %s, %d plane(s): %s' % (w, h, pf.to_bytes(4, 'little').decode(errors='replace'),
                                                 nplanes, ['size %d bpl %d' % p for p in planes]))
# the driver does not report num_planes back; ask QUERYBUF (Pablo adds a meta plane)
nplanes = 8

# Buffers are dma-bufs from /dev/dma_heap/system-uncached: the Pablo QBUF path treats every
# plane as a dma-buf fd (dmabuf-container), so MEMORY_MMAP fails ("invalid dmabuf fd(0)").
MEMORY_DMABUF = 4
DMA_HEAP_IOCTL_ALLOC = 0xc0184800       # struct dma_heap_allocation_data {u64 len; u32 fd, fd_flags; u64 heap_flags}
heap = os.open('/dev/dma_heap/system-uncached', os.O_RDWR)


def heap_alloc(size):
    d = bytearray(24)
    struct.pack_into('QII', d, 0, (size + 4095) & ~4095, 0, os.O_RDWR | os.O_CLOEXEC)
    fcntl.ioctl(heap, DMA_HEAP_IOCTL_ALLOC, d)
    return struct.unpack_from('I', d, 8)[0]


req = bytearray(20)
struct.pack_into('3I', req, 0, a.b, BUF_TYPE, MEMORY_DMABUF)
fcntl.ioctl(fd, VIDIOC_REQBUFS, req)
nbuf = struct.unpack_from('I', req, 0)[0]
log('REQBUFS %d (DMABUF)' % nbuf)


def v4l2_buffer(index, plane_arr, n):
    # struct v4l2_buffer (arm64): index type bytesused flags field | timestamp(16) @24 |
    # timecode(16) @40 | sequence @56 memory @60 | m.planes ptr @64 | length @72 | ... 88 bytes
    b = bytearray(88)
    struct.pack_into('2I', b, 0, index, BUF_TYPE)
    struct.pack_into('I', b, 60, MEMORY_DMABUF)
    struct.pack_into('Q', b, 64, ctypes.addressof(ctypes.c_char.from_buffer(plane_arr)))
    struct.pack_into('I', b, 72, n)
    return b


# struct v4l2_plane: bytesused length | m (8, fd for DMABUF) @8 | data_offset @16 | reserved[11] -> 64 B
probe = bytearray(64 * nplanes)
qb = v4l2_buffer(0, probe, nplanes)
fcntl.ioctl(fd, VIDIOC_QUERYBUF, qb)
nplanes = struct.unpack_from('I', qb, 72)[0]
lengths = [struct.unpack_from('I', probe, p * 64 + 4)[0] for p in range(nplanes)]
log('QUERYBUF: %d plane(s), lengths %s' % (nplanes, lengths))

bufs = []                                   # per buffer: [(dmabuf fd, mmap), ...] per plane


def qbuf(i):
    if nplanes > 1:                         # meta plane: valid camera2_shot magic, or the frame is cancelled
        bufs[i][-1][1][SHOT_MAGIC_OFFSET:SHOT_MAGIC_OFFSET + 4] = struct.pack('<I', SHOT_MAGIC)
    pl = bytearray(64 * nplanes)            # must stay alive during the ioctl (holds its address)
    for p, (dfd, _) in enumerate(bufs[i]):
        struct.pack_into('IIi', pl, p * 64, lengths[p], lengths[p], dfd)   # bytesused, length, m.fd
    fcntl.ioctl(fd, VIDIOC_QBUF, v4l2_buffer(i, pl, nplanes))


for i in range(nbuf):
    planes = []
    for p in range(nplanes):
        dfd = heap_alloc(lengths[p])
        planes.append((dfd, mmap.mmap(dfd, lengths[p], mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)))
    bufs.append(planes)
    bufs[i][0][1][:4096] = b'\x5a' * 4096   # marker: still 0x5a after DQBUF = DMA never wrote
log('allocated %d buffers' % nbuf)

# STREAMON *before* QBUF: buffers queued earlier are handed to the driver ahead of start_streaming,
# their group shots run before is_group_start(), which then sema_init()s the group's
# smp_resource under a waiting shot -> the group thread never wakes up (no frame ever done).
fcntl.ioctl(fd, VIDIOC_STREAMON, struct.pack('I', BUF_TYPE))
log('STREAMON')
for i in range(nbuf):
    qbuf(i)
log('queued %d buffers' % nbuf)
# STREAMON starts the CSIS/DMA side ("back start"); the sensor itself streams on S_STREAM
fcntl.ioctl(fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_STREAM, 1))
log('S_CTRL S_STREAM on')
saved = 0
t0 = time.time()
try:
    while saved < a.n:
        # OUTPUT queue: a finished buffer shows up as POLLOUT (writable)
        r, w, _x = select.select([fd], [fd], [], a.t)
        if not r and not w:
            log('timeout: no frame in %.1f s' % a.t)
            break
        pl = bytearray(64 * nplanes)
        b = v4l2_buffer(0, pl, nplanes)
        try:
            fcntl.ioctl(fd, VIDIOC_DQBUF, b)
        except BlockingIOError:
            time.sleep(0.05)
            continue
        idx, flags, seq = struct.unpack_from('I', b, 0)[0], struct.unpack_from('I', b, 12)[0], \
            struct.unpack_from('I', b, 56)[0]
        data = bufs[idx][0][1][:lengths[0]]
        name = '%s_%d.raw' % (a.o, saved)
        open(name, 'wb').write(data)
        marker = data[:4096] == b'\x5a' * 4096
        log('frame %d: buf %d seq %d flags 0x%x -> %s%s' % (saved, idx, seq, flags, name,
                                                           '  (marker intact: no data)' if marker else ''))
        saved += 1
        bufs[idx][0][1][:4096] = b'\x5a' * 4096
        qbuf(idx)
finally:
    try:
        fcntl.ioctl(fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_STREAM, 0))
    except OSError as e:
        log('S_STREAM off: %s' % e)
    fcntl.ioctl(fd, VIDIOC_STREAMOFF, struct.pack('I', BUF_TYPE))
    log('STREAMOFF after %.1f s, %d frame(s)' % (time.time() - t0, saved))
sys.exit(0 if saved else 1)
