#ifndef NTLMSSP_H
#define NTLMSSP_H

#include <stdint.h>
#include "ntcrypto.h"

/*
 * src/ntlmssp.h — the other way a Windows network proves who you are.
 *
 * Kerberos, in src/kerberos.h, is the one a domain uses when it can.
 * NTLM is the one it falls back to: when there is no domain, when the
 * server was reached by IP address rather than by name, when the clocks
 * disagree by more than five minutes. Every SMB implementation still
 * needs it, which is why src/ntcrypto.h exists at all.
 *
 * ---- three messages ----
 *
 * The client says what it can do. The server replies with eight random
 * bytes -- the challenge -- and a list of names for itself. The client
 * proves it knows the password by HMAC-ing that challenge under a key
 * derived from it, and never sends the password.
 *
 * The derivation is NTLMv2, and version two matters. NTLMv1 signed the
 * server's challenge alone, so a malicious server could hand a client
 * a challenge it had just been given by a *third* machine and relay the
 * answer. v2 folds the user name, the domain, a client-chosen nonce and
 * the server's own claimed identity into the response, so an answer
 * computed for one server does not verify at another.
 *
 * ---- where RC4 comes in ----
 *
 * src/ntcrypto.h says of RC4: "exists only because NTLM's session
 * sealing is defined in terms of it." This is that. When key exchange
 * is negotiated, the client picks the session key itself at random and
 * sends it RC4-encrypted under the key derived from the password. That
 * is the whole use, and the reason the client picks it is worth stating:
 * the derived key is a deterministic function of a password, so two
 * sessions by the same user would otherwise share a signing key.
 *
 * ---- what this is not ----
 *
 * This authenticates. It does not encrypt anything afterwards: sealing
 * is not implemented, and the SMB layer above signs rather than seals.
 * NTLM also cannot authenticate the *server* to the client without
 * channel binding, which is not here either. If both ends can speak
 * Kerberos, they should.
 */

#define NTLMSSP_NEGOTIATE_UNICODE          0x00000001u
#define NTLMSSP_REQUEST_TARGET             0x00000004u
#define NTLMSSP_NEGOTIATE_SIGN             0x00000010u
#define NTLMSSP_NEGOTIATE_NTLM             0x00000200u
#define NTLMSSP_NEGOTIATE_ALWAYS_SIGN      0x00008000u
#define NTLMSSP_NEGOTIATE_EXTENDED_SEC     0x00080000u
#define NTLMSSP_NEGOTIATE_TARGET_INFO      0x00800000u
#define NTLMSSP_NEGOTIATE_128              0x20000000u
#define NTLMSSP_NEGOTIATE_KEY_EXCH         0x40000000u
#define NTLMSSP_NEGOTIATE_56               0x80000000u

/* AV pair identifiers inside the server's target information. */
#define MSV_AV_EOL          0
#define MSV_AV_TIMESTAMP    7

#define NTLM_MAX_TARGET   1024

typedef struct {
    uint8_t  challenge[8];
    uint8_t  target_info[NTLM_MAX_TARGET];
    uint32_t target_len;
    uint32_t flags;
    uint64_t timestamp;          /* from the server, if it offered one */
    int      have_timestamp;
} ntlm_challenge_t;

/* The key SMB signs with once authentication has succeeded. */
typedef struct {
    uint8_t session_key[16];
    int     valid;
} ntlm_session_t;

static void ntlm_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void ntlm_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static uint16_t ntlm_get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t ntlm_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * Windows counts time in hundred-nanosecond intervals since the first
 * of January 1601 -- the start of the 400-year Gregorian cycle that
 * contained the date the format was designed.
 *
 * The civil-date arithmetic is the standard shifted-era trick: move the
 * year boundary to March so that the leap day falls at the end, which
 * removes every special case from the month-length table.
 */
static uint64_t ntlm_days_from_civil(int y, int m, int d) {
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    uint32_t yoe = (uint32_t)(y - era * 400);
    uint32_t doy = (uint32_t)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint64_t)((int64_t)era * 146097 + (int64_t)doe);
}

static uint64_t ntlm_filetime(int yr, int mo, int d, int hh, int mm, int ss) {
    uint64_t days = ntlm_days_from_civil(yr, mo, d) -
                    ntlm_days_from_civil(1601, 1, 1);
    uint64_t secs = days * 86400ull + (uint64_t)hh * 3600 +
                    (uint64_t)mm * 60 + (uint64_t)ss;
    return secs * 10000000ull;
}

/* ===== message one: what this client can do ===== */

