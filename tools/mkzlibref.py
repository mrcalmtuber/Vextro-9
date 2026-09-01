#!/usr/bin/env python3
"""tools/mkzlibref.py -- write apps/zlib_ref.h.

The DEFLATE in this file is produced by **Apple's libcompression**, not by
zlib, and that is the whole point of the file. zlib is both the compressor
and the decompressor in this port, so a round trip through deflate() and
back into inflate() would agree with itself even if both halves shared a
wrong idea of how a dynamic Huffman block is laid out. `compression_tool
-encode -a zlib` is a different implementation of RFC 1951 that has never
seen this one.

Two things are worth knowing about that tool. It emits **raw** DEFLATE --
RFC 1951 with no zlib header and no Adler-32, which is what
COMPRESSION_ZLIB means in libcompression and is not what the name suggests
-- so the streams here are decoded with windowBits = -15. And because it
gives us only the deflate payload, the zlib and gzip *containers* around
it are built here from RFC 1950 and RFC 1952 by hand rather than by
zlib's own writer. That makes the wrapper tests stronger rather than
weaker: the bytes zlib's header parser is asked to accept were written
from the specification, so a parser that agreed with its own writer about
something the specification does not say would fail here.

Run from the repository root; needs /usr/bin/compression_tool (macOS).

    python3 tools/mkzlibref.py > apps/zlib_ref.h
"""

import os
import struct
import subprocess
import sys
import tempfile
import zlib

# The passage that gets compressed. Chosen for shape rather than for
# content: enough repetition that a dynamic Huffman block is worth the
# encoder's while, and enough distinct bytes that the literal tree is not
# trivial.
TEXT = (
    b"Every layer a normal application takes for granted had to be built "
    b"first. There is a TrueType rasteriser because there was no way to "
    b"draw a letter. There is a Zstandard decompressor because the "
    b"archives are compressed with it. There is an NVMe driver because a "
    b"modern machine has nowhere else to keep an encyclopedia. There is "
    b"no operating system underneath any of it, and there is no C "
    b"library beside it that anybody else wrote.\n"
)


def apple_raw(data):
    """Raw DEFLATE of `data`, produced by libcompression."""
    with tempfile.TemporaryDirectory() as d:
        src = os.path.join(d, "in")
        dst = os.path.join(d, "out")
        with open(src, "wb") as f:
            f.write(data)
        subprocess.run(
            ["/usr/bin/compression_tool", "-encode", "-a", "zlib",
             "-i", src, "-o", dst],
            check=True,
        )
        with open(dst, "rb") as f:
            out = f.read()
    # Refuse to write a reference file whose bytes zlib cannot read at
    # all: this script would otherwise fail on the machine rather than
    # on the build host, hours later, as 40 mysterious assertions.
    assert zlib.decompress(out, -15) == data, "libcompression round trip"
    return out


def zlib_wrap(raw, data):
    """RFC 1950 around a raw DEFLATE stream.

    CMF 0x78 is CM=8 (deflate) with CINFO=7 (a 32 KiB window). FLG 0x9C
    sets FLEVEL=2 and no preset dictionary, and is the value that makes
    (CMF << 8 | FLG) a multiple of 31, which is the header's own check.
    """
    assert (0x78 << 8 | 0x9C) % 31 == 0
    return b"\x78\x9c" + raw + struct.pack(">I", zlib.adler32(data))


def gzip_wrap(raw, data, name=None, comment=None, mtime=0):
    """RFC 1952 around a raw DEFLATE stream."""
    flg = 0
    if name is not None:
        flg |= 0x08          # FNAME
    if comment is not None:
        flg |= 0x10          # FCOMMENT
    head = struct.pack("<BBBBIBB", 0x1F, 0x8B, 8, flg, mtime, 0, 3)
    if name is not None:
        head += name + b"\0"
    if comment is not None:
        head += comment + b"\0"
    return (head + raw
            + struct.pack("<I", zlib.crc32(data) & 0xFFFFFFFF)
            + struct.pack("<I", len(data) & 0xFFFFFFFF))


