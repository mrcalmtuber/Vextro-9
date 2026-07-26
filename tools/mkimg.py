#!/usr/bin/env python3
"""Convert a PNG or PPM image into the Socrates .sci format.

.sci is PNG's idea reused: per-row prediction filters followed by one
LZMA stream (see src/sci.h).  The kernel decodes it with the same LZMA
decoder it uses for xz-compressed ZIM clusters.

PNG is decoded here with nothing but Python's own zlib, so the build
needs no Pillow and no opencv.  Supported PNG subset: bit depth 8,
colour types 0 (grey), 2 (RGB), 3 (palette), 4 (grey+alpha) and 6
(RGBA), non-interlaced.  PPM/PGM (P5/P6, maxval 255) also work.

Usage:
    mkimg.py [-o OUT.sci] [--max W H] [--rgba] IN.png
    mkimg.py --info FILE.sci
"""
import argparse
import lzma
import os
import struct
import sys
import zlib

MAGIC = b'SCI\x01'
HDR_SIZE = 20
FILTER_PNG = 1
CODEC_LZMA = 1

MAX_W, MAX_H = 1280, 800          # must match SCI_MAX_W / SCI_MAX_H


# ---------------------------------------------------------------- PNG in

def png_paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def png_unfilter(raw, width, height, bpp):
    stride = width * bpp
    out = bytearray(stride * height)
    pos = 0
    for y in range(height):
        ft = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        base = y * stride
        prev = out[base - stride:base] if y else bytes(stride)
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + png_paeth(a, prev[i], c)) & 0xFF
        elif ft != 0:
            raise SystemExit(f'unsupported PNG row filter {ft}')
        out[base:base + stride] = line
    return out


def read_png(data):
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        return None
    pos = 8
    width = height = depth = ctype = interlace = None
    idat = bytearray()
    palette = None
    trns = None
    while pos + 8 <= len(data):
        length, ctag = struct.unpack('>I4s', data[pos:pos + 8])
        pos += 8
        body = data[pos:pos + length]
        pos += length + 4                      # skip the chunk CRC
        if ctag == b'IHDR':
            width, height, depth, ctype, _, _, interlace = \
                struct.unpack('>IIBBBBB', body)
        elif ctag == b'PLTE':
            palette = body
        elif ctag == b'tRNS':
            trns = body
        elif ctag == b'IDAT':
            idat += body
        elif ctag == b'IEND':
            break

    if width is None:
        raise SystemExit('PNG has no IHDR')
    if depth != 8:
        raise SystemExit(f'PNG bit depth {depth} is not supported (need 8)')
    if interlace:
        raise SystemExit('interlaced PNG is not supported')

    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if channels is None:
        raise SystemExit(f'PNG colour type {ctype} is not supported')

    raw = zlib.decompress(bytes(idat))
    plane = png_unfilter(raw, width, height, channels)

    # normalise everything to RGB or RGBA
    npx = width * height
    if ctype == 2:
        return width, height, 3, bytes(plane)
    if ctype == 6:
        return width, height, 4, bytes(plane)
    if ctype == 0:
        out = bytearray(npx * 3)
        for i in range(npx):
            g = plane[i]
            out[i * 3:i * 3 + 3] = bytes((g, g, g))
        return width, height, 3, bytes(out)
    if ctype == 4:
        out = bytearray(npx * 4)
        for i in range(npx):
            g, a = plane[i * 2], plane[i * 2 + 1]
            out[i * 4:i * 4 + 4] = bytes((g, g, g, a))
        return width, height, 4, bytes(out)
    # palette
    if palette is None:
        raise SystemExit('palette PNG without a PLTE chunk')
    has_alpha = trns is not None
    step = 4 if has_alpha else 3
    out = bytearray(npx * step)
    for i in range(npx):
        idx = plane[i]
        out[i * step:i * step + 3] = palette[idx * 3:idx * 3 + 3]
        if has_alpha:
            out[i * step + 3] = trns[idx] if idx < len(trns) else 255
    return width, height, step, bytes(out)


# ---------------------------------------------------------------- PPM in

def read_pnm(data):
    if data[:2] not in (b'P5', b'P6'):
        return None
    channels = 1 if data[:2] == b'P5' else 3
    fields, pos = [], 2
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b'#':
            while pos < len(data) and data[pos] != 0x0A:
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1                                    # single whitespace byte
    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit('PNM maxval must be 255')
    body = data[pos:pos + width * height * channels]
    if channels == 1:
        out = bytearray(width * height * 3)
        for i in range(width * height):
            g = body[i]
            out[i * 3:i * 3 + 3] = bytes((g, g, g))
        return width, height, 3, bytes(out)
    return width, height, 3, body


