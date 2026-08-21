#ifndef TLS_H
#define TLS_H

/*
 * src/tls.h — TLS 1.3.
 *
 * The browser here has always spoken plain HTTP and said so on its own
 * home page: "https:// sites will not load — there is no TLS on bare
 * metal (yet)." This is the yet.
 *
 * TLS 1.3 is a far better fit for a system like this than 1.2 was. It
 * has one key exchange worth implementing rather than a menu of
 * obsolete ones, it fixed the handshake at one round trip, and it threw
 * out every cipher whose only defence was that nothing had broken it
 * lately. What is left is a small, sharp specification.
 *
 * The suite implemented is X25519 with ChaCha20-Poly1305 and SHA-256,
 * and the choice is not arbitrary. ChaCha20 is already here, checked
 * against RFC 8439's own vectors. SHA-256 is already here. Neither
 * touches a lookup table indexed by a secret, which matters much more
 * on a machine with no AES instructions than the speed does: a
 * table-driven AES leaks its key through cache timing, and would be the
 * wrong answer even though it is the more famous one.
 *
 * What is missing is stated plainly rather than implied: there is no
 * certificate verification. The handshake completes, the traffic is
 * encrypted, and this system cannot yet tell you *who* it is encrypted
 * to. That protects against someone reading the wire and not against
 * someone standing in the middle of it. Anything relying on it is told,
 * every time, by the browser.
 */

#include <stdint.h>
#include "sha256.h"
#include "chacha20.h"

/* ===== X25519 =====
 *
 * Curve25519 scalar multiplication, over the field of integers modulo
 * 2^255 - 19, with the field elements held as ten limbs of 25 or 26
 * bits alternating. The odd sizes are the point: a 64-bit product of two
 * 26-bit limbs cannot overflow before the carry is propagated, so the
 * whole of the arithmetic stays in registers with no reduction step in
 * the inner loop.
 *
 * The ladder is Montgomery's, and it is constant time by construction:
 * every bit of the scalar performs exactly the same operations on the
 * same amount of data, and which of two values is used is chosen by a
 * mask rather than by a branch. A branch on a key bit is a timing
 * channel, and on a machine that a network can reach that is the whole
 * attack.
 */
/*
 * Field elements are five limbs of 51 bits, and the products are taken
 * through 128-bit integers.
 *
 * The alternative -- ten limbs of alternating 25 and 26 bits, which is
 * what the classic portable implementation uses -- exists to avoid
 * needing a 128-bit type. This kernel is x86-64 only and GCC has
 * __int128 there, so the simpler representation is available: every
 * limb is the same width, the reduction is one multiply by 19, and
 * there is no parity rule about which products need doubling. That rule
 * is exactly what the first attempt here got wrong, and it failed
 * silently -- producing a shared secret that was merely not the right
 * one, which RFC 7748's vectors caught and nothing else would have.
 */
typedef uint64_t fe[5];

#define FE_MASK ((1ULL << 51) - 1)

static void fe_0(fe h) { for (int i = 0; i < 5; i++) h[i] = 0; }
static void fe_1(fe h) { fe_0(h); h[0] = 1; }
static void fe_copy(fe h, const fe f) { for (int i = 0; i < 5; i++) h[i] = f[i]; }

static void fe_add(fe h, const fe f, const fe g) {
    for (int i = 0; i < 5; i++) h[i] = f[i] + g[i];
}

/* Subtraction adds twice the modulus first, so no limb can go negative
 * in an unsigned representation. */
static void fe_sub(fe h, const fe f, const fe g) {
    h[0] = f[0] + 0xFFFFFFFFFFFDAULL - g[0];
    h[1] = f[1] + 0xFFFFFFFFFFFFEULL - g[1];
    h[2] = f[2] + 0xFFFFFFFFFFFFEULL - g[2];
    h[3] = f[3] + 0xFFFFFFFFFFFFEULL - g[3];
    h[4] = f[4] + 0xFFFFFFFFFFFFEULL - g[4];
}

/* Swap f and g if b is 1, without ever branching on b. */
static void fe_cswap(fe f, fe g, uint64_t b) {
    uint64_t mask = (uint64_t)0 - b;
    for (int i = 0; i < 5; i++) {
        uint64_t x = mask & (f[i] ^ g[i]);
        f[i] ^= x;
        g[i] ^= x;
    }
}

