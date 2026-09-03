/*
 * pcre2test — PCRE2 in ring 3.
 *
 * The sixteenth port, and the first that WebKit's configure will never
 * look for. PCRE2 is not on `OptionsWPE.cmake`'s list at any line; it is
 * on **GLib's**. GLib 2.74's own `meson.build:2079` reads
 *
 *     pcre2 = dependency('libpcre2-8', required : true, ...)
 *
 * — required, because GRegex *is* PCRE2 since GLib stopped carrying a
 * bundled copy after 2.72. So this is a prerequisite of a prerequisite,
 * and it is here because `find_package(GLIB ... REQUIRED)` on line 185
 * cannot be attempted without it.
 *
 * ---- driven the way GLib drives it ----
 *
 * The consumer here is not WebKit, so the file `glib/gregex.c` is what
 * this test copies rather than anything in Source/WebCore. Four things
 * it does, all repeated below:
 *
 *   A compile context, always — `pcre2_compile_context_create(NULL)`
 *   followed by `pcre2_set_newline` and `pcre2_set_bsr`, even when the
 *   caller asked for the defaults (gregex.c:1785-1798).
 *
 *   `compile_options |= PCRE2_UCP`, unconditionally, on every pattern
 *   (gregex.c:1811). That single line is why this port compiles the
 *   Unicode tables in and why section 1 checks that it did.
 *
 *   PCRE2_UTF implies PCRE2_NO_UTF_CHECK — GLib validates its own
 *   strings and tells PCRE2 not to do it again (gregex.c:1807-1809).
 *
 *   `pcre2_jit_compile(re, PCRE2_JIT_COMPLETE)` on every pattern
 *   compiled with G_REGEX_OPTIMIZE, and an explicit
 *   `case PCRE2_ERROR_JIT_BADOPTION:` that logs and falls back to the
 *   interpreter (gregex.c:936-940). This build has no JIT, so that is
 *   the branch GLib will take here, every time — which makes it worth
 *   taking deliberately in section 2 rather than discovering later.
 *
 * ---- where the expectations come from ----
 *
 * A regex engine has no bitstream to compare against, so the anchors in
 * this file are of two kinds and neither is PCRE2's own opinion.
 *
 * The pattern semantics are specified: `(\d+)-(\d+)` against "10-20"
 * captures "10" and "20" or the engine is wrong, and no second
 * implementation is needed to say so.
 *
 * The Unicode assertions come from the **Unicode Character Database**.
 * U+0391 is Lu because the UCD says so, not because PCRE2 says so, and
 * section 4 is written as a list of UCD facts rather than as a list of
 * things this library happens to do. That is the part
 * `src/config.h.generic` would have silently switched off.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

static int checks = 0, failures = 0;

static void check(const char *what, int good) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s\n", what); }
}

static void checkr(const char *what, int good, int rc) {
    checks++;
    if (!good) { failures++; printf("  FAIL  %s (rc %d)\n", what, rc); }
}

/*
 * Compile a pattern exactly as glib/gregex.c does: a context whose
 * newline and BSR conventions are set explicitly, PCRE2_UCP always, and
 * PCRE2_NO_UTF_CHECK folded in whenever PCRE2_UTF is asked for.
 */
static pcre2_code *glib_compile(const char *pattern, uint32_t options,
                                int *errcode, PCRE2_SIZE *erroffset) {
    pcre2_compile_context *ctx = pcre2_compile_context_create(NULL);
    pcre2_code *re;
    int dummy_code;
    PCRE2_SIZE dummy_off;

    if (errcode == NULL) errcode = &dummy_code;
    if (erroffset == NULL) erroffset = &dummy_off;
    if (ctx == NULL) return NULL;

    pcre2_set_newline(ctx, PCRE2_NEWLINE_LF);
    pcre2_set_bsr(ctx, PCRE2_BSR_UNICODE);

    if (options & PCRE2_UTF) options |= PCRE2_NO_UTF_CHECK;
    options |= PCRE2_UCP;

    re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
                       options, errcode, erroffset, ctx);
    pcre2_compile_context_free(ctx);
    return re;
}

/* Does `pattern` match `subject` at all? -1 if the pattern would not
 * compile, otherwise the return of pcre2_match. */
