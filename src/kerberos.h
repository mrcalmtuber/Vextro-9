#ifndef KERBEROS_H
#define KERBEROS_H

#include <stdint.h>
#include "vxnet.h"
#include "ber.h"
#include "krb5crypto.h"
/* For the credential cache at the bottom of this file: SHA-256 is the
 * password KDF, ChaCha20 encrypts the cache. Both are self-contained and
 * carry their own guards, so including them here costs nothing when
 * desktop.h includes them again later. */
#include "sha256.h"
#include "chacha20.h"

/*
 * src/kerberos.h — proving who you are without sending your password.
 *
 * LDAP, next door in src/ldap.h, answers "who is this person". Kerberos
 * answers the harder question: "is this person who they say they are",
 * and it answers it for a whole network at once. It is what a Windows
 * domain actually runs on. Every "single sign-on" is this protocol
 * underneath.
 *
 * ---- the shape of it ----
 *
 * There are three parties and the client never sends its password to
 * any of them.
 *
 *   1. The client asks the KDC for a ticket-granting ticket. The KDC
 *      replies with one, encrypted under a key derived from the user's
 *      password. Only someone who knows the password can open it. This
 *      is the AS exchange.
 *
 *   2. To reach a file server, the client presents the TGT back to the
 *      KDC and asks for a ticket to that service. This is the TGS
 *      exchange, and the password is not involved at all -- the TGT is.
 *
 *   3. The client presents the service ticket to the file server, which
 *      can read it because it is encrypted under the *server's* key.
 *      This is the AP exchange, and the KDC is not involved.
 *
 * The elegance is in step 3: the server validates the ticket without
 * talking to anyone. That is why one KDC can serve a network, and it is
 * why a stolen ticket is dangerous until it expires.
 *
 * ---- pre-authentication, and why the first request is expected to fail
 *
 * A KDC will happily encrypt a TGT under a user's password key for
 * anybody who asks, and that reply is then an offline password cracking
 * target -- AS-REP roasting. Pre-authentication closes it: the client
 * must first encrypt the current time under its own key and send that,
 * proving it knows the password before the KDC replies with anything.
 *
 * So krb_get_tgt() sends a request with no pre-authentication and
 * *expects* KDC_ERR_PREAUTH_REQUIRED back. That error is not a failure;
 * it carries the salt the KDC wants used, in PA-ETYPE-INFO2. Guessing
 * the salt instead is possible -- it is realm followed by principal
 * name, usually -- and "usually" is doing a lot of work in that
 * sentence: any principal whose salt was ever changed, or which was
 * migrated between realms, has a different one, and the failure looks
 * exactly like a wrong password.
 *
 * ---- what is here and what is not ----
 *
 * Here: the AS exchange with PA-ENC-TIMESTAMP, the TGS exchange, and
 * AP-REQ construction so a service can be reached. aes256-cts and
 * aes128-cts only -- see src/krb5crypto.h for why rc4-hmac is refused
 * rather than merely deprioritised.
 *
 * Not here: cross-realm referrals, renewal and forwarding of tickets,
 * and PKINIT.
 *
 * The credential cache *is* here now, at the bottom of this file, and it
 * replaces a sentence this comment used to end on -- that tickets never
 * touched the disk, "which is a limitation and also means there is no
 * ccache file for anything to steal." Half of that was a genuine
 * security property and giving it up needs an argument rather than a
 * changelog entry; the argument is at the head of that section, with
 * what was done instead.
 *
 * One connection at a time. The buffers below are single instances, the
 * same way src/ldap.h has one connection -- a machine that needs two
 * simultaneous KDC conversations is not a machine this is for.
 */

#define KRB_PORT      88
#define KRB_BUF       16384      /* AD tickets carry a PAC and are large */
#define KRB_MAX_TKT   8192

/* Message types, which are also the application tag numbers. */
#define KRB_MSG_AS_REQ    10
#define KRB_MSG_AS_REP    11
#define KRB_MSG_TGS_REQ   12
#define KRB_MSG_TGS_REP   13
#define KRB_MSG_AP_REQ    14
#define KRB_MSG_ERROR     30

/* Application tags that are not message types. */
#define KRB_APP_TICKET        1
#define KRB_APP_AUTHENTICATOR 2
#define KRB_APP_ENC_AS_REP    25
#define KRB_APP_ENC_TGS_REP   26

/* PA-DATA types. */
#define KRB_PA_TGS_REQ         1
#define KRB_PA_ENC_TIMESTAMP   2
#define KRB_PA_PW_SALT         3
#define KRB_PA_ETYPE_INFO2    19

/* Principal name types. */
#define KRB_NT_PRINCIPAL   1
#define KRB_NT_SRV_INST    2

/* KDCOptions bits, numbered from the top of the first octet. */
#define KRB_OPT_FORWARDABLE   0x40000000u
#define KRB_OPT_PROXIABLE     0x10000000u
#define KRB_OPT_RENEWABLE_OK  0x00000010u

/* Error codes this client can do something useful about. */
#define KDC_ERR_C_PRINCIPAL_UNKNOWN   6
#define KDC_ERR_S_PRINCIPAL_UNKNOWN   7
#define KDC_ERR_ETYPE_NOSUPP         14
#define KDC_ERR_PREAUTH_FAILED       24
#define KDC_ERR_PREAUTH_REQUIRED     25
#define KRB_AP_ERR_SKEW              37

#define KRB_MAX_NAME  4

/*
 * The wall clock, which lives in src/gfx.h next to the taskbar that
 * displays it.
 *
 * Declared rather than included, because pulling a graphics header into
 * a network protocol to reach one function would drag the whole
 * framebuffer in with it and fix the include order of kernel.c in the
 * bargain. A static prototype ahead of the definition costs one line.
 *
 * Kerberos is unusually sensitive to this: the default tolerance is
 * five minutes, and a clock outside it produces KRB_AP_ERR_SKEW, which
 * reads to a user as "authentication failed" and has nothing to do with
 * their password. The CMOS clock is assumed to be UTC -- true under
 * QEMU, and true on any machine that has not been set up to dual-boot
 * with Windows.
 */
static void rtc_read(int *hh, int *mm, int *ss, int *day, int *mon, int *yr);

typedef struct {
    int      valid;
    char     realm[64];
    char     sname[KRB_MAX_NAME][64];
    int      nsname;
    uint8_t  ticket[KRB_MAX_TKT];   /* the Ticket's DER, kept verbatim */
    uint32_t tlen;
    krb_key_t session;
    char     endtime[20];
    uint32_t flags;
} krb_cred_t;

typedef struct {
    int      sock;
    int      open;
    char     realm[64];
    char     user[64];
    char     last_error[160];
    int      last_code;
    int      pref_etype;
    uint8_t  salt[128];
    uint32_t saltlen;
    krb_key_t ckey;                 /* the user's long-term key */
    krb_cred_t tgt;
    krb_cred_t svc;
    uint8_t  tx[KRB_BUF];
    uint8_t  rx[KRB_BUF];
} krb_ctx_t;

static krb_ctx_t krb;

/* Defined with the credential cache at the bottom of this file, and
 * called by both exchanges as soon as they succeed -- so a machine that
 * loses power between acquiring a ticket and shutting down cleanly still
 * comes back holding it. */
static int krb_cc_save(void);

static const char *krb_error_name(int code) {
    switch (code) {
    case 0:                            return "success";
    case KDC_ERR_C_PRINCIPAL_UNKNOWN:  return "client principal unknown";
    case KDC_ERR_S_PRINCIPAL_UNKNOWN:  return "server principal unknown";
    case KDC_ERR_ETYPE_NOSUPP:         return "no supported encryption type";
    case KDC_ERR_PREAUTH_FAILED:       return "pre-authentication failed "
                                              "(wrong password)";
    case KDC_ERR_PREAUTH_REQUIRED:     return "pre-authentication required";
    case KRB_AP_ERR_SKEW:              return "clock skew too great";
    default:                           return "KDC error";
    }
}

/* ===========================================================
 * time
 * =========================================================== */

