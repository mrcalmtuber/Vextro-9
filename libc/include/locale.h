#ifndef _LOCALE_H
#define _LOCALE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * locale.h — there is one locale and it is "C".
 *
 * ---- and that is an answer, not a gap ----
 *
 * The C locale is a real, specified locale: decimal point is '.', there
 * is no thousands separator, no currency symbol, and the character
 * classification in <ctype.h> is the ASCII one. Every function here
 * reports exactly that, and nothing in this system can change it,
 * because setlocale's whole job is to select from locale *data* the
 * host provides and this host provides none.
 *
 * Saying so through the standard interface is worth more than not having
 * the header. Ported code overwhelmingly calls setlocale to *ask* what
 * the locale is rather than to set one, and "C" is the truthful reply.
 *
 * ICU 74's putil.cpp is the caller that asked for this. It queries
 * setlocale(LC_CTYPE, nullptr) and getenv("LC_ALL") to guess a default
 * locale, gets "C" and a null, and falls back to its own root locale --
 * which is the correct outcome on a machine with no user locale, and it
 * reaches it through the ordinary path rather than a special case.
 *
 * The one thing this must never do is claim a locale it does not have.
 * setlocale returns null for any request other than "C", "POSIX" or the
 * empty string, which is the standard's own way of saying "that locale
 * is not available" -- so a caller that checks the result learns the
 * truth instead of being handed a lie about how to format money.
 */

#include <stddef.h>

/*
 * The categories. Distinct values rather than a shared one, because
 * ICU's uprv_getPOSIXIDForCategory branches on which category it was
 * given and code that switch()es on these needs them to differ.
 */
#define LC_ALL      0
#define LC_COLLATE  1
#define LC_CTYPE    2
#define LC_MONETARY 3
#define LC_NUMERIC  4
#define LC_TIME     5
#define LC_MESSAGES 6

/*
 * struct lconv, as the C locale defines it.
 *
 * Every char field except decimal_point is "" and every char-typed
 * numeric field is CHAR_MAX, which is the standard's encoding of "this
 * quantity is not available in this locale". A caller formatting
 * currency reads CHAR_MAX for frac_digits and knows to fall back;
 * a caller that reads 0 instead would print money with no decimals and
 * never find out why.
 */
struct lconv {
    char *decimal_point;
    char *thousands_sep;
    char *grouping;

    char *int_curr_symbol;
    char *currency_symbol;
    char *mon_decimal_point;
    char *mon_thousands_sep;
    char *mon_grouping;
    char *positive_sign;
    char *negative_sign;

    char int_frac_digits;
    char frac_digits;
    char p_cs_precedes;
    char p_sep_by_space;
    char n_cs_precedes;
    char n_sep_by_space;
    char p_sign_posn;
    char n_sign_posn;
    char int_p_cs_precedes;
    char int_p_sep_by_space;
    char int_n_cs_precedes;
    char int_n_sep_by_space;
    char int_p_sign_posn;
    char int_n_sign_posn;
};

char         *setlocale(int category, const char *locale);
struct lconv *localeconv(void);

#ifdef __cplusplus
}
#endif

#endif /* _LOCALE_H */
