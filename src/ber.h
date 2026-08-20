#ifndef BER_H
#define BER_H

#include <stdint.h>

/*
 * src/ber.h — tag, length, value.
 *
 * Two protocols in this system are defined in ASN.1 and encode
 * themselves this way: LDAP (src/ldap.h) and Kerberos (src/kerberos.h).
 * They had one encoder between them from the start -- this one, which
 * lived inside ldap.h until Kerberos needed it -- and it is here rather
 * than duplicated because two copies of a byte-level encoder drift, and
 * the drift is invisible until a server somewhere rejects a message
 * that looks perfectly good in a hex dump.
 *
 * ---- a writer and a reader, not a library ----
 *
 * There is deliberately no general BER encoder here. BER is
 * self-describing, permits several encodings of the same value, and has
 * an indefinite-length form whose contents are terminated by a sentinel
 * -- which is to say, a parser whose termination depends on finding two
 * particular bytes in attacker-supplied data. General BER decoders are
 * a well-known source of remote code execution for exactly these
 * reasons.
 *
 * So: a writer that emits the shapes these two clients send, and a
 * reader that walks a response with every length checked against the
 * end of the buffer before it is used. The indefinite form is refused
 * outright. It is legal BER, no server needs it, and accepting it buys
 * nothing but a way to be attacked.
 *
 * ---- DER, where it matters ----
 *
 * Kerberos requires DER, which is BER with the ambiguity removed:
 * lengths must be minimal. That is not a separate mode here because the
 * writer has always emitted minimal lengths -- see ber_close(), which
 * shifts the body rather than reserving three bytes and padding. Doing
 * it the other way produces valid BER that a DER parser rejects, and
 * the resulting KDC error says only "the request was malformed".
 */

/* Universal tags. */
#define BER_BOOL      0x01
#define BER_INT       0x02
#define BER_BITSTR    0x03
#define BER_OCTET     0x04
#define BER_NULL      0x05
#define BER_ENUM      0x0A
#define BER_SEQ       0x30
#define BER_SET       0x31

/* The two string types Kerberos uses. GeneralString is what every
 * principal name and realm is; GeneralizedTime is "YYYYMMDDHHMMSSZ". */
#define BER_GENSTR    0x1B
#define BER_GENTIME   0x18

/* Context-specific constructed tag n -- [n] in an ASN.1 module. Nearly
 * every field of every Kerberos structure is wrapped in one. */
#define BER_CTX(n)    ((uint8_t)(0xA0 | (n)))
/* And the primitive form, for the few fields that are not. */
#define BER_CTXP(n)   ((uint8_t)(0x80 | (n)))
/* [APPLICATION n], constructed: how Kerberos names its message types. */
#define BER_APP(n)    ((uint8_t)(0x60 | (n)))

/* ===========================================================
 * the writer
 *
 * Lengths are not known until the contents have been written, so every
 * constructed element is opened, filled, and then closed -- at which
 * point the length is known and the body is shifted if the length
 * needed more than one byte to express. Shifting is cheaper than the
 * alternative of encoding twice, and far cheaper than the usual
 * mistake of reserving three bytes and emitting a non-minimal length,
 * which some servers reject.
 * =========================================================== */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t n;
    int      overflow;
} ber_w;

static void ber_put(ber_w *w, uint8_t b) {
    if (w->n >= w->cap) { w->overflow = 1; return; }
    w->buf[w->n++] = b;
}

static void ber_puts(ber_w *w, const uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) ber_put(w, p[i]);
}

static uint32_t ber_strlen(const char *s) {
    uint32_t n = 0; while (s && s[n]) n++; return n;
}

/* How many bytes a definite length needs, minimally encoded. */
static uint32_t ber_len_bytes(uint32_t len) {
    if (len < 0x80) return 1;
    if (len <= 0xFF) return 2;
    if (len <= 0xFFFF) return 3;
    if (len <= 0xFFFFFF) return 4;
    return 5;
}