# --------------------------------------------------------------- encoding

def nearest_resize(px, w, h, ch, nw, nh):
    out = bytearray(nw * nh * ch)
    for y in range(nh):
        sy = y * h // nh
        srow = sy * w * ch
        drow = y * nw * ch
        for x in range(nw):
            sx = x * w // nw
            s = srow + sx * ch
            out[drow + x * ch: drow + x * ch + ch] = px[s:s + ch]
    return bytes(out)


def filter_plane(px, w, h, ch):
    """PNG adaptive filtering: pick the row filter with the lowest
    absolute-sum, which is the heuristic the PNG spec recommends."""
    stride = w * ch
    out = bytearray()
    prev = bytes(stride)
    for y in range(h):
        row = px[y * stride:(y + 1) * stride]
        best, best_score, best_ft = None, None, 0
        for ft in range(5):
            cand = bytearray(stride)
            for i in range(stride):
                a = row[i - ch] if i >= ch else 0
                b = prev[i]
                c = prev[i - ch] if i >= ch else 0
                if ft == 0:
                    p = 0
                elif ft == 1:
                    p = a
                elif ft == 2:
                    p = b
                elif ft == 3:
                    p = (a + b) >> 1
                else:
                    p = png_paeth(a, b, c)
                cand[i] = (row[i] - p) & 0xFF
            score = sum(v if v < 128 else 256 - v for v in cand)
            if best_score is None or score < best_score:
                best, best_score, best_ft = cand, score, ft
        out.append(best_ft)
        out += best
        prev = row
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('input')
    ap.add_argument('-o', '--output')
    ap.add_argument('--max', nargs=2, type=int, metavar=('W', 'H'),
                    default=[MAX_W, MAX_H],
                    help='downscale to fit inside W x H (default 1280 800)')
    ap.add_argument('--rgba', action='store_true',
                    help='keep the alpha channel instead of flattening it')
    ap.add_argument('--info', action='store_true',
                    help='describe an existing .sci file and exit')
    args = ap.parse_args()

    if args.info:
        d = open(args.input, 'rb').read()
        if d[:4] != MAGIC:
            raise SystemExit('not a .sci file')
        w, h = struct.unpack_from('<HH', d, 4)
        ch, filt, codec = d[8], d[9], d[10]
        raw, comp = struct.unpack_from('<II', d, 12)
        print(f'{args.input}: {w}x{h}, {ch} channels, filter {filt}, '
              f'codec {codec}')
        print(f'  raw {raw} bytes -> {comp} compressed '
              f'({comp * 100 // max(raw, 1)}% of raw, file {len(d)} bytes)')
        return 0

    data = open(args.input, 'rb').read()
    got = read_png(data) or read_pnm(data)
    if got is None:
        raise SystemExit(f'{args.input}: not a PNG or a binary PPM/PGM')
    w, h, ch, px = got

    if not args.rgba and ch == 4:
        flat = bytearray(w * h * 3)
        for i in range(w * h):
            a = px[i * 4 + 3]
            for k in range(3):
                v = px[i * 4 + k]
                flat[i * 3 + k] = (v * a + 255 * (255 - a)) // 255
        px, ch = bytes(flat), 3

    maxw, maxh = args.max
    if w > maxw or h > maxh:
        scale = min(maxw / w, maxh / h)
        nw, nh = max(1, int(w * scale)), max(1, int(h * scale))
        px = nearest_resize(px, w, h, ch, nw, nh)
        print(f'  scaled {w}x{h} -> {nw}x{nh}')
        w, h = nw, nh

    plane = filter_plane(px, w, h, ch)
    comp = lzma.compress(plane, format=lzma.FORMAT_ALONE,
                         filters=[{'id': lzma.FILTER_LZMA1, 'preset': 9}])

    hdr = MAGIC + struct.pack('<HHBBBBII', w, h, ch, FILTER_PNG, CODEC_LZMA,
                              0, len(plane), len(comp))
    assert len(hdr) == HDR_SIZE, len(hdr)

    out = args.output or os.path.splitext(args.input)[0] + '.sci'
    with open(out, 'wb') as f:
        f.write(hdr)
        f.write(comp)

    total = HDR_SIZE + len(comp)
    print(f'{out}: {w}x{h}x{ch}  raw {len(plane)} -> {total} bytes '
          f'({total * 100 // len(plane)}% of raw)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
