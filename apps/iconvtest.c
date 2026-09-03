/*
 * iconvtest — GNU libiconv in ring 3.
 *
 * The eighteenth port, and the one that closes a gap this tree has been
 * writing down rather than fixing. `third_party/libxml2-port/config.h`
 * has said since it was written that "there is no iconv in this C
 * library", and GLib 2.74's meson.build:2060 makes
 * `dependency('iconv')` required outright. They were the same absence.
 *
 * ---- where the expectations come from ----
 *
 * A character-encoding library is the one kind of port where the
 * reference needs no second implementation at all, because the mappings
 * *are* the standard. ISO-8859-15 puts a euro sign at 0xA4 because
 * ISO/IEC 8859-15 says so; Windows-1252 puts one at 0x80 because
 * Microsoft's published table says so; Shift_JIS encodes U+3042 as
 * 0x82 0xA0 because JIS X 0208 and its Shift encoding say so. None of
 * those is libiconv's opinion, and every table below is written as the
 * standard's fact rather than as an observation of this library.
 *
 * Two of the sections are chosen to *discriminate* rather than merely
 * to pass. 0xA4 is a currency sign in Latin-1 and a euro sign in
 * Latin-9, so section 2 fails if the two tables were confused — which
 * is the mistake a table-driven converter actually makes, and one that
 * a test using only unambiguous bytes would never see.
 *
 * ---- and the three errno values that are the whole contract ----
 *
 * iconv() has one return value and three distinct failures, and a
 * caller that cannot tell them apart cannot stream. EILSEQ means the
 * input is wrong and will still be wrong later; EINVAL means the input
 * is incomplete and may be fine once more arrives; E2BIG means the
 * *output* ran out and nothing is wrong with the input at all. Section
 * 6 separates them, because a converter fed by a socket meets all
 * three and must do something different each time.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <iconv.h>
#include <localcharset.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

static char out[256];

/*
 * Convert one buffer in one call. Returns the number of bytes produced,
 * or -1 with errno set — which is the shape every check below wants,
 * because the errno is half the answer.
 */
static long conv(const char *to, const char *from,
                 const void *in, size_t inlen) {
    iconv_t cd = iconv_open(to, from);
    char *inp = (char *)in;
    char *outp = out;
    size_t inleft = inlen, outleft = sizeof out;
    size_t rc;

    if (cd == (iconv_t)-1) return -2;
    memset(out, 0, sizeof out);
    rc = iconv(cd, &inp, &inleft, &outp, &outleft);
    if (rc == (size_t)-1) {
        int saved = errno;
        iconv_close(cd);
        errno = saved;
        return -1;
    }
    iconv_close(cd);
    return (long)(sizeof out - outleft);
}

/* Did the last conversion produce exactly these bytes? */
static int is(const void *expect, size_t n, long got) {
    return got == (long)n && memcmp(out, expect, n) == 0;
}