static void ber_write_len(ber_w *w, uint32_t len) {
    if (len < 0x80) { ber_put(w, (uint8_t)len); return; }
    uint32_t nb = ber_len_bytes(len) - 1;
    ber_put(w, (uint8_t)(0x80 | nb));
    for (uint32_t i = nb; i-- > 0; ) ber_put(w, (uint8_t)(len >> (i * 8)));
}

/* Open a constructed element; returns the offset to close it with. */
static uint32_t ber_open(ber_w *w, uint8_t tag) {
    ber_put(w, tag);
    ber_put(w, 0);          /* one byte reserved for the length */
    return w->n;            /* where the body starts */
}

static void ber_close(ber_w *w, uint32_t body_start) {
    uint32_t len = w->n - body_start;
    uint32_t need = ber_len_bytes(len);
    if (need == 1) {
        w->buf[body_start - 1] = (uint8_t)len;
        return;
    }
    /* The length needs more room than the one byte reserved. Shift the
     * body up and write the longer form. */
    uint32_t extra = need - 1;
    if (w->n + extra > w->cap) { w->overflow = 1; return; }
    for (uint32_t i = w->n; i-- > body_start; )
        w->buf[i + extra] = w->buf[i];
    w->n += extra;
    uint32_t at = body_start - 1;
    w->buf[at++] = (uint8_t)(0x80 | extra);
    for (uint32_t i = extra; i-- > 0; )
        w->buf[at++] = (uint8_t)(len >> (i * 8));
}

static void ber_int(ber_w *w, uint8_t tag, int32_t v) {
    ber_put(w, tag);
    /* Minimal two's-complement, which for the small positive values
     * these protocols use is one byte unless the top bit would make it
     * look negative. */
    if (v >= 0 && v < 0x80) { ber_put(w, 1); ber_put(w, (uint8_t)v); return; }
    uint8_t tmp[4];
    int n = 0;
    uint32_t u = (uint32_t)v;
    for (int i = 3; i >= 0; i--) {
        uint8_t b = (uint8_t)(u >> (i * 8));
        if (n == 0 && b == 0 && i != 0) continue;
        tmp[n++] = b;
    }
    if (tmp[0] & 0x80) { ber_put(w, (uint8_t)(n + 1)); ber_put(w, 0); }
    else                 ber_put(w, (uint8_t)n);
    ber_puts(w, tmp, (uint32_t)n);
}

static void ber_str(ber_w *w, uint8_t tag, const char *s) {
    uint32_t n = ber_strlen(s);
    ber_put(w, tag);
    ber_write_len(w, n);
    ber_puts(w, (const uint8_t *)s, n);
}

static void ber_bytes(ber_w *w, uint8_t tag, const uint8_t *p, uint32_t n) {
    ber_put(w, tag);
    ber_write_len(w, n);
    ber_puts(w, p, n);
}

static void ber_bool(ber_w *w, int v) {
    ber_put(w, BER_BOOL); ber_put(w, 1); ber_put(w, v ? 0xFF : 0x00);
}

/*
 * A 32-bit BIT STRING, which is what every flags field in Kerberos is.
 *
 * The leading byte is the number of unused bits in the final octet, and
 * it is part of the *contents*, not the length. Omitting it produces a
 * four-byte bit string whose first octet the server reads as the pad
 * count -- so a flags word of 0x40000000 arrives as 0x40 unused bits,
 * which is not a number, and the request is rejected as malformed.
 *
 * The bits are numbered from the most significant bit of the first
 * octet, so bit 1 (forwardable) is 0x40 of byte zero.
 */
static void ber_bitstring32(ber_w *w, uint8_t tag, uint32_t bits) {
    ber_put(w, tag);
    ber_put(w, 5);
    ber_put(w, 0);                       /* no unused trailing bits */
    ber_put(w, (uint8_t)(bits >> 24));
    ber_put(w, (uint8_t)(bits >> 16));
    ber_put(w, (uint8_t)(bits >> 8));
    ber_put(w, (uint8_t)bits);
}

/* ===========================================================
 * the reader
 * =========================================================== */