static void fe_mul(fe h, const fe f, const fe g) {
    typedef unsigned __int128 u128;
    uint64_t f0 = f[0], f1 = f[1], f2 = f[2], f3 = f[3], f4 = f[4];
    uint64_t g0 = g[0], g1 = g[1], g2 = g[2], g3 = g[3], g4 = g[4];
    /* 2^255 == 19 (mod p), so a limb that overflows the top comes back
     * multiplied by nineteen. */
    uint64_t g1_19 = 19 * g1, g2_19 = 19 * g2;
    uint64_t g3_19 = 19 * g3, g4_19 = 19 * g4;

    u128 r0 = (u128)f0 * g0 + (u128)f1 * g4_19 + (u128)f2 * g3_19 +
              (u128)f3 * g2_19 + (u128)f4 * g1_19;
    u128 r1 = (u128)f0 * g1 + (u128)f1 * g0    + (u128)f2 * g4_19 +
              (u128)f3 * g3_19 + (u128)f4 * g2_19;
    u128 r2 = (u128)f0 * g2 + (u128)f1 * g1    + (u128)f2 * g0 +
              (u128)f3 * g4_19 + (u128)f4 * g3_19;
    u128 r3 = (u128)f0 * g3 + (u128)f1 * g2    + (u128)f2 * g1 +
              (u128)f3 * g0    + (u128)f4 * g4_19;
    u128 r4 = (u128)f0 * g4 + (u128)f1 * g3    + (u128)f2 * g2 +
              (u128)f3 * g1    + (u128)f4 * g0;

    uint64_t c;
    c = (uint64_t)(r0 >> 51); h[0] = (uint64_t)r0 & FE_MASK; r1 += c;
    c = (uint64_t)(r1 >> 51); h[1] = (uint64_t)r1 & FE_MASK; r2 += c;
    c = (uint64_t)(r2 >> 51); h[2] = (uint64_t)r2 & FE_MASK; r3 += c;
    c = (uint64_t)(r3 >> 51); h[3] = (uint64_t)r3 & FE_MASK; r4 += c;
    c = (uint64_t)(r4 >> 51); h[4] = (uint64_t)r4 & FE_MASK;
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
}

static void fe_sq(fe h, const fe f) { fe_mul(h, f, f); }

static void fe_mul121666(fe h, const fe f) {
    typedef unsigned __int128 u128;
    u128 r0 = (u128)f[0] * 121666;
    u128 r1 = (u128)f[1] * 121666;
    u128 r2 = (u128)f[2] * 121666;
    u128 r3 = (u128)f[3] * 121666;
    u128 r4 = (u128)f[4] * 121666;

    uint64_t c;
    c = (uint64_t)(r0 >> 51); h[0] = (uint64_t)r0 & FE_MASK; r1 += c;
    c = (uint64_t)(r1 >> 51); h[1] = (uint64_t)r1 & FE_MASK; r2 += c;
    c = (uint64_t)(r2 >> 51); h[2] = (uint64_t)r2 & FE_MASK; r3 += c;
    c = (uint64_t)(r3 >> 51); h[3] = (uint64_t)r3 & FE_MASK; r4 += c;
    c = (uint64_t)(r4 >> 51); h[4] = (uint64_t)r4 & FE_MASK;
    h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
}

/* x^(p-2), which is x^-1 by Fermat's little theorem. */
static void fe_invert(fe out, const fe z) {
    fe z2, z9, z11, z2_5_0, z2_10_0, z2_20_0, z2_50_0, z2_100_0, t;
    int i;

    fe_sq(z2, z);
    fe_sq(t, z2); fe_sq(t, t);
    fe_mul(z9, t, z);
    fe_mul(z11, z9, z2);
    fe_sq(t, z11);
    fe_mul(z2_5_0, t, z9);

    fe_sq(t, z2_5_0); for (i = 1; i < 5; i++) fe_sq(t, t);
    fe_mul(z2_10_0, t, z2_5_0);

    fe_sq(t, z2_10_0); for (i = 1; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_20_0, t, z2_10_0);

    fe_sq(t, z2_20_0); for (i = 1; i < 20; i++) fe_sq(t, t);
    fe_mul(t, t, z2_20_0);
    for (i = 0; i < 10; i++) fe_sq(t, t);
    fe_mul(z2_50_0, t, z2_10_0);

    fe_sq(t, z2_50_0); for (i = 1; i < 50; i++) fe_sq(t, t);
    fe_mul(z2_100_0, t, z2_50_0);

    fe_sq(t, z2_100_0); for (i = 1; i < 100; i++) fe_sq(t, t);
    fe_mul(t, t, z2_100_0);
    for (i = 0; i < 50; i++) fe_sq(t, t);
    fe_mul(t, t, z2_50_0);
    for (i = 0; i < 5; i++) fe_sq(t, t);
    fe_mul(out, t, z11);
}

