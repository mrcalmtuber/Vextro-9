/*
 * libc/locale.c — the C locale, reported honestly.
 *
 * See libc/include/locale.h for why this is an answer rather than a
 * placeholder. The short version: the C locale is a specified locale,
 * this system is in it, and nothing can leave it.
 */

#include <locale.h>
#include <limits.h>
#include <string.h>

/*
 * The name is a modifiable buffer rather than a string literal because
 * setlocale is specified to return a pointer to storage the
 * implementation owns, and callers are allowed to hold it across calls.
 * A literal would be in a read-only page, which is correct here only
 * because nothing ever writes it -- but the standard's contract is
 * about ownership, and this is the honest shape of it.
 */
static char c_locale_name[] = "C";

/*
 * The C locale's lconv, exactly as the standard specifies it.
 *
 * CHAR_MAX in the numeric fields is not a sentinel invented here: it is
 * the value the standard requires for a quantity the locale does not
 * define, and a caller that formats currency is expected to test for it.
 */
static struct lconv c_lconv = {
    (char *)".",     /* decimal_point      */
    (char *)"",      /* thousands_sep      */
    (char *)"",      /* grouping           */

    (char *)"",      /* int_curr_symbol    */
    (char *)"",      /* currency_symbol    */
    (char *)"",      /* mon_decimal_point  */
    (char *)"",      /* mon_thousands_sep  */
    (char *)"",      /* mon_grouping       */
    (char *)"",      /* positive_sign      */
    (char *)"",      /* negative_sign      */

    CHAR_MAX,        /* int_frac_digits    */
    CHAR_MAX,        /* frac_digits        */
    CHAR_MAX,        /* p_cs_precedes      */
    CHAR_MAX,        /* p_sep_by_space     */
    CHAR_MAX,        /* n_cs_precedes      */
    CHAR_MAX,        /* n_sep_by_space     */
    CHAR_MAX,        /* p_sign_posn        */
    CHAR_MAX,        /* n_sign_posn        */
    CHAR_MAX,        /* int_p_cs_precedes  */
    CHAR_MAX,        /* int_p_sep_by_space */
    CHAR_MAX,        /* int_n_cs_precedes  */
    CHAR_MAX,        /* int_n_sep_by_space */
    CHAR_MAX,        /* int_p_sign_posn    */
    CHAR_MAX,        /* int_n_sign_posn    */
};

/*
 * Query returns "C". A request for "C", "POSIX" or "" succeeds, because
 * all three name the locale this system is already in -- "" means "the
 * locale from the environment", there is no environment, and the
 * environment-less default is the C locale.
 *
 * Anything else returns null, which is the standard's way of saying the
 * locale is unavailable. That refusal is the important half: a caller
 * that asks for de_DE and is told "C" would format numbers wrongly and
 * silently, whereas one that is told null can decide what to do.
 */
char *setlocale(int category, const char *locale) {
    (void)category;

    if (locale == 0) return c_locale_name;                /* query */
    if (locale[0] == '\0') return c_locale_name;          /* "from the environment" */
    if (strcmp(locale, "C") == 0) return c_locale_name;
    if (strcmp(locale, "POSIX") == 0) return c_locale_name;

    return 0;
}

struct lconv *localeconv(void) { return &c_lconv; }
