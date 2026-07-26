#!/usr/bin/env python3
"""Convert a binary PPM (P6) screendump to PNG using only the stdlib."""
import struct
import sys
import zlib


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        data = f.read()

    # parse P6 header: magic, whitespace/comments, w, h, maxval
    tokens = []
    i = 0
    while len(tokens) < 4 and i < len(data):
        while i < len(data) and data[i:i+1].isspace():
            i += 1
        if data[i:i+1] == b'#':
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        start = i
        while i < len(data) and not data[i:i+1].isspace():
            i += 1
        tokens.append(data[start:i])
    i += 1  # single whitespace after maxval

    assert tokens[0] == b'P6', tokens[0]
    w, h = int(tokens[1]), int(tokens[2])
    raw = data[i:i + w * h * 3]

    # PNG: 8-bit RGB
    def chunk(tag, payload):
        c = struct.pack('>I', len(payload)) + tag + payload
        return c + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)

    scan = bytearray()
    stride = w * 3
    for y in range(h):
        scan.append(0)
        scan += raw[y * stride:(y + 1) * stride]

    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(scan), 6))
    png += chunk(b'IEND', b'')

    with open(dst, 'wb') as f:
        f.write(png)
    print(f'{dst}: {w}x{h}')


if __name__ == '__main__':
    main()
