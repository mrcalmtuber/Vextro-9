#ifndef AES_H
#define AES_H

#include <stdint.h>

/*
 * src/aes.h — AES, on the instruction the processor already has.
 *
 * This exists because Kerberos and SMB need it. Not because AES is the
 * best cipher available here: src/tls.h says outright that ChaCha20 was
 * chosen over AES for the record layer, and the reason it gives still
 * holds --
 *
 *     "Neither touches a lookup table indexed by a secret, which
 *      matters much more on a machine with no AES instructions than
 *      the speed does: a table-driven AES leaks its key through cache
 *      timing, and would be the wrong answer even though it is the
 *      more famous one."
 *
 * The operative clause is *on a machine with no AES instructions*.
 * Kerberos does not offer a choice of cipher the way TLS does -- a
 * domain controller speaks aes256-cts-hmac-sha1-96 or it speaks RC4,
 * and there is no third option to prefer. So the cipher is not
 * negotiable and the implementation is where the argument has to be
 * won.
 *
 * ---- two implementations, and why both are here ----
 *
 * AESENC has been in x86 processors since 2010. It performs a whole
 * round -- SubBytes, ShiftRows, MixColumns, AddRoundKey -- in one
 * instruction, from registers, with no table in memory at all. There is
 * no cache line whose eviction depends on the key, because there is no
 * cache line. It is constant time by construction rather than by
 * effort, which is a much better guarantee than a careful software
 * implementation can offer.
 *
 * So: AESENC when CPUID says the processor has it, and a portable
 * implementation when it does not. The portable one is honestly
 * labelled -- it indexes a 256-byte table with key-dependent values and
 * therefore has the timing channel the comment above describes. It is
 * the fallback, not the plan, and on anything this decade it does not
 * run.
 *
 * The two paths are checked against each other. That matters more than
 * it sounds: a fallback nothing exercises is a fallback that has never
 * worked, and every machine this is developed on has AES-NI. So
 * tools/aes_test.c runs the portable path against FIPS-197 on the host,
 * and the kernel's selftest runs *both* and requires that they agree
 * byte for byte on every vector.
 *
 * ---- the S-box is derived, not transcribed ----
 *
 * aes_init() computes the S-box from its algebraic definition: the
 * multiplicative inverse in GF(2^8), then the affine transform. It is
 * about fifteen lines and it runs once.
 *
 * The alternative is 256 hexadecimal bytes typed into a header, and
 * this repository has already paid for that kind of transcription once
 * -- a test vector written from memory rather than from the document,
 * where the implementation was right and the expectation was wrong.
 * A derived table cannot be mistyped. It can only be wrong in a way
 * FIPS-197's own vectors catch immediately.
 */

/* ===== GF(2^8), the field AES is defined over ===== */

/* Multiplication by x, modulo x^8 + x^4 + x^3 + x + 1. */
static uint8_t aes_xtime(uint8_t a) {
    return (uint8_t)((a << 1) ^ ((a & 0x80) ? 0x1B : 0x00));
}

static uint8_t aes_mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = aes_xtime(a);
        b >>= 1;
    }
    return r;
}

static uint8_t aes_sbox[256];
static uint8_t aes_inv_sbox[256];
static int     aes_tables_ready = 0;

static uint8_t aes_rotl8(uint8_t x, int n) {
    return (uint8_t)((x << n) | (x >> (8 - n)));
}

/*
 * The S-box: b -> affine(inverse(b)).
 *
 * The inverse is found by search rather than by a log table. 65,536
 * multiplications, once, at boot -- microseconds, and no second table
 * to get wrong.
 */
static void aes_init(void) {
    if (aes_tables_ready) return;

    uint8_t inv[256];
    inv[0] = 0;                        /* zero has no inverse; AES uses 0 */
    for (int a = 1; a < 256; a++) {
        for (int b = 1; b < 256; b++) {
            if (aes_mul((uint8_t)a, (uint8_t)b) == 1) { inv[a] = (uint8_t)b; break; }
        }
    }

    for (int i = 0; i < 256; i++) {
        uint8_t x = inv[i];
        uint8_t y = (uint8_t)(x ^ aes_rotl8(x, 1) ^ aes_rotl8(x, 2) ^
                              aes_rotl8(x, 3) ^ aes_rotl8(x, 4) ^ 0x63);
        aes_sbox[i] = y;
        aes_inv_sbox[y] = (uint8_t)i;
    }
    aes_tables_ready = 1;
}

