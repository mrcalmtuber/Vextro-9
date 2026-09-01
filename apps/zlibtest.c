/*
 * zlibtest — zlib in ring 3.
 *
 * zlib is the thirteenth library ported here and the first whose output
 * this system already had a use for before WebKit asked for it: PNG's
 * IDAT chunks are DEFLATE, `src/sci.h`'s images are DEFLATE's filters
 * without DEFLATE, and `tools/mkimg.py` has been decoding PNG on the
 * *build* machine for as long as there have been pictures on the volume.
 * What was missing was any of that on the machine itself.
 *
 * ---- what this file is actually trying to catch ----
 *
 * A compression library is unusually easy to test wrongly. Compress and
 * decompress with the same code and the answer comes back right whatever
 * either half believes, because the two halves believe it together. So
 * the centre of this file is section 3: streams produced by **Apple's
 * libcompression** on the build machine, in apps/zlib_ref.h, decoded
 * here. If this port's inflate is wrong about a distance code, those
 * bytes are where it shows.
 *
 * Around that are the three things a *port* can get wrong that a
 * correctly-compiled library cannot:
 *
 *   The configuration a consumer sees. zlib has no config.h; it has two
 *   #ifdefs in zconf.h, and this port answers them twice — with -D on
 *   the command line for the archive, and with a generated header for
 *   everything that links against it. Section 1 is compiled with
 *   *neither* define present, against the generated header alone, so it
 *   fails if the two answers ever drift apart. That is not a theoretical
 *   worry: it is the same hazard the FreeType and JPEG staging notes in
 *   the Makefile are about, and it decides sizeof(z_off_t).
 *
 *   The C library underneath. zlib's gz* layer is the only part that
 *   touches the operating system, and it does so with open, read, write,
 *   lseek, close, snprintf, vsnprintf and strerror — nine calls, all of
 *   which this system grew for other reasons. Section 9 drives a real
 *   gzip file onto the NTFS volume and reads it back, so those are
 *   exercised against the actual filesystem rather than a pipe.
 *
 *   The boundaries. Section 5 feeds a stream one byte in and one byte
 *   out, which is the loop shape every real caller has and the one where
 *   a library that quietly assumes it can always make progress fails.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include <zlib.h>

#include "zlib_ref.h"

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

/* Prints the code as well, for the checks where "it did not work" and
 * "it worked and gave the wrong answer" are different findings. */
static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/* Working room. 64 KiB is more than any stream here needs; the point of
 * a fixed buffer is that a decode which overruns it is a failure rather
 * than a realloc. */
static unsigned char out[65536];
static unsigned char out2[65536];

/*
 * Adler-32 with the right starting value, which is 1 and not 0.
 *
 * This exists because getting it wrong is invisible in exactly the way
 * this whole file is written to avoid: crc32() starts at 0 and adler32()
 * starts at 1, both take the seed as their first argument, and a wrong
 * seed produces a checksum that is perfectly self-consistent — every
 * incremental call agrees with every other one — and matches nobody
 * else's. zlib's own documented way of saying it is adler32(0L, Z_NULL,
 * 0), which returns 1, and section 2 checks that too.
 */
static uLong adler_of(const void *p, size_t n) {
    return adler32(adler32(0L, Z_NULL, 0), (const Bytef *)p, (uInt)n);
}

/* Raw inflate of a whole stream in one call, with the window bits given.
 * Returns the byte count, or -1. */
static long inflate_all(const unsigned char *in, size_t inlen,
                        unsigned char *dst, size_t dstcap, int windowBits) {
    z_stream s;
    int rc;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, windowBits) != Z_OK) return -1;
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)inlen;
    s.next_out = dst;
    s.avail_out = (uInt)dstcap;
    rc = inflate(&s, Z_FINISH);
    if (rc != Z_STREAM_END) { inflateEnd(&s); return -1; }
    rc = (int)s.total_out;
    inflateEnd(&s);
    return rc;
}