typedef struct {
    const uint8_t *buf;
    uint32_t n;
    uint32_t at;
    int      bad;
} ber_r;

/*
 * Read one tag and length. Returns the tag, sets *len and leaves `at`
 * on the first content byte.
 *
 * The indefinite form -- length byte 0x80, contents terminated by two
 * zero bytes -- is refused outright. It is legal BER and no server here
 * needs it, and accepting it means a parser whose termination depends
 * on finding a sentinel in attacker-supplied data.
 */
static int ber_next(ber_r *r, uint32_t *len) {
    if (r->bad || r->at + 2 > r->n) { r->bad = 1; return -1; }
    int tag = r->buf[r->at++];
    uint32_t l = r->buf[r->at++];

    if (l == 0x80) { r->bad = 1; return -1; }      /* indefinite */
    if (l & 0x80) {
        uint32_t nb = l & 0x7F;
        if (nb > 4 || r->at + nb > r->n) { r->bad = 1; return -1; }
        l = 0;
        for (uint32_t i = 0; i < nb; i++) l = (l << 8) | r->buf[r->at++];
    }
    if (r->at + l > r->n) { r->bad = 1; return -1; }
    *len = l;
    return tag;
}

/* What the next tag is, without consuming it. Returns -1 at the end of
 * the buffer rather than marking the reader bad, because "is there an
 * optional field here?" is a question Kerberos asks constantly. */
static int ber_peek(ber_r *r) {
    if (r->bad || r->at >= r->n) return -1;
    return r->buf[r->at];
}

static int32_t ber_read_int(ber_r *r) {
    uint32_t len;
    int tag = ber_next(r, &len);
    if (tag < 0 || (tag != BER_INT && tag != BER_ENUM) || len == 0 || len > 5) {
        r->bad = 1; return 0;
    }
    /* Five bytes is legal and means a positive value with the top bit
     * of its first significant byte set -- a nonce above 2^31, which
     * Kerberos generates routinely. The leading zero is the sign. */
    if (len == 5 && r->buf[r->at] != 0) { r->bad = 1; return 0; }

    int32_t v = (r->buf[r->at] & 0x80) ? -1 : 0;
    for (uint32_t i = 0; i < len; i++) v = (int32_t)(((uint32_t)v << 8) | r->buf[r->at + i]);
    r->at += len;
    return v;
}

static void ber_read_str(ber_r *r, char *out, uint32_t max) {
    uint32_t len;
    int tag = ber_next(r, &len);
    if (out && max) out[0] = 0;
    if (tag < 0) { r->bad = 1; return; }
    uint32_t take = len < max - 1 ? len : (max ? max - 1 : 0);
    for (uint32_t i = 0; i < take; i++) out[i] = (char)r->buf[r->at + i];
    if (max) out[take] = 0;
    r->at += len;
}

/* An OCTET STRING into a caller's buffer. Returns the length, or -1 --
 * including when the value is longer than the buffer, because silently
 * keeping the first part of a ciphertext is worse than failing. */
static int ber_read_bytes(ber_r *r, uint8_t *out, uint32_t max) {
    uint32_t len;
    int tag = ber_next(r, &len);
    if (tag < 0) { r->bad = 1; return -1; }
    if (len > max) { r->bad = 1; return -1; }
    for (uint32_t i = 0; i < len; i++) out[i] = r->buf[r->at + i];
    r->at += len;
    return (int)len;
}

static void ber_skip(ber_r *r) {
    uint32_t len;
    if (ber_next(r, &len) < 0) return;
    r->at += len;
}

/* Step into a constructed element whose tag must be `tag`, leaving the
 * reader positioned on its first member. Returns the offset just past
 * it, so the caller can tell where the contents end. */
static uint32_t ber_enter(ber_r *r, uint8_t tag) {
    uint32_t len;
    int t = ber_next(r, &len);
    if (t < 0 || (uint8_t)t != tag) { r->bad = 1; return r->at; }
    return r->at + len;
}

#endif /* BER_H */
