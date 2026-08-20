/*
 * tools/aes_test.c — AES against FIPS-197, RFC 3962 and RFC 4493.
 *
 * Every expectation in this file was published by somebody else, and
 * every one of them was checked against a second implementation before
 * being written down here -- openssl(1) on the host, run against the
 * same inputs. That second step is not paranoia. This repository has
 * already had a test fail because the *vector* was wrong, written from
 * memory rather than from the document, with the implementation
 * correct all along. A vector confirmed by a third party costs one
 * command and removes that whole class of afternoon.
 *
 * What runs here is the portable implementation, because the machine
 * this is developed on is an arm64 Mac and has no AESENC. That is the
 * right way round: the portable path is the one that would otherwise
 * never be exercised, since every x86 machine made since 2010 takes
 * the AES-NI branch and the fallback would rot unnoticed. The kernel's
 * own selftest runs both and requires that they agree.
 *
 * The CTS vectors matter most. Ciphertext stealing has three variants
 * that differ only in when the final two blocks are swapped, they all
 * round-trip perfectly against themselves, and only one of them is the
 * one Kerberos uses.
 */

#include <stdio.h>
#include <string.h>
#include "aes.h"

static int checks = 0, fails = 0;

static void hexcheck(const char *what, const uint8_t *got, const char *want,
                     int n) {
    char h[256];
    for (int i = 0; i < n; i++) sprintf(h + i * 2, "%02x", got[i]);
    h[n * 2] = 0;
    checks++;
    if (strcmp(h, want)) {
        fails++;
        printf("  FAIL %s\n    got  %s\n    want %s\n", what, h, want);
    } else {
        printf("  ok   %s\n", what);
    }
}

static int unhex(const char *s, uint8_t *out) {
    int n = 0;
    for (; s[0] && s[1]; s += 2) {
        int hi = s[0] <= '9' ? s[0] - '0' : (s[0] | 32) - 'a' + 10;
        int lo = s[1] <= '9' ? s[1] - '0' : (s[1] | 32) - 'a' + 10;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    return n;
}

int main(void) {
    uint8_t key[32], in[128], out[128], back[128], iv[16];
    aes_key_t k;

    printf("AES block cipher (FIPS-197 appendix C)\n");
    {
        struct { const char *key; int bits; const char *ct; } v[] = {
            { "000102030405060708090a0b0c0d0e0f", 128,
              "69c4e0d86a7b0430d8cdb78070b4c55a" },
            { "000102030405060708090a0b0c0d0e0f1011121314151617", 192,
              "dda97ca4864cdfe06eaf70a0ec0d7191" },
            { "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
              256, "8ea2b7ca516745bfeafc49904b496089" },
        };
        for (int i = 0; i < 3; i++) {
            unhex(v[i].key, key);
            unhex("00112233445566778899aabbccddeeff", in);
            aes_setkey(&k, key, v[i].bits);
            aes_encrypt_block(&k, in, out);
            char label[64];
            sprintf(label, "aes-%d encrypt", v[i].bits);
            hexcheck(label, out, v[i].ct, 16);

            /* Decryption is checked against the plaintext rather than
             * against nothing: an inverse cipher with the wrong round
             * key order still inverts *something*. */
            aes_decrypt_block(&k, out, back);
            sprintf(label, "aes-%d decrypt", v[i].bits);
            hexcheck(label, back, "00112233445566778899aabbccddeeff", 16);
        }
    }

    printf("\nAES-CTS (RFC 3962 appendix B)\n");
    {
        /* One key and one running plaintext, sliced to seven lengths --
         * which is the point of the vector set. The interesting cases
         * are 31 and 32 bytes: a length that is an exact multiple of
         * the block size still swaps its final two blocks, and an
         * implementation that only swaps when there is a partial block
         * passes every other case here and fails that one. */
        static const char pt[] =
            "I would like the General Gau's Chicken, please, "
            "and wonton soup.";
        static const struct { int len; const char *ct; } v[] = {
            { 17, "c6353568f2bf8cb4d8a580362da7ff7f97" },
            { 31, "fc00783e0efdb2c1d445d4c8eff7ed22"
                  "97687268d6ecccc0c07b25e25ecfe5" },
            { 32, "39312523a78662d5be7fcbcc98ebf5a8"
                  "97687268d6ecccc0c07b25e25ecfe584" },
            { 47, "97687268d6ecccc0c07b25e25ecfe584"
                  "b3fffd940c16a18c1b5549d2f838029e"
                  "39312523a78662d5be7fcbcc98ebf5" },
            { 48, "97687268d6ecccc0c07b25e25ecfe584"
                  "9dad8bbb96c4cdc03bc103e1a194bbd8"
                  "39312523a78662d5be7fcbcc98ebf5a8" },
            { 64, "97687268d6ecccc0c07b25e25ecfe584"
                  "39312523a78662d5be7fcbcc98ebf5a8"
                  "4807efe836ee89a526730dbc2f7bc840"
                  "9dad8bbb96c4cdc03bc103e1a194bbd8" },
        };

        const uint8_t *plain = (const uint8_t *)pt;
        if (strlen(pt) != 64) { printf("  FAIL plaintext is not 64 bytes\n"); return 1; }

        memcpy(key, "chicken teriyaki", 16);
        aes_setkey(&k, key, 128);

        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            /* Strip any spaces the expectation was wrapped with. */
            char want[256];
            int w = 0;
            for (const char *s = v[i].ct; *s; s++)
                if (*s != ' ') want[w++] = *s;
            want[w] = 0;

            memset(iv, 0, 16);
            aes_cts_encrypt(&k, iv, plain, out, (uint32_t)v[i].len);
            char label[64];
            sprintf(label, "cts encrypt, %d bytes", v[i].len);
            hexcheck(label, out, want, v[i].len);

            memset(iv, 0, 16);
            aes_cts_decrypt(&k, iv, out, back, (uint32_t)v[i].len);
            checks++;
            if (memcmp(back, plain, (size_t)v[i].len)) {
                fails++;
                printf("  FAIL cts decrypt, %d bytes\n", v[i].len);
            } else {
                printf("  ok   cts decrypt, %d bytes\n", v[i].len);
            }
        }
    }

    printf("\nAES-CMAC (RFC 4493)\n");
    {
        uint8_t msg[64];
        unhex("6bc1bee22e409f96e93d7e117393172a"
              "ae2d8a571e03ac9c9eb76fac45af8e51"
              "30c81c46a35ce411e5fbc1191a0a52ef"
              "f69f2445df4f9b17ad2b417be66c3710", msg);
        unhex("2b7e151628aed2a6abf7158809cf4f3c", key);
        aes_setkey(&k, key, 128);

        static const struct { int len; const char *mac; } v[] = {
            {  0, "bb1d6929e95937287fa37d129b756746" },
            { 16, "070a16b46b4d4144f79bdd9dd04a287c" },
            { 40, "dfa66747de9ae63030ca32611497c827" },
            { 64, "51f0bebf7e3b9d92fc49741779363cfe" },
        };
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            aes_cmac(&k, msg, (uint32_t)v[i].len, out);
            char label[64];
            sprintf(label, "cmac of %d bytes", v[i].len);
            hexcheck(label, out, v[i].mac, 16);
        }
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
