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

    printf("\n%d checks, %d failures\n\n", checks, failures);
    return failures ? 1 : 0;
}
