#ifndef SHA256_H
#define SHA256_H

/*
 * SHA-256, integer only.
 *
 * Passwords were stored and compared as plaintext, which meant anything
 * that could read the volume could read the keycode — the Files app and
 * `cat` included. This is what replaces that.
 *
 * Pure 32-bit integer arithmetic with no tables beyond the round
 * constants, so it compiles under the kernel's -mno-sse -mno-80387 and
 * the aarch64 tree's -mgeneral-regs-only without being an exception to
 * either.
 *
 * Worth being plain about what this does and does not buy. It stops a
 * stored password from being *readable*, and salting stops one leaked
 * hash from being reused against the other accounts on the volume. It is
 * not a defence against someone who has the disk and is willing to spend
 * time on it: there is no disk encryption here, and a `.bsd` application
 * runs with kernel privileges in a shared address space, so anything
 * already executing can read whatever it likes. The login screen is a
 * door, not a vault.
 */

typedef struct {
    uint32_t h[8];
    uint64_t len;          /* message length in bytes */
    uint8_t  buf[64];
    uint32_t buflen;
} sha256_t;

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t sha256_ror(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

static void sha256_block(sha256_t *s, const uint8_t *p) {
    uint32_t w[64];

    /* Big-endian, assembled a byte at a time: the input may sit at any
     * alignment and this kernel does not do unaligned word loads. */
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];

    for (int i = 16; i < 64; i++) {
        uint32_t s0 = sha256_ror(w[i - 15], 7) ^ sha256_ror(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = sha256_ror(w[i - 2], 17) ^ sha256_ror(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], hh = s->h[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = sha256_ror(e, 6) ^ sha256_ror(e, 11) ^ sha256_ror(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = hh + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = sha256_ror(a, 2) ^ sha256_ror(a, 13) ^ sha256_ror(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += hh;
}

static void sha256_init(sha256_t *s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u;
    s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu;
    s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->len = 0;
    s->buflen = 0;
}

static void sha256_update(sha256_t *s, const void *data, uint32_t n) {
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n > 0) {
        uint32_t take = 64 - s->buflen;
        if (take > n) take = n;
        for (uint32_t i = 0; i < take; i++) s->buf[s->buflen + i] = p[i];
        s->buflen += take;
        p += take;
        n -= take;
        if (s->buflen == 64) {
            sha256_block(s, s->buf);
            s->buflen = 0;
        }
    }
}

static void sha256_final(sha256_t *s, uint8_t out[32]) {
    uint64_t bits = s->len * 8;

    s->buf[s->buflen++] = 0x80;
    if (s->buflen > 56) {
        while (s->buflen < 64) s->buf[s->buflen++] = 0;
        sha256_block(s, s->buf);
        s->buflen = 0;
    }
    while (s->buflen < 56) s->buf[s->buflen++] = 0;
    for (int i = 7; i >= 0; i--)
        s->buf[s->buflen++] = (uint8_t)(bits >> (i * 8));
    sha256_block(s, s->buf);

    for (int i = 0; i < 8; i++) {
        out[i * 4]     = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(s->h[i]);
    }
}

/* The one-shot form. Nothing in the kernel needs it yet -- passwords go
 * through pw_hash below -- but it is what the host-side tests check the
 * implementation against, so it stays. */
__attribute__((unused))
static void sha256(const void *data, uint32_t n, uint8_t out[32]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, data, n);
    sha256_final(&s, out);
}

/*
 * Iterated salted hash.
 *
 * A single pass over a short password is quick enough to be worth
 * grinding through offline. Repeating it makes each guess cost the same
 * multiple, which is the whole benefit; 4096 keeps a login under a
 * millisecond on real hardware and imperceptible even under emulation.
 */
#define PW_ITERS 4096

static void pw_hash(const uint8_t *salt, uint32_t saltlen,
                    const char *pw, uint32_t pwlen, uint8_t out[32]) {
    sha256_t s;
    sha256_init(&s);
    sha256_update(&s, salt, saltlen);
    sha256_update(&s, pw, pwlen);
    sha256_final(&s, out);

    for (uint32_t i = 1; i < PW_ITERS; i++) {
        uint8_t tmp[32];
        sha256_init(&s);
        sha256_update(&s, out, 32);
        sha256_update(&s, salt, saltlen);
        sha256_final(&s, tmp);
        for (int k = 0; k < 32; k++) out[k] = tmp[k];
    }
}

/*
 * Compare without leaking where the mismatch was.
 *
 * The old password check returned at the first differing byte, so how
 * long it took depended on how much of the guess was right. Accumulating
 * the difference takes the same time whatever the input.
 */
static int ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

#endif /* SHA256_H */
