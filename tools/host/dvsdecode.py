#!/usr/bin/env python3
"""Walk an S5KRC1S capture (RAW8 container, 32-bit big-endian AER packets, manual Fig. 49/50)
and summarise it: packet types, columns, events, frame-end packets, unknown words.

  dvsdecode.py frame.raw [-W 8192]
"""
import argparse
import struct


def kind(w):
    if w == 0:
        return 'pad'
    if w >> 31:
        return 'mgroup' if (w >> 26) & 0x1f else 'sgroup'
    t = w >> 26
    if t == 0b000001:
        return 'column'
    if t == 0b000011:
        return 'frame_end'
    if t == 0b000010:
        return 'sub_ts' if (w >> 23) & 1 else 'ref_ts'
    return 'unknown'


def events(w):
    if kind(w) == 'sgroup':
        return bin(w & 0xffff).count('1')
    return bin(w & 0xffff).count('1')  # mgroup: group2 [15:8] + group1 [7:0]


def walk(data):
    n = {}
    ev = 0
    cols = set()
    frames = []
    for (w,) in struct.iter_unpack('>I', data[:len(data) & ~3]):
        k = kind(w)
        n[k] = n.get(k, 0) + 1
        if k in ('sgroup', 'mgroup'):
            ev += events(w)
        elif k == 'column':
            cols.add(w & 0x7ff)
        elif k == 'frame_end':
            frames.append((w >> 11) & 0xff)
    return n, ev, cols, frames


def selftest():
    words = [0x08000003, 0x088002ad, 0x04200000, 0x8403ffff, 0x80000001, 0x0c000800, 0]
    n, ev, cols, frames = walk(b''.join(struct.pack('>I', w) for w in words))
    assert n == {'ref_ts': 1, 'sub_ts': 1, 'column': 1, 'mgroup': 1, 'sgroup': 1,
                 'frame_end': 1, 'pad': 1}, n
    assert ev == 17 and cols == {0} and frames == [1], (ev, cols, frames)


if __name__ == '__main__':
    selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument('raw', nargs='+')
    ap.add_argument('-W', type=int, default=8192, help='line length, bytes')
    a = ap.parse_args()
    for f in a.raw:
        d = open(f, 'rb').read()
        lines = [i // a.W for i in range(0, len(d), a.W) if any(d[i:i + a.W])]
        n, ev, cols, frames = walk(d)
        print(f'{f}: {len(d)} B, data lines {len(lines)} ({lines[:1]}..{lines[-1:]})')
        print(f'  packets {n}')
        print(f'  events {ev}, columns {len(cols)} (min {min(cols, default=None)} '
              f'max {max(cols, default=None)}), frame_end frame# {frames[:10]}')
