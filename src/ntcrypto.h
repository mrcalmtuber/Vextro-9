#ifndef NTCRYPTO_H
#define NTCRYPTO_H

#include <stdint.h>

/*
 * src/ntcrypto.h — the old algorithms Windows networking is built on.
 *
 * MD4, MD5, SHA-1 and RC4. Every one of them is broken for the purpose
 * it was designed for, and every one of them is required to talk to a
 * Windows domain: NTLM hashes a password with MD4, signs with HMAC-MD5,
 * and seals with RC4; SMB2 and Kerberos both still carry SHA-1 in
 * places no amount of modernisation has reached.
 *
 * ---- why these are written here and not switched on in Mbed TLS ----
 *
 * Mbed TLS has SHA-1 and MD5 and would compile them with two lines in
 * third_party/mbedtls/vextro_config.h. That is exactly what must not
 * happen. That config is stripped to TLS 1.3, and the reason TLS 1.3 is
 * safe is that it *removed* these: a build where the TLS library can be
 * persuaded to hash with SHA-1 is a build where a downgrade has
 * somewhere to land. Keeping them in a separate file with no path into
 * the TLS stack means the record layer cannot reach them however it is
 * configured.
 *
 * MD4 settles the question anyway -- Mbed TLS 3.x removed it entirely,
 * and NTLM cannot be done without it.
 *
 * ---- what these are for, and what they are not ----
 *
 * These authenticate to a file server. They are not, and must never
 * become, the source of a key that protects anything: no cipher in this
 * file is used to encrypt data at rest, and RC4 here exists only
 * because NTLM's session sealing is defined in terms of it. Every
 * function is named for its protocol rather than its primitive, so a
 * call site reads as "the thing SMB needs" rather than "a hash".
 */

/* ===== MD4 (RFC 1320) =====
 *
 * Present for exactly one reason: the NT password hash is MD4 of the
 * password in UTF-16LE, and that definition is frozen into every
 * Windows domain in existence. MD4 has been trivially collidable since
 * 1995 and preimage-weak since 2008; none of that can be fixed here,
 * because the server is checking against the same value.
 */
typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t n;
} md4_ctx;

static uint32_t nt_rol32(uint32_t v, int c) {
    return (v << c) | (v >> (32 - c));
}

static void md4_block(md4_ctx *s, const uint8_t *p) {
    uint32_t x[16];
    for (int i = 0; i < 16; i++)
        x[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];

#define F(x,y,z) (((x) & (y)) | (~(x) & (z)))
#define G(x,y,z) (((x) & (y)) | ((x) & (z)) | ((y) & (z)))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define R1(a,b,c,d,k,sft) a = nt_rol32(a + F(b,c,d) + x[k], sft)
#define R2(a,b,c,d,k,sft) a = nt_rol32(a + G(b,c,d) + x[k] + 0x5A827999u, sft)
#define R3(a,b,c,d,k,sft) a = nt_rol32(a + H(b,c,d) + x[k] + 0x6ED9EBA1u, sft)

    R1(a,b,c,d,0,3);  R1(d,a,b,c,1,7);  R1(c,d,a,b,2,11); R1(b,c,d,a,3,19);
    R1(a,b,c,d,4,3);  R1(d,a,b,c,5,7);  R1(c,d,a,b,6,11); R1(b,c,d,a,7,19);
    R1(a,b,c,d,8,3);  R1(d,a,b,c,9,7);  R1(c,d,a,b,10,11);R1(b,c,d,a,11,19);
    R1(a,b,c,d,12,3); R1(d,a,b,c,13,7); R1(c,d,a,b,14,11);R1(b,c,d,a,15,19);

    R2(a,b,c,d,0,3);  R2(d,a,b,c,4,5);  R2(c,d,a,b,8,9);  R2(b,c,d,a,12,13);
    R2(a,b,c,d,1,3);  R2(d,a,b,c,5,5);  R2(c,d,a,b,9,9);  R2(b,c,d,a,13,13);
    R2(a,b,c,d,2,3);  R2(d,a,b,c,6,5);  R2(c,d,a,b,10,9); R2(b,c,d,a,14,13);
    R2(a,b,c,d,3,3);  R2(d,a,b,c,7,5);  R2(c,d,a,b,11,9); R2(b,c,d,a,15,13);

    R3(a,b,c,d,0,3);  R3(d,a,b,c,8,9);  R3(c,d,a,b,4,11); R3(b,c,d,a,12,15);
    R3(a,b,c,d,2,3);  R3(d,a,b,c,10,9); R3(c,d,a,b,6,11); R3(b,c,d,a,14,15);
    R3(a,b,c,d,1,3);  R3(d,a,b,c,9,9);  R3(c,d,a,b,5,11); R3(b,c,d,a,13,15);
    R3(a,b,c,d,3,3);  R3(d,a,b,c,11,9); R3(c,d,a,b,7,11); R3(b,c,d,a,15,15);

#undef F
#undef G
#undef H
#undef R1
#undef R2
#undef R3

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
}