/* "YYYYMMDDHHMMSSZ", which is what a KerberosTime is on the wire. */
static void krb_now(char out[16]) {
    int hh = 0, mm = 0, ss = 0, d = 1, mo = 1, yr = 2000;
    rtc_read(&hh, &mm, &ss, &d, &mo, &yr);

    int f[7] = { yr / 1000 % 10 * 10 + yr / 100 % 10,   /* century */
                 yr / 10 % 10 * 10 + yr % 10,           /* year */
                 mo, d, hh, mm, ss };
    for (int i = 0; i < 7; i++) {
        out[i * 2]     = (char)('0' + (f[i] / 10) % 10);
        out[i * 2 + 1] = (char)('0' + f[i] % 10);
    }
    out[14] = 'Z';
    out[15] = 0;
}

/*
 * A time far enough ahead to serve as the ticket's requested expiry.
 *
 * Kerberos wants an absolute instant, and computing "now plus ten
 * hours" correctly means carrying through month lengths and leap years.
 * The KDC caps the lifetime at its own policy regardless of what is
 * asked, so asking for the end of a distant year is both simpler and
 * exactly as effective -- and unlike arithmetic on the current date, it
 * cannot produce the 31st of February.
 */
static void krb_far_future(char out[16]) {
    const char *s = "20370913024805Z";
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* ===========================================================
 * pieces of ASN.1 that recur
 * =========================================================== */

static void krb_put_principal(ber_w *w, uint8_t ctx, int type,
                              const char *const *name, int n) {
    uint32_t a = ber_open(w, ctx);
    uint32_t b = ber_open(w, BER_SEQ);

    uint32_t c = ber_open(w, BER_CTX(0));
    ber_int(w, BER_INT, type);
    ber_close(w, c);

    uint32_t d = ber_open(w, BER_CTX(1));
    uint32_t e = ber_open(w, BER_SEQ);
    for (int i = 0; i < n; i++) ber_str(w, BER_GENSTR, name[i]);
    ber_close(w, e);
    ber_close(w, d);

    ber_close(w, b);
    ber_close(w, a);
}

/* EncryptedData: the etype, no kvno, and the ciphertext. */
static void krb_put_encdata(ber_w *w, uint8_t ctx, int etype,
                            const uint8_t *cipher, uint32_t clen) {
    uint32_t a = ber_open(w, ctx);
    uint32_t b = ber_open(w, BER_SEQ);

    uint32_t c = ber_open(w, BER_CTX(0));
    ber_int(w, BER_INT, etype);
    ber_close(w, c);

    uint32_t d = ber_open(w, BER_CTX(2));
    ber_bytes(w, BER_OCTET, cipher, clen);
    ber_close(w, d);

    ber_close(w, b);
    ber_close(w, a);
}

/* Read an EncryptedData, returning the etype and copying the cipher. */
static int krb_get_encdata(ber_r *r, uint8_t ctx, int *etype,
                           uint8_t *cipher, uint32_t max) {
    ber_enter(r, ctx);
    uint32_t end = ber_enter(r, BER_SEQ);
    int clen = -1;
    *etype = 0;

    while (r->at < end && !r->bad) {
        int t = ber_peek(r);
        if (t == (int)BER_CTX(0)) {
            ber_enter(r, BER_CTX(0));
            *etype = ber_read_int(r);
        } else if (t == (int)BER_CTX(1)) {
            ber_enter(r, BER_CTX(1));
            (void)ber_read_int(r);              /* kvno, not needed */
        } else if (t == (int)BER_CTX(2)) {
            ber_enter(r, BER_CTX(2));
            clen = ber_read_bytes(r, cipher, max);
        } else {
            ber_skip(r);
        }
    }
    r->at = end;
    return r->bad ? -1 : clen;
}

/* ===========================================================
 * transport
 *
 * Kerberos over TCP frames each message with a four-byte big-endian
 * length. Over UDP there is no prefix at all -- and no way to carry a
 * modern ticket, which with a Windows PAC in it routinely exceeds what
 * a datagram will hold. TCP is used here for both reasons.
 * =========================================================== */

static int krb_connect(const char *host, uint16_t port) {
    krb_ctx_t *c = &krb;
    uint8_t ip[4];
    if (!vxnet_resolve(host, ip)) {
        serial_puts("[krb5] cannot resolve ");
        serial_puts(host);
        serial_puts("\n");
        return -1;
    }
    c->sock = vxnet_socket();
    if (c->sock < 0) return -1;
    vxnet_timeout(c->sock, 15000);
    if (vxnet_connect(c->sock, ip, port) != 0) {
        vxnet_close(c->sock);
        serial_puts("[krb5] no KDC answering there\n");
        return -1;
    }
    c->open = 1;
    return 0;
}

static void krb_disconnect(void) {
    if (krb.open) { vxnet_close(krb.sock); krb.open = 0; }
}

static int krb_exchange(uint32_t txlen, uint32_t *rxlen) {
    krb_ctx_t *c = &krb;
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(txlen >> 24); hdr[1] = (uint8_t)(txlen >> 16);
    hdr[2] = (uint8_t)(txlen >> 8);  hdr[3] = (uint8_t)txlen;
    if (vxnet_send(c->sock, hdr, 4) != 4) return -1;
    if (vxnet_send(c->sock, c->tx, (int)txlen) != (int)txlen) return -1;

    uint32_t have = 0;
    while (have < 4) {
        int n = vxnet_recv(c->sock, hdr + have, (int)(4 - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    uint32_t total = ((uint32_t)hdr[0] << 24) | ((uint32_t)hdr[1] << 16) |
                     ((uint32_t)hdr[2] << 8)  | (uint32_t)hdr[3];
    if (total == 0 || total > KRB_BUF) return -1;

    have = 0;
    while (have < total) {
        int n = vxnet_recv(c->sock, c->rx + have, (int)(total - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    *rxlen = total;
    return 0;
}

/* ===========================================================
 * KRB-ERROR
 *
 * Parsed rather than merely detected, for two reasons. The error code
 * is the difference between "wrong password" and "your clock is wrong",
 * which a user cannot guess. And the e-data of a
 * KDC_ERR_PREAUTH_REQUIRED carries the salt, without which the right
 * password still produces the wrong key.
 * =========================================================== */

static int krb_parse_etype_info2(const uint8_t *p, uint32_t n) {
    krb_ctx_t *c = &krb;
    ber_r r = { p, n, 0, 0 };
    uint32_t end = ber_enter(&r, BER_SEQ);

    int found = 0;
    while (r.at < end && !r.bad) {
        uint32_t eend = ber_enter(&r, BER_SEQ);
        int etype = 0;
        uint8_t salt[128];
        uint32_t slen = 0;
        int have_salt = 0;

        while (r.at < eend && !r.bad) {
            int t = ber_peek(&r);
            if (t == (int)BER_CTX(0)) {
                ber_enter(&r, BER_CTX(0));
                etype = ber_read_int(&r);
            } else if (t == (int)BER_CTX(1)) {
                ber_enter(&r, BER_CTX(1));
                int got = ber_read_bytes(&r, salt, sizeof salt);
                if (got >= 0) { slen = (uint32_t)got; have_salt = 1; }
            } else {
                ber_skip(&r);          /* s2kparams, which AES does not use */
            }
        }
        r.at = eend;

        /* Take the first entry naming an encryption type this client
         * will speak. The KDC lists them in its own order of
         * preference, and honouring that is the point of asking. */
        if (!found && (etype == KRB_ETYPE_AES256_CTS ||
                       etype == KRB_ETYPE_AES128_CTS)) {
            c->pref_etype = etype;
            if (have_salt) {
                c->saltlen = slen;
                for (uint32_t i = 0; i < slen; i++) c->salt[i] = salt[i];
            }
            found = 1;
        }
    }
    return found ? 0 : -1;
}

/* Returns the error code, or -1 if the message is not a KRB-ERROR. */
static int krb_parse_error(const uint8_t *p, uint32_t n) {
    krb_ctx_t *c = &krb;
    ber_r r = { p, n, 0, 0 };

    if (ber_peek(&r) != (int)BER_APP(KRB_MSG_ERROR)) return -1;
    ber_enter(&r, BER_APP(KRB_MSG_ERROR));
    uint32_t end = ber_enter(&r, BER_SEQ);

    int code = 0;
    c->last_error[0] = 0;

    while (r.at < end && !r.bad) {
        int t = ber_peek(&r);
        if (t == (int)BER_CTX(6)) {
            ber_enter(&r, BER_CTX(6));
            code = ber_read_int(&r);
        } else if (t == (int)BER_CTX(11)) {
            ber_enter(&r, BER_CTX(11));
            ber_read_str(&r, c->last_error, sizeof c->last_error);
        } else if (t == (int)BER_CTX(12)) {
            /* e-data. For a pre-authentication challenge this is a
             * sequence of PA-DATA, one of which names the salt. */
            ber_enter(&r, BER_CTX(12));
            uint8_t ed[512];
            int elen = ber_read_bytes(&r, ed, sizeof ed);
            if (elen > 0) {
                ber_r er = { ed, (uint32_t)elen, 0, 0 };
                uint32_t eend = ber_enter(&er, BER_SEQ);
                while (er.at < eend && !er.bad) {
                    uint32_t pend = ber_enter(&er, BER_SEQ);
                    int ptype = 0;
                    uint8_t pv[256];
                    int pvlen = -1;
                    while (er.at < pend && !er.bad) {
                        int pt = ber_peek(&er);
                        if (pt == (int)BER_CTX(1)) {
                            ber_enter(&er, BER_CTX(1));
                            ptype = ber_read_int(&er);
                        } else if (pt == (int)BER_CTX(2)) {
                            ber_enter(&er, BER_CTX(2));
                            pvlen = ber_read_bytes(&er, pv, sizeof pv);
                        } else {
                            ber_skip(&er);
                        }
                    }
                    er.at = pend;
                    if (ptype == KRB_PA_ETYPE_INFO2 && pvlen > 0)
                        krb_parse_etype_info2(pv, (uint32_t)pvlen);
                    else if (ptype == KRB_PA_PW_SALT && pvlen > 0) {
                        /* The old way of saying the same thing. Only
                         * used if ETYPE-INFO2 did not appear. */
                        if (c->saltlen == 0) {
                            c->saltlen = (uint32_t)pvlen;
                            for (int i = 0; i < pvlen; i++) c->salt[i] = pv[i];
                        }
                    }
                }
            }
        } else {
            ber_skip(&r);
        }
    }
    c->last_code = code;
    return code;
}

/* ===========================================================
 * the request body, shared by AS-REQ and TGS-REQ
 * =========================================================== */

static uint32_t krb_nonce(void) {
    uint32_t v = 0;
    if (vx_random((uint8_t *)&v, 4) != 4) return 0;
    return v & 0x7FFFFFFFu;      /* positive, so the INTEGER stays four bytes */
}

/*
 * Writes the KDC-REQ-BODY and reports where it landed, because the TGS
 * exchange has to checksum the exact bytes that were sent. Re-encoding
 * it to compute that checksum would be the obvious approach and a
 * subtle disaster: two encodings that differ anywhere produce a
 * checksum over something other than what the KDC verifies.
 */
static void krb_put_req_body(ber_w *w, uint32_t options,
                             const char *cname, const char *realm,
                             const char *const *sname, int nsname,
                             uint32_t nonce,
                             uint32_t *body_off, uint32_t *body_len) {
    uint32_t start = w->n;
    uint32_t a = ber_open(w, BER_CTX(4));
    uint32_t b = ber_open(w, BER_SEQ);

    uint32_t c = ber_open(w, BER_CTX(0));
    ber_bitstring32(w, BER_BITSTR, options);
    ber_close(w, c);

    if (cname) {
        const char *n[1] = { cname };
        krb_put_principal(w, BER_CTX(1), KRB_NT_PRINCIPAL, n, 1);
    }

    uint32_t d = ber_open(w, BER_CTX(2));
    ber_str(w, BER_GENSTR, realm);
    ber_close(w, d);

    krb_put_principal(w, BER_CTX(3), KRB_NT_SRV_INST, sname, nsname);

    uint32_t e = ber_open(w, BER_CTX(5));
    char till[16];
    krb_far_future(till);
    ber_str(w, BER_GENTIME, till);
    ber_close(w, e);

    uint32_t f = ber_open(w, BER_CTX(7));
    ber_int(w, BER_INT, (int32_t)nonce);
    ber_close(w, f);

    /* The encryption types offered, best first. Two entries, and
     * neither of them is RC4. */
    uint32_t g = ber_open(w, BER_CTX(8));
    uint32_t h = ber_open(w, BER_SEQ);
    ber_int(w, BER_INT, KRB_ETYPE_AES256_CTS);
    ber_int(w, BER_INT, KRB_ETYPE_AES128_CTS);
    ber_close(w, h);
    ber_close(w, g);

    ber_close(w, b);
    ber_close(w, a);

    /* The body the checksum covers is the KDC-REQ-BODY element itself,
     * context tag and all. */
    *body_off = start;
    *body_len = w->n - start;
}

/* ===========================================================
 * EncKDCRepPart — what a successful reply actually contains
 * =========================================================== */

static int krb_parse_enc_kdc_rep(const uint8_t *p, uint32_t n,
                                 uint32_t expect_nonce, krb_cred_t *out) {
    ber_r r = { p, n, 0, 0 };

    /*
     * The application tag should be 25 for an AS-REP and 26 for a
     * TGS-REP, and both are accepted for either. RFC 4120 section 5.4.2
     * records that some implementations unconditionally send 26; a
     * client that insists on 25 fails against them for a reason no
     * amount of staring at the ciphertext reveals.
     */
    int app = ber_peek(&r);
    if (app != (int)BER_APP(KRB_APP_ENC_AS_REP) &&
        app != (int)BER_APP(KRB_APP_ENC_TGS_REP)) return -1;
    ber_enter(&r, (uint8_t)app);
    uint32_t end = ber_enter(&r, BER_SEQ);

    int got_key = 0, nonce_ok = 0;

    while (r.at < end && !r.bad) {
        int t = ber_peek(&r);
        if (t == (int)BER_CTX(0)) {
            /* EncryptionKey */
            ber_enter(&r, BER_CTX(0));
            uint32_t kend = ber_enter(&r, BER_SEQ);
            while (r.at < kend && !r.bad) {
                int kt = ber_peek(&r);
                if (kt == (int)BER_CTX(0)) {
                    ber_enter(&r, BER_CTX(0));
                    out->session.etype = ber_read_int(&r);
                } else if (kt == (int)BER_CTX(1)) {
                    ber_enter(&r, BER_CTX(1));
                    int kl = ber_read_bytes(&r, out->session.data,
                                            sizeof out->session.data);
                    if (kl > 0) { out->session.len = (uint32_t)kl; got_key = 1; }
                } else {
                    ber_skip(&r);
                }
            }
            r.at = kend;
        } else if (t == (int)BER_CTX(2)) {
            ber_enter(&r, BER_CTX(2));
            uint32_t got = (uint32_t)ber_read_int(&r);
            /*
             * The nonce is the whole of the replay protection on this
             * reply. It was generated for this request; a reply that
             * echoes a different one is a recorded message being played
             * back, and accepting it means accepting a ticket the
             * attacker chose the timing of.
             */
            nonce_ok = (got == expect_nonce);
        } else if (t == (int)BER_CTX(4)) {
            ber_enter(&r, BER_CTX(4));
            uint32_t l;
            if (ber_next(&r, &l) == BER_BITSTR && l == 5) {
                out->flags = ((uint32_t)r.buf[r.at + 1] << 24) |
                             ((uint32_t)r.buf[r.at + 2] << 16) |
                             ((uint32_t)r.buf[r.at + 3] << 8)  |
                              (uint32_t)r.buf[r.at + 4];
                r.at += l;
            } else {
                r.bad = 1;
            }
        } else if (t == (int)BER_CTX(7)) {
            ber_enter(&r, BER_CTX(7));
            ber_read_str(&r, out->endtime, sizeof out->endtime);
        } else if (t == (int)BER_CTX(9)) {
            ber_enter(&r, BER_CTX(9));
            ber_read_str(&r, out->realm, sizeof out->realm);
        } else if (t == (int)BER_CTX(10)) {
            /* sname */
            ber_enter(&r, BER_CTX(10));
            uint32_t send_ = ber_enter(&r, BER_SEQ);
            out->nsname = 0;
            while (r.at < send_ && !r.bad) {
                int st = ber_peek(&r);
                if (st == (int)BER_CTX(1)) {
                    ber_enter(&r, BER_CTX(1));
                    uint32_t nend = ber_enter(&r, BER_SEQ);
                    while (r.at < nend && !r.bad && out->nsname < KRB_MAX_NAME)
                        ber_read_str(&r, out->sname[out->nsname++], 64);
                    r.at = nend;
                } else {
                    ber_skip(&r);
                }
            }
            r.at = send_;
        } else {
            ber_skip(&r);
        }
    }

    if (r.bad || !got_key) return -1;
    if (!nonce_ok) {
        serial_puts("[krb5] the reply's nonce does not match the request; "
                    "refusing it as a replay\n");
        return -1;
    }
    out->valid = 1;
    return 0;
}

/*
 * A KDC-REP: pull out the ticket verbatim and decrypt the enc-part.
 *
 * The ticket is copied rather than parsed. It is encrypted under the
 * *service's* key, not ours -- there is nothing in it this client can
 * read, and nothing it may usefully change. Keeping the exact bytes and
 * handing them back untouched is both the only thing that works and the
 * only thing that is safe.
 */
static int krb_parse_kdc_rep(const uint8_t *p, uint32_t n, int want_msg,
                             const krb_key_t *key, uint32_t usage,
                             uint32_t nonce, krb_cred_t *out) {
    ber_r r = { p, n, 0, 0 };

    if (ber_peek(&r) != (int)BER_APP(want_msg)) return -1;
    ber_enter(&r, BER_APP((uint8_t)want_msg));
    uint32_t end = ber_enter(&r, BER_SEQ);

    int have_ticket = 0, have_enc = 0;
    static uint8_t cipher[KRB_BUF];
    static uint8_t plain[KRB_BUF];
    int clen = 0, etype = 0;

    while (r.at < end && !r.bad) {
        int t = ber_peek(&r);
        if (t == (int)BER_CTX(5)) {
            /* The Ticket. Its DER starts at the [APPLICATION 1] tag
             * inside this explicit wrapper. */
            ber_enter(&r, BER_CTX(5));
            uint32_t tstart = r.at;
            uint32_t tlen;
            int tt = ber_next(&r, &tlen);
            if (tt != (int)BER_APP(KRB_APP_TICKET)) { r.bad = 1; break; }
            uint32_t whole = (r.at - tstart) + tlen;
            if (whole > KRB_MAX_TKT) {
                serial_puts("[krb5] ticket is larger than this client's "
                            "buffer\n");
                return -1;
            }
            for (uint32_t i = 0; i < whole; i++) out->ticket[i] = p[tstart + i];
            out->tlen = whole;
            r.at = tstart + whole;
            have_ticket = 1;
        } else if (t == (int)BER_CTX(6)) {
            clen = krb_get_encdata(&r, BER_CTX(6), &etype, cipher, sizeof cipher);
            have_enc = (clen > 0);
        } else {
            ber_skip(&r);
        }
    }

    if (r.bad || !have_ticket || !have_enc) return -1;
    if (etype != key->etype) {
        serial_puts("[krb5] the reply is encrypted in an encryption type "
                    "this client did not offer\n");
        return -1;
    }

    int plen = krb_decrypt(key, usage, cipher, (uint32_t)clen,
                           plain, sizeof plain);
    if (plen < 0) {
        /*
         * There is exactly one honest thing to say here. The tag failed,
         * and the overwhelmingly likely cause is that the key is wrong,
         * which means the password is wrong -- but a corrupted reply
         * looks identical, and so does the wrong salt.
         */
        serial_puts("[krb5] could not decrypt the reply: wrong password, "
                    "wrong salt, or a damaged message\n");
        return -1;
    }

    return krb_parse_enc_kdc_rep(plain, (uint32_t)plen, nonce, out);
}

/* ===========================================================
 * the AS exchange — a password becomes a ticket-granting ticket
 * =========================================================== */

static int krb_send_as_req(const char *user, const char *realm,
                           int with_preauth, uint32_t *nonce_out) {
    krb_ctx_t *c = &krb;
    ber_w w = { c->tx, KRB_BUF, 0, 0 };
    uint32_t nonce = krb_nonce();
    if (nonce == 0) return -1;
    *nonce_out = nonce;

    uint32_t a = ber_open(&w, BER_APP(KRB_MSG_AS_REQ));
    uint32_t b = ber_open(&w, BER_SEQ);

    uint32_t p = ber_open(&w, BER_CTX(1));
    ber_int(&w, BER_INT, 5);
    ber_close(&w, p);

    uint32_t m = ber_open(&w, BER_CTX(2));
    ber_int(&w, BER_INT, KRB_MSG_AS_REQ);
    ber_close(&w, m);

    if (with_preauth) {
        /*
         * PA-ENC-TIMESTAMP: the current time, encrypted under the
         * user's key. Proving knowledge of the password without
         * transmitting it, and the reason the reply is not an offline
         * cracking target.
         */
        uint8_t inner[64];
        ber_w iw = { inner, sizeof inner, 0, 0 };
        uint32_t s = ber_open(&iw, BER_SEQ);
        uint32_t t0 = ber_open(&iw, BER_CTX(0));
        char now[16];
        krb_now(now);
        ber_str(&iw, BER_GENTIME, now);
        ber_close(&iw, t0);
        uint32_t t1 = ber_open(&iw, BER_CTX(1));
        ber_int(&iw, BER_INT, 0);                 /* microseconds */
        ber_close(&iw, t1);
        ber_close(&iw, s);
        if (iw.overflow) return -1;

        uint8_t ct[128];
        int cl = krb_encrypt(&c->ckey, KRB_KU_PA_ENC_TIMESTAMP,
                             inner, iw.n, ct, sizeof ct);
        if (cl < 0) return -1;

        /* The EncryptedData is itself DER, and *that* is the padata
         * value -- an OCTET STRING whose contents are a nested
         * structure. Encoding the ciphertext directly instead is the
         * classic mistake and produces a request the KDC calls
         * malformed. */
        uint8_t ed[256];
        ber_w ew = { ed, sizeof ed, 0, 0 };
        {
            uint32_t x = ber_open(&ew, BER_SEQ);
            uint32_t y = ber_open(&ew, BER_CTX(0));
            ber_int(&ew, BER_INT, c->ckey.etype);
            ber_close(&ew, y);
            uint32_t z = ber_open(&ew, BER_CTX(2));
            ber_bytes(&ew, BER_OCTET, ct, (uint32_t)cl);
            ber_close(&ew, z);
            ber_close(&ew, x);
        }
        if (ew.overflow) return -1;

        uint32_t pd = ber_open(&w, BER_CTX(3));
        uint32_t pl = ber_open(&w, BER_SEQ);
        uint32_t one = ber_open(&w, BER_SEQ);
        uint32_t ty = ber_open(&w, BER_CTX(1));
        ber_int(&w, BER_INT, KRB_PA_ENC_TIMESTAMP);
        ber_close(&w, ty);
        uint32_t va = ber_open(&w, BER_CTX(2));
        ber_bytes(&w, BER_OCTET, ed, ew.n);
        ber_close(&w, va);
        ber_close(&w, one);
        ber_close(&w, pl);
        ber_close(&w, pd);
    }

    const char *krbtgt[2] = { "krbtgt", realm };
    uint32_t off, len;
    krb_put_req_body(&w, KRB_OPT_FORWARDABLE | KRB_OPT_RENEWABLE_OK,
                     user, realm, krbtgt, 2, nonce, &off, &len);

    ber_close(&w, b);
    ber_close(&w, a);
    if (w.overflow) return -1;
    return (int)w.n;
}

/*
 * Get a ticket-granting ticket.
 *
 * Two round trips by design: the first request carries no
 * pre-authentication and is expected to be refused, because the refusal
 * is what names the salt. See the note at the top of this file.
 */
static int krb_get_tgt(const char *kdc_host, uint16_t port, const char *realm,
                       const char *user, const char *password) {
    krb_ctx_t *c = &krb;

    for (uint32_t i = 0; i < sizeof c->realm && realm[i]; i++) c->realm[i] = realm[i];
    for (uint32_t i = 0; i < sizeof c->user && user[i]; i++) c->user[i] = user[i];
    c->realm[sizeof c->realm - 1] = 0;
    c->user[sizeof c->user - 1] = 0;
    c->pref_etype = KRB_ETYPE_AES256_CTS;
    c->saltlen = 0;
    c->tgt.valid = 0;

    if (krb_connect(kdc_host, port) != 0) return -1;

    /* ---- round one: ask, and be told what salt to use ---- */
    uint32_t nonce = 0;
    int n = krb_send_as_req(user, realm, 0, &nonce);
    if (n < 0) { krb_disconnect(); return -1; }

    uint32_t rl = 0;
    if (krb_exchange((uint32_t)n, &rl) != 0) { krb_disconnect(); return -1; }

    int code = krb_parse_error(c->rx, rl);
    if (code > 0 && code != KDC_ERR_PREAUTH_REQUIRED) {
        serial_puts("[krb5] ");
        serial_puts(krb_error_name(code));
        serial_puts("\n");
        krb_disconnect();
        return -1;
    }
    if (code < 0) {
        /* A KDC with pre-authentication disabled answers the first
         * request outright. Unusual, legal, and not something to
         * refuse -- but the salt then has to be the default one. */
        serial_puts("[krb5] the KDC does not require pre-authentication\n");
    }

    if (c->saltlen == 0) {
        const char *nm[1] = { user };
        c->saltlen = krb_salt(realm, nm, 1, c->salt, sizeof c->salt);
    }

    if (krb_string_to_key(c->pref_etype, password, c->salt, c->saltlen,
                          4096, &c->ckey) != 0) {
        serial_puts("[krb5] the KDC offered no encryption type this client "
                    "will use\n");
        krb_disconnect();
        return -1;
    }

    /* ---- round two: with the timestamp, and this time it works ---- */
    n = krb_send_as_req(user, realm, 1, &nonce);
    if (n < 0) { krb_disconnect(); return -1; }
    if (krb_exchange((uint32_t)n, &rl) != 0) { krb_disconnect(); return -1; }

    code = krb_parse_error(c->rx, rl);
    if (code >= 0) {
        serial_puts("[krb5] ");
        serial_puts(krb_error_name(code));
        if (c->last_error[0]) {
            serial_puts(" -- ");
            serial_puts(c->last_error);
        }
        serial_puts("\n");
        krb_disconnect();
        return -1;
    }

    if (krb_parse_kdc_rep(c->rx, rl, KRB_MSG_AS_REP, &c->ckey,
                          KRB_KU_AS_REP_ENCPART, nonce, &c->tgt) != 0) {
        krb_disconnect();
        return -1;
    }

    serial_puts("[krb5] ticket-granting ticket for ");
    serial_puts(user);
    serial_puts("@");
    serial_puts(realm);
    serial_puts(" until ");
    serial_puts(c->tgt.endtime);
    serial_puts("\n");
    krb_disconnect();
    krb_cc_save();
    return 0;
}

/* ===========================================================
 * AP-REQ — presenting a ticket
 * =========================================================== */

/*
 * Build an AP-REQ for a credential.
 *
 * `cksum` is the twelve-byte checksum to place in the Authenticator, or
 * null for none. A TGS-REQ requires one, over the request body; a
 * service AP-REQ ordinarily does not.
 *
 * The Authenticator is what stops a ticket being replayed: it is
 * encrypted under the session key, which only the ticket's rightful
 * holder has, and it carries the current time. A recorded AP-REQ can be
 * replayed, and will be rejected as too old.
 */
static int krb_make_ap_req(const krb_cred_t *cred, const char *crealm,
                           const char *cname, uint32_t usage,
                           const uint8_t *cksum, int cksumtype,
                           uint8_t *out, uint32_t max) {
    /* --- the Authenticator --- */
    uint8_t auth[512];
    ber_w aw = { auth, sizeof auth, 0, 0 };
    uint32_t a1 = ber_open(&aw, BER_APP(KRB_APP_AUTHENTICATOR));
    uint32_t a2 = ber_open(&aw, BER_SEQ);

    uint32_t v = ber_open(&aw, BER_CTX(0));
    ber_int(&aw, BER_INT, 5);
    ber_close(&aw, v);

    uint32_t cr = ber_open(&aw, BER_CTX(1));
    ber_str(&aw, BER_GENSTR, crealm);
    ber_close(&aw, cr);

    { const char *nm[1] = { cname };
      krb_put_principal(&aw, BER_CTX(2), KRB_NT_PRINCIPAL, nm, 1); }

    if (cksum) {
        uint32_t ck = ber_open(&aw, BER_CTX(3));
        uint32_t cs = ber_open(&aw, BER_SEQ);
        uint32_t ct = ber_open(&aw, BER_CTX(0));
        ber_int(&aw, BER_INT, cksumtype);
        ber_close(&aw, ct);
        uint32_t cv = ber_open(&aw, BER_CTX(1));
        ber_bytes(&aw, BER_OCTET, cksum, KRB_MACLEN);
        ber_close(&aw, cv);
        ber_close(&aw, cs);
        ber_close(&aw, ck);
    }

    uint32_t cu = ber_open(&aw, BER_CTX(4));
    ber_int(&aw, BER_INT, 0);                    /* cusec */
    ber_close(&aw, cu);

    uint32_t ti = ber_open(&aw, BER_CTX(5));
    char now[16];
    krb_now(now);
    ber_str(&aw, BER_GENTIME, now);
    ber_close(&aw, ti);

    ber_close(&aw, a2);
    ber_close(&aw, a1);
    if (aw.overflow) return -1;

    uint8_t ct[768];
    int cl = krb_encrypt(&cred->session, usage, auth, aw.n, ct, sizeof ct);
    if (cl < 0) return -1;

    /* --- the AP-REQ around it --- */
    ber_w w = { out, max, 0, 0 };
    uint32_t b1 = ber_open(&w, BER_APP(KRB_MSG_AP_REQ));
    uint32_t b2 = ber_open(&w, BER_SEQ);

    uint32_t p = ber_open(&w, BER_CTX(0));
    ber_int(&w, BER_INT, 5);
    ber_close(&w, p);

    uint32_t m = ber_open(&w, BER_CTX(1));
    ber_int(&w, BER_INT, KRB_MSG_AP_REQ);
    ber_close(&w, m);

    uint32_t o = ber_open(&w, BER_CTX(2));
    ber_bitstring32(&w, BER_BITSTR, 0);          /* no AP options */
    ber_close(&w, o);

    /* The ticket goes back exactly as it arrived. */
    uint32_t tk = ber_open(&w, BER_CTX(3));
    ber_puts(&w, cred->ticket, cred->tlen);
    ber_close(&w, tk);

    krb_put_encdata(&w, BER_CTX(4), cred->session.etype, ct, (uint32_t)cl);

    ber_close(&w, b2);
    ber_close(&w, b1);
    return w.overflow ? -1 : (int)w.n;
}

/* ===========================================================
 * the TGS exchange — a ticket-granting ticket becomes a service ticket
 * =========================================================== */

/*
 * Ask for a ticket to a service.
 *
 * `service` and `instance` are the two halves of a service principal:
 * ("cifs", "files.vextro.test") is the file server, ("ldap", host) the
 * directory. The password is not used and is not held anywhere in this
 * path -- the TGT is what authenticates the request, which is the whole
 * point of having one.
 */
static int krb_get_service_ticket(const char *kdc_host, uint16_t port,
                                  const char *service, const char *instance) {
    krb_ctx_t *c = &krb;
    if (!c->tgt.valid) {
        serial_puts("[krb5] no ticket-granting ticket; ask for one first\n");
        return -1;
    }
    if (krb_connect(kdc_host, port) != 0) return -1;

    ber_w w = { c->tx, KRB_BUF, 0, 0 };
    uint32_t nonce = krb_nonce();
    if (nonce == 0) { krb_disconnect(); return -1; }

    /*
     * The body has to be encoded before the padata that checksums it,
     * and it has to appear in the message *after* that padata. So it is
     * built once into scratch, checksummed, and then copied into place
     * -- rather than encoded twice, which is the arrangement that
     * silently produces a checksum over bytes that were never sent.
     */
    static uint8_t bodybuf[KRB_BUF];
    ber_w bw = { bodybuf, sizeof bodybuf, 0, 0 };
    const char *sname[2] = { service, instance };
    uint32_t off, len;
    krb_put_req_body(&bw, KRB_OPT_FORWARDABLE | KRB_OPT_RENEWABLE_OK,
                     0, c->realm, sname, 2, nonce, &off, &len);
    if (bw.overflow) { krb_disconnect(); return -1; }

    uint8_t cksum[KRB_MACLEN];
    krb_checksum(&c->tgt.session, KRB_KU_TGS_REQ_AUTH_CKSUM,
                 bodybuf + off, len, cksum);

    static uint8_t apreq[KRB_BUF];
    int al = krb_make_ap_req(&c->tgt, c->realm, c->user,
                             KRB_KU_TGS_REQ_AUTH, cksum,
                             krb_cksumtype(&c->tgt.session),
                             apreq, sizeof apreq);
    if (al < 0) { krb_disconnect(); return -1; }

    uint32_t a = ber_open(&w, BER_APP(KRB_MSG_TGS_REQ));
    uint32_t b = ber_open(&w, BER_SEQ);

    uint32_t p = ber_open(&w, BER_CTX(1));
    ber_int(&w, BER_INT, 5);
    ber_close(&w, p);

    uint32_t m = ber_open(&w, BER_CTX(2));
    ber_int(&w, BER_INT, KRB_MSG_TGS_REQ);
    ber_close(&w, m);

    uint32_t pd = ber_open(&w, BER_CTX(3));
    uint32_t pl = ber_open(&w, BER_SEQ);
    uint32_t one = ber_open(&w, BER_SEQ);
    uint32_t ty = ber_open(&w, BER_CTX(1));
    ber_int(&w, BER_INT, KRB_PA_TGS_REQ);
    ber_close(&w, ty);
    uint32_t va = ber_open(&w, BER_CTX(2));
    ber_bytes(&w, BER_OCTET, apreq, (uint32_t)al);
    ber_close(&w, va);
    ber_close(&w, one);
    ber_close(&w, pl);
    ber_close(&w, pd);

    ber_puts(&w, bodybuf + off, len);
    ber_close(&w, b);
    ber_close(&w, a);
    if (w.overflow) { krb_disconnect(); return -1; }

    uint32_t rl = 0;
    if (krb_exchange(w.n, &rl) != 0) { krb_disconnect(); return -1; }

    int code = krb_parse_error(c->rx, rl);
    if (code >= 0) {
        serial_puts("[krb5] ");
        serial_puts(krb_error_name(code));
        if (c->last_error[0]) { serial_puts(" -- "); serial_puts(c->last_error); }
        serial_puts("\n");
        krb_disconnect();
        return -1;
    }

    c->svc.valid = 0;
    /*
     * The reply is encrypted under the TGT's session key, and the usage
     * says which: 8 when the KDC used the session key, 9 when the
     * request carried a subkey. This client sends no subkey, so it is
     * always 8 -- and asking for 9 would fail to decrypt, indistinguishably
     * from a wrong ticket.
     */
    if (krb_parse_kdc_rep(c->rx, rl, KRB_MSG_TGS_REP, &c->tgt.session,
                          KRB_KU_TGS_REP_ENCPART, nonce, &c->svc) != 0) {
        krb_disconnect();
        return -1;
    }

    serial_puts("[krb5] service ticket for ");
    serial_puts(service);
    serial_puts("/");
    serial_puts(instance);
    serial_puts(" until ");
    serial_puts(c->svc.endtime);
    serial_puts("\n");
    krb_disconnect();
    krb_cc_save();
    return 0;
}

/* ===========================================================
 * the credential cache — tickets that survive a reboot
 * ===========================================================
 *
 * ---- what is stored, and why it is not an AP-REQ ----
 *
 * An AP-REQ cannot be cached, and the comment above krb_make_ap_req
 * says why in the course of explaining something else: it wraps an
 * Authenticator encrypted under the session key carrying the *current
 * time*, and "a recorded AP-REQ can be replayed, and will be rejected
 * as too old." Writing one to disk and presenting it after a reboot
 * would fail against any correct server, and against an incorrect one
 * it would be a replay.
 *
 * What survives a reboot is the credential the AP-REQ is built *from*,
 * and that is what every credential cache in existence actually holds:
 * the Ticket exactly as the KDC issued it -- opaque here, encrypted
 * under the service's own key -- together with the session key, the
 * expiry and the flags. From those, krb_make_ap_req() mints a fresh
 * AP-REQ with a current timestamp each time one is needed. So the
 * tickets persist and the authenticators never do, which is the correct
 * division and also the only one that works.
 *
 * ---- the part that needs justifying ----
 *
 * The header of this file used to end with: "Tickets live in memory for
 * the life of the boot and are not written to disk, which is a
 * limitation and also means there is no ccache file for anything to
 * steal." That sentence was true and it was a real security property.
 * This removes it, so something has to take its place.
 *
 * A session key is a bearer token. Anyone who can read it can
 * impersonate the user to the services the ticket names, with no
 * password and no further checks, until it expires. MIT krb5 writes
 * /tmp/krb5cc_<uid> and protects it with file permissions alone;
 * Windows keeps its cache in LSA process memory and never writes it at
 * all. Neither of those is available here, so this does two things:
 *
 *   1. The file lives inside the user's profile tree, which src/swap.h's
 *      neighbour src/profile.h now genuinely protects: fs_* refuses a
 *      non-administrator any path under another account's profile. That
 *      is the MIT model and it has the MIT model's limits -- an
 *      administrator, or anyone holding the disk image, walks past it.
 *
 *   2. The file is encrypted under a key derived from the *login*
 *      password, which closes exactly the gap that (1) leaves open. The
 *      password is available at precisely one moment -- when the person
 *      types it to log in -- and that is the moment the requirement
 *      calls for the cache to be read. So the derived key is taken
 *      then, held for the session, and wiped at logout. An attacker
 *      with the volume has an encrypted file and no key; an attacker
 *      with the running machine had the session anyway.
 *
 * What this deliberately does not do is encrypt under a key stored
 * beside the file. That would look like protection and provide none,
 * and a security property that is really obfuscation is worse than an
 * admitted absence, because it stops people asking.
 *
 * ---- what is still true ----
 *
 * A stolen cache plus a stolen password is a full compromise, and so is
 * a stolen cache within its lifetime if the password is guessed: the
 * KDF here is iterated SHA-256, not a memory-hard function, for the
 * reason chacha20.h already gives. Ticket lifetimes are the KDC's
 * policy and are typically ten hours, which bounds it.
 */

#define KRB_CC_MAGIC0  'V'
#define KRB_CC_MAGIC1  'X'
#define KRB_CC_MAGIC2  'K'
#define KRB_CC_MAGIC3  '1'
#define KRB_CC_HDR     64
#define KRB_CC_MAX     24576        /* two credentials with AD-sized PACs */
#define KRB_CC_FILE    "/krb5cc.dat"

/*
 * From the filesystem and profile layers, which are compiled after this
 * file -- kernel.c reaches kerberos.h at line 35 and desktop.h at 42.
 * Declared rather than included: pulling desktop.h in here would invert
 * the include order of half the system to reach four functions, and a
 * static declared ahead of its definition costs four lines. The same
 * pattern src/trap.h uses for its structured-exception hook.
 */
static void        profile_home(const char *name, char *out, int max);
static int         fs_write_file(const char *path, const void *data, uint32_t len);
static const void *fs_read_file(const char *filename, uint64_t *out_size);
static int         fs_delete(const char *path);

typedef struct {
    uint8_t magic[4];
    uint8_t version;
    uint8_t count;
    uint8_t salt[16];
    uint8_t nonce[12];
    uint8_t verifier[16];
    uint8_t plain_len[4];       /* little-endian */
    uint8_t reserved[10];
} __attribute__((packed)) krb_cc_hdr_t;

/* The key derived from the login password, held for the session so that
 * a ticket acquired at three in the afternoon can be written without
 * asking for the password again. Wiped by krb_cc_logout(). */
static uint8_t krb_cc_key[CC20_KEY_BYTES];
static uint8_t krb_cc_salt[16];
static int     krb_cc_bound = 0;
static char    krb_cc_user[64] = "";

static uint8_t krb_cc_buf[KRB_CC_MAX];

/*
 * A second, domain-separated hash of the key.
 *
 * ChaCha20 has no idea whether it was given the right key: decrypting
 * with the wrong one produces plausible bytes, and a "loaded" cache of
 * noise would mean a session key of noise and authentication failures
 * with no explanation. Same reasoning, and the same construction, as
 * the vault in src/security.h.
 */
static void krb_cc_verifier(const uint8_t key[CC20_KEY_BYTES], uint8_t out[16]) {
    uint8_t tmp[CC20_KEY_BYTES + 8];
    for (int i = 0; i < CC20_KEY_BYTES; i++) tmp[i] = key[i];
    const char *tag = "vxkrbcc1";
    for (int i = 0; i < 8; i++) tmp[CC20_KEY_BYTES + i] = (uint8_t)tag[i];
    uint8_t d[32];
    sha256(tmp, CC20_KEY_BYTES + 8, d);
    for (int i = 0; i < 16; i++) out[i] = d[i];
}

static void krb_cc_path(const char *user, char *out, int max) {
    profile_home(user, out, max);
    int n = 0;
    while (out[n]) n++;
    const char *suffix = KRB_CC_FILE;
    for (int i = 0; suffix[i] && n < max - 1; i++) out[n++] = suffix[i];
    out[n] = '\0';
}

/*
 * Has this credential expired?
 *
 * Both times are "YYYYMMDDHHMMSSZ", fixed width and most-significant
 * field first, which is the one property that makes a string compare a
 * correct chronological compare. No parsing, no arithmetic, and nothing
 * to get wrong about month lengths.
 *
 * A wrong clock produces a wrong answer in the safe direction more
 * often than not: a machine whose RTC reads earlier than reality keeps
 * a ticket the KDC has already retired, and the server rejects it. A
 * machine reading later discards a good one and asks for another. Both
 * are recoverable; neither is silent.
 */
static int krb_cc_expired(const char *endtime) {
    char now[16];
    krb_now(now);
    for (int i = 0; i < 14; i++) {
        if (!endtime[i]) return 1;            /* truncated: treat as expired */
        if (endtime[i] != now[i])
            return endtime[i] < now[i];
    }
    return 0;                                  /* expiring this very second */
}

/* ---- serialisation ---- */

static uint32_t kcc_put16(uint8_t *p, uint32_t off, uint16_t v) {
    p[off] = (uint8_t)v; p[off + 1] = (uint8_t)(v >> 8);
    return off + 2;
}
static uint32_t kcc_put32(uint8_t *p, uint32_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) p[off + i] = (uint8_t)(v >> (i * 8));
    return off + 4;
}
static uint16_t kcc_get16(const uint8_t *p, uint32_t off) {
    return (uint16_t)(p[off] | (p[off + 1] << 8));
}
static uint32_t kcc_get32(const uint8_t *p, uint32_t off) {
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--) v = (v << 8) | p[off + i];
    return v;
}

static uint32_t kcc_put_str(uint8_t *p, uint32_t off, const char *s, uint32_t cap) {
    uint32_t n = 0;
    while (s[n]) n++;
    if (off + 2 + n > cap) return cap + 1;         /* overflow, caught by caller */
    off = kcc_put16(p, off, (uint16_t)n);
    for (uint32_t i = 0; i < n; i++) p[off + i] = (uint8_t)s[i];
    return off + n;
}

static uint32_t kcc_get_str(const uint8_t *p, uint32_t off, uint32_t len,
                            char *out, uint32_t max) {
    if (off + 2 > len) return len + 1;
    uint32_t n = kcc_get16(p, off);
    off += 2;
    if (off + n > len) return len + 1;
    uint32_t k = 0;
    for (; k < n && k < max - 1; k++) out[k] = (char)p[off + k];
    out[k] = '\0';
    return off + n;
}

static uint32_t krb_cc_encode_cred(const krb_cred_t *c, uint8_t *p,
                                   uint32_t off, uint32_t cap) {
    off = kcc_put_str(p, off, c->realm, cap);
    if (off > cap) return cap + 1;

    if (off + 1 > cap) return cap + 1;
    p[off++] = (uint8_t)c->nsname;
    for (int i = 0; i < c->nsname; i++) {
        off = kcc_put_str(p, off, c->sname[i], cap);
        if (off > cap) return cap + 1;
    }

    off = kcc_put_str(p, off, c->endtime, cap);
    if (off > cap) return cap + 1;

    if (off + 12 + c->session.len + 4 + c->tlen > cap) return cap + 1;
    off = kcc_put32(p, off, c->flags);
    off = kcc_put32(p, off, (uint32_t)c->session.etype);
    off = kcc_put32(p, off, c->session.len);
    for (uint32_t i = 0; i < c->session.len; i++) p[off + i] = c->session.data[i];
    off += c->session.len;

    off = kcc_put32(p, off, c->tlen);
    for (uint32_t i = 0; i < c->tlen; i++) p[off + i] = c->ticket[i];
    return off + c->tlen;
}

static uint32_t krb_cc_decode_cred(const uint8_t *p, uint32_t off, uint32_t len,
                                   krb_cred_t *c) {
    c->valid = 0;

    off = kcc_get_str(p, off, len, c->realm, sizeof c->realm);
    if (off > len) return len + 1;

    if (off + 1 > len) return len + 1;
    int ns = p[off++];
    if (ns < 0 || ns > KRB_MAX_NAME) return len + 1;
    c->nsname = ns;
    for (int i = 0; i < ns; i++) {
        off = kcc_get_str(p, off, len, c->sname[i], sizeof c->sname[i]);
        if (off > len) return len + 1;
    }

    off = kcc_get_str(p, off, len, c->endtime, sizeof c->endtime);
    if (off > len) return len + 1;

    if (off + 12 > len) return len + 1;
    c->flags        = kcc_get32(p, off); off += 4;
    c->session.etype = (int)kcc_get32(p, off); off += 4;
    c->session.len  = kcc_get32(p, off); off += 4;
    if (c->session.len > sizeof c->session.data) return len + 1;
    if (off + c->session.len > len) return len + 1;
    for (uint32_t i = 0; i < c->session.len; i++) c->session.data[i] = p[off + i];
    off += c->session.len;

    if (off + 4 > len) return len + 1;
    c->tlen = kcc_get32(p, off); off += 4;
    if (c->tlen > KRB_MAX_TKT) return len + 1;
    if (off + c->tlen > len) return len + 1;
    for (uint32_t i = 0; i < c->tlen; i++) c->ticket[i] = p[off + i];
    off += c->tlen;

    c->valid = 1;
    return off;
}

/* ---- the file ---- */

/*
 * Write whatever is currently held and unexpired.
 *
 * Called after each successful exchange, so the cache tracks the
 * session rather than being written once at logout -- a machine that
 * loses power between acquiring a ticket and shutting down cleanly
 * should still come back with it.
 *
 * Silently does nothing when no key is bound, which is the case for a
 * machine nobody has logged into and for the headless self-tests. That
 * is deliberate: acquiring a ticket must not start failing because
 * there is nowhere to cache it.
 */
static int krb_cc_save(void) {
    if (!krb_cc_bound) return 0;

    const krb_cred_t *creds[2] = { &krb.tgt, &krb.svc };
    uint32_t off = 0;
    int count = 0;

    for (int i = 0; i < 2; i++) {
        if (!creds[i]->valid) continue;
        if (krb_cc_expired(creds[i]->endtime)) continue;
        uint32_t next = krb_cc_encode_cred(creds[i], krb_cc_buf, off, KRB_CC_MAX);
        if (next > KRB_CC_MAX) {
            serial_puts("[krb5] credential too large to cache; skipped\n");
            continue;
        }
        off = next;
        count++;
    }

    char path[288];
    krb_cc_path(krb_cc_user, path, sizeof path);

    if (count == 0) {                    /* nothing worth keeping */
        fs_delete(path);
        return 0;
    }

    static uint8_t out[KRB_CC_HDR + KRB_CC_MAX];
    krb_cc_hdr_t h;
    h.magic[0] = KRB_CC_MAGIC0; h.magic[1] = KRB_CC_MAGIC1;
    h.magic[2] = KRB_CC_MAGIC2; h.magic[3] = KRB_CC_MAGIC3;
    h.version = 1;
    h.count   = (uint8_t)count;
    for (int i = 0; i < 16; i++) h.salt[i] = krb_cc_salt[i];
    if (vx_random(h.nonce, 12) != 12) return -1;
    krb_cc_verifier(krb_cc_key, h.verifier);
    for (int i = 0; i < 4; i++) h.plain_len[i] = (uint8_t)(off >> (i * 8));
    for (int i = 0; i < 10; i++) h.reserved[i] = 0;

    const uint8_t *hp = (const uint8_t *)&h;
    for (uint32_t i = 0; i < KRB_CC_HDR; i++)
        out[i] = i < sizeof h ? hp[i] : 0;
    for (uint32_t i = 0; i < off; i++) out[KRB_CC_HDR + i] = krb_cc_buf[i];

    /* A fresh nonce every write, so two saves under the same key never
     * share a keystream -- which for a stream cipher would xor two
     * ticket blobs together and leak both. */
    cc20_xor(krb_cc_key, h.nonce, 0, out + KRB_CC_HDR, off);

    int rc = fs_write_file(path, out, KRB_CC_HDR + off);

    /* The plaintext staging buffer is a copy of every session key held.
     * It does not get to outlive the call. */
    for (uint32_t i = 0; i < off; i++) krb_cc_buf[i] = 0;

    if (rc != 0) {
        serial_puts("[krb5] could not write the credential cache\n");
        return -1;
    }
    serial_puts("[krb5] cached ");
    serial_put_dec((uint32_t)count);
    serial_puts(" credential(s) to the profile\n");
    return 0;
}

/*
 * Read the cache and adopt anything still valid.
 *
 * Returns the number of credentials loaded. Every failure below is
 * quiet and returns zero: a missing cache is the ordinary first-login
 * case, and a cache that will not decrypt is what a changed password
 * looks like. Neither is an error worth stopping a login for.
 */
static int krb_cc_load(void) {
    if (!krb_cc_bound) return 0;

    char path[288];
    krb_cc_path(krb_cc_user, path, sizeof path);

    uint64_t len = 0;
    const void *raw = fs_read_file(path, &len);
    if (!raw || len < KRB_CC_HDR + 8) return 0;

    const uint8_t *p = (const uint8_t *)raw;
    const krb_cc_hdr_t *h = (const krb_cc_hdr_t *)p;
    if (h->magic[0] != KRB_CC_MAGIC0 || h->magic[1] != KRB_CC_MAGIC1 ||
        h->magic[2] != KRB_CC_MAGIC2 || h->magic[3] != KRB_CC_MAGIC3 ||
        h->version != 1)
        return 0;

    uint32_t plain = 0;
    for (int i = 3; i >= 0; i--) plain = (plain << 8) | h->plain_len[i];
    if (plain == 0 || plain > KRB_CC_MAX || KRB_CC_HDR + plain > len) return 0;

    /*
     * The key comes from the password and the salt in this file, not
     * from the salt held in memory -- a cache written under a previous
     * password has a different one, and deriving with the wrong salt is
     * how a wrong-password load turns into garbage rather than a clean
     * refusal.
     */
    uint8_t key[CC20_KEY_BYTES], want[16];
    for (int i = 0; i < 16; i++) krb_cc_salt[i] = h->salt[i];
    for (int i = 0; i < CC20_KEY_BYTES; i++) key[i] = krb_cc_key[i];
    krb_cc_verifier(key, want);
    if (!cc20_equal(want, h->verifier, 16)) {
        serial_puts("[krb5] the cached credentials are not this password's; "
                    "leaving them alone\n");
        return 0;
    }

    for (uint32_t i = 0; i < plain; i++) krb_cc_buf[i] = p[KRB_CC_HDR + i];
    cc20_xor(key, h->nonce, 0, krb_cc_buf, plain);

    int loaded = 0, expired = 0;
    uint32_t off = 0;
    for (int i = 0; i < h->count && off < plain; i++) {
        krb_cred_t c;
        uint32_t next = krb_cc_decode_cred(krb_cc_buf, off, plain, &c);
        if (next > plain) {
            serial_puts("[krb5] the credential cache is malformed; "
                        "ignoring the rest\n");
            break;
        }
        off = next;

        if (krb_cc_expired(c.endtime)) { expired++; continue; }

        /*
         * A ticket whose first name component is "krbtgt" is the
         * ticket-granting ticket; anything else is a service ticket.
         * That is how it is told apart on the wire and it is how it is
         * told apart here, rather than by the order it was written in.
         */
        int is_tgt = 1;
        {
            const char *want = "krbtgt";
            if (c.nsname < 1) is_tgt = 0;
            else for (int k = 0; k < 7; k++)
                if (c.sname[0][k] != want[k]) { is_tgt = 0; break; }
        }
        if (is_tgt) krb.tgt = c; else krb.svc = c;

        /* The realm the client belongs to comes back with the TGT, so a
         * reloaded session knows who it is without another AS exchange. */
        if (is_tgt) {
            for (uint32_t k = 0; k < sizeof krb.realm; k++)
                krb.realm[k] = c.realm[k < sizeof c.realm ? k : 0];
            krb.realm[sizeof krb.realm - 1] = '\0';
        }
        loaded++;
    }

    for (uint32_t i = 0; i < plain; i++) krb_cc_buf[i] = 0;

    if (loaded || expired) {
        serial_puts("[krb5] credential cache: ");
        serial_put_dec((uint32_t)loaded);
        serial_puts(" loaded, ");
        serial_put_dec((uint32_t)expired);
        serial_puts(" expired and discarded\n");
    }
    if (expired && !loaded) fs_delete(path);
    return loaded;
}

/*
 * Bind the cache to whoever has just logged in, and read it.
 *
 * The password is used here and nowhere else, and is not retained --
 * only the key derived from it, which cannot be turned back into the
 * password and cannot be used to log in.
 */
static int krb_cc_login(const char *user, const char *password) {
    krb_cc_bound = 0;
    if (!user || !user[0] || !password) return 0;

    int n = 0;
    for (; user[n] && n < (int)sizeof(krb_cc_user) - 1; n++)
        krb_cc_user[n] = user[n];
    krb_cc_user[n] = '\0';

    /*
     * A salt for the case where there is no cache yet. If one exists,
     * krb_cc_load() replaces this with the file's own -- which is why
     * the key is derived twice on the path where a cache is found, and
     * why that is not worth avoiding: it happens once per login.
     */
    if (vx_random(krb_cc_salt, 16) != 16) return 0;

    char pw[128];
    int k = 0;
    for (; password[k] && k < (int)sizeof(pw) - 1; k++) pw[k] = password[k];
    pw[k] = '\0';

    /* Peek at the file's salt first, so the common path derives once. */
    char path[288];
    krb_cc_path(krb_cc_user, path, sizeof path);
    uint64_t len = 0;
    const void *raw = fs_read_file(path, &len);
    if (raw && len >= KRB_CC_HDR) {
        const krb_cc_hdr_t *h = (const krb_cc_hdr_t *)raw;
        if (h->magic[0] == KRB_CC_MAGIC0 && h->magic[1] == KRB_CC_MAGIC1 &&
            h->magic[2] == KRB_CC_MAGIC2 && h->magic[3] == KRB_CC_MAGIC3)
            for (int i = 0; i < 16; i++) krb_cc_salt[i] = h->salt[i];
    }

    cc20_derive_key(pw, krb_cc_salt, krb_cc_key);
    for (int i = 0; i < (int)sizeof(pw); i++) pw[i] = 0;
    krb_cc_bound = 1;

    return krb_cc_load();
}

/*
 * Logging out.
 *
 * The key goes, and so do the tickets in memory -- leaving a session
 * key resident after the person who owns it has left is exactly the
 * bearer-token problem this file spent a page describing. The *file*
 * stays: that is the point of it, and it is encrypted under a key
 * nobody present can now derive.
 */
static void krb_cc_logout(void) {
    for (int i = 0; i < CC20_KEY_BYTES; i++) krb_cc_key[i] = 0;
    for (int i = 0; i < 16; i++) krb_cc_salt[i] = 0;
    krb_cc_user[0] = '\0';
    krb_cc_bound = 0;

    for (uint32_t i = 0; i < sizeof krb.tgt.session.data; i++)
        krb.tgt.session.data[i] = 0;
    for (uint32_t i = 0; i < sizeof krb.svc.session.data; i++)
        krb.svc.session.data[i] = 0;
    krb.tgt.valid = 0;
    krb.svc.valid = 0;
    for (uint32_t i = 0; i < sizeof krb.ckey.data; i++) krb.ckey.data[i] = 0;
}

/* Throw the cache away without logging out -- `kdestroy`, in effect. */
static int krb_cc_purge(void) {
    if (!krb_cc_bound) return -1;
    char path[288];
    krb_cc_path(krb_cc_user, path, sizeof path);
    krb.tgt.valid = 0;
    krb.svc.valid = 0;
    return fs_delete(path);
}

static int krb_have_tgt(void)     { return krb.tgt.valid; }
static int krb_have_service(void) { return krb.svc.valid; }
static const krb_cred_t *krb_service_cred(void) { return &krb.svc; }

#endif /* KERBEROS_H */
