/*
 * crypto_test.c — ChaCha20 against the RFC 8439 vectors.
 *
 * A cipher that is merely self-consistent is worthless: encrypting and
 * decrypting with the same wrong implementation round-trips perfectly and
 * protects nothing. These are the published test vectors, so a pass means
 * this produces the same keystream as every other implementation.
 *
 * Built and run on the host with the same header the kernel compiles.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "../src/sha256.h"
#include "../src/tls.h"
#include "../src/chacha20.h"

static int checks = 0, failures = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) {
        printf("  ok    %s\n", what);
    } else {
        failures++;
        printf("  FAIL  %s\n", what);
    }
}

static void hexdump(const uint8_t *p, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2]     = h[p[i] >> 4];
        out[i * 2 + 1] = h[p[i] & 15];
    }
    out[n * 2] = '\0';
}

int main(void) {
    printf("\nTEST ChaCha20 block function (RFC 8439 section 2.3.2)\n");
    {
        uint8_t key[32], nonce[12], out[64];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        /* nonce 00:00:00:09 00:00:00:4a 00:00:00:00 */
        memset(nonce, 0, 12);
        nonce[3] = 0x09; nonce[7] = 0x4a;

        cc20_block(key, 1, nonce, out);

        /* First 16 bytes of the expected keystream from the RFC. */
        static const uint8_t want[16] = {
            0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15,
            0x50, 0x0f, 0xdd, 0x1f, 0xa3, 0x20, 0x71, 0xc4
        };
        char got[40], exp[40];
        hexdump(out, 16, got);
        hexdump(want, 16, exp);
        printf("        keystream %s\n        expected  %s\n", got, exp);
        ok("keystream matches the published vector", memcmp(out, want, 16) == 0);
    }

    printf("\nTEST ChaCha20 encryption (RFC 8439 section 2.4.2)\n");
    {
        uint8_t key[32], nonce[12];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)i;
        memset(nonce, 0, 12);
        nonce[3] = 0x00; nonce[7] = 0x4a; nonce[11] = 0x00;
        nonce[3] = 0x00;
        /* nonce = 00:00:00:00 00:00:00:4a 00:00:00:00 */
        memset(nonce, 0, 12);
        nonce[7] = 0x4a;

        const char *plain =
            "Ladies and Gentlemen of the class of '99: If I could offer you "
            "only one tip for the future, sunscreen would be it.";
        uint8_t buf[200];
        const uint32_t n = (uint32_t)strlen(plain);
        memcpy(buf, plain, n);

        /* The RFC's example starts at counter 1, which is byte offset 64. */
        cc20_xor(key, nonce, 64, buf, n);

        static const uint8_t want[16] = {
            0x6e, 0x2e, 0x35, 0x9a, 0x25, 0x68, 0xf9, 0x80,
            0x41, 0xba, 0x07, 0x28, 0xdd, 0x0d, 0x69, 0x81
        };
        char got[40], exp[40];
        hexdump(buf, 16, got);
        hexdump(want, 16, exp);
        printf("        ciphertext %s\n        expected   %s\n", got, exp);
        ok("ciphertext matches the published vector", memcmp(buf, want, 16) == 0);

        /* And back again: the cipher is its own inverse. */
        cc20_xor(key, nonce, 64, buf, n);
        ok("decrypts to the original", memcmp(buf, plain, n) == 0);
    }

    printf("\nTEST chunked processing gives the same answer\n");
    {
        uint8_t key[32], nonce[12];
        for (int i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
        for (int i = 0; i < 12; i++) nonce[i] = (uint8_t)(i * 3);

        uint8_t a[300], b[300];
        for (int i = 0; i < 300; i++) a[i] = b[i] = (uint8_t)(i & 0xFF);

        cc20_xor(key, nonce, 0, a, 300);
        /* Same stream, fed in awkward pieces that straddle block edges. */
        uint32_t off = 0;
        const uint32_t chunks[] = { 1, 63, 64, 65, 107 };
        for (unsigned c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            cc20_xor(key, nonce, off, b + off, chunks[c]);
            off += chunks[c];
        }
        ok("a file encrypted in pieces matches one encrypted whole",
           off == 300 && memcmp(a, b, 300) == 0);
    }

    printf("\nTEST passphrase derivation\n");
    {
        uint8_t salt[16], k1[32], k2[32], k3[32];
        for (int i = 0; i < 16; i++) salt[i] = (uint8_t)(i + 1);

        cc20_derive_key("correct horse", salt, k1);
        cc20_derive_key("correct horse", salt, k2);
        ok("the same passphrase and salt give the same key",
           memcmp(k1, k2, 32) == 0);

        cc20_derive_key("correct horsf", salt, k3);
        ok("one changed character gives a different key",
           memcmp(k1, k3, 32) != 0);

        salt[0] ^= 1;
        cc20_derive_key("correct horse", salt, k3);
        ok("a different salt gives a different key",
           memcmp(k1, k3, 32) != 0);
    }

    printf("\nTEST constant-time compare\n");
    {
        uint8_t a[8] = {1,2,3,4,5,6,7,8}, b[8] = {1,2,3,4,5,6,7,8};
        ok("equal buffers compare equal", cc20_equal(a, b, 8));
        b[7] = 9;
        ok("a difference in the last byte is caught", !cc20_equal(a, b, 8));
        b[7] = 8; b[0] = 9;
        ok("a difference in the first byte is caught", !cc20_equal(a, b, 8));
    }

    /*
     * X25519 against RFC 7748's own vectors.
     *
     * The first is the section 5.2 scalar multiplication; the second is
     * the section 6.1 Diffie-Hellman, which is the one that matters --
     * it checks that two independently derived shared secrets agree,
     * which is the property the whole handshake rests on.
     */
    printf("\nTEST X25519 (RFC 7748)\n");
    {
        static const uint8_t scalar[32] = {
            0xa5,0x46,0xe3,0x6b,0xf0,0x52,0x7c,0x9d,0x3b,0x16,0x15,0x4b,
            0x82,0x46,0x5e,0xdd,0x62,0x14,0x4c,0x0a,0xc1,0xfc,0x5a,0x18,
            0x50,0x6a,0x22,0x44,0xba,0x44,0x9a,0xc4 };
        static const uint8_t point[32] = {
            0xe6,0xdb,0x68,0x67,0x58,0x30,0x30,0xdb,0x35,0x94,0xc1,0xa4,
            0x24,0xb1,0x5f,0x7c,0x72,0x66,0x24,0xec,0x26,0xb3,0x35,0x3b,
            0x10,0xa9,0x03,0xa6,0xd0,0xab,0x1c,0x4c };
        static const uint8_t want[32] = {
            0xc3,0xda,0x55,0x37,0x9d,0xe9,0xc6,0x90,0x8e,0x94,0xea,0x4d,
            0xf2,0x8d,0x08,0x4f,0x32,0xec,0xcf,0x03,0x49,0x1c,0x71,0xf7,
            0x54,0xb4,0x07,0x55,0x77,0xa2,0x85,0x52 };
        uint8_t out[32];
        x25519(out, scalar, point);
        ok("section 5.2 scalar multiplication matches", memcmp(out, want, 32) == 0);
    }
    {
        static const uint8_t a_priv[32] = {
            0x77,0x07,0x6d,0x0a,0x73,0x18,0xa5,0x7d,0x3c,0x16,0xc1,0x72,
            0x51,0xb2,0x66,0x45,0xdf,0x4c,0x2f,0x87,0xeb,0xc0,0x99,0x2a,
            0xb1,0x77,0xfb,0xa5,0x1d,0xb9,0x2c,0x2a };
        static const uint8_t b_priv[32] = {
            0x5d,0xab,0x08,0x7e,0x62,0x4a,0x8a,0x4b,0x79,0xe1,0x7f,0x8b,
            0x83,0x80,0x0e,0xe6,0x6f,0x3b,0xb1,0x29,0x26,0x18,0xb6,0xfd,
            0x1c,0x2f,0x8b,0x27,0xff,0x88,0xe0,0xeb };
        static const uint8_t a_pub_want[32] = {
            0x85,0x20,0xf0,0x09,0x89,0x30,0xa7,0x54,0x74,0x8b,0x7d,0xdc,
            0xb4,0x3e,0xf7,0x5a,0x0d,0xbf,0x3a,0x0d,0x26,0x38,0x1a,0xf4,
            0xeb,0xa4,0xa9,0x8e,0xaa,0x9b,0x4e,0x6a };
        static const uint8_t shared_want[32] = {
            0x4a,0x5d,0x9d,0x5b,0xa4,0xce,0x2d,0xe1,0x72,0x8e,0x3b,0xf4,
            0x80,0x35,0x0f,0x25,0xe0,0x7e,0x21,0xc9,0x47,0xd1,0x9e,0x33,
            0x76,0xf0,0x9b,0x3c,0x1e,0x16,0x17,0x42 };
        uint8_t a_pub[32], b_pub[32], s1[32], s2[32];
        x25519(a_pub, a_priv, x25519_base);
        x25519(b_pub, b_priv, x25519_base);
        ok("the public key derived from a known private key matches",
           memcmp(a_pub, a_pub_want, 32) == 0);
        x25519(s1, a_priv, b_pub);
        x25519(s2, b_priv, a_pub);
        ok("both sides derive the same shared secret", memcmp(s1, s2, 32) == 0);
        ok("and it is the one RFC 7748 publishes",
           memcmp(s1, shared_want, 32) == 0);
    }

    printf("\nTEST Poly1305 (RFC 8439 section 2.5.2)\n");
    {
        static const uint8_t key[32] = {
            0x85,0xd6,0xbe,0x78,0x57,0x55,0x6d,0x33,0x7f,0x44,0x52,0xfe,
            0x42,0xd5,0x06,0xa8,0x01,0x03,0x80,0x8a,0xfb,0x0d,0xb2,0xfd,
            0x4a,0xbf,0xf6,0xaf,0x41,0x49,0xf5,0x1b };
        const char *msg = "Cryptographic Forum Research Group";
        static const uint8_t want[16] = {
            0xa8,0x06,0x1d,0xc1,0x30,0x51,0x36,0xc6,0xc2,0x2b,0x8b,0xaf,
            0x0c,0x01,0x27,0xa9 };
        uint8_t mac[16];
        poly1305_t p;
        poly1305_init(&p, key);
        poly1305_update(&p, (const uint8_t *)msg, (uint32_t)strlen(msg));
        poly1305_final(&p, mac);
        ok("the published tag is reproduced", memcmp(mac, want, 16) == 0);
    }

    printf("\nTEST HKDF-SHA256 (RFC 5869 test case 1)\n");
    {
        static const uint8_t ikm[22] = {
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,
            0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b,0x0b };
        static const uint8_t salt[13] = {
            0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c };
        static const uint8_t info[10] = {
            0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9 };
        static const uint8_t prk_want[32] = {
            0x07,0x77,0x09,0x36,0x2c,0x2e,0x32,0xdf,0x0d,0xdc,0x3f,0x0d,
            0xc4,0x7b,0xba,0x63,0x90,0xb6,0xc7,0x3b,0xb5,0x0f,0x9c,0x31,
            0x22,0xec,0x84,0x4a,0xd7,0xc2,0xb3,0xe5 };
        static const uint8_t okm_want[42] = {
            0x3c,0xb2,0x5f,0x25,0xfa,0xac,0xd5,0x7a,0x90,0x43,0x4f,0x64,
            0xd0,0x36,0x2f,0x2a,0x2d,0x2d,0x0a,0x90,0xcf,0x1a,0x5a,0x4c,
            0x5d,0xb0,0x2d,0x56,0xec,0xc4,0xc5,0xbf,0x34,0x00,0x72,0x08,
            0xd5,0xb8,0x87,0x18,0x58,0x65 };
        uint8_t prk[32], okm[42];
        hkdf_extract(salt, sizeof(salt), ikm, sizeof(ikm), prk);
        ok("extract produces the published pseudorandom key",
           memcmp(prk, prk_want, 32) == 0);
        hkdf_expand(prk, info, sizeof(info), okm, sizeof(okm));
        ok("expand produces the published output key material",
           memcmp(okm, okm_want, 42) == 0);
    }

    printf("\n%d checks, %d failures\n\n", checks, failures);
    return failures ? 1 : 0;
}
