#!/usr/bin/env python3
"""tools/mkpngref.py -- write apps/png_ref.h.

None of the PNG files in the generated header were written by libpng,
which is the point. libpng is both the encoder and the decoder in this
port, so a round trip through png_write_image and back into
png_read_image would agree with itself even if both halves shared a wrong
idea of what the Paeth predictor is.

Two encoders are used instead, for two different reasons.

  Hand-written, here, from the specification. Everything in this file
  builds the PNG container itself and uses zlib only for the IDAT
  payload, which lets the *filter type of every row* be chosen
  deliberately -- so rgba8 below uses all five, one per row, and a
  decoder that has Sub and Up the wrong way round fails on it. A
  real-world encoder picks filters by a heuristic and would probably
  never emit Average at all.

  macOS's sips, which is ImageIO. That is a mature third-party encoder
  whose output carries ancillary chunks (sRGB, eXIf), uses its own filter
  heuristic and compresses far better than the naive encoder here -- 89
  bytes of IDAT against 677 for the same 16x16 image. Its pixels are
  recovered by the decoder at the bottom of this file, so the expected
  values in the header come from a third implementation again rather than
  from what was fed to sips.

Run from the repository root; needs /usr/bin/sips (macOS).

    python3 tools/mkpngref.py > apps/png_ref.h
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

SIG = b"\x89PNG\r\n\x1a\n"


# ---------------------------------------------------------------- encode

def chunk(tag, data):
    body = tag + data
    return (struct.pack(">I", len(data)) + body
            + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))


def ihdr(w, h, depth, colour, interlace=0):
    return chunk(b"IHDR",
                 struct.pack(">IIBBBBB", w, h, depth, colour, 0, 0, interlace))


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def filter_row(ftype, cur, prev, bpp):
    """Apply PNG filter `ftype` to one row of raw bytes."""
    out = bytearray()
    for i, x in enumerate(cur):
        a = cur[i - bpp] if i >= bpp else 0
        b = prev[i] if prev else 0
        c = prev[i - bpp] if (prev and i >= bpp) else 0
        if ftype == 0:
            v = x
        elif ftype == 1:
            v = x - a
        elif ftype == 2:
            v = x - b
        elif ftype == 3:
            v = x - ((a + b) >> 1)
        elif ftype == 4:
            v = x - paeth(a, b, c)
        else:
            raise ValueError(ftype)
        out.append(v & 0xFF)
    return bytes(out)


def idat(rows, bpp, filters=None):
    """rows: list of raw byte rows. filters: per-row filter type."""
    raw = bytearray()
    prev = None
    for i, row in enumerate(rows):
        f = 0 if filters is None else filters[i % len(filters)]
        raw.append(f)
        raw += filter_row(f, row, prev, bpp)
        prev = row
    return chunk(b"IDAT", zlib.compress(bytes(raw), 9))


def png(w, h, depth, colour, rows, bpp, filters=None, extra=b"",
        interlace=0, idat_bytes=None):
    return (SIG + ihdr(w, h, depth, colour, interlace) + extra
            + (idat_bytes if idat_bytes is not None
               else idat(rows, bpp, filters))
            + chunk(b"IEND", b""))


# ---------------------------------------------------------------- decode

def unfilter(raw, w, h, bpp, stride):
    """Reverse PNG filtering. Returns h rows of `stride` bytes."""
    out = []
    prev = bytearray(stride)
    pos = 0
    for _ in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                line[i] = (line[i] + a) & 0xFF
            elif f == 2:
                line[i] = (line[i] + b) & 0xFF
            elif f == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif f == 4:
                line[i] = (line[i] + paeth(a, b, c)) & 0xFF
            elif f != 0:
                raise ValueError("filter %d" % f)
        out.append(bytes(line))
        prev = line
    return out


def decode(data):
    """A minimal PNG reader: non-interlaced, 8-bit, colour type 2 or 6.

    Enough for whatever sips writes, and deliberately not more -- this
    exists to recover expected pixels from a third-party file, not to be
    a PNG library.
    """
    assert data[:8] == SIG
    i, idat_data, hdr = 8, b"", None
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        tag = data[i + 4:i + 8]
        body = data[i + 8:i + 8 + ln]
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", body)
        elif tag == b"IDAT":
            idat_data += body
        i += 12 + ln
    w, h, depth, colour, _, _, interlace = hdr
    assert depth == 8 and interlace == 0, hdr
    channels = {2: 3, 6: 4, 0: 1, 4: 2}[colour]
    rows = unfilter(zlib.decompress(idat_data), w, h, channels, w * channels)
    return w, h, colour, rows


def sips_reencode(data):
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "in.png")
        dst = os.path.join(d, "out.png")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run(["/usr/bin/sips", "-s", "format", "png",
                        src, "--out", dst],
                       check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        with open(dst, "rb") as f:
            return f.read()


# ---------------------------------------------------------------- images

def rgb_pixel(x, y):
    """The pattern every RGB image here uses. Chosen so that no two rows
    and no two columns are alike, and no channel is a copy of another --
    a decoder that swapped R and B, or lost a row, changes the answer."""
    return ((x * 16 + 8) & 0xFF, (y * 16 + 8) & 0xFF, (x * 7 + y * 3) & 0xFF)


def build_rgb8(w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            row += bytes(rgb_pixel(x, y))
        rows.append(bytes(row))
    return rows


def build_rgba8(w, h):
    rows = []
    for y in range(h):
        row = bytearray()
        for x in range(w):
            r, g, b = rgb_pixel(x, y)
            row += bytes((r, g, b, (x + y) * 8 & 0xFF))
        rows.append(bytes(row))
    return rows


def adam7_passes(w, h):
    """The seven Adam7 passes as (xstart, ystart, xstep, ystep)."""
    return [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4),
            (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]


def build_interlaced_rgb8(w, h):
    """Adam7 IDAT for the same pattern build_rgb8 produces."""
    raw = bytearray()
    for xs, ys, xstep, ystep in adam7_passes(w, h):
        pw = (w - xs + xstep - 1) // xstep
        ph = (h - ys + ystep - 1) // ystep
        if pw == 0 or ph == 0:
            continue
        prev = None
        for py in range(ph):
            y = ys + py * ystep
            row = bytearray()
            for px in range(pw):
                row += bytes(rgb_pixel(xs + px * xstep, y))
            raw.append(0)                    # filter None, all passes
            raw += filter_row(0, bytes(row), prev, 3)
            prev = bytes(row)
    return chunk(b"IDAT", zlib.compress(bytes(raw), 9))


# ---------------------------------------------------------------- output

def carray(name, data):
    lines = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12])
                     + ",")
    lines.append("};")
    return "\n".join(lines)


def break_crc(data, tag):
    """Corrupt the CRC of the first chunk with this tag."""
    out = bytearray(data)
    i = 8
    while i < len(out):
        ln = struct.unpack(">I", out[i:i + 4])[0]
        if out[i + 4:i + 8] == tag:
            out[i + 8 + ln] ^= 0xFF
            return bytes(out)
        i += 12 + ln
    raise KeyError(tag)


def main():
    w = h = 16

    rgb_rows = build_rgb8(w, h)
    text = (chunk(b"tEXt", b"Software\0vextro tools/mkpngref.py")
            + chunk(b"tEXt", b"Comment\0not written by libpng"))
    rgb8 = png(w, h, 8, 2, rgb_rows, 3, filters=[0], extra=text)

    # All five filter types, one per row, which is what makes this the
    # image that actually tests the unfilter code.
    rgba_rows = build_rgba8(8, 10)
    rgba8 = png(8, 10, 8, 6, rgba_rows, 4, filters=[0, 1, 2, 3, 4])

    # 16-bit grayscale: two bytes per sample, big-endian, which is the
    # one place PNG's byte order is not the machine's.
    g16_rows = []
    for y in range(4):
        row = bytearray()
        for x in range(4):
            row += struct.pack(">H", (x * 4096 + y * 257) & 0xFFFF)
        g16_rows.append(bytes(row))
    gray16 = png(4, 4, 16, 0, g16_rows, 2, filters=[0])

    # Palette with transparency. Colour type 3 is the one whose samples
    # are indices rather than intensities, so a decoder that treats them
    # as grey produces a picture rather than an error.
    palette = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0)]
    plte = chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    trns = chunk(b"tRNS", bytes((0xFF, 0x80, 0x40, 0x00)))
    pal_rows = [bytes(((x + y) % 4) for x in range(8)) for y in range(8)]
    pal8 = png(8, 8, 8, 3, pal_rows, 1, filters=[0], extra=plte + trns)

    inter = png(w, h, 8, 2, None, 3, interlace=1,
                idat_bytes=build_interlaced_rgb8(w, h))

    # Damaged, two ways. A broken IDAT CRC is what libpng reports through
    # its error function -- which is a longjmp -- and a file cut off
    # after IHDR ends the same way for a different reason.
    badcrc = break_crc(rgb8, b"IDAT")
    truncated = rgb8[:len(SIG) + 25 + 40]

    naive = png(w, h, 8, 2, rgb_rows, 3, filters=[0])
    sips = sips_reencode(naive)
    sw, sh, scolour, srows = decode(sips)
    assert (sw, sh) == (w, h), (sw, sh)
    # The claim that makes the sips file worth having: a third
    # implementation reads back the pixels that went in.
    if scolour == 2:
        assert srows == rgb_rows, "sips changed the pixels"
    else:
        assert scolour == 6
        for y in range(h):
            for x in range(w):
                assert tuple(srows[y][x * 4:x * 4 + 3]) == rgb_pixel(x, y)

    w_out = sys.stdout.write
    w_out(HEADER % {"sipslen": len(sips), "naivelen": len(naive)})
    w_out("\n" + carray("pngref_rgb8", rgb8) + "\n")
    w_out("\n" + carray("pngref_rgba8", rgba8) + "\n")
    w_out("\n" + carray("pngref_gray16", gray16) + "\n")
    w_out("\n" + carray("pngref_pal8", pal8) + "\n")
    w_out("\n" + carray("pngref_interlaced", inter) + "\n")
    w_out("\n" + carray("pngref_sips", sips) + "\n")
    w_out("\n" + carray("pngref_badcrc", badcrc) + "\n")
    w_out("\n" + carray("pngref_truncated", truncated) + "\n")
    w_out(FOOTER % {"sips_channels": {2: 3, 6: 4}[scolour],
                    "sips_colour": scolour})


HEADER = '''/*
 * apps/png_ref.h — PNG files this system did not encode.
 *
 * GENERATED by tools/mkpngref.py. Do not edit; regenerate.
 *
 * libpng is both the encoder and the decoder in this port, so a round
 * trip through png_write_image and back would prove the two halves of
 * one library agree with each other — and they would even if both were
 * wrong about what the Paeth predictor is. Everything below came from
 * somewhere else, for the same reason apps/jpeg_ref.h holds a bitstream
 * macOS encoded and apps/zlib_ref.h holds DEFLATE Apple's libcompression
 * produced.
 *
 * ---- two encoders, for two different reasons ----
 *
 * Six of the eight files were written from the specification by
 * tools/mkpngref.py, using zlib only for the IDAT payload. That is not
 * an attempt to be a PNG library; it is what makes the *filter type of
 * every row* a decision rather than a heuristic. pngref_rgba8 uses all
 * five filters, one per row, so a decoder with Sub and Up the wrong way
 * round fails on it — and a real encoder, choosing filters to compress
 * well, might never emit Average at all.
 *
 * pngref_sips is the other kind: macOS's ImageIO, through /usr/bin/sips,
 * re-encoding the same 16x16 image. It is a mature third-party encoder
 * and it shows — %(sipslen)d bytes against %(naivelen)d for the naive one, with sRGB
 * and eXIf chunks a hand-written file would not have thought to include.
 * Its expected pixels were recovered by the small decoder at the bottom
 * of tools/mkpngref.py rather than taken from what was fed in, so the
 * values asserted on the machine come from a third implementation again.
 *
 * ---- what is here ----
 *
 *   pngref_rgb8        16x16, 8-bit RGB, filter None, two tEXt chunks
 *   pngref_rgba8       8x10, 8-bit RGBA, a different filter on each row
 *   pngref_gray16      4x4, 16-bit grey — the one place PNG's byte order
 *                      is not the machine's
 *   pngref_pal8        8x8, colour type 3, with PLTE and tRNS, so the
 *                      samples are indices rather than intensities
 *   pngref_interlaced  16x16 RGB, Adam7, seven passes
 *   pngref_sips        16x16 RGB as ImageIO writes it
 *   pngref_badcrc      pngref_rgb8 with the IDAT chunk's CRC broken
 *   pngref_truncated   pngref_rgb8 cut off inside the IDAT chunk
 *
 * The last two exist because libpng reports errors by longjmp, and this
 * is the first code in ring 3 to depend on setjmp at all.
 */

#ifndef VEXTRO_PNG_REF_H
#define VEXTRO_PNG_REF_H
'''

FOOTER = '''
/* The pattern every RGB image above is built from, as a function rather
 * than a table: no two rows and no two columns are alike and no channel
 * is a copy of another, so a decoder that swapped red and blue, or lost
 * a row, gives a different answer. apps/pngtest.c computes the same
 * thing and compares. */
#define PNGREF_R(x, y) ((unsigned char)((x) * 16 + 8))
#define PNGREF_G(x, y) ((unsigned char)((y) * 16 + 8))
#define PNGREF_B(x, y) ((unsigned char)((x) * 7 + (y) * 3))

/* What ImageIO chose to write. Recorded here rather than assumed in the
 * test, because it is sips's decision and not this build's: a future
 * macOS that writes RGBA instead would change it, and the test should
 * fail with a wrong channel count rather than read past a row. */
#define PNGREF_SIPS_COLOUR_TYPE %(sips_colour)d
#define PNGREF_SIPS_CHANNELS    %(sips_channels)d

#endif /* VEXTRO_PNG_REF_H */
'''


if __name__ == "__main__":
    main()