static void md4_init(md4_ctx *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu; s->h[3] = 0x10325476u;
    s->len = 0; s->n = 0;
}

static void md4_update(md4_ctx *s, const uint8_t *p, uint32_t n) {
    s->len += n;
    while (n) {
        uint32_t take = 64 - s->n;
        if (take > n) take = n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->n + i] = p[i];
        s->n += take; p += take; n -= take;
        if (s->n == 64) { md4_block(s, s->buf); s->n = 0; }
    }
}

static void md4_final(md4_ctx *s, uint8_t out[16]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    md4_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) md4_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (i * 8));
    md4_update(s, lb, 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(s->h[i] >> (j * 8));
}

static void md4(const uint8_t *p, uint32_t n, uint8_t out[16]) {
    md4_ctx s; md4_init(&s); md4_update(&s, p, n); md4_final(&s, out);
}

/* ===== MD5 (RFC 1321) ===== */

typedef struct {
    uint32_t h[4];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t n;
} md5_ctx;

static const uint32_t md5_k[64] = {
    0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu,
    0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
    0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu,
    0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
    0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau,
    0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
    0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu,
    0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
    0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu,
    0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
    0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u,
    0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
    0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u,
    0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
    0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u,
    0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u
};
static const uint8_t md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void md5_block(md5_ctx *s, const uint8_t *p) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++)
        m[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f; int g;
        if (i < 16)      { f = (b & c) | (~b & d);            g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);            g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                     g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);                  g = (7*i) & 15; }
        uint32_t tmp = d; d = c; c = b;
        b = b + nt_rol32(a + f + md5_k[i] + m[g], md5_s[i]);
        a = tmp;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
}

static void md5_init(md5_ctx *s) {
    s->h[0] = 0x67452301u; s->h[1] = 0xefcdab89u;
    s->h[2] = 0x98badcfeu; s->h[3] = 0x10325476u;
    s->len = 0; s->n = 0;
}

static void md5_update(md5_ctx *s, const uint8_t *p, uint32_t n) {
    s->len += n;
    while (n) {
        uint32_t take = 64 - s->n;
        if (take > n) take = n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->n + i] = p[i];
        s->n += take; p += take; n -= take;
        if (s->n == 64) { md5_block(s, s->buf); s->n = 0; }
    }
}

static void md5_final(md5_ctx *s, uint8_t out[16]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    md5_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) md5_update(s, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (i * 8));
    md5_update(s, lb, 8);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(s->h[i] >> (j * 8));
}

static void md5(const uint8_t *p, uint32_t n, uint8_t out[16]) {
    md5_ctx s; md5_init(&s); md5_update(&s, p, n); md5_final(&s, out);
}

/* ===== HMAC-MD5 (RFC 2104) =====
 *
 * The whole of NTLMv2's response construction, twice over. */
static void hmac_md5(const uint8_t *key, uint32_t klen,
                     const uint8_t *msg, uint32_t mlen, uint8_t out[16]) {
    uint8_t k[64], ipad[64], opad[64], inner[16];
    for (int i = 0; i < 64; i++) k[i] = 0;

    if (klen > 64) md5(key, klen, k);
    else for (uint32_t i = 0; i < klen; i++) k[i] = key[i];

    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }

    md5_ctx s;
    md5_init(&s); md5_update(&s, ipad, 64); md5_update(&s, msg, mlen);
    md5_final(&s, inner);
    md5_init(&s); md5_update(&s, opad, 64); md5_update(&s, inner, 16);
    md5_final(&s, out);
}

/* ===== SHA-1 (FIPS 180-4) =====
 *
 * Collidable since 2017 and still load-bearing in SMB2's signing and in
 * Kerberos's older encryption types. Here for compatibility and for
 * nothing else. */
typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t  buf[64];
    uint32_t n;
} sha1_ctx;