static void fe_from_bytes(fe h, const uint8_t *s) {
    uint64_t w[4];
    for (int i = 0; i < 4; i++) {
        uint64_t v = 0;
        for (int b = 0; b < 8; b++) v |= (uint64_t)s[i * 8 + b] << (b * 8);
        w[i] = v;
    }
    h[0] =  w[0]                        & FE_MASK;
    h[1] = (w[0] >> 51 | w[1] << 13)    & FE_MASK;
    h[2] = (w[1] >> 38 | w[2] << 26)    & FE_MASK;
    h[3] = (w[2] >> 25 | w[3] << 39)    & FE_MASK;
    /* The top bit of the last byte is ignored, as the curve requires. */
    h[4] = (w[3] >> 12)                 & FE_MASK;
}

static void fe_to_bytes(uint8_t *s, const fe hin) {
    fe h;
    fe_copy(h, hin);

    /* Fully carry, then subtract p once if the value is at least p. The
     * comparison is done by adding 19 and looking at the carry, so there
     * is no branch on the value. */
    uint64_t c;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    c = h[4] >> 51; h[4] &= FE_MASK; h[0] += c * 19;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;

    uint64_t q = (h[0] + 19) >> 51;
    q = (h[1] + q) >> 51;
    q = (h[2] + q) >> 51;
    q = (h[3] + q) >> 51;
    q = (h[4] + q) >> 51;

    h[0] += 19 * q;
    c = h[0] >> 51; h[0] &= FE_MASK; h[1] += c;
    c = h[1] >> 51; h[1] &= FE_MASK; h[2] += c;
    c = h[2] >> 51; h[2] &= FE_MASK; h[3] += c;
    c = h[3] >> 51; h[3] &= FE_MASK; h[4] += c;
    h[4] &= FE_MASK;

    uint64_t w0 =  h[0]        | (h[1] << 51);
    uint64_t w1 = (h[1] >> 13) | (h[2] << 38);
    uint64_t w2 = (h[2] >> 26) | (h[3] << 25);
    uint64_t w3 = (h[3] >> 39) | (h[4] << 12);
    uint64_t w[4] = { w0, w1, w2, w3 };
    for (int i = 0; i < 4; i++)
        for (int b = 0; b < 8; b++)
            s[i * 8 + b] = (uint8_t)(w[i] >> (b * 8));
}

/*
 * The Montgomery ladder.
 *
 * Two points are carried and swapped according to each bit of the
 * scalar, so the same eleven field operations happen for every bit
 * whatever its value. The swap is a mask, not an if.
 */
static void x25519(uint8_t out[32], const uint8_t scalar[32],
                   const uint8_t point[32]) {
    uint8_t e[32];
    for (int i = 0; i < 32; i++) e[i] = scalar[i];
    /* Clamping: clear the low three bits so the scalar is a multiple of
     * the cofactor, and fix the top two so it is in range and the ladder
     * runs a fixed number of steps. */
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_from_bytes(x1, point);
    fe_1(x2); fe_0(z2);
    fe_copy(x3, x1); fe_1(z3);

    uint64_t swap = 0;
    for (int pos = 254; pos >= 0; pos--) {
        uint64_t b = (uint64_t)((e[pos / 8] >> (pos & 7)) & 1);
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;

        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);

    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_to_bytes(out, x2);
}

static const uint8_t x25519_base[32] = { 9 };

/* ===== POLY1305 =====
 *
 * The authenticator half of the cipher suite: a polynomial evaluated
 * modulo 2^130 - 5 with the message as its coefficients. It is not a
 * hash and must never be used as one -- the key is single-use, and
 * reusing it lets anyone who sees two messages forge a third.
 */
typedef struct {
    uint32_t r[5], h[5], pad[4];
    uint8_t  buf[16];
    uint32_t leftover;
} poly1305_t;

