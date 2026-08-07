#ifndef VEXTRO_CHACHA20_H
#define VEXTRO_CHACHA20_H

/*
 * src/chacha20.h — ChaCha20 (RFC 8439), and a passphrase KDF over it.
 *
 * Chosen over AES for one reason that matters here: it is add, xor and
 * rotate on 32-bit words and nothing else. No S-box tables to hold, no
 * carry-less multiply, and — the point — no floating point and no
 * instruction this kernel is compiled without. AES on x86 without AES-NI
 * means a table-driven implementation whose timing depends on the key,
 * which is worse than useless; ChaCha20 is constant-time by construction
 * because it never indexes memory with a secret.
 *
 * This is the stream cipher only. There is no authentication here, so
 * what it protects is confidentiality, not integrity — a container can be
 * decrypted with the wrong passphrase and will produce garbage rather
 * than an error, which is why the vault stores a verifier alongside.
 */

#define CC20_KEY_BYTES   32
#define CC20_NONCE_BYTES 12
#define CC20_BLOCK       64

static inline uint32_t cc20_rotl(uint32_t v, int n) {
    return (v << n) | (v >> (32 - n));
}

#define CC20_QR(a, b, c, d)                       \
    a += b; d ^= a; d = cc20_rotl(d, 16);         \
    c += d; b ^= c; b = cc20_rotl(b, 12);         \
    a += b; d ^= a; d = cc20_rotl(d, 8);          \
    c += d; b ^= c; b = cc20_rotl(b, 7)

/* One 64-byte keystream block for (key, counter, nonce). */
static void cc20_block(const uint8_t key[CC20_KEY_BYTES], uint32_t counter,
                       const uint8_t nonce[CC20_NONCE_BYTES],
                       uint8_t out[CC20_BLOCK]) {
    /* "expand 32-byte k", as little-endian words. Spelled out rather than
     * parsed from the string so there is no dependency on char signedness. */
    uint32_t s[16];
    s[0] = 0x61707865u; s[1] = 0x3320646eu;
    s[2] = 0x79622d32u; s[3] = 0x6b206574u;
    for (int i = 0; i < 8; i++)
        s[4 + i] = (uint32_t)key[i * 4]
                 | ((uint32_t)key[i * 4 + 1] << 8)
                 | ((uint32_t)key[i * 4 + 2] << 16)
                 | ((uint32_t)key[i * 4 + 3] << 24);
    s[12] = counter;
    for (int i = 0; i < 3; i++)
        s[13 + i] = (uint32_t)nonce[i * 4]
                  | ((uint32_t)nonce[i * 4 + 1] << 8)
                  | ((uint32_t)nonce[i * 4 + 2] << 16)
                  | ((uint32_t)nonce[i * 4 + 3] << 24);

    uint32_t x[16];
    for (int i = 0; i < 16; i++) x[i] = s[i];

    for (int i = 0; i < 10; i++) {          /* 20 rounds = 10 double rounds */
        CC20_QR(x[0], x[4], x[8],  x[12]);
        CC20_QR(x[1], x[5], x[9],  x[13]);
        CC20_QR(x[2], x[6], x[10], x[14]);
        CC20_QR(x[3], x[7], x[11], x[15]);
        CC20_QR(x[0], x[5], x[10], x[15]);
        CC20_QR(x[1], x[6], x[11], x[12]);
        CC20_QR(x[2], x[7], x[8],  x[13]);
        CC20_QR(x[3], x[4], x[9],  x[14]);
    }

    for (int i = 0; i < 16; i++) {
        const uint32_t v = x[i] + s[i];
        out[i * 4]     = (uint8_t)(v);
        out[i * 4 + 1] = (uint8_t)(v >> 8);
        out[i * 4 + 2] = (uint8_t)(v >> 16);
        out[i * 4 + 3] = (uint8_t)(v >> 24);
    }
}

/*
 * Encrypt or decrypt in place. The cipher is its own inverse, so there is
 * one function and not two — which also means a caller cannot get the
 * direction wrong.
 *
 * `offset` is the byte position of `buf` within the whole stream, so a
 * file can be processed a chunk at a time without holding it all.
 */
static void cc20_xor(const uint8_t key[CC20_KEY_BYTES],
                     const uint8_t nonce[CC20_NONCE_BYTES],
                     uint64_t offset, uint8_t *buf, uint32_t len) {
    uint8_t ks[CC20_BLOCK];
    uint32_t counter = (uint32_t)(offset / CC20_BLOCK);
    uint32_t skip = (uint32_t)(offset % CC20_BLOCK);
    uint32_t i = 0;

    while (i < len) {
        cc20_block(key, counter, nonce, ks);
        for (uint32_t j = skip; j < CC20_BLOCK && i < len; j++, i++)
            buf[i] ^= ks[j];
        skip = 0;
        counter++;
    }
}

/*
 * Passphrase to key.
 *
 * SHA-256 over salt ‖ passphrase, iterated. This is the same shape the
 * account system already uses for passwords, deliberately: one hashing
 * discipline in the system is easier to reason about than two, and this
 * one is already understood here.
 *
 * The iteration count is the whole defence. It is not a memory-hard KDF —
 * this kernel has no allocator to make one out of — so what it buys is a
 * constant factor against guessing, and the vault says so rather than
 * implying more.
 */
#define CC20_KDF_ROUNDS 8192

static void cc20_derive_key(const char *passphrase,
                            const uint8_t salt[16],
                            uint8_t out[CC20_KEY_BYTES]) {
    uint8_t buf[16 + 64];
    int n = 0;
    for (; n < 16; n++) buf[n] = salt[n];
    for (int i = 0; passphrase[i] && n < (int)sizeof(buf); i++, n++)
        buf[n] = (uint8_t)passphrase[i];

    uint8_t digest[32];
    sha256(buf, (uint32_t)n, digest);
    for (uint32_t r = 1; r < CC20_KDF_ROUNDS; r++)
        sha256(digest, 32, digest);

    for (int i = 0; i < CC20_KEY_BYTES; i++) out[i] = digest[i];
}

/* Compare without leaking where two byte strings first differ. */
static int cc20_equal(const uint8_t *a, const uint8_t *b, int n) {
    uint8_t diff = 0;
    for (int i = 0; i < n; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

#endif /* VEXTRO_CHACHA20_H */