/* ===== the key schedule ===== */

/*
 * Round keys for both directions.
 *
 * `dk` is the *equivalent inverse cipher* schedule, which is what makes
 * one schedule serve both AESDEC and the portable inverse: rather than
 * running the rounds backwards, the inverse cipher runs forwards over
 * round keys that have had InvMixColumns applied to them. AESDEC
 * assumes exactly this arrangement, so the two implementations can
 * share a key expansion instead of each having its own.
 *
 * ---- why bytes and not words ----
 *
 * The obvious representation is uint32_t[60], one word per column, and
 * it is wrong here in a way that is invisible until the two
 * implementations are compared.
 *
 * A word built with `(p[0]<<24)|(p[1]<<16)|...` holds the four key
 * bytes in *arithmetic* order. Code that takes them back out by
 * shifting is endian-agnostic and correct. But AESENC does not shift:
 * it reads sixteen bytes of memory, and on a little-endian machine
 * those bytes come out of each word reversed. The result is a cipher
 * that is internally consistent, produces stable output, and matches
 * no AES anywhere -- which is exactly what happened, and it presented
 * as a Kerberos KDC insisting the password was wrong.
 *
 * So the schedule is a byte array in the order the standard defines,
 * and both implementations index it identically.
 *
 * Aligned to 16 because these are memory operands to AESENC. The AES-NI
 * instructions are exception type 4 and tolerate unaligned operands,
 * but relying on that is free to avoid.
 */
typedef struct {
    uint8_t ek[16 * 15] __attribute__((aligned(16)));
    uint8_t dk[16 * 15] __attribute__((aligned(16)));
    int     nr;
} aes_key_t;

static const uint8_t aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36
};

static uint32_t aes_sub_word(uint32_t w) {
    return ((uint32_t)aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((uint32_t)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((uint32_t)aes_sbox[(w >>  8) & 0xFF] <<  8) |
           ((uint32_t)aes_sbox[ w        & 0xFF]);
}

static uint32_t aes_rot_word(uint32_t w) {
    return (w << 8) | (w >> 24);
}

static uint32_t aes_get32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void aes_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

/* InvMixColumns on a packed column, for the decryption schedule. */
static uint32_t aes_inv_mix(uint32_t w) {
    uint8_t a0 = (uint8_t)(w >> 24), a1 = (uint8_t)(w >> 16);
    uint8_t a2 = (uint8_t)(w >> 8),  a3 = (uint8_t)w;
    uint8_t b0 = (uint8_t)(aes_mul(a0,0x0E) ^ aes_mul(a1,0x0B) ^ aes_mul(a2,0x0D) ^ aes_mul(a3,0x09));
    uint8_t b1 = (uint8_t)(aes_mul(a0,0x09) ^ aes_mul(a1,0x0E) ^ aes_mul(a2,0x0B) ^ aes_mul(a3,0x0D));
    uint8_t b2 = (uint8_t)(aes_mul(a0,0x0D) ^ aes_mul(a1,0x09) ^ aes_mul(a2,0x0E) ^ aes_mul(a3,0x0B));
    uint8_t b3 = (uint8_t)(aes_mul(a0,0x0B) ^ aes_mul(a1,0x0D) ^ aes_mul(a2,0x09) ^ aes_mul(a3,0x0E));
    return ((uint32_t)b0 << 24) | ((uint32_t)b1 << 16) |
           ((uint32_t)b2 << 8)  | (uint32_t)b3;
}

/* bits is 128, 192 or 256. Returns 0, or -1 for a size AES does not have. */
static int aes_setkey(aes_key_t *k, const uint8_t *key, int bits) {
    aes_init();

    int nk;
    switch (bits) {
    case 128: nk = 4; k->nr = 10; break;
    case 192: nk = 6; k->nr = 12; break;
    case 256: nk = 8; k->nr = 14; break;
    default: return -1;
    }
    int total = 4 * (k->nr + 1);

    uint32_t w[60];
    for (int i = 0; i < nk; i++) w[i] = aes_get32(key + 4 * i);

    for (int i = nk; i < total; i++) {
        uint32_t t = w[i - 1];
        if (i % nk == 0) {
            t = aes_sub_word(aes_rot_word(t)) ^ ((uint32_t)aes_rcon[i / nk] << 24);
        } else if (nk > 6 && i % nk == 4) {
            /* The extra SubWord at the four-word mark exists only for
             * 256-bit keys. Leaving it out produces a schedule that is
             * self-consistent and decrypts its own ciphertext, and
             * agrees with no other implementation on earth. */
            t = aes_sub_word(t);
        }
        w[i] = w[i - nk] ^ t;
    }

    for (int i = 0; i < total; i++) aes_put32(k->ek + 4 * i, w[i]);

    /* The equivalent inverse schedule: first and last round keys are
     * carried across unchanged, the middle ones pass through
     * InvMixColumns, and the order is reversed. */
    for (int i = 0; i <= k->nr; i++) {
        for (int j = 0; j < 4; j++) {
            uint32_t v = w[4 * (k->nr - i) + j];
            aes_put32(k->dk + 16 * i + 4 * j,
                      (i == 0 || i == k->nr) ? v : aes_inv_mix(v));
        }
    }
    return 0;
}

/* ===== AES-NI =====
 *
 * Only on x86-64, and only when CPUID says so. The host-side tests in
 * tools/ build on whatever machine this repository is edited from --
 * which is an arm64 Mac -- so the whole block has to disappear for a
 * compiler that would not know what AESENC is.
 */
#if defined(__x86_64__)

static int aes_ni_flag = -1;

static int aes_have_ni(void) {
    if (aes_ni_flag < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1u), "c"(0u));
        aes_ni_flag = (int)((c >> 25) & 1u);       /* CPUID.1:ECX.AESNI */
    }
    return aes_ni_flag;
}