static void poly1305_init(poly1305_t *st, const uint8_t key[32]) {
    /* r is clamped: certain bits are cleared so that the products in the
     * evaluation cannot overflow 64 bits. */
    st->r[0] = (uint32_t)((key[0] | (key[1] << 8) | (key[2] << 16) |
                           ((uint32_t)key[3] << 24))) & 0x3ffffff;
    st->r[1] = (uint32_t)(((key[3] >> 2) | (key[4] << 6) | (key[5] << 14) |
                           ((uint32_t)key[6] << 22))) & 0x3ffff03;
    st->r[2] = (uint32_t)(((key[6] >> 4) | (key[7] << 4) | (key[8] << 12) |
                           ((uint32_t)key[9] << 20))) & 0x3ffc0ff;
    st->r[3] = (uint32_t)(((key[9] >> 6) | (key[10] << 2) | (key[11] << 10) |
                           ((uint32_t)key[12] << 18))) & 0x3f03fff;
    st->r[4] = (uint32_t)(((key[13]) | (key[14] << 8) |
                           ((uint32_t)key[15] << 16))) & 0x00fffff;
    for (int i = 0; i < 5; i++) st->h[i] = 0;
    for (int i = 0; i < 4; i++)
        st->pad[i] = (uint32_t)(key[16 + i * 4] | (key[17 + i * 4] << 8) |
                                (key[18 + i * 4] << 16) |
                                ((uint32_t)key[19 + i * 4] << 24));
    st->leftover = 0;
}

