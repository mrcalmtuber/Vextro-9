#!/usr/bin/env python3
"""tools/mkwebpref.py -- write apps/webp_ref.h.

    python3 tools/mkwebpref.py > apps/webp_ref.h

Run from the repository root, after `make libs-fetch`. Needs a host C
compiler and /usr/bin/sips (macOS).

================================================================
what this file can honestly claim, and what it cannot
================================================================

The other reference headers in apps/ are built by encoders that have
never seen the one being tested: apps/jpeg_ref.h came out of macOS's own
JPEG codec, apps/zlib_ref.h out of Apple's libcompression, and six of the
eight images in apps/png_ref.h were written from the PNG specification by
tools/mkpngref.py.

**That is not available for WebP, and it is worth saying plainly rather
than quietly implying otherwise.** There is no second implementation of
VP8 or VP8L on this machine. macOS decodes .webp through ImageIO, and
ImageIO *bundles libwebp* -- which is how the 2023 VP8L heap overflow
became a macOS security update. So a `sips` cross-check is a check
against a different **build** of the same codebase, not against a
different implementation, and this file says "build" everywhere it would
have said "implementation" for the other three.

What is left is still worth having, and it is three things:

  Three builds, two compilers, two architectures. The bitstreams here are
  encoded by the vendored sources compiled with the host's clang for
  arm64; they are decoded on the machine by the same sources compiled
  with x86_64-elf-gcc; and they are decoded a third time by Apple's build
  inside ImageIO. The SIMD kernels those three take are disjoint -- NEON
  on the host, SSE2/SSE4.1 on the machine, whatever Apple shipped -- so
  agreement across them is a real result even though the C is shared.

  Pixels nothing in the chain invented. The lossless images below are
  generated from a formula (see PATTERN), so the round trip is
  formula -> host encoder -> ring-3 decoder -> formula, and lossless
  WebP is bit-exact by definition. Nothing has to be trusted for that
  one to mean something.

  **The container, which is genuinely independent.** WebP's RIFF
  container -- VP8X, ANIM, ANMF -- is a documented format, and the
  animation below is built here from that documentation rather than by
  libwebp's own muxer. That is exactly what the RFC 1950 and RFC 1952
  wrappers in tools/mkzlibref.py are to zlib, and it matters more here:
  `find_package(WebP REQUIRED COMPONENTS demux)` asks for the demuxer
  specifically, and the demuxer is the thing this hand-built file tests.
  libwebpmux.a is not built in this port, so there was no muxer to use
  even if it had been the right choice.

================================================================
the five places a hand-built container goes wrong
================================================================

Read against src/demux/demux.c, ParseVP8X and ParseAnimationFrame:

  VP8X canvas width and height are stored *minus one*, in 24 bits,
  little-endian. `1 + ReadLE24s(mem)`.

  ANMF x and y offsets are stored *halved*. `2 * ReadLE24s(mem)`. So an
  odd offset cannot be expressed and every frame origin here is even.

  ANMF width and height are stored minus one, like the canvas; duration
  is three bytes; and then there is one flags byte, in that order.

  That flags byte is not the VP8X flags byte. Bit 0 set means dispose to
  background, bit 1 set means *no* blend -- both inverted from the names
  a reader expects, and both read back through WebPIterator, which is
  where apps/webptest.c checks them.

  Every chunk is padded to an even size, and the padding byte is not
  counted in the chunk's own size field but is counted in the RIFF size.
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

# ---------------------------------------------------------------- pixels

W = H = 32
ANIM_W = ANIM_H = 24


def PATTERN(x, y):
    """The RGB every lossless image here is built from.

    A formula rather than a table, so apps/webptest.c can compute the
    same thing and the expectation is not a copy of the encoder's input.
    No two rows and no two columns are alike and no channel repeats
    another, so a decoder that lost a row or swapped red and blue gives
    a different answer.
    """
    return ((x * 8 + 3) & 0xFF, (y * 8 + 5) & 0xFF, (x * 5 + y * 11) & 0xFF)


def ALPHA(x, y):
    return (255 - ((x + y) * 4)) & 0xFF


def rgba_image(w, h, with_alpha, shift=0):
    buf = bytearray()
    for y in range(h):
        for x in range(w):
            r, g, b = PATTERN((x + shift) % 256, y)
            buf += bytes((r, g, b, ALPHA(x, y) if with_alpha else 255))
    return bytes(buf)


# ---------------------------------------------------------- host encoder

HELPER_C = r"""
/* Built by tools/mkwebpref.py. Encodes and decodes with the vendored
 * libwebp compiled for the *host*, so that the bitstreams in
 * apps/webp_ref.h come out of a different build of the sources being
 * tested rather than out of the build being tested. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "src/webp/encode.h"
#include "src/webp/decode.h"

static void die(const char* m) { fprintf(stderr, "helper: %s\n", m); exit(1); }

int main(int argc, char** argv) {
  /* encode <mode> <w> <h> <in.rgba> <out.webp>
     decode <in.webp> <out.rgba>  (prints "w h" on stdout) */
  if (argc == 7 && !strcmp(argv[1], "encode")) {
    const int w = atoi(argv[3]), h = atoi(argv[4]);
    FILE* f = fopen(argv[5], "rb");
    uint8_t* rgba = (uint8_t*)malloc((size_t)w * h * 4);
    uint8_t* out = NULL;
    size_t n;
    if (!f || !rgba) die("input");
    if (fread(rgba, 1, (size_t)w * h * 4, f) != (size_t)w * h * 4)
      die("short read");
    fclose(f);
    if (!strcmp(argv[2], "lossless")) {
      n = WebPEncodeLosslessRGBA(rgba, w, h, w * 4, &out);
    } else if (!strcmp(argv[2], "lossless-rgb")) {
      /* Drop alpha first, so the file has no ALPH/alpha plane at all. */
      uint8_t* rgb = (uint8_t*)malloc((size_t)w * h * 3);
      int i;
      for (i = 0; i < w * h; ++i) memcpy(rgb + i * 3, rgba + i * 4, 3);
      n = WebPEncodeLosslessRGB(rgb, w, h, w * 3, &out);
      free(rgb);
    } else if (!strcmp(argv[2], "lossy-rgb")) {
      uint8_t* rgb = (uint8_t*)malloc((size_t)w * h * 3);
      int i;
      for (i = 0; i < w * h; ++i) memcpy(rgb + i * 3, rgba + i * 4, 3);
      n = WebPEncodeRGB(rgb, w, h, w * 3, 90.0f, &out);
      free(rgb);
    } else {                          /* lossy with alpha */
      n = WebPEncodeRGBA(rgba, w, h, w * 4, 90.0f, &out);
    }
    if (n == 0) die("encode failed");
    f = fopen(argv[6], "wb");
    if (!f) die("output");
    fwrite(out, 1, n, f);
    fclose(f);
    WebPFree(out);
    free(rgba);
    return 0;
  }
  if (argc == 4 && !strcmp(argv[1], "decode")) {
    FILE* f = fopen(argv[2], "rb");
    uint8_t* data;
    long len;
    int w = 0, h = 0;
    uint8_t* rgba;
    if (!f) die("input");
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    data = (uint8_t*)malloc(len);
    if (fread(data, 1, len, f) != (size_t)len) die("short read");
    fclose(f);
    rgba = WebPDecodeRGBA(data, len, &w, &h);
    if (rgba == NULL) die("decode failed");
    f = fopen(argv[3], "wb");
    fwrite(rgba, 1, (size_t)w * h * 4, f);
    fclose(f);
    printf("%d %d\n", w, h);
    WebPFree(rgba);
    free(data);
    return 0;
  }
  die("usage");
  return 1;
}
"""


class Helper:
    """The vendored libwebp, compiled for the host."""

    def __init__(self, root, workdir):
        self.root = root
        self.bin = os.path.join(workdir, "webphelper")
        src = os.path.join(workdir, "helper.c")
        with open(src, "w") as f:
            f.write(HELPER_C)
        sources = [src]
        for d in ("sharpyuv", "src/dec", "src/dsp", "src/enc", "src/utils"):
            full = os.path.join(root, *d.split("/"))
            sources += sorted(os.path.join(full, n)
                              for n in os.listdir(full) if n.endswith(".c"))
        cc = os.environ.get("CC", "cc")
        print("mkwebpref: compiling %d sources with %s"
              % (len(sources), cc), file=sys.stderr)
        subprocess.run([cc, "-O2", "-w", "-I" + root, "-o", self.bin]
                       + sources, check=True)

    def encode(self, mode, w, h, rgba, workdir):
        raw = os.path.join(workdir, "in.rgba")
        out = os.path.join(workdir, "out.webp")
        with open(raw, "wb") as f:
            f.write(rgba)
        subprocess.run([self.bin, "encode", mode, str(w), str(h), raw, out],
                       check=True)
        with open(out, "rb") as f:
            return f.read()

    def decode(self, data, workdir):
        src = os.path.join(workdir, "d.webp")
        out = os.path.join(workdir, "d.rgba")
        with open(src, "wb") as f:
            f.write(data)
        r = subprocess.run([self.bin, "decode", src, out],
                           check=True, capture_output=True, text=True)
        w, h = (int(v) for v in r.stdout.split())
        with open(out, "rb") as f:
            return w, h, f.read()


# ------------------------------------------------------- the RIFF container

def chunk(tag, payload):
    """One RIFF chunk, padded to an even size.

    The pad byte is not counted in the chunk's own size field. It *is*
    counted in the RIFF size, which is why riff() below measures the
    finished bytes rather than summing payload lengths.
    """
    out = tag + struct.pack("<I", len(payload)) + payload
    if len(payload) & 1:
        out += b"\0"
    return out


def le24(v):
    return struct.pack("<I", v)[:3]


def riff(payload):
    return b"RIFF" + struct.pack("<I", len(payload) + 4) + b"WEBP" + payload


def vp8x(width, height, flags):
    """VP8X: one flags byte, three reserved, then width-1 and height-1 as
    24-bit little-endian values (demux.c ParseVP8X)."""
    return chunk(b"VP8X",
                 bytes((flags,)) + b"\0\0\0"
                 + le24(width - 1) + le24(height - 1))


def anim(bgcolour, loop_count):
    return chunk(b"ANIM", struct.pack("<I", bgcolour)
                 + struct.pack("<H", loop_count))


def anmf(x, y, w, h, duration, dispose_background, no_blend, framedata):
    """ANMF: offsets are halved, sizes are minus one, duration is three
    bytes, and then one flags byte whose two bits are both named for the
    negative case (demux.c ParseAnimationFrame:327-335)."""
    assert x % 2 == 0 and y % 2 == 0, "ANMF offsets are stored halved"
    bits = (1 if dispose_background else 0) | (2 if no_blend else 0)
    head = (le24(x // 2) + le24(y // 2) + le24(w - 1) + le24(h - 1)
            + le24(duration) + bytes((bits,)))
    return chunk(b"ANMF", head + framedata)


def extract_image_chunk(simple_webp):
    """Pull the whole VP8/VP8L/ALPH chunk sequence out of a simple-format
    file, header and all.

    An ANMF payload is a *chunk*, not a bare bitstream, so what goes into
    the frame is these bytes verbatim.
    """
    assert simple_webp[:4] == b"RIFF" and simple_webp[8:12] == b"WEBP"
    return simple_webp[12:]


# --------------------------------------------------------- the ImageIO check

def sips_decode(data, workdir):
    """Decode a .webp through macOS's ImageIO and return its pixels.

    ImageIO bundles libwebp, so this is a second *build* rather than a
    second implementation -- see the note at the head of this file. It is
    run anyway: the reference files would be worth much less if nothing
    outside this repository could open them at all.
    """
    src = os.path.join(workdir, "s.webp")
    dst = os.path.join(workdir, "s.png")
    with open(src, "wb") as f:
        f.write(data)
    r = subprocess.run(["/usr/bin/sips", "-s", "format", "png", src,
                        "--out", dst], capture_output=True)
    if r.returncode != 0:
        return None
    return png_decode(open(dst, "rb").read())


def png_decode(data):
    """Minimal PNG reader: 8-bit, non-interlaced, colour type 2 or 6.

    The same one tools/mkpngref.py carries, for the same reason: enough
    to recover pixels from a file somebody else wrote, and no more.
    """
    def paeth(a, b, c):
        p = a + b - c
        pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
        return a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)

    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    i, idat, hdr = 8, b"", None
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        tag = data[i + 4:i + 8]
        if tag == b"IHDR":
            hdr = struct.unpack(">IIBBBBB", data[i + 8:i + 8 + ln])
        elif tag == b"IDAT":
            idat += data[i + 8:i + 8 + ln]
        i += 12 + ln
    w, h, depth, colour, _, _, interlace = hdr
    assert depth == 8 and interlace == 0, hdr
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    raw = zlib.decompress(idat)
    stride, pos, rows, prev = w * ch, 0, [], bytearray(w * ch)
    for _ in range(h):
        f = raw[pos]; pos += 1
        line = bytearray(raw[pos:pos + stride]); pos += stride
        for k in range(stride):
            a = line[k - ch] if k >= ch else 0
            b = prev[k]
            c = prev[k - ch] if k >= ch else 0
            if f == 1:   line[k] = (line[k] + a) & 0xFF
            elif f == 2: line[k] = (line[k] + b) & 0xFF
            elif f == 3: line[k] = (line[k] + ((a + b) >> 1)) & 0xFF
            elif f == 4: line[k] = (line[k] + paeth(a, b, c)) & 0xFF
        rows.append(bytes(line)); prev = line
    return w, h, ch, rows


# ---------------------------------------------------------------- output

def carray(name, data):
    lines = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 12):
        lines.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 12])
                     + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    root = "third_party/libwebp"
    if not os.path.exists(os.path.join(root, "src/webp/decode.h")):
        sys.exit("mkwebpref: run `make libs-fetch` first")

    with tempfile.TemporaryDirectory() as wd:
        helper = Helper(root, wd)

        rgba = rgba_image(W, H, with_alpha=True)
        rgb_only = rgba_image(W, H, with_alpha=False)

        lossless = helper.encode("lossless-rgb", W, H, rgb_only, wd)
        lossless_a = helper.encode("lossless", W, H, rgba, wd)
        lossy = helper.encode("lossy-rgb", W, H, rgb_only, wd)
        lossy_a = helper.encode("lossy", W, H, rgba, wd)

        # Lossless must be exact against the formula. This is the one
        # claim in the file that depends on nothing at all.
        for name, blob, alpha in (("lossless", lossless, False),
                                  ("lossless_alpha", lossless_a, True)):
            w, h, px = helper.decode(blob, wd)
            assert (w, h) == (W, H), name
            for y in range(H):
                for x in range(W):
                    r, g, b = PATTERN(x, y)
                    o = (y * W + x) * 4
                    exp = (r, g, b, ALPHA(x, y) if alpha else 255)
                    assert tuple(px[o:o + 4]) == exp, (name, x, y)

        # Lossy is not exact against anything, so the host build's own
        # decode is what gets embedded -- VP8 reconstruction is exactly
        # specified, so the machine must reproduce it byte for byte, and
        # any difference is a miscompile rather than a rounding choice.
        _, _, lossy_px = helper.decode(lossy, wd)
        _, _, lossy_a_px = helper.decode(lossy_a, wd)

        # Three frames, offsets even because ANMF halves them.
        frames = [
            (0, 0, W, H, 100, False, False,
             extract_image_chunk(helper.encode(
                 "lossless-rgb", W, H, rgba_image(W, H, False, 0), wd))),
            (4, 2, ANIM_W, ANIM_H, 250, False, True,
             extract_image_chunk(helper.encode(
                 "lossless-rgb", ANIM_W, ANIM_H,
                 rgba_image(ANIM_W, ANIM_H, False, 7), wd))),
            (2, 6, ANIM_W, ANIM_H, 33, True, False,
             extract_image_chunk(helper.encode(
                 "lossless-rgb", ANIM_W, ANIM_H,
                 rgba_image(ANIM_W, ANIM_H, False, 19), wd))),
        ]
        body = vp8x(W, H, 0x02)                    # ANIMATION_FLAG
        body += anim(0xFF204060, 3)                # BGRA background, 3 loops
        for (x, y, fw, fh, dur, disp, noblend, data) in frames:
            body += anmf(x, y, fw, fh, dur, disp, noblend, data)
        animated = riff(body)

        corrupt = bytearray(lossless)
        corrupt[len(lossless) * 3 // 4] ^= 0x55
        truncated = lossy[:len(lossy) // 2]

        # Every file gets opened by a build of libwebp that is not ours.
        imageio = {}
        for name, blob in (("lossless", lossless), ("lossless_alpha",
                           lossless_a), ("lossy", lossy),
                           ("lossy_alpha", lossy_a), ("anim", animated)):
            imageio[name] = sips_decode(blob, wd) is not None
        assert imageio["anim"], (
            "ImageIO refused the hand-built animation -- the container is "
            "wrong, and libwebp's own demuxer agreeing with the generator "
            "would not have caught it")
        assert all(imageio.values()), imageio

    w_out = sys.stdout.write
    w_out(HEADER % {
        "w": W, "h": H, "aw": ANIM_W, "ah": ANIM_H,
        "lossless": len(lossless), "lossy": len(lossy),
        "anim": len(animated), "frames": len(frames),
    })
    w_out("\n" + carray("webpref_lossless", lossless) + "\n")
    w_out("\n" + carray("webpref_lossless_alpha", lossless_a) + "\n")
    w_out("\n" + carray("webpref_lossy", lossy) + "\n")
    w_out("\n" + carray("webpref_lossy_pixels", lossy_px) + "\n")
    w_out("\n" + carray("webpref_lossy_alpha", lossy_a) + "\n")
    w_out("\n" + carray("webpref_lossy_alpha_pixels", lossy_a_px) + "\n")
    w_out("\n" + carray("webpref_anim", animated) + "\n")
    w_out("\n" + carray("webpref_corrupt", bytes(corrupt)) + "\n")
    w_out("\n" + carray("webpref_truncated", truncated) + "\n")
    w_out(FOOTER % {"w": W, "h": H, "aw": ANIM_W, "ah": ANIM_H,
                    "frames": len(frames)})


HEADER = '''/*
 * apps/webp_ref.h — WebP bitstreams, and one container written from the
 * specification.
 *
 * GENERATED by tools/mkwebpref.py. Do not edit; regenerate.
 *
 * ---- what this file claims, and what it does not ----
 *
 * apps/jpeg_ref.h holds a bitstream macOS's own codec produced.
 * apps/zlib_ref.h holds DEFLATE from Apple's libcompression. Six of the
 * eight images in apps/png_ref.h were written from the PNG
 * specification. In each case the point is the same: the reference came
 * from an implementation that has never seen the one being tested.
 *
 * **That is not available for WebP.** There is no second implementation
 * of VP8 or VP8L on this machine — macOS reads .webp through ImageIO,
 * and ImageIO bundles libwebp, which is how the 2023 VP8L overflow
 * became a macOS security update. So the bitstreams below were encoded
 * by the *vendored sources compiled for the host*, and every check
 * against them is a check across three **builds** of one codebase rather
 * than across implementations. That is a weaker claim and it is the true
 * one.
 *
 * What the three builds do not share is the part most likely to be
 * wrong: the host's is arm64 clang taking NEON kernels, this one is
 * x86_64-elf-gcc taking SSE2 and SSE4.1, and Apple's is a third. The
 * lossless images are also generated from a *formula* rather than from a
 * picture, so for those the chain is formula → encoder → decoder →
 * formula, and lossless WebP is bit-exact by definition: nothing has to
 * be trusted for that one to mean something.
 *
 * ---- the animation, which is genuinely independent ----
 *
 * webpref_anim was not written by libwebp. Its RIFF container — VP8X,
 * ANIM and %(frames)d ANMF chunks — was assembled by tools/mkwebpref.py from
 * the WebP container specification, with each frame's payload being a
 * complete VP8L chunk lifted out of a simple-format file. That makes it
 * to the demuxer what the RFC 1950 and RFC 1952 wrappers in
 * tools/mkzlibref.py are to zlib, and it matters here more than there:
 * `find_package(WebP REQUIRED COMPONENTS demux)` asks for the demuxer by
 * name, and the demuxer is what this file tests. libwebpmux.a is not
 * built in this port, so there was no muxer available to write it even
 * if that had been the right choice.
 *
 * The generator refuses to emit this header unless macOS's ImageIO also
 * opens the animation, because libwebp's demuxer agreeing with the
 * generator would prove only that the two made the same mistake.
 *
 * ---- what is here ----
 *
 *   webpref_lossless        %(w)dx%(h)d VP8L, no alpha channel at all —
 *                           %(lossless)d bytes
 *   webpref_lossless_alpha  the same picture with a real alpha ramp
 *   webpref_lossy           %(w)dx%(h)d VP8 at quality 90 — %(lossy)d bytes
 *   webpref_lossy_pixels    what the host build decoded it to, which is
 *                           what the machine must reproduce exactly
 *   webpref_lossy_alpha     VP8 with an ALPH chunk beside it, so the
 *                           alpha decoder and the alpha_processing dsp
 *                           run — this is the shape WebKit meets most
 *   webpref_lossy_alpha_pixels   likewise
 *   webpref_anim            %(anim)d bytes: VP8X + ANIM + %(frames)d ANMF, built
 *                           from the specification. Frame 2 is %(aw)dx%(ah)d at
 *                           an offset with no blending; frame 3 disposes
 *                           to background. Both are read back through
 *                           WebPIterator and checked.
 *   webpref_corrupt         webpref_lossless with one byte flipped
 *   webpref_truncated       webpref_lossy cut in half — incomplete, which
 *                           is a different answer from wrong
 */

#ifndef VEXTRO_WEBP_REF_H
#define VEXTRO_WEBP_REF_H
'''

FOOTER = '''
/* The formula every lossless image above is built from. apps/webptest.c
 * computes the same thing, so the expectation is not a copy of what the
 * encoder was handed. */
#define WEBPREF_W  %(w)d
#define WEBPREF_H  %(h)d
#define WEBPREF_ANIM_W %(aw)d
#define WEBPREF_ANIM_H %(ah)d

#define WEBPREF_R(x, y) ((unsigned char)((x) * 8 + 3))
#define WEBPREF_G(x, y) ((unsigned char)((y) * 8 + 5))
#define WEBPREF_B(x, y) ((unsigned char)((x) * 5 + (y) * 11))
#define WEBPREF_A(x, y) ((unsigned char)(255 - ((x) + (y)) * 4))

/* What the generator put in the container, for the demux checks. */
#define WEBPREF_ANIM_FRAMES     %(frames)d
#define WEBPREF_ANIM_LOOPS      3
#define WEBPREF_ANIM_BACKGROUND 0xff204060u

#endif /* VEXTRO_WEBP_REF_H */
'''


if __name__ == "__main__":
    main()