int main(void) {
    printf("zlibtest: zlib %s\n", zlibVersion());

    /* ============================================================
     *  1. the configuration a consumer sees
     * ============================================================
     *
     * This translation unit is compiled with -I on build/zlib/include
     * and nothing else — not the vendored tree, and without
     * -DHAVE_UNISTD_H or -DHAVE_STDARG_H. So every answer below comes
     * from the generated zconf.h alone, which is the file that gets
     * staged into the sysroot and the file WebKit will compile against.
     *
     * If the generated header ever stops matching the archive, this
     * section is where it is caught, and it is caught as a wrong type
     * size rather than as a link error five libraries later.
     */
    {
#ifdef Z_HAVE_UNISTD_H
        check("the staged zconf.h answers HAVE_UNISTD_H by itself", 1);
#else
        check("the staged zconf.h answers HAVE_UNISTD_H by itself", 0);
#endif
#ifdef Z_HAVE_STDARG_H
        check("and HAVE_STDARG_H", 1);
#else
        check("and HAVE_STDARG_H", 0);
#endif
        /* The consequence of the first one. Without Z_HAVE_UNISTD_H
         * zconf.h falls through to `#define z_off_t long`, which is the
         * same eight bytes here — so this check would pass on this
         * machine either way, and the two above are the ones that
         * actually hold the line. It is here because gz_statep's offsets
         * are z_off64_t and a consumer that disagreed about their width
         * would corrupt the structure rather than fail to compile. */
        check("z_off_t is 64 bits", sizeof(z_off_t) == 8);
        check("z_off64_t is 64 bits", sizeof(z_off64_t) == 8);

        check("the header's version matches the archive's",
              strcmp(ZLIB_VERSION, zlibVersion()) == 0);
        check("which is 1.3.1", strcmp(zlibVersion(), "1.3.1") == 0);

        /* zlibCompileFlags packs the four type sizes two bits each,
         * 00 = 16 bits, 01 = 32, 10 = 64, 11 = other. This is the
         * archive's own account of how it was built, so it is the other
         * half of the claim the two #ifdefs above make. */
        {
            uLong f = zlibCompileFlags();
            check("compile flags: uInt is 32 bits",   (f & 3) == 1);
            check("compile flags: uLong is 64 bits",  ((f >> 2) & 3) == 2);
            check("compile flags: pointers are 64 bits",
                  ((f >> 4) & 3) == 2);
            check("compile flags: z_off_t is 64 bits",
                  ((f >> 6) & 3) == 2);
            /* Content bits, all of which must be zero: the gz layer can
             * compress, deflate can write gzip, and gzprintf is the
             * vsnprintf variant rather than the one zlib's own comment
             * calls "not secure!". That last is only zero because this
             * C library has vsnprintf, which it did not always. */
            check("gz* can compress",        ((f >> 16) & 1) == 0);
            check("gzip streams are built in", ((f >> 17) & 1) == 0);
            check("no PKZIP bug workaround", ((f >> 20) & 1) == 0);
            check("not the FASTEST build",   ((f >> 21) & 1) == 0);
            check("gzprintf uses vsnprintf", ((f >> 24) & 7) == 0);
        }
    }

    /* ============================================================
     *  2. the two checksums, against values defined by specifications
     * ============================================================
     *
     * CRC-32 and Adler-32 are the parts of zlib with published answers
     * that owe nothing to any implementation, which makes them the only
     * things here that can be checked absolutely rather than against
     * another program's output.
     */
    {
        check("crc32 of the empty message is zero",
              crc32(0L, Z_NULL, 0) == 0);
        check("adler32 of the empty message is one",
              adler32(0L, Z_NULL, 0) == 1);

        /* The published check value for CRC-32/ISO-HDLC. */
        check("crc32(\"123456789\") is the catalogue's check value",
              crc32(0L, (const Bytef *)ZREF_CHECK_STRING,
                    (uInt)strlen(ZREF_CHECK_STRING)) == ZREF_CHECK_CRC32);

        check("crc32 of the reference text",
              crc32(0L, zref_text, (uInt)sizeof zref_text) == ZREF_TEXT_CRC32);
        check("adler32 of the reference text",
              adler_of(zref_text, sizeof zref_text) == ZREF_TEXT_ADLER32);

        /* Incremental must equal one-shot, split at an awkward place
         * rather than a round one. */
        {
            uLong a = adler32(0L, Z_NULL, 0), c = crc32(0L, Z_NULL, 0);
            size_t cut = 173;
            a = adler32(a, zref_text, (uInt)cut);
            a = adler32(a, zref_text + cut, (uInt)(sizeof zref_text - cut));
            c = crc32(c, zref_text, (uInt)cut);
            c = crc32(c, zref_text + cut, (uInt)(sizeof zref_text - cut));
            check("adler32 in two pieces equals adler32 in one",
                  a == ZREF_TEXT_ADLER32);
            check("crc32 in two pieces equals crc32 in one",
                  c == ZREF_TEXT_CRC32);

            /* And the combine functions, which are the same claim made
             * without re-reading the first half. */
            {
                uLong a1 = adler_of(zref_text, cut);
                uLong a2 = adler_of(zref_text + cut, sizeof zref_text - cut);
                uLong c1 = crc32(0L, zref_text, (uInt)cut);
                uLong c2 = crc32(0L, zref_text + cut,
                                 (uInt)(sizeof zref_text - cut));
                check("adler32_combine agrees",
                      adler32_combine(a1, a2,
                                      (z_off_t)(sizeof zref_text - cut))
                          == ZREF_TEXT_ADLER32);
                check("crc32_combine agrees",
                      crc32_combine(c1, c2,
                                    (z_off_t)(sizeof zref_text - cut))
                          == ZREF_TEXT_CRC32);
            }
        }

        /* The _z variants take a size_t rather than a uInt. Same answer;
         * different prototype, and the one a 64-bit caller should use. */
        check("crc32_z agrees with crc32",
              crc32_z(0L, zref_text, sizeof zref_text) == ZREF_TEXT_CRC32);
        check("adler32_z agrees with adler32",
              adler32_z(adler32(0L, Z_NULL, 0), zref_text, sizeof zref_text)
                  == ZREF_TEXT_ADLER32);
    }

    /* ============================================================
     *  3. decoding what this system did not encode
     * ============================================================
     *
     * The centre of the file. Every stream here was produced by Apple's
     * libcompression on the build machine — a different implementation
     * of RFC 1951 that has never seen this one — and the wrappers around
     * them were written from RFC 1950 and RFC 1952 rather than by
     * zlib's own writer. See the head of apps/zlib_ref.h.
     */
    {
        long n = inflate_all(zref_text_raw, sizeof zref_text_raw,
                             out, sizeof out, -15);
        check("raw DEFLATE from libcompression inflates",
              n == (long)sizeof zref_text);
        check("and the bytes are right",
              n > 0 && memcmp(out, zref_text, sizeof zref_text) == 0);

        n = inflate_all(zref_text_zlib, sizeof zref_text_zlib,
                        out, sizeof out, 15);
        check("the same payload inside an RFC 1950 wrapper inflates",
              n == (long)sizeof zref_text);
        check("and the bytes are right",
              n > 0 && memcmp(out, zref_text, sizeof zref_text) == 0);

        n = inflate_all(zref_text_gzip, sizeof zref_text_gzip,
                        out, sizeof out, 15 + 16);
        check("and inside an RFC 1952 wrapper, with windowBits 31",
              n == (long)sizeof zref_text);
        check("and the bytes are right",
              n > 0 && memcmp(out, zref_text, sizeof zref_text) == 0);

        /* windowBits 32+15 means "work out which of the two it is",
         * which is the mode a caller that does not know uses — and the
         * one WebKit's ContentEncoding path wants. Both wrappers must
         * come out of the same call. */
        n = inflate_all(zref_text_zlib, sizeof zref_text_zlib,
                        out, sizeof out, 15 + 32);
        check("automatic wrapper detection reads the zlib stream",
              n == (long)sizeof zref_text);
        n = inflate_all(zref_text_gzip, sizeof zref_text_gzip,
                        out2, sizeof out2, 15 + 32);
        check("and the gzip stream",
              n == (long)sizeof zref_text);
        check("and produced the same bytes both ways",
              memcmp(out, out2, sizeof zref_text) == 0);

        /* The incompressible blob. 4096 bytes of a linear congruential
         * generator's output, which DEFLATE cannot shrink — so the
         * encoder emitted stored blocks, and this is the only check here
         * that reaches inflate's stored path at all. */
        n = inflate_all(zref_blob_raw, sizeof zref_blob_raw,
                        out, sizeof out, -15);
        checkr("stored blocks inflate", n == 4096, (int)n);
        check("and the blob's CRC is right",
              n == 4096 && crc32(0L, out, 4096) == ZREF_BLOB_CRC32);
        check("the blob really did not compress",
              sizeof zref_blob_raw > 4096);
    }

    /* ============================================================
     *  4. the gzip header, read rather than skipped
     * ============================================================
     *
     * inflateGetHeader is how a caller sees FNAME, FCOMMENT and the rest
     * instead of having them discarded. The stream it reads was built
     * from RFC 1952 by hand, so what is being checked is that this
     * library's idea of the header layout matches the specification's
     * and not its own writer's.
     */
    {
        z_stream s;
        gz_header head;
        unsigned char name[64], comment[128];
        int rc;

        memset(&s, 0, sizeof s);
        rc = inflateInit2(&s, 15 + 16);
        checkr("inflateInit2 for a gzip stream", rc == Z_OK, rc);

        memset(&head, 0, sizeof head);
        head.name = name;       head.name_max = sizeof name;
        head.comment = comment; head.comm_max = sizeof comment;
        rc = inflateGetHeader(&s, &head);
        checkr("inflateGetHeader is accepted", rc == Z_OK, rc);

        s.next_in = (Bytef *)zref_text_gzip_named;
        s.avail_in = (uInt)sizeof zref_text_gzip_named;
        s.next_out = out;
        s.avail_out = (uInt)sizeof out;
        rc = inflate(&s, Z_FINISH);
        checkr("the named stream inflates", rc == Z_STREAM_END, rc);
        check("and the payload is the reference text",
              s.total_out == sizeof zref_text &&
              memcmp(out, zref_text, sizeof zref_text) == 0);

        check("the header was reported complete", head.done == 1);
        check("FNAME came through", strcmp((char *)name, "vextro.txt") == 0);
        check("FCOMMENT came through",
              strcmp((char *)comment,
                     "encoded by Apple libcompression") == 0);
        check("the OS byte is Unix", head.os == 3);
        check("and it is not a text stream by declaration", head.text == 0);
        inflateEnd(&s);

        /* A gzip stream *without* those fields must report done as well,
         * with the two pointers left alone — "no name" and "not looked
         * yet" are different states and gz_header distinguishes them. */
        memset(&s, 0, sizeof s);
        inflateInit2(&s, 15 + 16);
        memset(&head, 0, sizeof head);
        head.name = name; head.name_max = sizeof name;
        name[0] = 0xAA;
        inflateGetHeader(&s, &head);
        s.next_in = (Bytef *)zref_text_gzip;
        s.avail_in = (uInt)sizeof zref_text_gzip;
        s.next_out = out; s.avail_out = (uInt)sizeof out;
        rc = inflate(&s, Z_FINISH);
        checkr("a header with no FNAME still completes",
               rc == Z_STREAM_END && head.done == 1, rc);
        check("and the name buffer was not touched", name[0] == 0xAA);
        inflateEnd(&s);
    }

    /* ============================================================
     *  5. one byte in, one byte out
     * ============================================================
     *
     * The loop shape every real caller has, driven to its worst case. A
     * library that assumes it can always make progress — or that keeps
     * state in the wrong place across a return — comes apart here and
     * nowhere else in this file.
     */
    {
        z_stream s;
        size_t i;
        int rc = Z_OK;
        size_t produced = 0;

        memset(&s, 0, sizeof s);
        rc = inflateInit(&s);
        checkr("inflateInit", rc == Z_OK, rc);

        for (i = 0; i < sizeof zref_text_zlib && rc != Z_STREAM_END; i++) {
            s.next_in = (Bytef *)zref_text_zlib + i;
            s.avail_in = 1;
            do {
                s.next_out = out + produced;
                s.avail_out = 1;
                rc = inflate(&s, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR)
                    break;
                produced = s.total_out;
            } while (s.avail_out == 0 && rc != Z_STREAM_END &&
                     produced < sizeof out);
        }
        checkr("a stream fed one byte at a time reaches the end",
               rc == Z_STREAM_END, rc);
        check("and produced every byte",
              produced == sizeof zref_text);
        check("and the right ones",
              memcmp(out, zref_text, sizeof zref_text) == 0);
        check("total_in accounts for the whole stream",
              s.total_in == sizeof zref_text_zlib);
        inflateEnd(&s);
    }

    /* ============================================================
     *  6. failures, which have to be failures
     * ============================================================
     *
     * A decompressor that returns success on damaged input is worse than
     * one that does not work, because the damage travels. Each of these
     * is a different way for a stream to be wrong.
     */
    {
        z_stream s;
        int rc;

        /* Corrupt: one byte of the payload flipped. The deflate data is
         * still *valid* — it decodes, to 431 bytes that go wrong at
         * offset 161 — so the only thing between the caller and silent
         * corruption is the Adler-32 at the tail. This check is really
         * about whether that trailer is verified at all. */
        memset(&s, 0, sizeof s);
        inflateInit(&s);
        s.next_in = (Bytef *)zref_text_zlib_corrupt;
        s.avail_in = (uInt)sizeof zref_text_zlib_corrupt;
        s.next_out = out; s.avail_out = (uInt)sizeof out;
        rc = inflate(&s, Z_FINISH);
        checkr("a flipped payload byte is caught", rc == Z_DATA_ERROR, rc);
        check("and inflate says why", s.msg != NULL);
        inflateEnd(&s);

        /* Truncated: the last six bytes cut off, which removes the
         * Adler-32 and one byte of payload. This must *not* report
         * Z_STREAM_END, and must not report Z_DATA_ERROR either — the
         * stream is incomplete rather than wrong, and a caller feeding a
         * socket needs those to be distinguishable. */
        memset(&s, 0, sizeof s);
        inflateInit(&s);
        s.next_in = (Bytef *)zref_text_zlib_truncated;
        s.avail_in = (uInt)sizeof zref_text_zlib_truncated;
        s.next_out = out; s.avail_out = (uInt)sizeof out;
        rc = inflate(&s, Z_FINISH);
        checkr("a truncated stream does not claim to have ended",
               rc != Z_STREAM_END && rc != Z_DATA_ERROR, rc);
        check("though it did produce most of the text",
              s.total_out > sizeof zref_text - 8);
        inflateEnd(&s);

        /* Not a stream at all: the RFC 1950 header's own check is that
         * (CMF << 8 | FLG) is a multiple of 31, so one changed bit in
         * the first two bytes is refused before any payload is read. */
        {
            unsigned char bad[16];
            memcpy(bad, zref_text_zlib, sizeof bad);
            bad[0] ^= 0x01;
            memset(&s, 0, sizeof s);
            inflateInit(&s);
            s.next_in = bad; s.avail_in = (uInt)sizeof bad;
            s.next_out = out; s.avail_out = (uInt)sizeof out;
            rc = inflate(&s, Z_NO_FLUSH);
            checkr("a wrong header check is refused immediately",
                   rc == Z_DATA_ERROR, rc);
            check("naming the header",
                  s.msg != NULL && strstr(s.msg, "header") != NULL);
            inflateEnd(&s);
        }

        /* And a version mismatch, which is the check inflateInit_ makes
         * before anything else: it is how a header from one build and an
         * archive from another are supposed to be caught. Asking for a
         * version this archive is not is the only way to see it fire. */
        {
            memset(&s, 0, sizeof s);
            rc = inflateInit_(&s, "0.0.0", (int)sizeof(z_stream));
            checkr("inflateInit_ refuses a mismatched header version",
                   rc == Z_VERSION_ERROR, rc);
        }
    }

    /* ============================================================
     *  7. round trips through this library
     * ============================================================
     *
     * These prove less than section 3 and are still worth having: they
     * are what catches a deflate that is broken in a way inflate happens
     * to forgive, and they are the only exercise the compressor gets.
     */
    {
        int level;
        for (level = 0; level <= 9; level += 3) {
            uLongf clen = (uLongf)sizeof out;
            uLongf ulen = (uLongf)sizeof out2;
            int rc = compress2(out, &clen, zref_text,
                               (uLong)sizeof zref_text, level);
            char what[64];
            snprintf(what, sizeof what, "compress2 at level %d", level);
            checkr(what, rc == Z_OK, rc);

            check("the result is within compressBound",
                  clen <= compressBound((uLong)sizeof zref_text));

            rc = uncompress(out2, &ulen, out, clen);
            snprintf(what, sizeof what, "and uncompress at level %d", level);
            checkr(what, rc == Z_OK && ulen == sizeof zref_text, rc);
            check("and the text survives the round trip",
                  memcmp(out2, zref_text, sizeof zref_text) == 0);
        }

        /* Level 0 stores rather than compresses, so it must come out
         * bigger than the input — a check that fails if the level
         * argument is being ignored, which is the failure mode a round
         * trip alone cannot see. */
        {
            uLongf a = (uLongf)sizeof out, b = (uLongf)sizeof out2;
            compress2(out, &a, zref_text, (uLong)sizeof zref_text, 0);
            compress2(out2, &b, zref_text, (uLong)sizeof zref_text, 9);
            check("level 0 stores", a > sizeof zref_text);
            check("level 9 compresses", b < sizeof zref_text);
            check("and level 9 beats level 0", b < a);
            /* 430 bytes of English prose comes out around 257, which is
             * also what libcompression made of it — so this is a loose
             * bound on purpose rather than a tight one nobody could
             * defend. What it catches is a level argument being ignored. */
            check("by a margin worth having", b * 4 < a * 3);
        }

        /* A destination too small must be refused rather than truncated
         * into. */
        {
            uLongf clen = (uLongf)sizeof out;
            uLongf ulen = 32;
            compress2(out, &clen, zref_text, (uLong)sizeof zref_text, 6);
            int rc = uncompress(out2, &ulen, out, clen);
            checkr("uncompress into too small a buffer is refused",
                   rc == Z_BUF_ERROR, rc);

            /* uncompress2 reports how much of the source it consumed,
             * which is what lets a caller find the next stream in a
             * concatenation. */
            ulen = (uLongf)sizeof out2;
            uLong slen = clen + 5;
            rc = uncompress2(out2, &ulen, out, &slen);
            checkr("uncompress2 succeeds", rc == Z_OK, rc);
            check("and reports the source length it actually used",
                  slen == clen);
        }
    }

    /* ============================================================
     *  8. the modes WebKit and PNG actually use
     * ============================================================
     *
     * Raw DEFLATE with no wrapper is what PNG's IDAT is not — PNG uses
     * the zlib wrapper — but it is what HTTP's `deflate` encoding turns
     * out to be in practice, and both are one argument apart. The
     * dictionary path is the one a caller who never sets a dictionary
     * still has to survive, because Z_NEED_DICT is a return value
     * inflate can produce whether or not you expected it.
     */
    {
        z_stream s;
        int rc;
        uLong clen;

        /* Raw deflate here, raw inflate back. */
        memset(&s, 0, sizeof s);
        rc = deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        checkr("deflateInit2 with windowBits -15", rc == Z_OK, rc);
        s.next_in = (Bytef *)zref_text; s.avail_in = (uInt)sizeof zref_text;
        s.next_out = out; s.avail_out = (uInt)sizeof out;
        rc = deflate(&s, Z_FINISH);
        checkr("raw deflate finishes", rc == Z_STREAM_END, rc);
        clen = s.total_out;
        check("and produced no wrapper",
              clen > 0 && out[0] != 0x78);
        deflateEnd(&s);
        check("and raw inflate reads it back",
              inflate_all(out, clen, out2, sizeof out2, -15)
                  == (long)sizeof zref_text &&
              memcmp(out2, zref_text, sizeof zref_text) == 0);

        /* gzip out, gzip in, with the header this library writes. */
        memset(&s, 0, sizeof s);
        rc = deflateInit2(&s, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
        checkr("deflateInit2 with windowBits 31", rc == Z_OK, rc);
        s.next_in = (Bytef *)zref_text; s.avail_in = (uInt)sizeof zref_text;
        s.next_out = out; s.avail_out = (uInt)sizeof out;
        rc = deflate(&s, Z_FINISH);
        checkr("gzip deflate finishes", rc == Z_STREAM_END, rc);
        clen = s.total_out;
        check("and wrote a gzip magic",
              out[0] == 0x1f && out[1] == 0x8b && out[2] == 0x08);
        deflateEnd(&s);
        check("and it reads back with automatic detection",
              inflate_all(out, clen, out2, sizeof out2, 15 + 32)
                  == (long)sizeof zref_text);

        /* deflateBound must be an upper bound on what deflate actually
         * produces, which is the number a caller sizes a buffer with. */
        {
            uLong bound;
            memset(&s, 0, sizeof s);
            deflateInit(&s, 9);
            bound = deflateBound(&s, (uLong)sizeof zref_text);
            s.next_in = (Bytef *)zref_text;
            s.avail_in = (uInt)sizeof zref_text;
            s.next_out = out; s.avail_out = (uInt)sizeof out;
            deflate(&s, Z_FINISH);
            check("deflateBound bounds the real output",
                  s.total_out <= bound);
            check("and is not absurdly loose",
                  bound < sizeof zref_text * 2 + 64);
            deflateEnd(&s);
        }

        /* A dictionary. inflate must stop with Z_NEED_DICT and report
         * the *dictionary's* Adler-32, which is how a caller picks the
         * right one out of a set it holds. */
        {
            static const unsigned char dict[] =
                "There is a  because there was no way to ";
            uLong dict_adler = adler_of(dict, sizeof dict - 1);
            uLong dlen;

            memset(&s, 0, sizeof s);
            deflateInit(&s, 6);
            rc = deflateSetDictionary(&s, dict, (uInt)(sizeof dict - 1));
            checkr("deflateSetDictionary", rc == Z_OK, rc);
            s.next_in = (Bytef *)zref_text;
            s.avail_in = (uInt)sizeof zref_text;
            s.next_out = out; s.avail_out = (uInt)sizeof out;
            rc = deflate(&s, Z_FINISH);
            checkr("deflate with a dictionary finishes",
                   rc == Z_STREAM_END, rc);
            dlen = s.total_out;
            deflateEnd(&s);

            memset(&s, 0, sizeof s);
            inflateInit(&s);
            s.next_in = out; s.avail_in = (uInt)dlen;
            s.next_out = out2; s.avail_out = (uInt)sizeof out2;
            rc = inflate(&s, Z_NO_FLUSH);
            checkr("inflate asks for the dictionary", rc == Z_NEED_DICT, rc);
            check("and names it by its Adler-32", s.adler == dict_adler);
            rc = inflateSetDictionary(&s, dict, (uInt)(sizeof dict - 1));
            checkr("inflateSetDictionary is accepted", rc == Z_OK, rc);
            rc = inflate(&s, Z_FINISH);
            checkr("and the stream then completes", rc == Z_STREAM_END, rc);
            check("with the original text",
                  s.total_out == sizeof zref_text &&
                  memcmp(out2, zref_text, sizeof zref_text) == 0);
            inflateEnd(&s);
        }
    }

    /* ============================================================
     *  9. gzFile, on the NTFS volume
     * ============================================================
     *
     * The only part of zlib that touches the operating system. gzlib.c
     * calls open with a mode argument, gzread.c calls read and lseek,
     * gzwrite.c calls write, gzclose calls close, and the error path
     * calls strerror and snprintf. This section drives all of that
     * against the real volume rather than a buffer.
     *
     * ---- and why the read half comes off a file the build wrote ----
     *
     * A boot self-test runs with nobody signed in, which is also the
     * state a machine sitting at its login screen is in, and creating a
     * file in that state is refused at open() by the elevation gateway.
     * apps/fdtest.c asserts that refusal rather than working around it,
     * and so does the write section below.
     *
     * So the reading is done on /zlibref.gz, which `make` puts on the
     * volume — the same bytes as zref_text_gzip_named, extracted from
     * the header by tools/carray.py, which means it is a gzip file
     * produced by Apple's compressor and wrapped from RFC 1952 rather
     * than anything this library wrote. That is a better test than
     * reading back what we just compressed would have been, and it works
     * whether or not anybody is signed in.
     */
    {
        static const char refpath[] = "/zlibref.gz";
        static const char badpath[] = "/zlibbad.gz";
        static const char plain[]   = "/about.txt";
        static const char scratch[] = "/zlibtest.gz";
        gzFile gz;
        int rc, n;
        char line[160];

        /* First, that the bytes on the disk are the bytes in the header.
         * If NTFS delivered something else, every check below would be
         * about the wrong file and the failures would read as zlib
         * failures. */
        {
            int fd = open(refpath, O_RDONLY);
            check("the reference gzip file is on the volume", fd >= 0);
            if (fd >= 0) {
                ssize_t got = read(fd, out, sizeof out);
                check("with the length it was built with",
                      got == (ssize_t)sizeof zref_text_gzip_named);
                check("and byte for byte the same content",
                      got > 0 &&
                      memcmp(out, zref_text_gzip_named,
                             sizeof zref_text_gzip_named) == 0);
                check("beginning with the gzip magic",
                      out[0] == 0x1f && out[1] == 0x8b && out[2] == 0x08);
                close(fd);
            }
        }

        gz = gzopen(refpath, "rb");
        check("gzopen for reading", gz != NULL);
        if (gz) {
            check("gzdirect is false for a compressed file",
                  gzdirect(gz) == 0);
            n = gzread(gz, out, (unsigned)sizeof zref_text);
            checkr("gzread returns the text", n == (int)sizeof zref_text, n);
            check("and it is the text",
                  memcmp(out, zref_text, sizeof zref_text) == 0);
            check("gztell is where the reads left it",
                  gztell(gz) == (z_off_t)sizeof zref_text);
            check("and gzeof is not yet true", gzeof(gz) == 0);

            /* gzeof reports that a read *has* hit the end, not that the
             * next one will — so it takes one more read to become
             * true, and a test that checked it too early would pass for
             * the wrong reason. */
            check("a read past the end returns nothing",
                  gzread(gz, out, 16) == 0);
            check("and now gzeof is true", gzeof(gz) != 0);

            /* Seeking backwards in a compressed file means restarting
             * the stream, which is the expensive path and the one worth
             * checking works at all. */
            check("gzseek back to the start",
                  gzseek(gz, 0, SEEK_SET) == 0);
            check("and re-reads the first bytes",
                  gzread(gz, out, 16) == 16 &&
                  memcmp(out, zref_text, 16) == 0);
            /* And forwards, which zlib implements by decompressing and
             * discarding rather than by seeking the descriptor. */
            check("gzseek forwards lands where it says",
                  gzseek(gz, 100, SEEK_SET) == 100);
            check("and reads the right bytes there",
                  gzread(gz, out, 16) == 16 &&
                  memcmp(out, zref_text + 100, 16) == 0);

            /* gzgets stops at a newline or at the buffer, whichever
             * comes first. The reference text is one paragraph with a
             * single newline at the very end, so with 160 bytes of room
             * this is the second case. */
            check("gzrewind", gzrewind(gz) == 0);
            check("gzgets fills the buffer when there is no newline",
                  gzgets(gz, line, (int)sizeof line) == line &&
                  strlen(line) == sizeof line - 1 &&
                  memcmp(line, zref_text, sizeof line - 1) == 0);

            rc = gzclose(gz);
            checkr("gzclose after reading", rc == Z_OK, rc);
        }

        /* A file that is not compressed at all. gzopen reads it through
         * unchanged, which is what makes gz* a drop-in for fopen — and
         * gzdirect is how a caller can tell which it got. /about.txt is
         * shipped by the build, so this needs no permission either. */
        {
            int fd = open(plain, O_RDONLY);
            ssize_t got = fd >= 0 ? read(fd, out2, sizeof out2) : -1;
            if (fd >= 0) close(fd);
            check("the plain text file is on the volume", got > 0);

            gz = gzopen(plain, "rb");
            check("gzopen accepts an uncompressed file", gz != NULL);
            if (gz) {
                n = gzread(gz, out, (unsigned)sizeof out);
                checkr("and passes its bytes through", n == (int)got, n);
                check("unchanged",
                      n == (int)got && memcmp(out, out2, (size_t)got) == 0);
                check("with gzdirect true", gzdirect(gz) != 0);
                gzclose(gz);
            }
        }

        /* The error path, which is the one that needs gz_error, and with
         * it snprintf and strerror. /zlibbad.gz carries the original
         * text's CRC-32 over a flipped payload, so it reads back a
         * plausible-looking 431 bytes and only then fails — which is
         * exactly the corruption a caller cannot detect for itself. */
        {
            int errnum = 0;
            const char *msg;

            gz = gzopen("/no-such-file-at-all.gz", "rb");
            check("gzopen of a missing path fails", gz == NULL);

            gz = gzopen(badpath, "rb");
            check("gzopen of the damaged file succeeds", gz != NULL);
            if (gz) {
                /* With the default buffer, zlib reads ahead far enough
                 * to reach the wrong CRC before it has returned any of
                 * the 431 bytes — so the caller gets -1 and *nothing*,
                 * which is the strongest outcome available and not the
                 * one I expected when this check was written. It is
                 * asserted rather than described because it depends on
                 * the file being smaller than the internal buffer, and
                 * a future reference file that is not would change it. */
                int total = 0;
                do {
                    n = gzread(gz, out, 64);
                    if (n > 0) total += n;
                } while (n > 0);
                checkr("reading the damaged file fails", n < 0, n);
                checkr("and with the default buffer, nothing at all is "
                       "handed back first", total == 0, total);
                msg = gzerror(gz, &errnum);
                checkr("gzerror reports a data error",
                       errnum == Z_DATA_ERROR, errnum);
                check("with a message", msg != NULL && *msg);
                gzclearerr(gz);
                msg = gzerror(gz, &errnum);
                check("gzclearerr resets it", errnum == Z_OK);
                gzclose(gz);
            }

            /* And the same file read through a buffer too small to see
             * the end coming, which is what a caller streaming a large
             * file is really doing. Here the earlier chunks *are* handed
             * back before anything is known to be wrong — so a program
             * that acts on bytes as they arrive has already acted on
             * some of them by the time the error appears. That is a
             * property of gzread rather than a fault in it, and it is
             * the reason the failure above is worth pinning down. */
            gz = gzopen(badpath, "rb");
            if (gz) {
                int total = 0;
                check("gzbuffer sets a small buffer before any read",
                      gzbuffer(gz, 64) == 0);
                do {
                    n = gzread(gz, out + total, 64);
                    if (n > 0) total += n;
                } while (n > 0);
                checkr("the damaged file still fails", n < 0, n);
                checkr("but only after handing back most of it",
                       total >= 256 && total <= (int)sizeof zref_text + 1,
                       total);
                /* And what it handed back was already wrong. The first
                 * 161 bytes are the real text — the flipped bit is
                 * further in than that — and everything after is not.
                 * Nothing in the stream said so at the time. */
                check("the first 161 bytes were genuinely the text",
                      total > 161 && memcmp(out, zref_text, 161) == 0);
                check("and the rest was already corrupt",
                      total > 161 &&
                      memcmp(out + 161, zref_text + 161,
                             (size_t)total - 161) != 0);
                gzclose(gz);
            }
        }

        /* ---- and the write half, which needs somebody signed in ----
         *
         * Two correct outcomes, the same two apps/fdtest.c has. With a
         * session that grants it the file is written, closed, read back
         * and removed; with nobody signed in, gzopen fails because the
         * open() underneath it was refused, and that refusal is the
         * security property. The gzwrite path is not otherwise
         * unreachable — section 8 compresses with the same deflate — so
         * what is lost in a headless run is the descriptor plumbing,
         * which fdtest and the kernel's own vfs_selftest cover.
         */
        errno = 0;
        gz = gzopen(scratch, "wb9");
        if (gz == NULL) {
            check("with nobody signed in, creating a file is refused",
                  errno == EPERM);
            printf("       (no session: the gzwrite half is not reachable "
                   "here)\n");
        } else {
            n = gzwrite(gz, zref_text, (unsigned)sizeof zref_text);
            checkr("gzwrite writes the whole buffer",
                   n == (int)sizeof zref_text, n);
            n = gzprintf(gz, "line %d of %s\n", 2, "two");
            checkr("gzprintf formats through vsnprintf", n == 14, n);
            rc = gzflush(gz, Z_SYNC_FLUSH);
            checkr("gzflush", rc == Z_OK, rc);
            rc = gzclose(gz);
            checkr("gzclose", rc == Z_OK, rc);

            gz = gzopen(scratch, "rb");
            check("and it reads back", gz != NULL);
            if (gz) {
                n = gzread(gz, out, (unsigned)sizeof out);
                checkr("as text and formatted line together",
                       n == (int)sizeof zref_text + 14, n);
                check("with the text first",
                      memcmp(out, zref_text, sizeof zref_text) == 0);
                check("and the formatted line after it",
                      memcmp(out + sizeof zref_text,
                             "line 2 of two\n", 14) == 0);
                gzclose(gz);
            }
            unlink(scratch);
        }
    }

    printf("zlibtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
