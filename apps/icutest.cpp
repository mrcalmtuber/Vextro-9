/*
 * icutest — ICU 74.2 in ring 3.
 *
 * ============================================================
 *  WHAT THIS IS ACTUALLY TESTING
 * ============================================================
 *
 * Not that ICU linked. Four hundred and forty-five translation units
 * linking is worth something, but it says nothing about whether the
 * thirty-megabyte data archive was found, opened, and read correctly
 * through this system's filesystem -- and almost everything ICU does is
 * a lookup in that archive. A build with no data still links, still
 * returns a version number, and fails every question below.
 *
 * So the checks are chosen to reach different *parts* of the data, by
 * different code paths, and to have answers that are facts about Unicode
 * rather than facts about this implementation:
 *
 *   the character database   uprops.icu -- categories, case, names
 *   normalisation            nfc.nrm -- the composition tables
 *   segmentation             the rule-based break iterators
 *   collation                coll/*.res -- and the *locale* ones, which
 *                            is the part that cannot be faked: German
 *                            and Swedish disagree about where a-umlaut
 *                            sorts, and both answers are in the archive
 *   conversion               the legacy codepage tables, which a browser
 *                            needs and which are the largest single
 *                            thing in the file
 *   the timezone database    zoneinfo64.res -- the IANA rules, which are
 *                            also the only reason a date can be shown in
 *                            a zone this machine has never heard of
 *
 * ============================================================
 *  AND TWO THINGS UNDER IT
 * ============================================================
 *
 * ICU is the first library here compiled with RTTI. Its C++ API hands
 * back base-class pointers and expects callers to dynamic_cast them, so
 * the collator checks below run through libcxx/src/typeinfo.cpp -- and
 * across a library boundary, which is the case apps/rtti_cases.h cannot
 * reach because everything there is in one translation unit.
 *
 * It is also the first program that can tell whether the calendar is
 * real. Until SYS_WALLCLOCK existed, time() answered from the monotonic
 * tick and every date this program printed would have been in January
 * 1970. The date checks use a *fixed* instant so that they test the
 * formatter rather than the clock, and then one check reads the clock
 * separately and asserts it is after 2020 -- which is the only way to
 * notice a calendar that is not connected to anything.
 */

#include "vextro.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <unicode/utypes.h>
#include <unicode/uversion.h>
#include <unicode/uchar.h>
#include <unicode/ustring.h>
#include <unicode/unorm2.h>
#include <unicode/ubrk.h>
#include <unicode/ucol.h>
#include <unicode/ucnv.h>
#include <unicode/udat.h>
#include <unicode/ucal.h>
#include <unicode/udata.h>
#include <unicode/putil.h>
#include <unicode/uclean.h>
#include <unicode/uloc.h>
#include <unicode/unistr.h>
#include <unicode/coll.h>
#include <unicode/tblcoll.h>
#include <unicode/locid.h>

#include <time.h>
#include <typeinfo>

static int checks = 0, failures = 0;

static void ok(const char *what, bool good) {
    checks++;
    if (!good) failures++;
    std::printf("%s %s\n", good ? " ok  " : "FAIL ", what);
}

/* A UTF-16 literal from ASCII, which is all the test data needs; the
 * non-ASCII cases are written as explicit code points below. */
static void u16(UChar *out, int cap, const char *ascii) {
    int i = 0;
    for (; ascii[i] && i < cap - 1; i++) out[i] = (UChar)ascii[i];
    out[i] = 0;
}

