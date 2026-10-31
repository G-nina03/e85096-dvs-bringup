"""Split large FILL chunks of an Android sparse image.

The E850-96 Android 10 LK hangs on a FILL chunk of 1 GiB (it stopped at
"CHUNK TYPE FILL (... sct: 2097144 ...)"); 64 MiB and 160 MiB fills worked.
img2simg emits one FILL chunk per run of zero blocks, so a mostly empty 3 GB
ext4 image has a few huge ones. This rewrites the image so that no FILL chunk
covers more than --max-mib (default 32 MiB). RAW and DONT_CARE chunks are copied.

    python3 resparse.py rootfs.simg rootfs-split.simg
"""
import argparse
import struct

HDR = struct.Struct('<IHHHHIIII')      # magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, blocks, chunks, crc
CHK = struct.Struct('<HHII')           # type, reserved, chunk_sz (blocks), total_sz (bytes incl. header)
RAW, FILL, DONT_CARE, CRC = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4

ap = argparse.ArgumentParser()
ap.add_argument('src')
ap.add_argument('dst')
ap.add_argument('--max-mib', type=int, default=32)
a = ap.parse_args()

with open(a.src, 'rb') as f, open(a.dst, 'wb') as o:
    magic, major, minor, fhs, chs, blk, blocks, chunks, crc = HDR.unpack(f.read(HDR.size))
    assert magic == 0xED26FF3A and fhs == HDR.size and chs == CHK.size, 'not a v1 sparse image'
    limit = a.max_mib * 1024 * 1024 // blk
    o.write(bytes(HDR.size))                       # header written at the end (chunk count changes)
    out = split = 0
    for _ in range(chunks):
        t, _r, n, total = CHK.unpack(f.read(CHK.size))
        body = f.read(total - CHK.size)
        if t == FILL and n > limit:
            while n:
                k = min(n, limit)
                o.write(CHK.pack(FILL, 0, k, CHK.size + 4) + body)
                n -= k
                out += 1
            split += 1
        elif t == CRC:
            continue                               # CRC chunk no longer valid after rewriting
        else:
            o.write(CHK.pack(t, 0, n, total) + body)
            out += 1
    o.seek(0)
    o.write(HDR.pack(magic, major, minor, fhs, chs, blk, blocks, out, 0))
print(f'{chunks} chunks -> {out}; {split} FILL chunks split to <= {a.max_mib} MiB')