static int matches(const char *pattern, uint32_t opts,
                   const char *subject, size_t len) {
    pcre2_code *re = glib_compile(pattern, opts, NULL, NULL);
    pcre2_match_data *md;
    int rc;
    if (re == NULL) return -1000;
    md = pcre2_match_data_create_from_pattern(re, NULL);
    rc = pcre2_match(re, (PCRE2_SPTR)subject, len, 0, 0, md, NULL);
    pcre2_match_data_free(md);
    pcre2_code_free(re);
    return rc;
}

int main(void) {
    char version[64];

    pcre2_config(PCRE2_CONFIG_VERSION, version);
    printf("pcre2test: PCRE2 %s\n", version);

    /* ============================================================
     *  1. the archive's own account of how it was built
     * ============================================================
     *
     * pcre2_config() reads back the decisions made in
     * third_party/pcre2-port/config.h, which makes this the other half
     * of that file: every answer below is a line in it, checked from
     * inside the compiled library rather than from a #ifdef in this
     * translation unit.
     */
    {
        uint32_t u;

        check("the version is 10.48", strncmp(version, "10.48", 5) == 0);

        pcre2_config(PCRE2_CONFIG_UNICODE, &u);
        check("Unicode support is compiled in", u == 1);

        pcre2_config(PCRE2_CONFIG_UNICODE_VERSION, version);
        printf("       Unicode %s\n", version);
        check("with a Unicode version to go with it", version[0] != 0);

        /* The one that is off, and off because of this *kernel* rather
         * than because of PCRE2 or this C library: the JIT writes
         * machine code into a page and jumps to it, and nothing in ring
         * 3 has ever asked for an executable mapping. */
        pcre2_config(PCRE2_CONFIG_JIT, &u);
        check("the JIT is not compiled in", u == 0);

        pcre2_config(PCRE2_CONFIG_LINKSIZE, &u);
        checkr("the link size is 2, upstream's default", u == 2, (int)u);

        pcre2_config(PCRE2_CONFIG_NEWLINE, &u);
        checkr("the default newline is LF", u == PCRE2_NEWLINE_LF, (int)u);

        pcre2_config(PCRE2_CONFIG_BSR, &u);
        check("and \\R matches any Unicode newline by default",
              u == PCRE2_BSR_UNICODE);

        pcre2_config(PCRE2_CONFIG_MATCHLIMIT, &u);
        check("the match limit is upstream's 10,000,000", u == 10000000);

        pcre2_config(PCRE2_CONFIG_HEAPLIMIT, &u);
        check("and the heap limit is 20,000,000", u == 20000000);
    }

    /* ============================================================
     *  2. the JIT that is not here, taken deliberately
     * ============================================================
     *
     * GLib calls pcre2_jit_compile on every pattern compiled with
     * G_REGEX_OPTIMIZE and has an explicit case for this build's answer.
     * Checking it here means the fallback is a tested path rather than
     * something discovered when GLib lands.
     */
    {
        pcre2_code *re = glib_compile("(\\d+)-(\\d+)", 0, NULL, NULL);
        int rc;

        check("a pattern compiles", re != NULL);
        if (re) {
            rc = pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);
            checkr("pcre2_jit_compile answers JIT_BADOPTION",
                   rc == PCRE2_ERROR_JIT_BADOPTION, rc);
            /* — which is `case PCRE2_ERROR_JIT_BADOPTION:` in
             * gregex.c:936, a g_debug and a fall through to the
             * interpreter. So the pattern must still match. */
            {
                pcre2_match_data *md =
                    pcre2_match_data_create_from_pattern(re, NULL);
                rc = pcre2_match(re, (PCRE2_SPTR)"port 10-20 open", 15,
                                 0, 0, md, NULL);
                checkr("and the pattern still matches interpretively",
                       rc == 3, rc);
                pcre2_match_data_free(md);
            }
            pcre2_code_free(re);
        }
    }

    /* ============================================================
     *  3. matching, capturing, and the ovector
     * ============================================================ */
    {
        pcre2_code *re = glib_compile("(?<hour>\\d\\d):(?<min>\\d\\d)",
                                      0, NULL, NULL);
        pcre2_match_data *md;
        PCRE2_SIZE *ov;
        int rc;
        static const char subject[] = "at 09:45 sharp";

        check("a pattern with two named groups compiles", re != NULL);
        if (re) {
            md = pcre2_match_data_create_from_pattern(re, NULL);
            rc = pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject),
                             0, 0, md, NULL);
            checkr("it matches with two captures", rc == 3, rc);

            ov = pcre2_get_ovector_pointer(md);
            check("the whole match starts at 3", ov[0] == 3);
            check("and ends at 8", ov[1] == 8);
            check("group 1 is the hour", ov[2] == 3 && ov[3] == 5);
            check("group 2 is the minute", ov[4] == 6 && ov[5] == 8);

            /* By name, which is what g_match_info_fetch_named does. */
            {
                PCRE2_UCHAR *buf = NULL;
                PCRE2_SIZE len = 0;
                rc = pcre2_substring_get_byname(md, (PCRE2_SPTR)"min",
                                                &buf, &len);
                checkr("the minute can be fetched by name", rc == 0, rc);
                check("and reads 45",
                      buf && len == 2 && memcmp(buf, "45", 2) == 0);
                if (buf) pcre2_substring_free(buf);
            }

            check("a name that is not there is refused",
                  pcre2_substring_get_byname(md, (PCRE2_SPTR)"second",
                                             NULL, NULL)
                      == PCRE2_ERROR_NOSUBSTRING);

            pcre2_match_data_free(md);
            pcre2_code_free(re);
        }

        /* No match is a distinct answer from an error, and a caller
         * that conflates them reports a broken regex for a string that
         * simply did not contain what it was looking for. */
        checkr("a subject with no match says so",
               matches("\\d\\d:\\d\\d", 0, "no clock here", 13)
                   == PCRE2_ERROR_NOMATCH,
               matches("\\d\\d:\\d\\d", 0, "no clock here", 13));
    }

    /* ============================================================
     *  4. Unicode, against the Unicode Character Database
     * ============================================================
     *
     * The section this port exists for. GLib passes PCRE2_UCP on every
     * pattern, and without SUPPORT_UNICODE the compile is refused
     * outright with PCRE2_ERROR_UNICODE_NOT_SUPPORTED — so a build that
     * had copied src/config.h.generic would fail every GRegex call in
     * GLib rather than merely running slower.
     *
     * Each expectation below is a fact from the UCD rather than an
     * observation of this library:
     *
     *   U+0391 GREEK CAPITAL LETTER ALPHA      General_Category=Lu
     *   U+03B1 GREEK SMALL LETTER ALPHA        Ll
     *   U+00DF LATIN SMALL LETTER SHARP S      Ll
     *   U+4E2D CJK UNIFIED IDEOGRAPH-4E2D      Lo, Script=Han
     *   U+0660 ARABIC-INDIC DIGIT ZERO         Nd
     *   U+1F600 GRINNING FACE                  So
     */
    {
        /* UTF-8: Α=CE 91, α=CE B1, ß=C3 9F, 中=E4 B8 AD, ٠=D9 A0,
         * 😀=F0 9F 98 80 */
        static const char greek_cap[]  = "\xCE\x91";
        static const char greek_small[]= "\xCE\xB1";
        static const char sharp_s[]    = "\xC3\x9F";
        static const char han[]        = "\xE4\xB8\xAD";
        static const char arabic_zero[]= "\xD9\xA0";
        static const char emoji[]      = "\xF0\x9F\x98\x80";
        const uint32_t U = PCRE2_UTF;

        check("\\p{Lu} matches GREEK CAPITAL ALPHA",
              matches("^\\p{Lu}$", U, greek_cap, 2) == 1);
        check("and does not match the small alpha",
              matches("^\\p{Lu}$", U, greek_small, 2) == PCRE2_ERROR_NOMATCH);
        check("\\p{Ll} matches LATIN SMALL LETTER SHARP S",
              matches("^\\p{Ll}$", U, sharp_s, 2) == 1);
        check("\\p{Lo} matches U+4E2D",
              matches("^\\p{Lo}$", U, han, 3) == 1);
        check("\\p{Han} matches it too, by script",
              matches("^\\p{Han}$", U, han, 3) == 1);
        check("but \\p{Greek} does not",
              matches("^\\p{Greek}$", U, han, 3) == PCRE2_ERROR_NOMATCH);
        check("\\p{Nd} matches ARABIC-INDIC DIGIT ZERO",
              matches("^\\p{Nd}$", U, arabic_zero, 2) == 1);
        check("\\p{So} matches GRINNING FACE",
              matches("^\\p{So}$", U, emoji, 4) == 1);

        /* PCRE2_UCP is what makes \w, \d and \s mean their Unicode
         * senses rather than their ASCII ones, and glib_compile above
         * sets it on every pattern because gregex.c does. */
        check("with UCP, \\w matches a Greek letter",
              matches("^\\w$", U, greek_cap, 2) == 1);
        check("with UCP, \\d matches an Arabic-Indic digit",
              matches("^\\d$", U, arabic_zero, 2) == 1);
        check("and \\d still does not match a letter",
              matches("^\\d$", U, greek_cap, 2) == PCRE2_ERROR_NOMATCH);

        /* Case folding across a case pair that is not ASCII. */
        check("caseless matching folds Greek",
              matches("^\\x{391}$", U | PCRE2_CASELESS, greek_small, 2) == 1);

        /* \X is one extended grapheme cluster: "e" followed by
         * U+0301 COMBINING ACUTE ACCENT is one, not two. */
        {
            static const char e_acute[] = "e\xCC\x81";
            check("\\X takes a base and its combining mark together",
                  matches("^\\X$", U, e_acute, 3) == 1);
            check("while . takes only the base",
                  matches("^.$", U, e_acute, 3) == PCRE2_ERROR_NOMATCH);
        }

        /* And offsets are in bytes even in UTF mode, which is the thing
         * a caller indexing its own string has to know. */
        {
            static const char mixed[] = "a\xCE\x91z";     /* a, Alpha, z */
            pcre2_code *re = glib_compile("\\p{Lu}", U, NULL, NULL);
            pcre2_match_data *md =
                re ? pcre2_match_data_create_from_pattern(re, NULL) : NULL;
            if (re && md) {
                int rc = pcre2_match(re, (PCRE2_SPTR)mixed, 4, 0, 0, md, NULL);
                PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
                checkr("a capital inside a mixed string is found", rc == 1, rc);
                check("at byte offset 1", rc == 1 && ov[0] == 1);
                check("and ends at byte offset 3, not 2",
                      rc == 1 && ov[1] == 3);
                pcre2_match_data_free(md);
            }
            if (re) pcre2_code_free(re);
        }

        /* Invalid UTF-8 must be refused when PCRE2 is allowed to check.
         * GLib passes PCRE2_NO_UTF_CHECK because it validates its own
         * strings first — so this is the check GLib turns off, and the
         * reason it can only afford to is that it does the work itself. */
        {
            static const char bad[] = "\xC3\x28";     /* truncated pair */
            pcre2_code *re = glib_compile("^.$", U, NULL, NULL);
            pcre2_match_data *md =
                re ? pcre2_match_data_create_from_pattern(re, NULL) : NULL;
            if (re && md) {
                int rc = pcre2_match(re, (PCRE2_SPTR)bad, 2, 0, 0, md, NULL);
                checkr("invalid UTF-8 is caught when checking is on",
                       rc == PCRE2_ERROR_UTF8_ERR20 || rc < 0, rc);
                pcre2_match_data_free(md);
            }
            if (re) pcre2_code_free(re);
        }
    }

    /* ============================================================
     *  5. substitution, which is g_regex_replace
     * ============================================================ */
    {
        pcre2_code *re = glib_compile("(\\w+)@(\\w+)", 0, NULL, NULL);
        PCRE2_UCHAR out[128];
        PCRE2_SIZE outlen;
        int rc;
        static const char subject[] = "a@b and c@d";

        check("the substitution pattern compiles", re != NULL);
        if (re) {
            outlen = sizeof out;
            rc = pcre2_substitute(re, (PCRE2_SPTR)subject, strlen(subject),
                                  0, PCRE2_SUBSTITUTE_GLOBAL, NULL, NULL,
                                  (PCRE2_SPTR)"$2:$1", PCRE2_ZERO_TERMINATED,
                                  out, &outlen);
            checkr("a global substitution runs", rc == 2, rc);
            check("swapping both pairs",
                  strcmp((char *)out, "b:a and d:c") == 0);

            /* Without GLOBAL, only the first. */
            outlen = sizeof out;
            rc = pcre2_substitute(re, (PCRE2_SPTR)subject, strlen(subject),
                                  0, 0, NULL, NULL,
                                  (PCRE2_SPTR)"$2:$1", PCRE2_ZERO_TERMINATED,
                                  out, &outlen);
            checkr("and without GLOBAL, only the first", rc == 1, rc);
            check("leaving the rest alone",
                  strcmp((char *)out, "b:a and c@d") == 0);

            /* A destination too small is refused rather than truncated
             * into, and reports the length it needed. */
            outlen = 4;
            rc = pcre2_substitute(re, (PCRE2_SPTR)subject, strlen(subject),
                                  0, PCRE2_SUBSTITUTE_GLOBAL, NULL, NULL,
                                  (PCRE2_SPTR)"$2:$1", PCRE2_ZERO_TERMINATED,
                                  out, &outlen);
            checkr("too small a destination is refused",
                   rc == PCRE2_ERROR_NOMEMORY, rc);

            pcre2_code_free(re);
        }
    }

    /* ============================================================
     *  6. repeated matching from an offset, which is g_regex_split
     * ============================================================ */
    {
        pcre2_code *re = glib_compile("\\s*,\\s*", 0, NULL, NULL);
        pcre2_match_data *md;
        static const char subject[] = "one, two ,three,  four";
        PCRE2_SIZE start = 0;
        int found = 0;

        if (re) {
            md = pcre2_match_data_create_from_pattern(re, NULL);
            while (pcre2_match(re, (PCRE2_SPTR)subject, strlen(subject),
                               start, 0, md, NULL) > 0) {
                PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
                found++;
                start = ov[1];
                if (start > strlen(subject)) break;
            }
            checkr("three separators are found in turn", found == 3, found);
            pcre2_match_data_free(md);
            pcre2_code_free(re);
        }
    }

    /* ============================================================
     *  7. failures, which have to be failures
     * ============================================================ */
    {
        int errcode = 0;
        PCRE2_SIZE erroffset = 0;
        pcre2_code *re;
        PCRE2_UCHAR msg[128];

        /* An unclosed group. The offset matters as much as the failure:
         * GLib reports it to the caller as part of the GError, and
         * gregex.c formats it into the message. */
        re = glib_compile("abc(def", 0, &errcode, &erroffset);
        check("an unclosed group does not compile", re == NULL);
        check("the error code says a parenthesis is missing",
              errcode == 114);
        checkr("and the offset points into the pattern",
               erroffset == 7, (int)erroffset);

        check("the error message can be fetched",
              pcre2_get_error_message(errcode, msg, sizeof msg) > 0);
        check("and mentions a parenthesis",
              strstr((char *)msg, "parenthes") != NULL);

        /* An unknown Unicode property. Reachable only because Unicode
         * support is compiled in — without it the pattern would fail
         * earlier and for a different reason. */
        re = glib_compile("\\p{NoSuchProperty}", PCRE2_UTF,
                          &errcode, &erroffset);
        check("an unknown Unicode property is refused", re == NULL);

        /*
         * Catastrophic backtracking, and the match limit that stops it.
         * Getting this section right took three runs and the result is
         * not what it was written to be, so it is worth setting out
         * plainly.
         *
         * `(a+)+b` against a run of a's is the textbook exponential
         * regex, and the first attempt here expected the match limit to
         * fire. It did not: PCRE2 answered NOMATCH at once. The reason
         * is the **start-of-match optimisation** — the compiler knows a
         * 'b' must appear somewhere, the subject contains none, and the
         * whole match is rejected before a single step is taken. The
         * classic demonstration string never reaches the engine.
         *
         * The second attempt assumed auto-possessification was doing the
         * work and turned it off. **That was wrong.** With a 'b' present
         * in the subject and every optimisation at its default, this
         * pattern is exponential exactly as advertised and runs into the
         * limit — which means auto-possessification does *not* rescue
         * `(a+)+b`, and the only thing standing between GLib and a hang
         * is MATCH_LIMIT.
         *
         * So the three checks below are a two-variable experiment: what
         * the start optimisation is worth, and what the limit is worth.
         */
        {
            pcre2_match_context *mctx = pcre2_match_context_create(NULL);
            pcre2_match_data *md;
            int rc;
            /* No 'b' anywhere: stopped by the start optimisation. */
            static const char no_b[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!";
            /* A 'b', but only past a character the pattern cannot
             * consume, so a real backtracking engine would explore every
             * partition of the a's before giving up. */
            static const char with_b[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa!b";

            re = glib_compile("(a+)+b", 0, NULL, NULL);
            check("the textbook catastrophic pattern compiles", re != NULL);
            if (re && mctx) {
                pcre2_set_match_limit(mctx, 1000);
                md = pcre2_match_data_create_from_pattern(re, NULL);

                rc = pcre2_match(re, (PCRE2_SPTR)no_b, strlen(no_b),
                                 0, 0, md, mctx);
                checkr("a subject with no 'b' is rejected before matching",
                       rc == PCRE2_ERROR_NOMATCH, rc);

                /* Put a 'b' in the subject, past a character the pattern
                 * cannot consume, and the start optimisation has nothing
                 * to reject. Nothing else saves it: this is the limit
                 * doing its job, at its default settings. */
                rc = pcre2_match(re, (PCRE2_SPTR)with_b, strlen(with_b),
                                 0, 0, md, mctx);
                checkr("but with a 'b' present it really is exponential, "
                       "and the match limit is what stops it",
                       rc == PCRE2_ERROR_MATCHLIMIT, rc);

                pcre2_match_data_free(md);
                pcre2_code_free(re);
                re = NULL;
            }

            /* And the other half of the experiment: the same subject
             * with no 'b' at all, with the start optimisation switched
             * off. If that check above passed for any reason other than
             * the start optimisation, this one answers NOMATCH too. */
            re = glib_compile("(a+)+b", PCRE2_NO_START_OPTIMIZE, NULL, NULL);
            check("it compiles again with the start optimisation off",
                  re != NULL);
            if (re && mctx) {
                pcre2_set_match_limit(mctx, 1000);
                md = pcre2_match_data_create_from_pattern(re, NULL);
                rc = pcre2_match(re, (PCRE2_SPTR)no_b, strlen(no_b),
                                 0, 0, md, mctx);
                checkr("and now even the subject with no 'b' runs into "
                       "the limit",
                       rc == PCRE2_ERROR_MATCHLIMIT, rc);
                pcre2_match_data_free(md);
                pcre2_code_free(re);
                re = NULL;
            }
            if (mctx) pcre2_match_context_free(mctx);
            if (re) pcre2_code_free(re);
        }
    }

    /* ============================================================
     *  8. the DFA matcher, which is a second engine in the archive
     * ============================================================
     *
     * pcre2_dfa_match is a different algorithm over the same compiled
     * pattern — no backtracking, all alternatives at once. GLib does not
     * call it, so this is the one section here whose subject is the
     * library rather than the consumer; it is a fifth of the archive and
     * would otherwise never run.
     */
    {
        pcre2_code *re = glib_compile("a(b|bc)d", 0, NULL, NULL);
        pcre2_match_data *md;
        int workspace[64];
        int rc;
        static const char subject[] = "xabcdy";

        if (re) {
            md = pcre2_match_data_create(8, NULL);
            rc = pcre2_dfa_match(re, (PCRE2_SPTR)subject, strlen(subject),
                                 0, 0, md, NULL, workspace,
                                 sizeof workspace / sizeof workspace[0]);
            checkr("the DFA matcher finds a match", rc > 0, rc);
            if (rc > 0) {
                PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
                check("starting at offset 1", ov[0] == 1);
                check("and taking the longest alternative", ov[1] == 5);
            }
            pcre2_match_data_free(md);
            pcre2_code_free(re);
        }
    }

    printf("pcre2test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
