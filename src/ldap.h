#ifndef LDAP_H
#define LDAP_H

#include <stdint.h>
#include "vxnet.h"
#include "ber.h"

/*
 * src/ldap.h — asking a directory who someone is.
 *
 * LDAP is the protocol behind "log in with your work account". A
 * directory server holds the users, the groups and the machines; a
 * client binds to it to prove who it is, then searches for the entry it
 * needs. Active Directory is an LDAP server with extra opinions, which
 * is why this is the first piece of section nine and everything else in
 * that section assumes it.
 *
 * ---- BER ----
 *
 * Every LDAP message is BER: a tag byte, a length, and a value, nested.
 * The writer and reader are in src/ber.h, which explains at length why
 * they are a fixed set of shapes rather than a general encoder. They
 * started life in this file and moved out when Kerberos turned out to
 * need the same bytes.
 *
 * What matters here is when the parsing happens: the first response is
 * read *before* the bind, so every length is checked against the end of
 * the buffer while the machine on the other end is still completely
 * unauthenticated.
 *
 * ---- what this does not do ----
 *
 * Simple bind only: the distinguished name and password go over the
 * connection as they are. On port 389 that is in the clear, which is
 * why ldap_bind refuses a password on an unencrypted connection unless
 * it is asked twice. SASL, GSSAPI and Kerberos binds are not here.
 */

#define LDAP_PORT      389
#define LDAP_PORT_TLS  636

/* Result codes worth naming. */
#define LDAP_SUCCESS                 0
#define LDAP_OPERATIONS_ERROR        1
#define LDAP_PROTOCOL_ERROR          2
#define LDAP_SIZE_LIMIT_EXCEEDED     4
#define LDAP_AUTH_METHOD_NOT_SUPPORTED 7
#define LDAP_NO_SUCH_OBJECT         32
#define LDAP_INVALID_CREDENTIALS    49
#define LDAP_INSUFFICIENT_ACCESS    50

/* Scopes. */
#define LDAP_SCOPE_BASE     0
#define LDAP_SCOPE_ONELEVEL 1
#define LDAP_SCOPE_SUBTREE  2

/* Application tags, which is how LDAP names its operations. */
#define LDAP_BIND_REQUEST     0x60
#define LDAP_BIND_RESPONSE    0x61
#define LDAP_UNBIND_REQUEST   0x42
#define LDAP_SEARCH_REQUEST   0x63
#define LDAP_SEARCH_ENTRY     0x64
#define LDAP_SEARCH_DONE      0x65

#define LDAP_AUTH_SIMPLE      0x80   /* [0] context, primitive */
#define LDAP_FILTER_EQUALITY  0xA3   /* [3] context, constructed */
#define LDAP_FILTER_PRESENT   0x87   /* [7] context, primitive */

#define LDAP_BUF   8192
#define LDAP_MAX_ATTRS 16
#define LDAP_MAX_VALS  8

/* ===========================================================
 * the connection
 * =========================================================== */

typedef struct {
    char name[64];
    char vals[LDAP_MAX_VALS][256];
    int  nvals;
} ldap_attr_t;

typedef struct {
    char dn[256];
    ldap_attr_t attrs[LDAP_MAX_ATTRS];
    int  nattrs;
} ldap_entry_t;

typedef struct {
    int      sock;
    int      open;
    int32_t  msgid;
    int      bound;
    int      last_result;
    char     last_error[128];
    uint8_t  tx[LDAP_BUF];
    uint8_t  rx[LDAP_BUF];
} ldap_conn_t;

static ldap_conn_t ldap_conn;

static const char *ldap_result_name(int code) {
    switch (code) {
    case LDAP_SUCCESS:               return "success";
    case LDAP_OPERATIONS_ERROR:      return "operations error";
    case LDAP_PROTOCOL_ERROR:        return "protocol error";
    case LDAP_SIZE_LIMIT_EXCEEDED:   return "size limit exceeded";
    case LDAP_AUTH_METHOD_NOT_SUPPORTED: return "auth method not supported";
    case LDAP_NO_SUCH_OBJECT:        return "no such object";
    case LDAP_INVALID_CREDENTIALS:   return "invalid credentials";
    case LDAP_INSUFFICIENT_ACCESS:   return "insufficient access";
    default:                         return "error";
    }
}

/*
 * Read one complete LDAPMessage.
 *
 * A TCP read returns whatever has arrived, which may be half a message
 * or three of them. The outer SEQUENCE's length says how much to expect,
 * so the header is read first and then exactly the rest -- rather than
 * the usual mistake of assuming one read is one message, which works on
 * a fast local network and fails the first time the server is busy.
 */