static uint32_t ntlm_negotiate(uint8_t *out, uint32_t max) {
    if (max < 40) return 0;
    for (uint32_t i = 0; i < 40; i++) out[i] = 0;

    const char sig[] = "NTLMSSP";
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)sig[i];   /* includes the NUL */
    ntlm_put32(out + 8, 1);

    /* No domain and no workstation name are offered. Both are optional,
     * both are advisory, and neither affects the key -- sending them
     * would only tell an unauthenticated server the name of this
     * machine. */
    ntlm_put32(out + 12, NTLMSSP_NEGOTIATE_UNICODE |
                         NTLMSSP_REQUEST_TARGET |
                         NTLMSSP_NEGOTIATE_SIGN |
                         NTLMSSP_NEGOTIATE_NTLM |
                         NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
                         NTLMSSP_NEGOTIATE_EXTENDED_SEC |
                         NTLMSSP_NEGOTIATE_TARGET_INFO |
                         NTLMSSP_NEGOTIATE_KEY_EXCH |
                         NTLMSSP_NEGOTIATE_128 |
                         NTLMSSP_NEGOTIATE_56);
    return 40;
}

/* ===== message two: the server's challenge ===== */

static int ntlm_parse_challenge(const uint8_t *p, uint32_t n,
                                ntlm_challenge_t *out) {
    if (n < 48) return -1;
    if (p[0] != 'N' || p[1] != 'T' || p[2] != 'L' || p[3] != 'M' ||
        p[4] != 'S' || p[5] != 'S' || p[6] != 'P' || p[7] != 0) return -1;
    if (ntlm_get32(p + 8) != 2) return -1;

    for (int i = 0; i < 8; i++) out->challenge[i] = p[24 + i];
    out->flags = ntlm_get32(p + 20);

    uint16_t tlen = ntlm_get16(p + 40);
    uint32_t toff = ntlm_get32(p + 44);
    out->target_len = 0;
    out->have_timestamp = 0;
    out->timestamp = 0;

    /* Bounds first: the offset and length are two independent numbers
     * from an unauthenticated server, and nothing has yet established
     * that they point inside the message. */
    if (tlen && toff <= n && (uint32_t)tlen <= n - toff &&
        tlen <= NTLM_MAX_TARGET) {
        out->target_len = tlen;
        for (uint32_t i = 0; i < tlen; i++) out->target_info[i] = p[toff + i];

        /* Walk the AV pairs for a timestamp. Using the server's rather
         * than ours is what MS-NLMP asks for, and it also sidesteps a
         * clock disagreement in the one place NTLM has one. */
        uint32_t at = 0;
        while (at + 4 <= tlen) {
            uint16_t id  = ntlm_get16(out->target_info + at);
            uint16_t len = ntlm_get16(out->target_info + at + 2);
            at += 4;
            if (len > tlen - at) break;
            if (id == MSV_AV_EOL) break;
            if (id == MSV_AV_TIMESTAMP && len == 8) {
                uint64_t v = 0;
                for (int i = 7; i >= 0; i--)
                    v = (v << 8) | out->target_info[at + (uint32_t)i];
                out->timestamp = v;
                out->have_timestamp = 1;
            }
            at += len;
        }
    }
    return 0;
}

/* ===== message three: the proof ===== */

static uint32_t ntlm_wide(const char *s, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    while (*s && n + 2 <= max) { out[n++] = (uint8_t)*s++; out[n++] = 0; }
    return n;
}

/*
 * Build the AUTHENTICATE message and derive the session key.
 *
 * `now` is a FILETIME used only when the server offered no timestamp of
 * its own.
 *
 * Returns the message length, or 0. On success `sess` holds the key
 * every subsequent SMB message is signed with.
 */