def lcg_blob(n):
    """A deterministic blob that does not compress.

    A 32-bit linear congruential generator, the one from the C standard's
    own example, so this is reproducible anywhere and is not random data
    smuggled in through a checked-in array. DEFLATE cannot do anything
    with it, so the encoder emits stored blocks -- a code path a test on
    English text never reaches.
    """
    out = bytearray()
    s = 1
    while len(out) < n:
        s = (s * 1103515245 + 12345) & 0xFFFFFFFF
        out.append((s >> 16) & 0xFF)
    return bytes(out)


def carray(name, data, indent="    "):
    lines = ["static const unsigned char %s[] = {" % name]
    for i in range(0, len(data), 12):
        row = ", ".join("0x%02x" % b for b in data[i:i + 12])
        lines.append(indent + row + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    text_raw = apple_raw(TEXT)
    blob = lcg_blob(4096)
    blob_raw = apple_raw(blob)

    zstream = zlib_wrap(text_raw, TEXT)
    gz_plain = gzip_wrap(text_raw, TEXT)
    gz_named = gzip_wrap(text_raw, TEXT,
                         name=b"vextro.txt",
                         comment=b"encoded by Apple libcompression",
                         mtime=0x5F5E0100)

    # One byte of the deflate payload flipped, with the zlib header and
    # the trailing Adler-32 left intact. What this actually produces is
    # the more interesting of the two possible outcomes: the payload is
    # still a *valid* deflate stream, it just decodes to 431 different
    # bytes, and the only thing standing between the caller and silent
    # corruption is the Adler-32 at the tail. An implementation that
    # skipped that check would return Z_STREAM_END and wrong data.
    bad = bytearray(zstream)
    bad[2 + len(text_raw) // 2] ^= 0x40
    truncated = zstream[:len(zstream) - 6]

    # The same damage inside a gzip container, which is what gets written
    # to the volume as /zlibbad.gz. The CRC-32 and ISIZE in the trailer
    # are the *original* text's, so this is a file that reads back a
    # plausible-looking 431 bytes and then fails at the very end -- the
    # shape of corruption a gzFile caller has to survive, and the only
    # way to reach gz_error() from a read-only test.
    bad_raw = bytearray(text_raw)
    bad_raw[len(text_raw) // 2] ^= 0x40
    gz_corrupt = gzip_wrap(bytes(bad_raw), TEXT)

    w = sys.stdout.write
    w(HEADER % {
        "textlen": len(TEXT),
        "rawlen": len(text_raw),
        "bloblen": len(blob),
        "blobrawlen": len(blob_raw),
        "adler": zlib.adler32(TEXT),
        "crc": zlib.crc32(TEXT) & 0xFFFFFFFF,
        "blobcrc": zlib.crc32(blob) & 0xFFFFFFFF,
        "flip": 2 + len(text_raw) // 2,
    })
    w("\n" + carray("zref_text", TEXT) + "\n")
    w("\n" + carray("zref_text_raw", text_raw) + "\n")
    w("\n" + carray("zref_text_zlib", zstream) + "\n")
    w("\n" + carray("zref_text_gzip", gz_plain) + "\n")
    w("\n" + carray("zref_text_gzip_named", gz_named) + "\n")
    w("\n" + carray("zref_text_zlib_corrupt", bytes(bad)) + "\n")
    w("\n" + carray("zref_text_gzip_corrupt", gz_corrupt) + "\n")
    w("\n" + carray("zref_text_zlib_truncated", truncated) + "\n")
    w("\n" + carray("zref_blob_raw", blob_raw) + "\n")
    w(FOOTER % {
        "adler": zlib.adler32(TEXT),
        "crc": zlib.crc32(TEXT) & 0xFFFFFFFF,
        "blobcrc": zlib.crc32(blob) & 0xFFFFFFFF,
    })


HEADER = '''/*
 * apps/zlib_ref.h — DEFLATE this system did not produce.
 *
 * GENERATED by tools/mkzlibref.py. Do not edit; regenerate.
 *
 * The compressed bytes below came out of **Apple's libcompression**, on
 * the build machine, with:
 *
 *     compression_tool -encode -a zlib -i plain -o deflated
 *
 * which despite the algorithm's name emits *raw* DEFLATE — RFC 1951 with
 * no zlib header and no Adler-32 — so everything here is decoded with
 * windowBits = -15 unless it has been wrapped.
 *
 * It comes from somewhere else for the same reason apps/jpeg_ref.h holds
 * a bitstream macOS encoded and apps/tasn1_ref.h holds DER OpenSSL
 * wrote: zlib is both the compressor and the decompressor in this port,
 * so a round trip through deflate() and back would prove the two halves
 * of one library agree with each other — and they would even if both
 * were wrong about how a dynamic Huffman block is laid out.
 *
 * ---- and the containers are hand-built, on purpose ----
 *
 * libcompression gives only the deflate payload, so the zlib and gzip
 * wrappers around it were written from RFC 1950 and RFC 1952 by
 * tools/mkzlibref.py rather than by zlib's own writer. That makes those
 * tests stronger rather than weaker: a header parser that agreed with
 * its own writer about something the specification does not say would
 * fail here. Only the two checksums inside the wrappers are computed
 * with the host's zlib, because Adler-32 and CRC-32 are defined by the
 * specification and not by an implementation — and both are checked
 * again on the machine against the constants at the foot of this file.
 *
 * ---- what is here ----
 *
 *   zref_text                 %(textlen)d bytes of plain text
 *   zref_text_raw             %(rawlen)d bytes, raw DEFLATE of it
 *   zref_text_zlib            the same payload inside RFC 1950
 *   zref_text_gzip            the same payload inside RFC 1952
 *   zref_text_gzip_named      RFC 1952 with FNAME and FCOMMENT set, so
 *                             inflateGetHeader has something to report
 *   zref_text_zlib_corrupt    zref_text_zlib with byte %(flip)d flipped. The
 *                             payload is still a valid deflate stream —
 *                             it decodes, to 431 bytes that are wrong
 *                             from offset 161 on — so the only thing
 *                             between the caller and silent corruption
 *                             is the Adler-32 at the tail
 *   zref_text_gzip_corrupt    the same damage inside RFC 1952, with the
 *                             *original* text's CRC-32 in the trailer.
 *                             This one is written to the volume as
 *                             /zlibbad.gz: a file that reads back a
 *                             plausible 431 bytes and then fails at the
 *                             very end, which is the only way a
 *                             read-only test reaches gz_error()
 *   zref_text_zlib_truncated  zref_text_zlib with the last six bytes cut
 *   zref_blob_raw             %(blobrawlen)d bytes, raw DEFLATE of %(bloblen)d bytes
 *                             that do not compress — the encoder falls
 *                             back to stored blocks, which a test on
 *                             English text never reaches
 */

#ifndef VEXTRO_ZLIB_REF_H
#define VEXTRO_ZLIB_REF_H
'''

FOOTER = '''
/* Checksums of zref_text and of the incompressible blob, computed on the
 * build machine. Both are defined by their specifications rather than by
 * an implementation, which is what makes them worth asserting against a
 * library that computes them a different way. */
#define ZREF_TEXT_ADLER32  0x%(adler)08xu
#define ZREF_TEXT_CRC32    0x%(crc)08xu
#define ZREF_BLOB_CRC32    0x%(blobcrc)08xu

/* The CRC-32 "check" value from the catalogue of parametrised CRC
 * algorithms: the CRC of the nine ASCII bytes "123456789" under
 * CRC-32/ISO-HDLC, which is the one zlib implements. It is here because
 * it is the single most widely published value this library can be held
 * to, and it is not derived from anything else in this file. */
#define ZREF_CHECK_STRING  "123456789"
#define ZREF_CHECK_CRC32   0xcbf43926u

#endif /* VEXTRO_ZLIB_REF_H */
'''


if __name__ == "__main__":
    main()