/*
 * One block, in registers.
 *
 * The round keys are read straight out of the schedule as memory
 * operands, so the loop is a pointer walk and the state never leaves
 * xmm0. Nothing here is indexed by a key byte.
 */
static void aes_ni_encrypt(const aes_key_t *k, const uint8_t in[16],
                           uint8_t out[16]) {
    const uint8_t *rk = k->ek;
    long n = k->nr - 1;
    __asm__ volatile(
        "movdqu (%[in]), %%xmm0\n\t"
        "pxor   (%[rk]), %%xmm0\n\t"
        "addq   $16, %[rk]\n\t"
        "1:\n\t"
        "aesenc (%[rk]), %%xmm0\n\t"
        "addq   $16, %[rk]\n\t"
        "decq   %[n]\n\t"
        "jnz    1b\n\t"
        "aesenclast (%[rk]), %%xmm0\n\t"
        "movdqu %%xmm0, (%[out])\n\t"
        /* Earlyclobber on both. Without it GCC is entitled to put an
         * input operand in the same register as an output, on the
         * assumption that every input is consumed before any output is
         * written -- and here `rk` and `n` are modified in the loop
         * while `out` is still needed at the end. */
        : [rk] "+&r"(rk), [n] "+&r"(n)
        : [in] "r"(in), [out] "r"(out)
        : "xmm0", "memory", "cc");
}

static void aes_ni_decrypt(const aes_key_t *k, const uint8_t in[16],
                           uint8_t out[16]) {
    const uint8_t *rk = k->dk;
    long n = k->nr - 1;
    __asm__ volatile(
        "movdqu (%[in]), %%xmm0\n\t"
        "pxor   (%[rk]), %%xmm0\n\t"
        "addq   $16, %[rk]\n\t"
        "1:\n\t"
        "aesdec (%[rk]), %%xmm0\n\t"
        "addq   $16, %[rk]\n\t"
        "decq   %[n]\n\t"
        "jnz    1b\n\t"
        "aesdeclast (%[rk]), %%xmm0\n\t"
        "movdqu %%xmm0, (%[out])\n\t"
        /* Earlyclobber on both. Without it GCC is entitled to put an
         * input operand in the same register as an output, on the
         * assumption that every input is consumed before any output is
         * written -- and here `rk` and `n` are modified in the loop
         * while `out` is still needed at the end. */
        : [rk] "+&r"(rk), [n] "+&r"(n)
        : [in] "r"(in), [out] "r"(out)
        : "xmm0", "memory", "cc");
}

