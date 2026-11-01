#!/usr/bin/env python3
"""Capture RAW10 frames from the OV5647 through the Pablo (fimc-is) sensor nodes, no ISP.

Two V4L2 MPLANE nodes are used (sensor instance 1):
  /dev/video102  exynos-is-ss1   group *leader*, OUTPUT queue: image plane + meta plane
                                 (camera2_shot_ext); drives the sensor group frame by frame
  /dev/video214  ss1vc0          VC0 capture node, CAPTURE queue: the RAW10 frames (CSIS WDMA)
The nodes do not implement ENUM_FMT, so v4l2-ctl refuses them; the ioctls are called directly.

Sequence (each step was needed, see docs/E85096-LINUX510-BOOT.md 9.5-9.7):
  leader: S_INPUT(position 1, vindex 2 = SS1, leader) -> S_CTRL(IS_S_SENSOR_SIZE w<<16|h) ->
          S_FMT(SBGGR10) -> REQBUFS(DMABUF)
  vc0:    S_INPUT -> S_FMT(SBGGR10) -> REQBUFS(DMABUF)
  STREAMON vc0, STREAMON leader, then QBUF (QBUF before STREAMON hangs the group thread)
  leader meta: shot.magicNumber + node_group.capture[0] = {vid 114 (SS1VC0), request 1, crop}
  S_CTRL(IS_S_STREAM 1): sensor stream on; DQBUF vc0 frames; S_STREAM 0; STREAMOFF.
Buffers are dma-bufs from /dev/dma_heap/system-uncached (the Pablo QBUF path wants dma-buf fds).

    ov5647-cap.py [-W 640] [-H 480] [-n 5] [-o /tmp/ov5647]   -> /tmp/ov5647_<i>.raw
Each file is one VC0 frame: packed RAW10, stride ALIGN(width*5/4, 16) (raw10-view.py shows it).
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

VIDIOC_S_FMT = 0xc0d05605
VIDIOC_REQBUFS = 0xc0145608
VIDIOC_QUERYBUF = 0xc0585609
VIDIOC_QBUF = 0xc058560f
VIDIOC_DQBUF = 0xc0585611
VIDIOC_STREAMON = 0x40045612
VIDIOC_STREAMOFF = 0x40045613
VIDIOC_S_INPUT = 0xc0045627
VIDIOC_S_CTRL = 0xc008561c
CID_IS_S_STREAM = 0x009a0000 | 0x1000 | 14         # sensor (front) stream on/off: sstream in bits 3:0
CID_IS_S_SENSOR_SIZE = 0x009a0000 | 0x1000 | 69    # sensor mode is looked up from this, not S_FMT
TYPE_CAPTURE, TYPE_OUTPUT = 9, 10                  # V4L2_BUF_TYPE_VIDEO_{CAPTURE,OUTPUT}_MPLANE
MEMORY_DMABUF = 4
PIX_FMT_SBGGR10 = 0x30314742                       # 'BG10'
INPUT = (1 << 16) | (2 << 8) | 1                   # position 1 (FRONT), vindex 2 (SS1), leader
VID_SS1, VID_SS1VC0 = 2, 114                       # IS_VIDEO_SS1_NUM, IS_VIDEO_SS1VC0_NUM

# camera2_shot_ext layout (fimc-is.ko DWARF, llvm-dwarfdump --name=camera2_shot_ext ...)
SHOT_MAGIC = 0x92345678                            # vendor/default/v8_20_0/is-metadata-vendor.h
SHOT_MAGIC_OFFSET = 0xb38 + 0xbc98                 # shot_ext.shot @0xb38, shot.magicNumber @0xbc98
NODE_GROUP = 0x58                                  # shot_ext.node_group: leader @+0, capture[] @+0x30
NODE_SIZE = 0x30                                   # camera2_node: vid, request, in crop[4], out crop[4], pixfmt, pixsize

DMA_HEAP_IOCTL_ALLOC = 0xc0184800


def log(msg):
    up = open('/proc/uptime').read().split()[0]     # same clock as dmesg, roughly
    print('[%8s] %s' % (up, msg), flush=True)


ap = argparse.ArgumentParser()
ap.add_argument('-W', type=int, default=640)
ap.add_argument('-H', type=int, default=480)
ap.add_argument('-n', type=int, default=5, help='frames to save')
ap.add_argument('-b', type=int, default=4, help='VC0 buffers')
ap.add_argument('-B', type=int, default=16,
                help='leader buffers: in bypass they are never completed (no 3AA), so one leader\n'
                     'buffer = one VC0 frame; queue as many as frames wanted')
ap.add_argument('-o', default='/tmp/ov5647')
ap.add_argument('-t', type=float, default=3.0, help='timeout per frame, s')
ap.add_argument('-v', action='store_true', help='log every ioctl in the loop')
a = ap.parse_args()

heap = os.open('/dev/dma_heap/system-uncached', os.O_RDWR)


def heap_alloc(size):
    d = bytearray(24)
    struct.pack_into('QII', d, 0, (size + 4095) & ~4095, 0, os.O_RDWR | os.O_CLOEXEC)
    fcntl.ioctl(heap, DMA_HEAP_IOCTL_ALLOC, d)
    return struct.unpack_from('I', d, 8)[0]


class Node:
    def __init__(self, path, btype, name):
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        self.type, self.name, self.bufs = btype, name, []

    def s_input(self, value):
        fcntl.ioctl(self.fd, VIDIOC_S_INPUT, struct.pack('I', value))

    def s_fmt(self, w, h):
        # struct v4l2_format: type, union @8 = v4l2_pix_format_mplane (w h pixfmt field colorspace ...)
        fmt = bytearray(208)
        struct.pack_into('I', fmt, 0, self.type)
        struct.pack_into('5I', fmt, 8, w, h, PIX_FMT_SBGGR10, 1, 0)
        fcntl.ioctl(self.fd, VIDIOC_S_FMT, fmt)

    def buffer(self, index, planes, n):
        # struct v4l2_buffer (arm64): index type bytesused flags field | timestamp @24 | timecode @40 |
        # sequence @56 memory @60 | m.planes @64 | length @72 | ... 88 bytes
        b = bytearray(88)
        struct.pack_into('2I', b, 0, index, self.type)
        struct.pack_into('I', b, 60, MEMORY_DMABUF)
        struct.pack_into('Q', b, 64, ctypes.addressof(ctypes.c_char.from_buffer(planes)))
        struct.pack_into('I', b, 72, n)
        return b

    def reqbufs(self, count):
        req = bytearray(20)
        struct.pack_into('3I', req, 0, count, self.type, MEMORY_DMABUF)
        fcntl.ioctl(self.fd, VIDIOC_REQBUFS, req)
        n = struct.unpack_from('I', req, 0)[0]
        probe = bytearray(64 * 8)               # struct v4l2_plane is 64 bytes
        qb = self.buffer(0, probe, 8)
        fcntl.ioctl(self.fd, VIDIOC_QUERYBUF, qb)
        self.nplanes = struct.unpack_from('I', qb, 72)[0]
        self.lengths = [struct.unpack_from('I', probe, p * 64 + 4)[0] for p in range(self.nplanes)]
        for i in range(n):
            planes = []
            for length in self.lengths:
                # 2x headroom: the CSIS WDMA wrote ~1 KiB past a 640x480 RAW10 plane
                # ("SysMMU WRITE PAGE FAULT" just after the buffer); the IOMMU maps the whole dma-buf
                size = length * 2
                dfd = heap_alloc(size)
                planes.append((dfd, mmap.mmap(dfd, size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)))
            self.bufs.append(planes)
        log('%s: %d buffers, %d plane(s) %s' % (self.name, n, self.nplanes, self.lengths))

    def qbuf(self, i):
        pl = bytearray(64 * self.nplanes)       # must stay alive during the ioctl (holds its address)
        for p, (dfd, _) in enumerate(self.bufs[i]):
            struct.pack_into('IIi', pl, p * 64, self.lengths[p], self.lengths[p], dfd)  # bytesused length m.fd
        fcntl.ioctl(self.fd, VIDIOC_QBUF, self.buffer(i, pl, self.nplanes))

    def dqbuf(self):
        pl = bytearray(64 * self.nplanes)
        b = self.buffer(0, pl, self.nplanes)
        fcntl.ioctl(self.fd, VIDIOC_DQBUF, b)
        return struct.unpack_from('I', b, 0)[0], struct.unpack_from('I', b, 12)[0], struct.unpack_from('I', pl, 0)[0]

    def stream(self, on):
        fcntl.ioctl(self.fd, VIDIOC_STREAMON if on else VIDIOC_STREAMOFF, struct.pack('I', self.type))


def node_entry(meta, off, vid, w, h):
    struct.pack_into('2I4I4I2I', meta, off, vid, 1, 0, 0, w, h, 0, 0, w, h, PIX_FMT_SBGGR10, 10)


ldr = Node('/dev/video102', TYPE_OUTPUT, 'ss1 (leader)')
ldr.s_input(INPUT)
fcntl.ioctl(ldr.fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_SENSOR_SIZE, (a.W << 16) | a.H))
ldr.s_fmt(a.W, a.H)
vc0 = Node('/dev/video214', TYPE_CAPTURE, 'ss1vc0')
vc0.s_input(0)
vc0.s_fmt(a.W, a.H)
log('S_INPUT 0x%x, sensor size %dx%d, S_FMT BG10 on both nodes' % (INPUT, a.W, a.H))
ldr.reqbufs(a.B)
vc0.reqbufs(a.b)

for i, planes in enumerate(ldr.bufs):
    meta = planes[-1][1]
    meta[SHOT_MAGIC_OFFSET:SHOT_MAGIC_OFFSET + 4] = struct.pack('<I', SHOT_MAGIC)
    node_entry(meta, NODE_GROUP, VID_SS1, a.W, a.H)                     # leader
    node_entry(meta, NODE_GROUP + NODE_SIZE, VID_SS1VC0, a.W, a.H)      # capture[0] = VC0
for planes in vc0.bufs:
    planes[0][1][:4096] = b'\x5a' * 4096       # marker: still 0x5a after DQBUF = DMA never wrote

vc0.stream(True)
ldr.stream(True)
log('STREAMON vc0 + leader')
for i in range(len(vc0.bufs)):
    vc0.qbuf(i)
for i in range(len(ldr.bufs)):
    ldr.qbuf(i)
fcntl.ioctl(ldr.fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_STREAM, 1))
log('queued, sensor stream on')

saved = 0
t0 = last = time.time()
try:
    while saved < a.n:
        # the OUTPUT (leader) fd can poll writable without a finished buffer: time out on the clock
        if time.time() - last > a.t:
            log('timeout: no VC0 frame in %.1f s' % a.t)
            break
        r, w, _x = select.select([vc0.fd], [ldr.fd], [], 0.2)
        if not w:
            time.sleep(0.01)
        if ldr.fd in w:                          # leader (meta) buffer back: requeue it
            try:
                a.v and log('ldr dqbuf')
                idx, flags, _ = ldr.dqbuf()
                a.v and log('ldr qbuf %d (flags 0x%x)' % (idx, flags))
                ldr.qbuf(idx)
            except BlockingIOError:
                time.sleep(0.005)
        if vc0.fd in r:
            try:
                idx, flags, used = vc0.dqbuf()
            except BlockingIOError:
                continue
            data = vc0.bufs[idx][0][1][:]           # whole allocation (2x the plane) to see overruns
            name = '%s_%d.raw' % (a.o, saved)
            open(name, 'wb').write(data)
            marker = data[:4096] == b'\x5a' * 4096
            log('vc0 frame %d: buf %d flags 0x%x bytesused %d -> %s%s' % (
                saved, idx, flags, used, name, '  (marker intact: no data)' if marker else ''))
            saved += 1
            last = time.time()
            vc0.bufs[idx][0][1][:4096] = b'\x5a' * 4096
            a.v and log('vc0 qbuf %d' % idx)
            vc0.qbuf(idx)
            a.v and log('vc0 qbuf done')
finally:
    try:
        fcntl.ioctl(ldr.fd, VIDIOC_S_CTRL, struct.pack('Ii', CID_IS_S_STREAM, 0))
    except OSError as e:
        log('S_STREAM off: %s' % e)
    for n in (ldr, vc0):
        try:
            n.stream(False)
        except OSError as e:
            log('%s STREAMOFF: %s' % (n.name, e))
    log('stopped after %.1f s, %d VC0 frame(s)' % (time.time() - t0, saved))
sys.exit(0 if saved else 1)