static int ldap_recv_message(ldap_conn_t *c, uint32_t *out_len) {
    uint32_t have = 0;

    /* Enough for the tag and the longest length form. */
    while (have < 6) {
        int n = vxnet_recv(c->sock, c->rx + have, (int)(6 - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    if (c->rx[0] != BER_SEQ) return -1;

    uint32_t hdr, body;
    if (c->rx[1] < 0x80) { hdr = 2; body = c->rx[1]; }
    else {
        uint32_t nb = c->rx[1] & 0x7F;
        if (nb == 0 || nb > 4) return -1;
        hdr = 2 + nb;
        body = 0;
        for (uint32_t i = 0; i < nb; i++) body = (body << 8) | c->rx[2 + i];
    }
    uint32_t total = hdr + body;
    if (total > LDAP_BUF) return -1;

    while (have < total) {
        int n = vxnet_recv(c->sock, c->rx + have, (int)(total - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    *out_len = total;
    return 0;
}

static int ldap_send(ldap_conn_t *c, uint32_t len) {
    return vxnet_send(c->sock, c->tx, (int)len) == (int)len ? 0 : -1;
}

static int ldap_open(const char *host, uint16_t port) {
    ldap_conn_t *c = &ldap_conn;
    if (c->open) return -1;

    uint8_t ip[4];
    if (!vxnet_resolve(host, ip)) {
        serial_puts("[ldap] cannot resolve ");
        serial_puts(host);
        serial_puts("\n");
        return -1;
    }
    c->sock = vxnet_socket();
    if (c->sock < 0) return -1;
    vxnet_timeout(c->sock, 15000);
    if (vxnet_connect(c->sock, ip, port) != 0) {
        vxnet_close(c->sock);
        serial_puts("[ldap] connection refused\n");
        return -1;
    }
    c->open  = 1;
    c->bound = 0;
    c->msgid = 0;
    c->last_result = -1;
    c->last_error[0] = 0;
    return 0;
}

static void ldap_close(void) {
    ldap_conn_t *c = &ldap_conn;
    if (!c->open) return;

    /* An unbind is a courtesy with a purpose: it tells the server this
     * was a deliberate end, so the connection is not held open waiting
     * for a client that has gone. It has no response by definition. */
    ber_w w = { c->tx, LDAP_BUF, 0, 0 };
    uint32_t m = ber_open(&w, BER_SEQ);
    ber_int(&w, BER_INT, ++c->msgid);
    ber_put(&w, LDAP_UNBIND_REQUEST);
    ber_put(&w, 0);
    ber_close(&w, m);
    if (!w.overflow) ldap_send(c, w.n);

    vxnet_close(c->sock);
    c->open = 0;
    c->bound = 0;
}

/*
 * Simple bind.
 *
 * `allow_cleartext` has to be passed explicitly when the connection is
 * not encrypted, and that is deliberate friction. A simple bind puts
 * the password on the wire as it is; on port 389 anyone on the path
 * reads it. Making the caller say so means the choice is visible at the
 * call site rather than buried in a default.
 */
static int ldap_bind(const char *dn, const char *password, int allow_cleartext) {
    ldap_conn_t *c = &ldap_conn;
    if (!c->open) return -1;

    if (password && password[0] && !allow_cleartext) {
        serial_puts("[ldap] refusing a simple bind with a password in the "
                    "clear\n");
        return -1;
    }

    ber_w w = { c->tx, LDAP_BUF, 0, 0 };
    uint32_t m = ber_open(&w, BER_SEQ);
    ber_int(&w, BER_INT, ++c->msgid);
    uint32_t b = ber_open(&w, LDAP_BIND_REQUEST);
    ber_int(&w, BER_INT, 3);                       /* LDAP v3 */
    ber_str(&w, BER_OCTET, dn ? dn : "");
    ber_str(&w, LDAP_AUTH_SIMPLE, password ? password : "");
    ber_close(&w, b);
    ber_close(&w, m);
    if (w.overflow) return -1;
    if (ldap_send(c, w.n) != 0) return -1;

    uint32_t len;
    if (ldap_recv_message(c, &len) != 0) return -1;

    ber_r r = { c->rx, len, 0, 0 };
    uint32_t l;
    if (ber_next(&r, &l) != BER_SEQ) return -1;
    (void)ber_read_int(&r);                        /* message id */
    if (ber_next(&r, &l) != LDAP_BIND_RESPONSE) return -1;

    int code = ber_read_int(&r);
    char matched[128];
    ber_read_str(&r, matched, sizeof matched);     /* matchedDN */
    ber_read_str(&r, c->last_error, sizeof c->last_error);
    if (r.bad) return -1;

    c->last_result = code;
    c->bound = (code == LDAP_SUCCESS);

    serial_puts("[ldap] bind ");
    serial_puts(dn && dn[0] ? dn : "(anonymous)");
    serial_puts(": ");
    serial_puts(ldap_result_name(code));
    serial_puts("\n");
    return c->bound ? 0 : -1;
}

/*
 * Search.
 *
 * `filter_attr` and `filter_value` make an equality match; passing a
 * null value makes a presence filter instead, which is how "every entry
 * that has this attribute at all" is asked.
 *
 * Results are read until the SearchResultDone arrives. A server may
 * send any number of entries in between, and stopping at the first is
 * the bug that makes a directory look like it holds one record.
 */
static int ldap_search(const char *base, int scope,
                       const char *filter_attr, const char *filter_value,
                       const char *const *attrs, int nattrs,
                       ldap_entry_t *out, int max_out) {
    ldap_conn_t *c = &ldap_conn;
    if (!c->open) return -1;

    ber_w w = { c->tx, LDAP_BUF, 0, 0 };
    uint32_t m = ber_open(&w, BER_SEQ);
    ber_int(&w, BER_INT, ++c->msgid);
    uint32_t s = ber_open(&w, LDAP_SEARCH_REQUEST);
    ber_str(&w, BER_OCTET, base ? base : "");
    ber_int(&w, BER_ENUM, scope);
    ber_int(&w, BER_ENUM, 0);                       /* never deref aliases */
    ber_int(&w, BER_INT, max_out);                  /* sizeLimit */
    ber_int(&w, BER_INT, 30);                       /* timeLimit, seconds */
    ber_bool(&w, 0);                                /* typesOnly */

    if (filter_value) {
        uint32_t f = ber_open(&w, LDAP_FILTER_EQUALITY);
        ber_str(&w, BER_OCTET, filter_attr);
        ber_str(&w, BER_OCTET, filter_value);
        ber_close(&w, f);
    } else {
        /* Presence is a primitive, not a constructed element: the
         * attribute name is the content directly. Encoding it as a
         * SEQUENCE is a protocol error the server reports as such. */
        ber_str(&w, LDAP_FILTER_PRESENT, filter_attr);
    }

    uint32_t a = ber_open(&w, BER_SEQ);
    for (int i = 0; i < nattrs; i++) ber_str(&w, BER_OCTET, attrs[i]);
    ber_close(&w, a);
    ber_close(&w, s);
    ber_close(&w, m);
    if (w.overflow) return -1;
    if (ldap_send(c, w.n) != 0) return -1;

    int found = 0;
    for (int guard = 0; guard < 1024; guard++) {
        uint32_t len;
        if (ldap_recv_message(c, &len) != 0) return -1;

        ber_r r = { c->rx, len, 0, 0 };
        uint32_t l;
        if (ber_next(&r, &l) != BER_SEQ) return -1;
        (void)ber_read_int(&r);
        int op = ber_next(&r, &l);

        if (op == LDAP_SEARCH_DONE) {
            int code = ber_read_int(&r);
            c->last_result = code;
            char matched[128];
            ber_read_str(&r, matched, sizeof matched);
            ber_read_str(&r, c->last_error, sizeof c->last_error);
            if (code != LDAP_SUCCESS && code != LDAP_SIZE_LIMIT_EXCEEDED) {
                serial_puts("[ldap] search: ");
                serial_puts(ldap_result_name(code));
                serial_puts("\n");
                return -1;
            }
            return found;
        }

        if (op != LDAP_SEARCH_ENTRY) { ber_skip(&r); continue; }
        if (found >= max_out) continue;   /* still drain to the done */

        ldap_entry_t *e = &out[found];
        e->nattrs = 0;
        ber_read_str(&r, e->dn, sizeof e->dn);

        uint32_t alen;
        if (ber_next(&r, &alen) != BER_SEQ) { r.bad = 1; continue; }
        uint32_t aend = r.at + alen;

        while (r.at < aend && !r.bad && e->nattrs < LDAP_MAX_ATTRS) {
            uint32_t one;
            if (ber_next(&r, &one) != BER_SEQ) break;
            uint32_t oend = r.at + one;

            ldap_attr_t *at = &e->attrs[e->nattrs];
            at->nvals = 0;
            ber_read_str(&r, at->name, sizeof at->name);

            uint32_t vlen;
            if (ber_next(&r, &vlen) == BER_SET) {
                uint32_t vend = r.at + vlen;
                while (r.at < vend && !r.bad && at->nvals < LDAP_MAX_VALS)
                    ber_read_str(&r, at->vals[at->nvals++], 256);
            }
            r.at = oend;
            e->nattrs++;
        }
        r.at = aend;
        if (!r.bad) found++;
    }
    return found;
}

static int ldap_bound(void)      { return ldap_conn.bound; }
static int ldap_last_result(void){ return ldap_conn.last_result; }

#endif /* LDAP_H */