int main(void) {
    printf("iconvtest: GNU libiconv %d.%d\n",
           _libiconv_version >> 8, _libiconv_version & 0xFF);

    /* ============================================================
     *  1. the library, and the locale it found
     * ============================================================ */
    {
        check("the version is 1.18", _libiconv_version == 0x0112);

        /*
         * locale_charset() is the one place in this library that asks
         * the operating system anything. Its first choice is
         * nl_langinfo(CODESET), which this C library does not have — so
         * it takes upstream's fallback: LC_ALL, LC_CTYPE, LANG out of
         * the environment, then setlocale(LC_CTYPE, NULL).
         *
         * A ring-3 process here starts with an empty environment and
         * the C locale, so the answer is ASCII. That is correct rather
         * than degraded: this machine has no locale, and ASCII is what
         * "no locale" means. It matters only to iconv_open("", ...),
         * the form that asks for "whatever the user's encoding is" —
         * and every consumer in this tree names its encodings.
         */
        {
            const char *cs = locale_charset();
            printf("       locale_charset() -> %s\n", cs ? cs : "(null)");
            check("locale_charset answers something", cs != NULL && *cs);
            check("and with no locale it is ASCII",
                  cs != NULL && strcmp(cs, "ASCII") == 0);
        }

        /* The empty name means "the locale's encoding", so it must open
         * and must behave as the answer above. */
        {
            iconv_t cd = iconv_open("UTF-8", "");
            check("iconv_open(\"UTF-8\", \"\") opens", cd != (iconv_t)-1);
            if (cd != (iconv_t)-1) iconv_close(cd);
        }

        /* An encoding that does not exist is refused, and refused with
         * EINVAL rather than by returning a converter that does
         * something approximate. A library that guessed here would
         * silently corrupt text. */
        errno = 0;
        check("an unknown encoding is refused",
              iconv_open("UTF-8", "NO-SUCH-CHARSET") == (iconv_t)-1);
        checkr("with EINVAL", errno == EINVAL, errno);

        /* Aliases are part of the standard too: ISO-8859-1 is also
         * latin1 and also ISO_8859-1:1987, and libiconv's alias table
         * is what makes a document that names any of them work. */
        {
            iconv_t cd = iconv_open("UTF-8", "latin1");
            check("the alias \"latin1\" resolves", cd != (iconv_t)-1);
            if (cd != (iconv_t)-1) iconv_close(cd);
            cd = iconv_open("UTF-8", "csisolatin1");
            check("and so does \"csisolatin1\"", cd != (iconv_t)-1);
            if (cd != (iconv_t)-1) iconv_close(cd);
        }
    }

    /* ============================================================
     *  2. the two Latin tables that differ in one byte
     * ============================================================
     *
     * ISO-8859-1 maps every byte to the codepoint of the same value —
     * Latin-1 *is* the first 256 codepoints of Unicode, by
     * construction. ISO-8859-15 is the same table with eight positions
     * replaced, and the famous one is 0xA4: CURRENCY SIGN in Latin-1,
     * EURO SIGN in Latin-9.
     *
     * That single byte is the discriminating test. A converter that had
     * the two tables confused passes every check made with unambiguous
     * bytes and fails here.
     */
    {
        static const unsigned char l1[]  = { 0xE9, 0xA4 };  /* é, ¤ */
        /* U+00E9 = C3 A9, U+00A4 = C2 A4 */
        static const unsigned char u8_l1[] = { 0xC3, 0xA9, 0xC2, 0xA4 };
        /* Latin-9: 0xE9 is still é, 0xA4 is U+20AC = E2 82 AC */
        static const unsigned char u8_l9[] = { 0xC3, 0xA9, 0xE2, 0x82, 0xAC };

        long n = conv("UTF-8", "ISO-8859-1", l1, sizeof l1);
        check("ISO-8859-1 0xE9 0xA4 becomes é and CURRENCY SIGN",
              is(u8_l1, sizeof u8_l1, n));

        n = conv("UTF-8", "ISO-8859-15", l1, sizeof l1);
        check("the same bytes in ISO-8859-15 give é and EURO SIGN",
              is(u8_l9, sizeof u8_l9, n));

        /* And back, which is a different table in the other direction. */
        n = conv("ISO-8859-15", "UTF-8", u8_l9, sizeof u8_l9);
        check("and the euro round-trips back to 0xA4 in Latin-9",
              is(l1, sizeof l1, n));

        /* A euro sign cannot be represented in Latin-1 at all, so the
         * conversion must fail rather than substitute something. */
        {
            static const unsigned char euro[] = { 0xE2, 0x82, 0xAC };
            errno = 0;
            n = conv("ISO-8859-1", "UTF-8", euro, sizeof euro);
            checkr("a euro sign has no Latin-1 encoding, and is refused",
                   n == -1 && errno == EILSEQ, (int)errno);
        }
    }

    /* ============================================================
     *  3. Windows-1252, which is not Latin-1 however often it is
     *     mislabelled as it
     * ============================================================
     *
     * CP1252 fills 0x80..0x9F, which ISO-8859-1 leaves as control
     * characters. This is the single most common encoding on the older
     * web and the one a browser meets constantly — HTML5 in fact
     * requires that a document declaring ISO-8859-1 be decoded as
     * windows-1252 instead.
     */
    {
        static const unsigned char cp[] = { 0x80, 0x93, 0x94 };
        /* U+20AC E2 82 AC, U+201C E2 80 9C, U+201D E2 80 9D */
        static const unsigned char u8[] = {
            0xE2, 0x82, 0xAC, 0xE2, 0x80, 0x9C, 0xE2, 0x80, 0x9D
        };
        long n = conv("UTF-8", "CP1252", cp, sizeof cp);
        check("CP1252 0x80 is a euro and 0x93/0x94 are curly quotes",
              is(u8, sizeof u8, n));

        /* 0x81 is one of the five positions Microsoft left unassigned,
         * and an unassigned byte is an error rather than a passthrough.
         * A converter that mapped it to U+0081 would be inventing a
         * character the encoding does not define. */
        {
            static const unsigned char bad[] = { 0x81 };
            errno = 0;
            n = conv("UTF-8", "CP1252", bad, sizeof bad);
            checkr("but 0x81 is unassigned and is refused",
                   n == -1 && errno == EILSEQ, (int)errno);
        }

        /* The same byte in Latin-1 is a control character and perfectly
         * legal, which is the other half of why the two must not be
         * confused. */
        {
            static const unsigned char c1[] = { 0x81 };
            static const unsigned char u8c1[] = { 0xC2, 0x81 };
            n = conv("UTF-8", "ISO-8859-1", c1, sizeof c1);
            check("while in Latin-1 the same byte is a legal control",
                  is(u8c1, sizeof u8c1, n));
        }
    }

    /* ============================================================
     *  4. the UTF family, including a character outside the BMP
     * ============================================================
     *
     * The surrogate pair is the part worth checking. U+1F600 is beyond
     * U+FFFF, so UTF-16 has to encode it as two units — D83D DE00 —
     * and a converter that treated UTF-16 as fixed-width would produce
     * one wrong character rather than fail.
     */
    {
        /* U+1F600 GRINNING FACE */
        static const unsigned char u8[]    = { 0xF0, 0x9F, 0x98, 0x80 };
        static const unsigned char u16le[] = { 0x3D, 0xD8, 0x00, 0xDE };
        static const unsigned char u16be[] = { 0xD8, 0x3D, 0xDE, 0x00 };
        static const unsigned char u32le[] = { 0x00, 0xF6, 0x01, 0x00 };
        long n;

        n = conv("UTF-16LE", "UTF-8", u8, sizeof u8);
        check("U+1F600 becomes a surrogate pair in UTF-16LE",
              is(u16le, sizeof u16le, n));
        n = conv("UTF-16BE", "UTF-8", u8, sizeof u8);
        check("and the same pair, byte-swapped, in UTF-16BE",
              is(u16be, sizeof u16be, n));
        n = conv("UCS-4LE", "UTF-8", u8, sizeof u8);
        check("UCS-4LE keeps it as one 32-bit unit",
              is(u32le, sizeof u32le, n));
        n = conv("UTF-8", "UTF-16LE", u16le, sizeof u16le);
        check("and it comes back to the same UTF-8",
              is(u8, sizeof u8, n));

        /* A lone surrogate is not a character. UTF-16 can hold the
         * bytes; Unicode says they do not encode anything, and the
         * converter must say so. */
        {
            static const unsigned char lone[] = { 0x3D, 0xD8 };
            errno = 0;
            n = conv("UTF-8", "UTF-16LE", lone, sizeof lone);
            checkr("a lone high surrogate is not a character",
                   n == -1 && (errno == EILSEQ || errno == EINVAL),
                   (int)errno);
        }

        /* Overlong UTF-8: C0 80 is a two-byte encoding of NUL, which is
         * exactly what the standard forbids, and is the classic way
         * past a filter that only looks for a bare 0x00. */
        {
            static const unsigned char overlong[] = { 0xC0, 0x80 };
            errno = 0;
            n = conv("UTF-16LE", "UTF-8", overlong, sizeof overlong);
            checkr("an overlong NUL is refused", n == -1 && errno == EILSEQ,
                   (int)errno);
        }
    }

    /* ============================================================
     *  5. the encodings that need a table rather than arithmetic
     * ============================================================
     *
     * Latin-1 and the UTFs can be converted with shifts and masks.
     * These cannot: every mapping is a row in a table, and the tables
     * are what makes libiconv a megabyte.
     */
    {
        /* U+3042 HIRAGANA LETTER A = E3 81 82 in UTF-8 */
        static const unsigned char u8_a[] = { 0xE3, 0x81, 0x82 };
        static const unsigned char sjis[]  = { 0x82, 0xA0 };
        static const unsigned char eucjp[] = { 0xA4, 0xA2 };
        /* U+0430 CYRILLIC SMALL LETTER A = D0 B0 in UTF-8 */
        static const unsigned char u8_ru[] = { 0xD0, 0xB0 };
        static const unsigned char koi8[]  = { 0xC1 };
        long n;

        n = conv("UTF-8", "SHIFT_JIS", sjis, sizeof sjis);
        check("Shift_JIS 0x82 0xA0 is HIRAGANA LETTER A",
              is(u8_a, sizeof u8_a, n));
        n = conv("UTF-8", "EUC-JP", eucjp, sizeof eucjp);
        check("and EUC-JP encodes the same character differently",
              is(u8_a, sizeof u8_a, n));
        n = conv("SHIFT_JIS", "UTF-8", u8_a, sizeof u8_a);
        check("it round-trips back into Shift_JIS",
              is(sjis, sizeof sjis, n));

        n = conv("UTF-8", "KOI8-R", koi8, sizeof koi8);
        check("KOI8-R 0xC1 is CYRILLIC SMALL LETTER A",
              is(u8_ru, sizeof u8_ru, n));

        /* GB18030 is the one that covers all of Unicode in a
         * variable-width Chinese encoding, and is what a browser meets
         * on mainland sites. U+4E2D = E4 B8 AD in UTF-8, D6 D0 in
         * GB18030. */
        {
            static const unsigned char u8_zh[] = { 0xE4, 0xB8, 0xAD };
            static const unsigned char gb[]    = { 0xD6, 0xD0 };
            n = conv("UTF-8", "GB18030", gb, sizeof gb);
            check("GB18030 0xD6 0xD0 is U+4E2D", is(u8_zh, sizeof u8_zh, n));
        }
    }

    /* ============================================================
     *  6. the three failures, which have to be three
     * ============================================================
     *
     * The whole of iconv's streaming contract. A caller that treats
     * these alike either discards good input or loops for ever on bad
     * input.
     */
    {
        iconv_t cd = iconv_open("UTF-8", "UTF-16LE");
        char *inp, *outp;
        size_t inleft, outleft, rc;

        check("a converter for the failure cases opens", cd != (iconv_t)-1);
        if (cd != (iconv_t)-1) {
            /* EINVAL: an incomplete unit at the end of the buffer.
             * Nothing is wrong — more bytes may be coming. */
            {
                static const unsigned char half[] = { 0x41 };
                inp = (char *)half; inleft = 1;
                outp = out; outleft = sizeof out;
                errno = 0;
                rc = iconv(cd, &inp, &inleft, &outp, &outleft);
                checkr("an incomplete character is EINVAL, not an error "
                       "in the input",
                       rc == (size_t)-1 && errno == EINVAL, (int)errno);
                check("and the unconsumed byte is left for next time",
                      inleft == 1);
            }

            /* E2BIG: the input is perfect and the output is full. */
            {
                static const unsigned char text[] = {
                    0x41, 0x00, 0x42, 0x00, 0x43, 0x00
                };
                char tiny[2];
                inp = (char *)text; inleft = sizeof text;
                outp = tiny; outleft = 1;
                errno = 0;
                rc = iconv(cd, &inp, &inleft, &outp, &outleft);
                checkr("a full output buffer is E2BIG",
                       rc == (size_t)-1 && errno == E2BIG, (int)errno);
                check("with one character converted and the rest waiting",
                      inleft == 4);
            }

            /* And the reset call, which is how a caller starts again
             * after any of the three. */
            errno = 0;
            outp = out; outleft = sizeof out;
            rc = iconv(cd, NULL, NULL, &outp, &outleft);
            check("iconv(cd, NULL, ...) resets the conversion state",
                  rc != (size_t)-1);
            iconv_close(cd);
        }

        /* EILSEQ, from the other direction: a character the target
         * encoding has no room for. */
        {
            static const unsigned char u8_a[] = { 0xE3, 0x81, 0x82 };
            errno = 0;
            checkr("a hiragana has no ASCII encoding, and that is EILSEQ",
                   conv("ASCII", "UTF-8", u8_a, sizeof u8_a) == -1 &&
                   errno == EILSEQ, (int)errno);
        }
    }

    /* ============================================================
     *  7. //TRANSLIT and //IGNORE, which are libiconv's own
     * ============================================================
     *
     * Neither is in POSIX; both are GNU extensions, and both are what a
     * caller reaches for when a hard failure is the wrong answer — a
     * browser rendering a page badly rather than not at all. They are
     * also the two suffixes that make an encoding name not just an
     * encoding name, so they exercise the parser in iconv_open.
     */
    {
        static const unsigned char u8[] = {
            'c', 'a', 'f', 0xC3, 0xA9        /* café */
        };
        long n;

        errno = 0;
        n = conv("ASCII", "UTF-8", u8, sizeof u8);
        checkr("café has no ASCII encoding", n == -1 && errno == EILSEQ,
               (int)errno);

        n = conv("ASCII//TRANSLIT", "UTF-8", u8, sizeof u8);
        checkr("//TRANSLIT converts it anyway", n > 0, (int)n);
        /*
         * And it writes `caf'e`, not `cafe`, which is worth getting
         * right rather than assuming. libiconv's table
         * (lib/translit.def:98) maps U+00E9 to *two* characters —
         * U+00B4 ACUTE ACCENT followed by 'e' — and then transliterates
         * the accent again, because U+00B4 is not ASCII either, into an
         * apostrophe. So the result is five bytes and the accent
         * survives as a separate mark rather than being dropped.
         *
         * That is a deliberate choice on upstream's part and the more
         * useful one: `caf'e` can be read back as an accented e, and
         * `cafe` cannot. This check was written expecting the naive
         * answer and the machine corrected it.
         */
        check("by keeping the accent as a mark: caf'e, not cafe",
              n == 5 && memcmp(out, "caf'e", 5) == 0);

        n = conv("ASCII//IGNORE", "UTF-8", u8, sizeof u8);
        /* //IGNORE still reports the loss through errno at the end —
         * it drops the character rather than the diagnosis. */
        check("//IGNORE drops what it cannot encode",
              (n == 3 && memcmp(out, "caf", 3) == 0) ||
              (n == -1 && memcmp(out, "caf", 3) == 0));

        /* And a transliteration with no plausible ASCII form is still a
         * failure with //TRANSLIT alone — the option is not a promise
         * to produce something for everything. */
        {
            static const unsigned char zh[] = { 0xE4, 0xB8, 0xAD };
            n = conv("ASCII//TRANSLIT", "UTF-8", zh, sizeof zh);
            check("but //TRANSLIT is not a promise: U+4E2D becomes '?'",
                  n <= 0 || (n >= 1 && out[0] == '?'));
        }
    }

    /* ============================================================
     *  8. what libxml2 and GLib will actually ask for
     * ============================================================
     *
     * Both consumers name their encodings from the document or from
     * their caller, and both meet the same handful constantly. This
     * section is that handful, opened by the names they use.
     */
    {
        static const char *const names[] = {
            "UTF-8", "UTF-16", "UTF-16LE", "UTF-16BE", "UCS-2", "UCS-4",
            "ISO-8859-1", "ISO-8859-2", "ISO-8859-5", "ISO-8859-15",
            "WINDOWS-1250", "WINDOWS-1251", "WINDOWS-1252",
            "KOI8-R", "SHIFT_JIS", "EUC-JP", "EUC-KR", "BIG5", "GBK",
            "GB18030", "ASCII"
        };
        int i, opened = 0;
        for (i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
            iconv_t cd = iconv_open("UTF-8", names[i]);
            if (cd != (iconv_t)-1) { opened++; iconv_close(cd); }
            else printf("       missing encoding: %s\n", names[i]);
        }
        checkr("every encoding a browser meets is present",
               opened == (int)(sizeof names / sizeof names[0]), opened);

        /* And one that is not built, because --enable-extra-encodings
         * is off. The check is that it is *refused* rather than
         * silently mapped to something near it: a converter that
         * answered EUC-JISX0213 with EUC-JP would corrupt text rather
         * than fail, which is the worse of the two outcomes. */
        {
            iconv_t cd = iconv_open("UTF-8", "EUC-JISX0213");
            check("and an extra encoding this build omits is refused, "
                  "not approximated",
                  cd == (iconv_t)-1);
            if (cd != (iconv_t)-1) iconv_close(cd);
        }
    }

    printf("iconvtest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