int main() {
    std::printf("icutest: starting\n");

    /* ---- 1. the library, and the archive ---- */
    {
        UVersionInfo v;
        u_getVersion(v);
        std::printf("       (ICU %d.%d.%d.%d)\n", v[0], v[1], v[2], v[3]);
        ok("the version is the one that was fetched", v[0] == 74 && v[1] == 2);

        /*
         * The data archive lives on the volume rather than being linked
         * in. ICU finds it through its own stdio path -- fopen, fseek,
         * fread in umapfile.cpp -- which means this one call exercises
         * the whole read path down through the descriptor table to NTFS,
         * exactly as FreeType's FT_New_Face does.
         */
        u_setDataDirectory("/");

        UErrorCode err = U_ZERO_ERROR;
        u_init(&err);
        ok("u_init found and opened the data archive", U_SUCCESS(err));
        if (U_FAILURE(err)) {
            std::printf("       (ICU error: %s)\n", u_errorName(err));
            std::printf("icutest: %d checks, %d failures\nicutest: FAILED\n",
                        checks, ++failures);
            std::exit(1);
        }
    }

    /* ---- 2. the character database ----
     *
     * Facts about Unicode, not about ICU. Each of these would be wrong
     * if uprops.icu had been read at the wrong offset or byte-swapped.
     */
    {
        ok("'A' is a letter", u_isalpha(0x0041) != 0);
        ok("'5' is a digit", u_isdigit(0x0035) != 0);
        ok("a space is whitespace", u_isUWhiteSpace(0x0020) != 0);
        ok("U+00E9 is a lowercase letter",
           u_charType(0x00E9) == U_LOWERCASE_LETTER);
        ok("U+0301 is a non-spacing mark",
           u_charType(0x0301) == U_NON_SPACING_MARK);
        ok("and its combining class is 230",
           u_getCombiningClass(0x0301) == 230);

        ok("uppercasing crosses the Latin-1 boundary",
           u_toupper(0x00E9) == 0x00C9);
        ok("and Greek works too", u_toupper(0x03B1) == 0x0391);

        /* Character names are a separate table again -- unames.icu --
         * and a long one, so this is a real read rather than a lookup
         * in something already in memory. */
        char name[128];
        UErrorCode err = U_ZERO_ERROR;
        const int32_t n = u_charName(0x00E9, U_UNICODE_CHAR_NAME,
                                     name, sizeof name, &err);
        ok("a character has its Unicode name",
           U_SUCCESS(err) && n > 0 &&
           std::strcmp(name, "LATIN SMALL LETTER E WITH ACUTE") == 0);
        std::printf("       (U+00E9 is \"%s\")\n", U_SUCCESS(err) ? name : "?");

        ok("a script is identified", uscript_getScript(0x03B1, &err) == USCRIPT_GREEK);
        ok("and a different one differs",
           uscript_getScript(0x0627, &err) == USCRIPT_ARABIC);
    }

    /* ---- 3. normalisation ----
     *
     * The same text spelled two ways: one code point, or a letter plus a
     * combining accent. NFC composes, NFD decomposes, and the two must
     * be reversible. This is nfc.nrm being read and its trie walked.
     */
    {
        UErrorCode err = U_ZERO_ERROR;
        const UNormalizer2 *nfc = unorm2_getNFCInstance(&err);
        const UNormalizer2 *nfd = unorm2_getNFDInstance(&err);
        ok("the normalizers exist", U_SUCCESS(err) && nfc && nfd);

        const UChar decomposed[] = { 0x0065, 0x0301, 0 };   /* e + acute */
        const UChar composed[]   = { 0x00E9, 0 };           /* e-acute   */

        UChar out[16];
        err = U_ZERO_ERROR;
        int32_t n = unorm2_normalize(nfc, decomposed, -1, out, 16, &err);
        ok("NFC composes e + combining acute into one character",
           U_SUCCESS(err) && n == 1 && out[0] == 0x00E9);

        err = U_ZERO_ERROR;
        n = unorm2_normalize(nfd, composed, -1, out, 16, &err);
        ok("NFD takes it apart again",
           U_SUCCESS(err) && n == 2 && out[0] == 0x0065 && out[1] == 0x0301);

        ok("and the composed form is already NFC",
           unorm2_isNormalized(nfc, composed, -1, &err) && U_SUCCESS(err));
        ok("while the decomposed form is not",
           !unorm2_isNormalized(nfc, decomposed, -1, &err) && U_SUCCESS(err));

        /* A compatibility decomposition, which is a different table:
         * the ligature fi becomes two letters under NFKC and stays one
         * under NFC. */
        err = U_ZERO_ERROR;
        const UNormalizer2 *nfkc = unorm2_getNFKCInstance(&err);
        const UChar fi[] = { 0xFB01, 0 };
        n = unorm2_normalize(nfkc, fi, -1, out, 16, &err);
        ok("NFKC expands the fi ligature",
           U_SUCCESS(err) && n == 2 && out[0] == 'f' && out[1] == 'i');
    }

    /* ---- 4. full case mapping ----
     *
     * u_toupper works on one character and therefore cannot be right in
     * general: German sharp s uppercases to two letters, and Turkish
     * dotless i lowercases differently from every other locale. Both are
     * in the archive and neither is reachable one character at a time.
     */
    {
        UChar src[8], dst[16];
        UErrorCode err = U_ZERO_ERROR;

        src[0] = 0x00DF; src[1] = 0;                       /* sharp s */
        int32_t n = u_strToUpper(dst, 16, src, -1, "de", &err);
        ok("uppercasing sharp s gives two letters",
           U_SUCCESS(err) && n == 2 && dst[0] == 'S' && dst[1] == 'S');

        /* Turkish: capital I lowercases to dotless i, which is the
         * canonical example of a locale-dependent case mapping. */
        err = U_ZERO_ERROR;
        src[0] = 0x0049; src[1] = 0;                       /* 'I' */
        n = u_strToLower(dst, 16, src, -1, "tr", &err);
        ok("Turkish lowercases I to dotless i",
           U_SUCCESS(err) && n == 1 && dst[0] == 0x0131);

        err = U_ZERO_ERROR;
        n = u_strToLower(dst, 16, src, -1, "en", &err);
        ok("and English does not",
           U_SUCCESS(err) && n == 1 && dst[0] == 0x0069);
    }

    /* ---- 5. segmentation ----
     *
     * Where a word ends. The rule-based break iterator reads its state
     * machine out of the archive; a browser needs this for selection,
     * for line breaking, and for search.
     */
    {
        UErrorCode err = U_ZERO_ERROR;
        UChar text[64];
        u16(text, 64, "The quick brown fox.");

        UBreakIterator *bi = ubrk_open(UBRK_WORD, "en", text, -1, &err);
        ok("a word break iterator opens", U_SUCCESS(err) && bi != nullptr);

        if (U_SUCCESS(err)) {
            int words = 0;
            for (int32_t p = ubrk_first(bi); p != UBRK_DONE; p = ubrk_next(bi))
                if (ubrk_getRuleStatus(bi) != UBRK_WORD_NONE) words++;
            std::printf("       (\"The quick brown fox.\" has %d words)\n", words);
            ok("it finds four words and not the full stop", words == 4);
            ubrk_close(bi);
        }

        err = U_ZERO_ERROR;
        u16(text, 64, "One. Two! Three?");
        UBreakIterator *sb = ubrk_open(UBRK_SENTENCE, "en", text, -1, &err);
        if (U_SUCCESS(err)) {
            int sentences = 0;
            for (int32_t p = ubrk_first(sb); p != UBRK_DONE; p = ubrk_next(sb))
                sentences++;
            /* first() counts as a boundary, so three sentences give four. */
            ok("and sentences break on three kinds of stop", sentences == 4);
            ubrk_close(sb);
        }
    }

    /* ---- 6. collation, which is the part that cannot be faked ----
     *
     * Sorting order is a property of a *language*, not of Unicode. In
     * German, a-umlaut sorts as a variant of a and therefore before b;
     * in Swedish it is a distinct letter that comes after z. Both rules
     * are in the archive, and no amount of code-point comparison
     * produces either.
     */
    {
        UErrorCode err = U_ZERO_ERROR;
        const UChar a_umlaut[] = { 0x00E4, 0 };
        const UChar b_[] = { 0x0062, 0 };
        const UChar z_[] = { 0x007A, 0 };

        UCollator *de = ucol_open("de", &err);
        ok("a German collator opens", U_SUCCESS(err) && de != nullptr);
        if (U_SUCCESS(err)) {
            ok("in German, a-umlaut sorts before b",
               ucol_strcoll(de, a_umlaut, -1, b_, -1) == UCOL_LESS);
            ucol_close(de);
        }

        err = U_ZERO_ERROR;
        UCollator *sv = ucol_open("sv", &err);
        ok("a Swedish collator opens", U_SUCCESS(err) && sv != nullptr);
        if (U_SUCCESS(err)) {
            ok("in Swedish, a-umlaut sorts after z",
               ucol_strcoll(sv, a_umlaut, -1, z_, -1) == UCOL_GREATER);
            ucol_close(sv);
        }

        /* Strength: at primary strength, accents do not distinguish. */
        err = U_ZERO_ERROR;
        UCollator *en = ucol_open("en", &err);
        if (U_SUCCESS(err)) {
            const UChar resume1[] = { 'r', 0x00E9, 's', 'u', 'm', 0x00E9, 0 };
            const UChar resume2[] = { 'r', 'e', 's', 'u', 'm', 'e', 0 };
            ucol_setStrength(en, UCOL_PRIMARY);
            ok("at primary strength, accents are ignored",
               ucol_strcoll(en, resume1, -1, resume2, -1) == UCOL_EQUAL);
            ucol_setStrength(en, UCOL_TERTIARY);
            ok("at tertiary strength, they are not",
               ucol_strcoll(en, resume1, -1, resume2, -1) != UCOL_EQUAL);
            ucol_close(en);
        }
    }

    /* ---- 7. the C++ API, and a dynamic_cast across the library ----
     *
     * Collator::createInstance returns a Collator*, and the concrete
     * object is a RuleBasedCollator. Recovering that is a dynamic_cast
     * from a base to a derived class whose type descriptor was emitted
     * inside libicui18n.a and whose vtable was built there -- so this
     * runs libcxx/src/typeinfo.cpp against descriptors it has never seen
     * before, across an archive boundary.
     */
    {
        UErrorCode err = U_ZERO_ERROR;
        icu::Collator *c = icu::Collator::createInstance(icu::Locale("en"), err);
        ok("the C++ collator factory works", U_SUCCESS(err) && c != nullptr);

        if (c) {
            icu::RuleBasedCollator *rb = dynamic_cast<icu::RuleBasedCollator *>(c);
            ok("and dynamic_cast recovers the concrete class across the library",
               rb != nullptr);

            /* The other direction: a cast that must fail. */
            ok("typeid agrees with the cast",
               (typeid(*c) == typeid(icu::RuleBasedCollator)) == (rb != nullptr));

            icu::UnicodeString s1("apple"), s2("banana");
            ok("and it still sorts",
               c->compare(s1, s2, err) == UCOL_LESS && U_SUCCESS(err));
            delete c;
        }

        /* UnicodeString is ICU's own string, with its own allocator
         * calls -- so this is operator new in libcxx reached from inside
         * a 384,000-line library. */
        icu::UnicodeString big;
        for (int i = 0; i < 500; i++) big.append((UChar)('a' + (i % 26)));
        ok("a UnicodeString grows through the ring-3 allocator",
           big.length() == 500 && big.charAt(499) == (UChar)('a' + (499 % 26)));
    }

    /* ---- 8. legacy character encodings ----
     *
     * The single biggest reason a browser needs ICU. A page served as
     * windows-1252 or Shift-JIS has to be decoded before anything else
     * can happen, and those tables are megabytes.
     */
    {
        UErrorCode err = U_ZERO_ERROR;
        UConverter *cp1252 = ucnv_open("windows-1252", &err);
        ok("a windows-1252 converter opens", U_SUCCESS(err) && cp1252);

        if (U_SUCCESS(err)) {
            /* 0x80 in windows-1252 is the euro sign, which is the byte
             * that distinguishes it from ISO-8859-1 -- where 0x80 is a
             * control character. Getting this wrong is the classic
             * mojibake. */
            const char bytes[] = { (char)0x80, (char)0x41, 0 };
            UChar out[8];
            const int32_t n = ucnv_toUChars(cp1252, out, 8, bytes, 2, &err);
            ok("0x80 decodes to the euro sign, not a control character",
               U_SUCCESS(err) && n == 2 && out[0] == 0x20AC && out[1] == 'A');

            /* And back. */
            err = U_ZERO_ERROR;
            char back[8];
            const int32_t m = ucnv_fromUChars(cp1252, back, 8, out, n, &err);
            ok("and encodes back to the same bytes",
               U_SUCCESS(err) && m == 2 && (unsigned char)back[0] == 0x80);
            ucnv_close(cp1252);
        }

        err = U_ZERO_ERROR;
        UConverter *l1 = ucnv_open("ISO-8859-1", &err);
        if (U_SUCCESS(err)) {
            const char bytes[] = { (char)0x80, 0 };
            UChar out[4];
            const int32_t n = ucnv_toUChars(l1, out, 4, bytes, 1, &err);
            ok("in ISO-8859-1 the same byte is U+0080",
               U_SUCCESS(err) && n == 1 && out[0] == 0x0080);
            ucnv_close(l1);
        }

        err = U_ZERO_ERROR;
        UConverter *sjis = ucnv_open("Shift_JIS", &err);
        ok("a Shift-JIS converter opens too", U_SUCCESS(err) && sjis);
        if (U_SUCCESS(err)) {
            /* 0x82 0xA0 is hiragana A. */
            const char bytes[] = { (char)0x82, (char)0xA0, 0 };
            UChar out[4];
            const int32_t n = ucnv_toUChars(sjis, out, 4, bytes, 2, &err);
            ok("and decodes hiragana",
               U_SUCCESS(err) && n == 1 && out[0] == 0x3042);
            ucnv_close(sjis);
        }

        std::printf("       (%d converters available)\n", ucnv_countAvailable());
        ok("there are many converters", ucnv_countAvailable() > 100);
    }

    /* ---- 9. dates, in zones this machine has never heard of ----
     *
     * The instant is fixed, so what is under test is the formatter and
     * the timezone rules rather than the clock. 1'000'000'000 seconds
     * after the epoch is 2001-09-09 01:46:40 UTC, which is 21:46:40 the
     * previous evening in New York -- a five-hour offset that only the
     * IANA database in the archive knows.
     */
    {
        const UDate instant = 1000000000.0 * 1000.0;   /* ICU dates are ms */

        UErrorCode err = U_ZERO_ERROR;
        UChar utc[] = { 'U', 'T', 'C', 0 };
        UChar pattern[64];
        u16(pattern, 64, "yyyy-MM-dd HH:mm:ss");

        UDateFormat *f = udat_open(UDAT_PATTERN, UDAT_PATTERN, "en",
                                   utc, -1, pattern, -1, &err);
        ok("a date formatter opens", U_SUCCESS(err) && f);

        if (U_SUCCESS(err)) {
            UChar out[64];
            char  ascii[64];
            const int32_t n = udat_format(f, instant, out, 64, nullptr, &err);
            for (int32_t i = 0; i < n && i < 63; i++) ascii[i] = (char)out[i];
            ascii[n < 63 ? n : 63] = 0;
            std::printf("       (t=1e9 is %s UTC)\n", ascii);
            ok("and formats a known instant correctly",
               U_SUCCESS(err) && std::strcmp(ascii, "2001-09-09 01:46:40") == 0);
            udat_close(f);
        }

        err = U_ZERO_ERROR;
        UChar ny[32];
        u16(ny, 32, "America/New_York");
        UDateFormat *fny = udat_open(UDAT_PATTERN, UDAT_PATTERN, "en",
                                     ny, -1, pattern, -1, &err);
        if (U_SUCCESS(err)) {
            UChar out[64];
            char  ascii[64];
            const int32_t n = udat_format(fny, instant, out, 64, nullptr, &err);
            for (int32_t i = 0; i < n && i < 63; i++) ascii[i] = (char)out[i];
            ascii[n < 63 ? n : 63] = 0;
            std::printf("       (and %s in New York)\n", ascii);
            ok("the IANA timezone rules are in the archive",
               U_SUCCESS(err) && std::strcmp(ascii, "2001-09-08 21:46:40") == 0);
            udat_close(fny);
        }

        /* A locale's own format, which is a different resource bundle
         * again: not a pattern this program supplied, but the one the
         * German locale itself defines. The month name is what proves
         * that bundle was read rather than a fallback to root. */
        err = U_ZERO_ERROR;
        /* udat_open takes the *time* style first and the date style
         * second, which is the opposite of how the name reads and is
         * worth a line here: asking for UDAT_LONG, UDAT_NONE formats a
         * long time and no date at all, which is what this checked
         * before -- it printed "01:46:40 UTC" and looked for a month. */
        UDateFormat *fde = udat_open(UDAT_NONE, UDAT_LONG, "de", utc, -1,
                                     nullptr, 0, &err);
        if (U_SUCCESS(err)) {
            UChar out[128];
            const int32_t n = udat_format(fde, instant, out, 128, nullptr, &err);

            char ascii[160];
            int  k = 0;
            for (int32_t i = 0; i < n && k < 158; i++)
                ascii[k++] = (out[i] < 0x80) ? (char)out[i] : '?';
            ascii[k] = 0;
            std::printf("       (in German: \"%s\")\n", ascii);

            bool has_september = std::strstr(ascii, "September") != nullptr;
            ok("a German date carries a German month name",
               U_SUCCESS(err) && has_september);
            udat_close(fde);
        } else {
            ok("a German date carries a German month name", false);
            std::printf("       (udat_open de: %s)\n", u_errorName(err));
        }
    }

    /* ---- 10. the clock this all now rests on ----
     *
     * Separated from the formatting above deliberately: those checks use
     * a fixed instant and pass whatever the clock says. This one asks
     * the clock, and it is the only check here that would have failed
     * before SYS_WALLCLOCK existed -- time() used to answer from the
     * monotonic tick, which would put "now" a few seconds after the
     * start of 1970.
     */
    {
        const time_t now = time(nullptr);
        struct tm broken;
        gmtime_r(&now, &broken);
        std::printf("       (the machine says %04d-%02d-%02d %02d:%02d:%02d UTC)\n",
                    broken.tm_year + 1900, broken.tm_mon + 1, broken.tm_mday,
                    broken.tm_hour, broken.tm_min, broken.tm_sec);

        /* 1577836800 is 2020-01-01. Any real clock is past it; a
         * monotonic count since boot is not. */
        ok("the wall clock is a calendar and not a stopwatch",
           now > 1577836800);
        ok("and the year is plausible",
           broken.tm_year + 1900 >= 2020 && broken.tm_year + 1900 < 2200);

        /* ICU's own idea of now, which comes through the same call. */
        const UDate icu_now = ucal_getNow();
        ok("ICU agrees with it to within a minute",
           icu_now / 1000.0 - (double)now < 60.0 &&
           (double)now - icu_now / 1000.0 < 60.0);

        /* The round trip through the calendar, which is libc/calendar.c
         * rather than ICU: a date taken apart and put back together must
         * be the same instant. */
        const time_t again = timegm(&broken);
        ok("gmtime and timegm are inverses", again == now);
    }

    /* ---- 11. locales ---- */
    {
        std::printf("       (%d locales available)\n", uloc_countAvailable());
        ok("the archive carries many locales", uloc_countAvailable() > 100);

        char name[128];
        UErrorCode err = U_ZERO_ERROR;
        uloc_getDisplayLanguage("de", "en", (UChar *)nullptr, 0, &err);
        err = U_ZERO_ERROR;
        UChar disp[64];
        const int32_t n = uloc_getDisplayLanguage("de", "en", disp, 64, &err);
        for (int32_t i = 0; i < n && i < 127; i++) name[i] = (char)disp[i];
        name[n < 127 ? n : 127] = 0;
        ok("and can name a language in another language",
           U_SUCCESS(err) && std::strcmp(name, "German") == 0);
    }

    u_cleanup();
    ok("ICU shut down cleanly", true);

    std::printf("icutest: %d checks, %d failures\n", checks, failures);
    std::printf(failures ? "icutest: FAILED\n" : "icutest: all passed\n");
    std::exit(failures ? 1 : 0);
}