static uint32_t ntlm_authenticate(const ntlm_challenge_t *ch,
                                  const char *domain, const char *user,
                                  const char *password, uint64_t now,
                                  uint8_t *out, uint32_t max,
                                  ntlm_session_t *sess) {
    uint8_t nt_hash[16], v2_hash[16];
    ntlm_nt_hash(password, nt_hash);
    ntlm_v2_hash(user, domain, nt_hash, v2_hash);

    /* ---- the blob the response is computed over ---- */
    uint8_t blob[NTLM_MAX_TARGET + 64];
    uint32_t b = 0;
    blob[b++] = 0x01; blob[b++] = 0x01;          /* version, always this */
    blob[b++] = 0; blob[b++] = 0;
    for (int i = 0; i < 4; i++) blob[b++] = 0;

    uint64_t ts = ch->have_timestamp ? ch->timestamp : now;
    for (int i = 0; i < 8; i++) blob[b++] = (uint8_t)(ts >> (i * 8));

    /* The client's own nonce. Without it, the response depends only on
     * values the server chose, and a server that reuses a challenge
     * gets the same answer twice. */
    uint8_t cnonce[8];
    if (vx_random(cnonce, 8) != 8) return 0;
    for (int i = 0; i < 8; i++) blob[b++] = cnonce[i];

    for (int i = 0; i < 4; i++) blob[b++] = 0;
    for (uint32_t i = 0; i < ch->target_len; i++) blob[b++] = ch->target_info[i];
    for (int i = 0; i < 4; i++) blob[b++] = 0;

    /* ---- the response, and the key that falls out of it ---- */
    uint8_t tmp[NTLM_MAX_TARGET + 128];
    for (int i = 0; i < 8; i++) tmp[i] = ch->challenge[i];
    for (uint32_t i = 0; i < b; i++) tmp[8 + i] = blob[i];

    uint8_t proof[16];
    hmac_md5(v2_hash, 16, tmp, 8 + b, proof);

    uint8_t base_key[16];
    hmac_md5(v2_hash, 16, proof, 16, base_key);

    /*
     * Key exchange. The session key is chosen here, at random, and sent
     * encrypted under the key derived from the password -- because the
     * derived key is a deterministic function of that password, so
     * without this step every session by the same user would sign with
     * the same key.
     *
     * This is the only thing RC4 is used for in this system.
     */
    uint8_t exported[16], sealed[16];
    if (vx_random(exported, 16) != 16) return 0;
    rc4_ctx rc;
    rc4_init(&rc, base_key, 16);
    rc4_apply(&rc, exported, sealed, 16);

    /* ---- lay the message out ---- */
    uint8_t wdom[128], wuser[128];
    uint32_t dlen = ntlm_wide(domain, wdom, sizeof wdom);
    uint32_t ulen = ntlm_wide(user, wuser, sizeof wuser);

    uint32_t ntlen = 16 + b;                    /* proof followed by blob */
    uint32_t hdr = 64;
    uint32_t need = hdr + 24 + ntlen + dlen + ulen + 16;
    if (need > max) return 0;

    for (uint32_t i = 0; i < hdr; i++) out[i] = 0;
    const char sig[] = "NTLMSSP";
    for (int i = 0; i < 8; i++) out[i] = (uint8_t)sig[i];
    ntlm_put32(out + 8, 3);

    uint32_t off = hdr;

    /* The LM response is 24 zero bytes. NTLMv2 defines one, and it is
     * worth nothing -- sending zeros is what every modern client does,
     * and a server that requires a real one is asking for NTLMv1. */
    ntlm_put16(out + 12, 24); ntlm_put16(out + 14, 24); ntlm_put32(out + 16, off);
    for (int i = 0; i < 24; i++) out[off + i] = 0;
    off += 24;

    ntlm_put16(out + 20, (uint16_t)ntlen); ntlm_put16(out + 22, (uint16_t)ntlen);
    ntlm_put32(out + 24, off);
    for (int i = 0; i < 16; i++) out[off + i] = proof[i];
    for (uint32_t i = 0; i < b; i++) out[off + 16 + i] = blob[i];
    off += ntlen;

    ntlm_put16(out + 28, (uint16_t)dlen); ntlm_put16(out + 30, (uint16_t)dlen);
    ntlm_put32(out + 32, off);
    for (uint32_t i = 0; i < dlen; i++) out[off + i] = wdom[i];
    off += dlen;

    ntlm_put16(out + 36, (uint16_t)ulen); ntlm_put16(out + 38, (uint16_t)ulen);
    ntlm_put32(out + 40, off);
    for (uint32_t i = 0; i < ulen; i++) out[off + i] = wuser[i];
    off += ulen;

    /* Workstation: absent, as in message one. */
    ntlm_put16(out + 44, 0); ntlm_put16(out + 46, 0); ntlm_put32(out + 48, off);

    ntlm_put16(out + 52, 16); ntlm_put16(out + 54, 16); ntlm_put32(out + 56, off);
    for (int i = 0; i < 16; i++) out[off + i] = sealed[i];
    off += 16;

    ntlm_put32(out + 60, NTLMSSP_NEGOTIATE_UNICODE |
                         NTLMSSP_REQUEST_TARGET |
                         NTLMSSP_NEGOTIATE_SIGN |
                         NTLMSSP_NEGOTIATE_NTLM |
                         NTLMSSP_NEGOTIATE_ALWAYS_SIGN |
                         NTLMSSP_NEGOTIATE_EXTENDED_SEC |
                         NTLMSSP_NEGOTIATE_TARGET_INFO |
                         NTLMSSP_NEGOTIATE_KEY_EXCH |
                         NTLMSSP_NEGOTIATE_128 |
                         NTLMSSP_NEGOTIATE_56);

    for (int i = 0; i < 16; i++) sess->session_key[i] = exported[i];
    sess->valid = 1;
    return off;
}

#endif /* NTLMSSP_H */