static void sha1_block(sha1_ctx *s, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 80; i++)
        w[i] = nt_rol32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);      k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;               k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b&c) | (b&d) | (c&d);   k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;               k = 0xCA62C1D6u; }
        uint32_t t = nt_rol32(a, 5) + f + e + k + w[i];
        e = d; d = c; c = nt_rol32(b, 30); b = a; a = t;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d; s->h[4]+=e;
}

static void sha1_init(sha1_ctx *s) {
    s->h[0]=0x67452301u; s->h[1]=0xEFCDAB89u; s->h[2]=0x98BADCFEu;
    s->h[3]=0x10325476u; s->h[4]=0xC3D2E1F0u;
    s->len = 0; s->n = 0;
}

static void sha1_update(sha1_ctx *s, const uint8_t *p, uint32_t n) {
    s->len += n;
    while (n) {
        uint32_t take = 64 - s->n;
        if (take > n) take = n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->n + i] = p[i];
        s->n += take; p += take; n -= take;
        if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
    }
}

static void sha1_final(sha1_ctx *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    sha1_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->n != 56) sha1_update(s, &z, 1);
    uint8_t lb[8];
    /* Big-endian length: SHA-1 is the other way round from MD4 and MD5,
     * and mixing the two is the classic way to produce a hash that is
     * self-consistent and matches nobody. */
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - i * 8));
    sha1_update(s, lb, 8);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[i * 4 + j] = (uint8_t)(s->h[i] >> (24 - j * 8));
}

static void sha1(const uint8_t *p, uint32_t n, uint8_t out[20]) {
    sha1_ctx s; sha1_init(&s); sha1_update(&s, p, n); sha1_final(&s, out);
}

/* ===== RC4 =====
 *
 * Only for NTLM session sealing, which defines itself in terms of it.
 * Not offered to anything else, and not to be: the first bytes of an
 * RC4 keystream are measurably biased, which is what broke it in TLS.
 */
typedef struct { uint8_t s[256]; uint8_t i, j; } rc4_ctx;

static void rc4_init(rc4_ctx *c, const uint8_t *key, uint32_t klen) {
    for (int i = 0; i < 256; i++) c->s[i] = (uint8_t)i;
    uint8_t j = 0;
    for (int i = 0; i < 256; i++) {
        j = (uint8_t)(j + c->s[i] + key[i % klen]);
        uint8_t t = c->s[i]; c->s[i] = c->s[j]; c->s[j] = t;
    }
    c->i = c->j = 0;
}

static void rc4_apply(rc4_ctx *c, const uint8_t *in, uint8_t *out, uint32_t n) {
    for (uint32_t k = 0; k < n; k++) {
        c->i = (uint8_t)(c->i + 1);
        c->j = (uint8_t)(c->j + c->s[c->i]);
        uint8_t t = c->s[c->i]; c->s[c->i] = c->s[c->j]; c->s[c->j] = t;
        out[k] = in[k] ^ c->s[(uint8_t)(c->s[c->i] + c->s[c->j])];
    }
}

/* ===== NTLM =====
 *
 * The two derivations every Windows authentication starts from.
 *
 * The NT hash is MD4 of the password in UTF-16LE -- unsalted, and
 * therefore equal for two people who chose the same password, which is
 * why it is worth as much as the password itself and is treated here
 * as a secret rather than as a hash.
 */
static uint32_t nt_utf16le(const char *s, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    while (*s && n + 2 <= max) {
        out[n++] = (uint8_t)*s++;
        out[n++] = 0;
    }
    return n;
}

static void ntlm_nt_hash(const char *password, uint8_t out[16]) {
    uint8_t wide[512];
    uint32_t n = nt_utf16le(password, wide, sizeof wide);
    md4(wide, n, out);
}

/*
 * NTLMv2's key: HMAC-MD5 of the uppercased user name concatenated with
 * the domain, keyed by the NT hash.
 *
 * The user name is uppercased and the domain is not. That asymmetry
 * looks like a bug and is not -- it is what the protocol specifies, and
 * a client that uppercases both authenticates against nothing.
 */
static void ntlm_v2_hash(const char *user, const char *domain,
                         const uint8_t nt_hash[16], uint8_t out[16]) {
    uint8_t buf[512];
    uint32_t n = 0;
    for (const char *p = user; *p && n + 2 <= sizeof buf; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        buf[n++] = (uint8_t)c;
        buf[n++] = 0;
    }
    for (const char *p = domain; *p && n + 2 <= sizeof buf; p++) {
        buf[n++] = (uint8_t)*p;
        buf[n++] = 0;
    }
    hmac_md5(nt_hash, 16, buf, n, out);
}

#endif /* NTCRYPTO_H */