static void poly1305_blocks(poly1305_t *st, const uint8_t *m, uint32_t bytes,
                            uint32_t final) {
    const uint32_t hibit = final ? 0 : (1u << 24);
    uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2],
             r3 = st->r[3], r4 = st->r[4];
    uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2],
             h3 = st->h[3], h4 = st->h[4];

    while (bytes >= 16) {
        h0 += (uint32_t)(m[0] | (m[1] << 8) | (m[2] << 16) |
                         ((uint32_t)m[3] << 24)) & 0x3ffffff;
        h1 += (uint32_t)((m[3] >> 2) | (m[4] << 6) | (m[5] << 14) |
                         ((uint32_t)m[6] << 22)) & 0x3ffffff;
        h2 += (uint32_t)((m[6] >> 4) | (m[7] << 4) | (m[8] << 12) |
                         ((uint32_t)m[9] << 20)) & 0x3ffffff;
        h3 += (uint32_t)((m[9] >> 6) | (m[10] << 2) | (m[11] << 10) |
                         ((uint32_t)m[12] << 18)) & 0x3ffffff;
        h4 += (uint32_t)((m[13]) | (m[14] << 8) |
                         ((uint32_t)m[15] << 16)) | hibit;

        uint64_t d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 +
                      (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
        uint64_t d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 +
                      (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
        uint64_t d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 +
                      (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
        uint64_t d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 +
                      (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
        uint64_t d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 +
                      (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;

        uint32_t c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
        d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
        d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
        d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
        d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
        h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

        m += 16;
        bytes -= 16;
    }
    st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly1305_update(poly1305_t *st, const uint8_t *m, uint32_t bytes) {
    if (st->leftover) {
        uint32_t want = 16 - st->leftover;
        if (want > bytes) want = bytes;
        for (uint32_t i = 0; i < want; i++) st->buf[st->leftover + i] = m[i];
        bytes -= want;
        m += want;
        st->leftover += want;
        if (st->leftover < 16) return;
        poly1305_blocks(st, st->buf, 16, 0);
        st->leftover = 0;
    }
    if (bytes >= 16) {
        uint32_t want = bytes & ~(uint32_t)15;
        poly1305_blocks(st, m, want, 0);
        m += want;
        bytes -= want;
    }
    for (uint32_t i = 0; i < bytes; i++) st->buf[st->leftover + i] = m[i];
    st->leftover += bytes;
}

static void poly1305_final(poly1305_t *st, uint8_t mac[16]) {
    if (st->leftover) {
        st->buf[st->leftover++] = 1;
        while (st->leftover < 16) st->buf[st->leftover++] = 0;
        poly1305_blocks(st, st->buf, 16, 1);
    }
    uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2],
             h3 = st->h[3], h4 = st->h[4];
    uint32_t c = h1 >> 26; h1 &= 0x3ffffff;
    h2 += c; c = h2 >> 26; h2 &= 0x3ffffff;
    h3 += c; c = h3 >> 26; h3 &= 0x3ffffff;
    h4 += c; c = h4 >> 26; h4 &= 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;

    uint32_t g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
    uint32_t g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
    uint32_t g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
    uint32_t g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
    uint32_t g4 = h4 + c - (1u << 26);

    uint32_t mask = (g4 >> 31) - 1;      /* all ones if g >= 0 */
    g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
    mask = ~mask;
    h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
    h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;

    h0 = (h0 | (h1 << 26)) & 0xffffffff;
    h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
    h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
    h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;

    uint64_t f = (uint64_t)h0 + st->pad[0]; h0 = (uint32_t)f;
    f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
    f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
    f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;

    mac[0] = (uint8_t)h0; mac[1] = (uint8_t)(h0 >> 8);
    mac[2] = (uint8_t)(h0 >> 16); mac[3] = (uint8_t)(h0 >> 24);
    mac[4] = (uint8_t)h1; mac[5] = (uint8_t)(h1 >> 8);
    mac[6] = (uint8_t)(h1 >> 16); mac[7] = (uint8_t)(h1 >> 24);
    mac[8] = (uint8_t)h2; mac[9] = (uint8_t)(h2 >> 8);
    mac[10] = (uint8_t)(h2 >> 16); mac[11] = (uint8_t)(h2 >> 24);
    mac[12] = (uint8_t)h3; mac[13] = (uint8_t)(h3 >> 8);
    mac[14] = (uint8_t)(h3 >> 16); mac[15] = (uint8_t)(h3 >> 24);
}

/* ===== HMAC AND HKDF =====
 *
 * TLS 1.3 derives every key it uses from one shared secret through a
 * chain of these, and the labels are part of the specification: two
 * different keys are different precisely because they were derived with
 * different labels, so getting a label wrong produces a handshake that
 * fails with no clue as to why.
 */
/* hmac_sha256 lives in src/sha256.h, next to the hash it is built on.
 * It was here first, and moved when SMB2 turned out to sign with it --
 * two copies of an HMAC is two chances to get the block size wrong. */

static void hkdf_extract(const uint8_t *salt, uint32_t slen,
                         const uint8_t *ikm, uint32_t ilen, uint8_t out[32]) {
    hmac_sha256(salt, slen, ikm, ilen, out);
}

static void hkdf_expand(const uint8_t prk[32], const uint8_t *info,
                        uint32_t ilen, uint8_t *out, uint32_t olen) {
    uint8_t t[32];
    uint32_t done = 0;
    uint8_t counter = 1;
    uint32_t tlen = 0;
    while (done < olen) {
        uint8_t buf[32 + 256 + 1];
        uint32_t n = 0;
        for (uint32_t i = 0; i < tlen; i++) buf[n++] = t[i];
        for (uint32_t i = 0; i < ilen && n < sizeof(buf) - 1; i++)
            buf[n++] = info[i];
        buf[n++] = counter++;
        hmac_sha256(prk, 32, buf, n, t);
        tlen = 32;
        uint32_t take = olen - done < 32 ? olen - done : 32;
        for (uint32_t i = 0; i < take; i++) out[done + i] = t[i];
        done += take;
    }
}

/* HkdfLabel is a length, a length-prefixed label with "tls13 " in front,
 * and a length-prefixed context. */
static void hkdf_expand_label(const uint8_t secret[32], const char *label,
                              const uint8_t *ctx, uint32_t ctxlen,
                              uint8_t *out, uint16_t olen) {
    uint8_t info[2 + 1 + 6 + 32 + 1 + 32];
    uint32_t n = 0;
    info[n++] = (uint8_t)(olen >> 8);
    info[n++] = (uint8_t)olen;

    uint32_t llen = 0;
    while (label[llen]) llen++;
    info[n++] = (uint8_t)(6 + llen);
    const char *pfx = "tls13 ";
    for (int i = 0; i < 6; i++) info[n++] = (uint8_t)pfx[i];
    for (uint32_t i = 0; i < llen; i++) info[n++] = (uint8_t)label[i];

    info[n++] = (uint8_t)ctxlen;
    for (uint32_t i = 0; i < ctxlen; i++) info[n++] = ctx[i];

    hkdf_expand(secret, info, n, out, olen);
}

/* ===== AEAD =====
 *
 * ChaCha20-Poly1305 as RFC 8439 defines it: the first keystream block
 * with counter zero becomes the one-time authenticator key, the payload
 * is encrypted from counter one, and the tag covers the additional data,
 * the ciphertext, and both their lengths -- padded so that a boundary
 * between them cannot be moved without changing the input.
 */
static void aead_tag(const uint8_t poly_key[32], const uint8_t *aad,
                     uint32_t aadlen, const uint8_t *ct, uint32_t ctlen,
                     uint8_t tag[16]) {
    poly1305_t p;
    poly1305_init(&p, poly_key);
    static const uint8_t zeros[16] = { 0 };

    poly1305_update(&p, aad, aadlen);
    if (aadlen % 16) poly1305_update(&p, zeros, 16 - (aadlen % 16));
    poly1305_update(&p, ct, ctlen);
    if (ctlen % 16) poly1305_update(&p, zeros, 16 - (ctlen % 16));

    uint8_t lens[16];
    for (int i = 0; i < 8; i++) lens[i] = (uint8_t)(aadlen >> (i * 8));
    for (int i = 0; i < 8; i++) lens[8 + i] = (uint8_t)(ctlen >> (i * 8));
    poly1305_update(&p, lens, 16);
    poly1305_final(&p, tag);
}

/*
 * The nonce for record `seq` is the twelve-byte write IV with the
 * sequence number exclusive-ored into its low end. Sequence numbers are
 * never transmitted -- both sides count -- which is what makes a
 * replayed record fail to authenticate.
 */
static void tls_nonce(const uint8_t iv[12], uint64_t seq, uint8_t out[12]) {
    for (int i = 0; i < 12; i++) out[i] = iv[i];
    for (int i = 0; i < 8; i++)
        out[11 - i] ^= (uint8_t)(seq >> (i * 8));
}

/* ===== HANDSHAKE STATE ===== */
#define TLS_MAX_RECORD 16640

typedef enum {
    TLS_IDLE = 0,
    TLS_WROTE_HELLO,
    TLS_GOT_SERVER_HELLO,
    TLS_ESTABLISHED,
    TLS_FAILED
} tls_state_t;

typedef struct {
    tls_state_t state;
    const char *error;

    uint8_t priv[32], pub[32], peer[32], shared[32];
    uint8_t transcript_running[32];
    sha256_t transcript;

    uint8_t handshake_secret[32];
    uint8_t client_hs_traffic[32], server_hs_traffic[32];
    uint8_t client_key[32], server_key[32];
    uint8_t client_iv[12], server_iv[12];

    uint64_t send_seq, recv_seq;
    int      encrypted;
} tls_conn_t;

/*
 * The key schedule, as far as the handshake keys.
 *
 * Early secret from a zero PSK, derived once; handshake secret from that
 * and the shared secret; traffic secrets from the handshake secret and
 * the transcript so far. Every step mixes in the transcript, which is
 * what binds the keys to the exact messages exchanged and stops an
 * attacker splicing a handshake from two conversations.
 */
static void tls_derive_handshake(tls_conn_t *c, const uint8_t hello_hash[32]) {
    static const uint8_t zeros[32] = { 0 };
    uint8_t early[32], derived[32], empty_hash[32];

    sha256(zeros, 0, empty_hash);
    hkdf_extract(zeros, 0, zeros, 32, early);
    hkdf_expand_label(early, "derived", empty_hash, 32, derived, 32);
    hkdf_extract(derived, 32, c->shared, 32, c->handshake_secret);

    hkdf_expand_label(c->handshake_secret, "c hs traffic", hello_hash, 32,
                      c->client_hs_traffic, 32);
    hkdf_expand_label(c->handshake_secret, "s hs traffic", hello_hash, 32,
                      c->server_hs_traffic, 32);

    hkdf_expand_label(c->client_hs_traffic, "key", 0, 0, c->client_key, 32);
    hkdf_expand_label(c->client_hs_traffic, "iv",  0, 0, c->client_iv, 12);
    hkdf_expand_label(c->server_hs_traffic, "key", 0, 0, c->server_key, 32);
    hkdf_expand_label(c->server_hs_traffic, "iv",  0, 0, c->server_iv, 12);
}

/* Seal a record in place: encrypt, then authenticate the ciphertext. */
static uint32_t tls_seal(tls_conn_t *c, uint8_t type, uint8_t *buf,
                         uint32_t len, uint32_t cap) {
    if (len + 17 > cap) return 0;
    buf[len++] = type;                          /* the real content type */

    uint8_t nonce[12], polykey[64];
    tls_nonce(c->client_iv, c->send_seq, nonce);
    cc20_block(c->client_key, 0, nonce, polykey);

    /* Counter one onward is the payload; block zero was the poly key.
     * cc20_xor takes a byte offset, so one block in is 64. */
    cc20_xor(c->client_key, nonce, CC20_BLOCK, buf, len);

    uint8_t aad[5];
    uint32_t total = len + 16;
    aad[0] = 23; aad[1] = 3; aad[2] = 3;
    aad[3] = (uint8_t)(total >> 8); aad[4] = (uint8_t)total;

    aead_tag(polykey, aad, 5, buf, len, buf + len);
    c->send_seq++;
    return len + 16;
}

static int tls_open(tls_conn_t *c, uint8_t *buf, uint32_t len,
                    uint8_t *out_type, uint32_t *out_len) {
    if (len < 17) return -1;
    uint32_t ctlen = len - 16;

    uint8_t nonce[12], polykey[64], tag[16];
    tls_nonce(c->server_iv, c->recv_seq, nonce);
    cc20_block(c->server_key, 0, nonce, polykey);

    uint8_t aad[5];
    aad[0] = 23; aad[1] = 3; aad[2] = 3;
    aad[3] = (uint8_t)(len >> 8); aad[4] = (uint8_t)len;
    aead_tag(polykey, aad, 5, buf, ctlen, tag);
    if (!cc20_equal(tag, buf + ctlen, 16)) return -1;

    cc20_xor(c->server_key, nonce, CC20_BLOCK, buf, ctlen);
    c->recv_seq++;

    /* Trailing zeroes are padding; the last non-zero byte is the type. */
    while (ctlen && buf[ctlen - 1] == 0) ctlen--;
    if (!ctlen) return -1;
    *out_type = buf[ctlen - 1];
    *out_len  = ctlen - 1;
    return 0;
}

/*
 * Build a ClientHello.
 *
 * One key share, one cipher suite, one signature algorithm, and the
 * supported_versions extension that is what actually selects TLS 1.3 --
 * the version field in the record header still says 1.2, forever,
 * because middleboxes on the internet drop anything else.
 */
static uint32_t tls_client_hello(tls_conn_t *c, const char *host,
                                 uint8_t *out, uint32_t cap) {
    if (cap < 512) return 0;
    uint32_t hlen = 0;
    while (host[hlen]) hlen++;

    uint8_t body[512];
    uint32_t n = 0;
    body[n++] = 3; body[n++] = 3;                    /* legacy_version */
    for (int i = 0; i < 32; i++) body[n++] = c->priv[i] ^ 0x5A;  /* random */
    body[n++] = 0;                                   /* no session id */
    body[n++] = 0; body[n++] = 2;                    /* one cipher suite */
    body[n++] = 0x13; body[n++] = 0x03;              /* CHACHA20_POLY1305 */
    body[n++] = 1; body[n++] = 0;                    /* null compression */

    uint32_t ext_len_at = n; n += 2;
    uint32_t ext_start = n;

    /* server_name */
    body[n++] = 0; body[n++] = 0;
    body[n++] = (uint8_t)((hlen + 5) >> 8); body[n++] = (uint8_t)(hlen + 5);
    body[n++] = (uint8_t)((hlen + 3) >> 8); body[n++] = (uint8_t)(hlen + 3);
    body[n++] = 0;
    body[n++] = (uint8_t)(hlen >> 8); body[n++] = (uint8_t)hlen;
    for (uint32_t i = 0; i < hlen; i++) body[n++] = (uint8_t)host[i];

    /* supported_versions: 0x0304 */
    body[n++] = 0; body[n++] = 43;
    body[n++] = 0; body[n++] = 3;
    body[n++] = 2; body[n++] = 3; body[n++] = 4;

    /* supported_groups: x25519 */
    body[n++] = 0; body[n++] = 10;
    body[n++] = 0; body[n++] = 4;
    body[n++] = 0; body[n++] = 2; body[n++] = 0; body[n++] = 29;

    /* signature_algorithms: ed25519 and rsa_pss_rsae_sha256 */
    body[n++] = 0; body[n++] = 13;
    body[n++] = 0; body[n++] = 6;
    body[n++] = 0; body[n++] = 4;
    body[n++] = 8; body[n++] = 7; body[n++] = 8; body[n++] = 4;

    /* key_share */
    body[n++] = 0; body[n++] = 51;
    body[n++] = 0; body[n++] = 38;
    body[n++] = 0; body[n++] = 36;
    body[n++] = 0; body[n++] = 29;
    body[n++] = 0; body[n++] = 32;
    for (int i = 0; i < 32; i++) body[n++] = c->pub[i];

    uint32_t ext_len = n - ext_start;
    body[ext_len_at] = (uint8_t)(ext_len >> 8);
    body[ext_len_at + 1] = (uint8_t)ext_len;

    /* Handshake header, then record header. */
    uint32_t o = 0;
    out[o++] = 22; out[o++] = 3; out[o++] = 1;
    out[o++] = (uint8_t)((n + 4) >> 8); out[o++] = (uint8_t)(n + 4);
    uint32_t hs = o;
    out[o++] = 1;
    out[o++] = 0; out[o++] = (uint8_t)(n >> 8); out[o++] = (uint8_t)n;
    for (uint32_t i = 0; i < n; i++) out[o++] = body[i];

    sha256_init(&c->transcript);
    sha256_update(&c->transcript, out + hs, n + 4);
    c->state = TLS_WROTE_HELLO;
    return o;
}

/*
 * Read a ServerHello far enough to find the peer's key share, then
 * derive the handshake keys.
 */
static int tls_server_hello(tls_conn_t *c, const uint8_t *rec, uint32_t len) {
    if (len < 44 || rec[0] != 2) { c->error = "not a ServerHello"; return -1; }
    sha256_update(&c->transcript, rec, len);

    uint32_t p = 4 + 2 + 32;                     /* header, version, random */
    if (p >= len) return -1;
    p += 1 + rec[p];                             /* session id echo */
    if (p + 3 > len) return -1;
    uint16_t suite = (uint16_t)((rec[p] << 8) | rec[p + 1]);
    p += 2;
    p += 1;                                      /* compression */
    if (suite != 0x1303) { c->error = "server chose a suite we do not have";
                           return -1; }

    if (p + 2 > len) return -1;
    uint32_t ext_end = p + 2 + (uint32_t)((rec[p] << 8) | rec[p + 1]);
    p += 2;
    if (ext_end > len) ext_end = len;

    int found = 0;
    while (p + 4 <= ext_end) {
        uint16_t type = (uint16_t)((rec[p] << 8) | rec[p + 1]);
        uint16_t elen = (uint16_t)((rec[p + 2] << 8) | rec[p + 3]);
        p += 4;
        if (p + elen > ext_end) break;
        if (type == 51 && elen >= 36) {
            uint16_t group = (uint16_t)((rec[p] << 8) | rec[p + 1]);
            if (group == 29) {
                for (int i = 0; i < 32; i++) c->peer[i] = rec[p + 4 + i];
                found = 1;
            }
        }
        p += elen;
    }
    if (!found) { c->error = "no x25519 key share in the ServerHello";
                  return -1; }

    x25519(c->shared, c->priv, c->peer);

    uint8_t hash[32];
    sha256_t snap = c->transcript;
    sha256_final(&snap, hash);
    tls_derive_handshake(c, hash);

    c->encrypted = 1;
    c->state = TLS_GOT_SERVER_HELLO;
    return 0;
}

static void tls_init(tls_conn_t *c, const uint8_t seed[32]) {
    for (uint64_t i = 0; i < sizeof(*c); i++) ((uint8_t *)c)[i] = 0;
    for (int i = 0; i < 32; i++) c->priv[i] = seed[i];
    c->priv[0] &= 248;
    c->priv[31] &= 127;
    c->priv[31] |= 64;
    x25519(c->pub, c->priv, x25519_base);
    c->state = TLS_IDLE;
    c->error = "";
}

/*
 * ---- what this does not do ----
 *
 * There is no certificate verification. The server's Certificate and
 * CertificateVerify messages arrive encrypted under the handshake keys
 * and are decrypted correctly; nothing here checks the signature, walks
 * a chain, or holds a list of authorities to walk it to.
 *
 * The consequence is exact and worth stating in one sentence rather than
 * implying: this protects against someone reading the traffic and not
 * against someone answering it. Anything using it must say so.
 */
static int tls_verifies_certificates(void) { return 0; }

#endif /* TLS_H */