#else
static int aes_have_ni(void) { return 0; }
#endif

/* ===== the portable implementation =====
 *
 * The state is the FIPS-197 column-major array: byte i of the block is
 * s[i % 4][i / 4]. Getting that transposition backwards produces a
 * cipher that round-trips perfectly and matches no published vector,
 * which is why the vectors are the test and self-consistency is not.
 */

static void aes_sw_encrypt(const aes_key_t *k, const uint8_t in[16],
                           uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i];

    /* AddRoundKey, round 0 */
    for (int i = 0; i < 16; i++) s[i] ^= k->ek[i];

    for (int round = 1; round <= k->nr; round++) {
        uint8_t t[16];

        /* SubBytes then ShiftRows, in one pass: row r moves left by r,
         * and the state is column-major, so the source of byte (r, c)
         * is byte (r, c + r). */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[4*c + r] = aes_sbox[s[4*((c + r) & 3) + r]];

        if (round != k->nr) {
            /* MixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t a0 = t[4*c+0], a1 = t[4*c+1], a2 = t[4*c+2], a3 = t[4*c+3];
                uint8_t x = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                s[4*c+0] = (uint8_t)(a0 ^ x ^ aes_xtime((uint8_t)(a0 ^ a1)));
                s[4*c+1] = (uint8_t)(a1 ^ x ^ aes_xtime((uint8_t)(a1 ^ a2)));
                s[4*c+2] = (uint8_t)(a2 ^ x ^ aes_xtime((uint8_t)(a2 ^ a3)));
                s[4*c+3] = (uint8_t)(a3 ^ x ^ aes_xtime((uint8_t)(a3 ^ a0)));
            }
        } else {
            for (int i = 0; i < 16; i++) s[i] = t[i];
        }

        for (int i = 0; i < 16; i++) s[i] ^= k->ek[16*round + i];
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* The equivalent inverse cipher, over the `dk` schedule. */
static void aes_sw_decrypt(const aes_key_t *k, const uint8_t in[16],
                           uint8_t out[16]) {
    uint8_t s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i];

    for (int i = 0; i < 16; i++) s[i] ^= k->dk[i];

    for (int round = 1; round <= k->nr; round++) {
        uint8_t t[16];

        /* InvSubBytes and InvShiftRows: row r moves *right* by r. */
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[4*c + r] = aes_inv_sbox[s[4*((c - r) & 3) + r]];

        for (int i = 0; i < 16; i++) s[i] = t[i];

        /*
         * InvMixColumns comes *before* the round key, and that ordering
         * is the whole of the equivalent inverse cipher.
         *
         * InvMixColumns is linear over XOR, so
         *
         *     InvMixColumns(state ^ w) == InvMixColumns(state) ^ InvMixColumns(w)
         *
         * which is what lets the transform be moved off the state and
         * onto the schedule, where it is paid once per key instead of
         * once per block. `dk` already holds InvMixColumns(w). Doing
         * the two in the other order applies it to a value that has
         * had the key mixed in and produces a cipher that inverts
         * nothing.
         */
        if (round != k->nr) {
            for (int c = 0; c < 4; c++) {
                uint32_t w = aes_inv_mix(((uint32_t)s[4*c+0] << 24) |
                                         ((uint32_t)s[4*c+1] << 16) |
                                         ((uint32_t)s[4*c+2] << 8)  |
                                          (uint32_t)s[4*c+3]);
                s[4*c+0] = (uint8_t)(w >> 24); s[4*c+1] = (uint8_t)(w >> 16);
                s[4*c+2] = (uint8_t)(w >> 8);  s[4*c+3] = (uint8_t)w;
            }
        }

        for (int i = 0; i < 16; i++) s[i] ^= k->dk[16*round + i];
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

/* ===== the block cipher, whichever way it is done here ===== */

static void aes_encrypt_block(const aes_key_t *k, const uint8_t in[16],
                              uint8_t out[16]) {
#if defined(__x86_64__)
    if (aes_have_ni()) { aes_ni_encrypt(k, in, out); return; }
#endif
    aes_sw_encrypt(k, in, out);
}

static void aes_decrypt_block(const aes_key_t *k, const uint8_t in[16],
                              uint8_t out[16]) {
#if defined(__x86_64__)
    if (aes_have_ni()) { aes_ni_decrypt(k, in, out); return; }
#endif
    aes_sw_decrypt(k, in, out);
}

static void aes_xor16(uint8_t *d, const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) d[i] = (uint8_t)(a[i] ^ b[i]);
}

/* ===== CBC ===== */

static void aes_cbc_encrypt(const aes_key_t *k, uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, uint32_t n) {
    uint8_t blk[16];
    for (uint32_t off = 0; off + 16 <= n; off += 16) {
        aes_xor16(blk, in + off, iv);
        aes_encrypt_block(k, blk, out + off);
        for (int i = 0; i < 16; i++) iv[i] = out[off + i];
    }
}

static void aes_cbc_decrypt(const aes_key_t *k, uint8_t iv[16],
                            const uint8_t *in, uint8_t *out, uint32_t n) {
    uint8_t blk[16], carry[16];
    for (uint32_t off = 0; off + 16 <= n; off += 16) {
        for (int i = 0; i < 16; i++) carry[i] = in[off + i];
        aes_decrypt_block(k, in + off, blk);
        aes_xor16(out + off, blk, iv);
        for (int i = 0; i < 16; i++) iv[i] = carry[i];
    }
}

/* ===== CBC with ciphertext stealing (RFC 3962) =====
 *
 * Kerberos encrypts messages of arbitrary length with a block cipher
 * and adds no padding at all: the ciphertext is exactly as long as the
 * plaintext. That is what ciphertext stealing buys, and it is why a
 * Kerberos message cannot be decrypted by a plain CBC routine even
 * though every block in the middle is plain CBC.
 *
 * The variant is the one people call CS3: the last two ciphertext
 * blocks are swapped *unconditionally*, including when the length is an
 * exact multiple of the block size. The conditional forms (CS1, CS2)
 * are the same algorithm with a different rule about when to swap, and
 * choosing the wrong one produces a ciphertext that differs from
 * Kerberos's only in the final thirty-two bytes -- which is to say, one
 * that decrypts to garbage exactly where the checksum is.
 *
 * Returns 0, or -1 if there is less than one whole block to work on.
 * `iv` is updated to the last block actually encrypted, which is the
 * cipher state a caller who chains messages needs.
 */
static int aes_cts_encrypt(const aes_key_t *k, uint8_t iv[16],
                           const uint8_t *in, uint8_t *out, uint32_t n) {
    if (n < 16) return -1;
    if (n == 16) {
        uint8_t blk[16];
        aes_xor16(blk, in, iv);
        aes_encrypt_block(k, blk, out);
        for (int i = 0; i < 16; i++) iv[i] = out[i];
        return 0;
    }

    uint32_t nb   = (n + 15) / 16;              /* blocks, last may be short */
    uint32_t d    = n - (nb - 1) * 16;          /* length of the last, 1..16 */
    uint32_t head = (nb - 2) * 16;              /* plain CBC covers this much */

    aes_cbc_encrypt(k, iv, in, out, head);

    /* Block nb-1, whole, chained onto whatever CBC left in iv. */
    uint8_t cprev[16], blk[16], clast[16];
    aes_xor16(blk, in + head, iv);
    aes_encrypt_block(k, blk, cprev);

    /* Block nb, zero-padded to a whole block. The padding is never
     * transmitted -- it is exactly the part that gets stolen. */
    uint8_t tail[16];
    for (uint32_t i = 0; i < 16; i++)
        tail[i] = (i < d) ? in[head + 16 + i] : 0;
    aes_xor16(blk, tail, cprev);
    aes_encrypt_block(k, blk, clast);

    /* The swap. */
    for (int i = 0; i < 16; i++) out[head + i] = clast[i];
    for (uint32_t i = 0; i < d; i++) out[head + 16 + i] = cprev[i];

    for (int i = 0; i < 16; i++) iv[i] = clast[i];
    return 0;
}

static int aes_cts_decrypt(const aes_key_t *k, uint8_t iv[16],
                           const uint8_t *in, uint8_t *out, uint32_t n) {
    if (n < 16) return -1;
    if (n == 16) {
        uint8_t blk[16];
        aes_decrypt_block(k, in, blk);
        aes_xor16(out, blk, iv);
        for (int i = 0; i < 16; i++) iv[i] = in[i];
        return 0;
    }

    uint32_t nb   = (n + 15) / 16;
    uint32_t d    = n - (nb - 1) * 16;
    uint32_t head = (nb - 2) * 16;

    aes_cbc_decrypt(k, iv, in, out, head);

    /* What is on the wire at these positions is the last two ciphertext
     * blocks the other way round: C_n whole, then the first d bytes of
     * C_{n-1}. The rest of C_{n-1} is recovered from the decryption of
     * C_n, because the plaintext it was XORed against there was zero. */
    const uint8_t *clast = in + head;
    uint8_t z[16];
    aes_decrypt_block(k, clast, z);

    uint8_t cprev[16];
    for (uint32_t i = 0; i < 16; i++)
        cprev[i] = (i < d) ? in[head + 16 + i] : z[i];

    /* The stolen bytes come back the same way they went. */
    for (uint32_t i = 0; i < d; i++)
        out[head + 16 + i] = (uint8_t)(z[i] ^ cprev[i]);

    uint8_t blk[16];
    aes_decrypt_block(k, cprev, blk);
    aes_xor16(out + head, blk, iv);

    for (int i = 0; i < 16; i++) iv[i] = clast[i];
    return 0;
}

/* ===== CMAC (NIST SP 800-38B) =====
 *
 * SMB 3.0 signs with AES-CMAC, so it is here rather than in a file of
 * its own. CMAC is CBC-MAC with the fix that makes it safe for messages
 * of varying length: the final block is XORed with a subkey derived by
 * doubling in GF(2^128), and *which* subkey depends on whether the
 * message needed padding. Using one subkey for both cases is CBC-MAC,
 * which is forgeable across lengths.
 */
static void aes_cmac_double(uint8_t b[16]) {
    int carry = b[0] >> 7;
    for (int i = 0; i < 15; i++) b[i] = (uint8_t)((b[i] << 1) | (b[i+1] >> 7));
    b[15] = (uint8_t)(b[15] << 1);
    if (carry) b[15] ^= 0x87;          /* the GF(2^128) reduction polynomial */
}

static void aes_cmac(const aes_key_t *k, const uint8_t *msg, uint32_t n,
                     uint8_t out[16]) {
    uint8_t l[16] = {0}, k1[16], k2[16];
    uint8_t zero[16] = {0};
    aes_encrypt_block(k, zero, l);
    for (int i = 0; i < 16; i++) k1[i] = l[i];
    aes_cmac_double(k1);
    for (int i = 0; i < 16; i++) k2[i] = k1[i];
    aes_cmac_double(k2);

    uint8_t x[16] = {0}, y[16];
    uint32_t nblocks = n / 16;
    uint32_t rem = n % 16;
    int whole = (n != 0 && rem == 0);
    if (whole) nblocks--;

    for (uint32_t i = 0; i < nblocks; i++) {
        aes_xor16(y, x, msg + i * 16);
        aes_encrypt_block(k, y, x);
    }

    uint8_t last[16];
    if (whole) {
        for (int i = 0; i < 16; i++) last[i] = (uint8_t)(msg[nblocks*16 + i] ^ k1[i]);
    } else {
        for (uint32_t i = 0; i < 16; i++) {
            uint8_t b = (i < rem) ? msg[nblocks*16 + i] : (i == rem ? 0x80 : 0x00);
            last[i] = (uint8_t)(b ^ k2[i]);
        }
    }
    aes_xor16(y, x, last);
    aes_encrypt_block(k, y, out);
}

#endif /* AES_H */
