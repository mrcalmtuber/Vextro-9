/*
 * tools/krb5_test.c — the Kerberos encryption profile, against RFC 3961
 * and RFC 3962.
 *
 * Two things are checked here and they are worth telling apart.
 *
 * n-fold is checked against the RFC's own appendix. It is a strange
 * function -- replicate, rotate right by thirteen bits, add with
 * end-around carry -- and there is no way to tell a correct
 * implementation from a plausible one by looking at it. The vectors are
 * the only oracle.
 *
 * string-to-key is checked against RFC 3962 appendix B, and that test
 * reaches much further than it looks. The final AES key in each case is
 *
 *     DK(PBKDF2(password, salt, iterations), "kerberos")
 *
 * so a matching key means PBKDF2-HMAC-SHA1 is right, *and* the 128-fold
 * of "kerberos" is right, *and* the DR chain that runs the block cipher
 * on its own output is right, *and* the truncation to the key length is
 * right. Any one of them wrong and the key is wrong. The appendix also
 * publishes the intermediate PBKDF2 output, so when the key does not
 * match, the two lines say immediately which half failed -- which is
 * the difference between a five-minute fix and an evening.
 *
 * What is *not* checked here is a published ciphertext, because RFC
 * 3962 does not publish one: the confounder is random, so there is
 * nothing fixed to compare against. The encrypt/decrypt round trip
 * below therefore proves only that the two are inverses. The real test
 * of those is tools/kdc.py, which decrypts what this code encrypts
 * using an implementation written separately from it.
 */

#include <stdio.h>
#include <string.h>
#include "krb5crypto.h"

static int checks = 0, fails = 0;

/*
 * A deterministic stand-in for the kernel's RDRAND-backed generator.
 *
 * Deterministic on purpose: this is a test, and a confounder that
 * changes every run makes a failure unreproducible. It is emphatically
 * not what the kernel links against -- see src/vxport_impl.h, which
 * fails hard rather than returning a buffer it did not write.
 */
uint32_t vx_random(uint8_t *out, uint32_t len) {
    static uint32_t s = 0x2545F491u;
    for (uint32_t i = 0; i < len; i++) {
        s = s * 1664525u + 1013904223u;
        out[i] = (uint8_t)(s >> 24);
    }
    return len;
}

static void hexcheck(const char *what, const uint8_t *got, const char *want,
                     int n) {
    char h[256], w[256];
    for (int i = 0; i < n; i++) sprintf(h + i * 2, "%02x", got[i]);
    h[n * 2] = 0;
    int j = 0;
    for (const char *s = want; *s; s++) if (*s != ' ') w[j++] = *s;
    w[j] = 0;
    checks++;
    if (strcmp(h, w)) {
        fails++;
        printf("  FAIL %s\n    got  %s\n    want %s\n", what, h, w);
    } else {
        printf("  ok   %s\n", what);
    }
}

int main(void) {
    uint8_t o[64];

    printf("n-fold (RFC 3961 appendix A.1)\n");
    {
        struct { const char *in; int outbytes; const char *want; } v[] = {
            { "012345",   8, "be072631276b1955" },
            { "password", 7, "78a07b6caf85fa" },
            { "kerberos", 8, "6b65726265726f73" },
            { "password", 21, "59e4a8ca7c0385c3c37b3f6d2000247cb6e6bd5b3e" },
            { "kerberos", 21, "8372c236344e5f1550cd0747e15d62ca7a5a3bcea4" },
        };
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            krb_nfold((const uint8_t *)v[i].in, (uint32_t)strlen(v[i].in),
                      o, (uint32_t)v[i].outbytes);
            char label[64];
            sprintf(label, "%d-fold(\"%s\")", v[i].outbytes * 8, v[i].in);
            hexcheck(label, o, v[i].want, v[i].outbytes);
        }
    }

    printf("\nPBKDF2-HMAC-SHA1 and string-to-key (RFC 3962 appendix B)\n");
    {
        struct {
            int iter; const char *pass; const char *salt;
            const char *pb128, *key128, *pb256, *key256;
        } v[] = {
            { 1, "password", "ATHENA.MIT.EDUraeburn",
              "cdedb5281bb2f801565a1122b2563515",
              "42263c6e89f4fc28b8df68ee09799f15",
              "cdedb5281bb2f801565a1122b25635150ad1f7a04bb9f3a333ecc0e2e1f70837",
              "fe697b52bc0d3ce14432ba036a92e65bbb52280990a2fa27883998d72af30161" },
            { 2, "password", "ATHENA.MIT.EDUraeburn",
              "01dbee7f4a9e243e988b62c73cda935d",
              "c651bf29e2300ac27fa469d693bdda13",
              "01dbee7f4a9e243e988b62c73cda935da05378b93244ec8f48a99e61ad799d86",
              "a2e16d16b36069c135d5e9d2e25f896102685618b95914b467c67622225824ff" },
            { 1200, "password", "ATHENA.MIT.EDUraeburn",
              "5c08eb61fdf71e4e4ec3cf6ba1f5512b",
              "4c01cd46d632d01e6dbe230a01ed642a",
              "5c08eb61fdf71e4e4ec3cf6ba1f5512ba7e52ddbc5e5142f708a31e2e62b1e13",
              "55a6ac740ad17b4846941051e1e8b0a7548d93b0ab30a8bc3ff16280382b8c2a" },
            { 1200, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
              "pass phrase equals block size",
              "139c30c0966bc32ba55fdbf212530ac9",
              "59d1bb789a828b1aa54ef9c2883f69ed",
              "139c30c0966bc32ba55fdbf212530ac9c5ec59f1a452f5cc9ad940fea0598ed1",
              "89adee3608db8bc71f1bfbfe459486b05618b70cbae22092534e56c553ba4b34" },
            { 1200, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX",
              "pass phrase exceeds block size",
              "9ccad6d468770cd51b10e6a68721be61",
              "cb8005dc5f90179a7f02104c0018751d",
              "9ccad6d468770cd51b10e6a68721be611a8b4d282601db3b36be9246915ec82a",
              "d78c5c9cb872a8c9dad4697f0bb5b2d21496c82beb2caeda2112fceea057401b" },
        };

        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) {
            char label[96];
            uint32_t slen = (uint32_t)strlen(v[i].salt);
            uint32_t plen = (uint32_t)strlen(v[i].pass);

            krb_pbkdf2((const uint8_t *)v[i].pass, plen,
                       (const uint8_t *)v[i].salt, slen,
                       (uint32_t)v[i].iter, o, 16);
            sprintf(label, "pbkdf2 128, iter %d, salt \"%.20s\"", v[i].iter, v[i].salt);
            hexcheck(label, o, v[i].pb128, 16);

            krb_pbkdf2((const uint8_t *)v[i].pass, plen,
                       (const uint8_t *)v[i].salt, slen,
                       (uint32_t)v[i].iter, o, 32);
            sprintf(label, "pbkdf2 256, iter %d, salt \"%.20s\"", v[i].iter, v[i].salt);
            hexcheck(label, o, v[i].pb256, 32);

            krb_key_t k;
            krb_string_to_key(KRB_ETYPE_AES128_CTS, v[i].pass,
                              (const uint8_t *)v[i].salt, slen,
                              (uint32_t)v[i].iter, &k);
            sprintf(label, "aes128 key, iter %d", v[i].iter);
            hexcheck(label, k.data, v[i].key128, 16);

            krb_string_to_key(KRB_ETYPE_AES256_CTS, v[i].pass,
                              (const uint8_t *)v[i].salt, slen,
                              (uint32_t)v[i].iter, &k);
            sprintf(label, "aes256 key, iter %d", v[i].iter);
            hexcheck(label, k.data, v[i].key256, 32);
        }
    }

    printf("\nkey derivation is separated by usage\n");
    {
        /* Not a published vector -- a property. Kc, Ke and Ki for the
         * same message must all differ, and the keys for two different
         * usages must differ, or the separation that makes a blob
         * encrypted for one purpose unusable for another does not
         * exist. A DK that ignored its constant would pass every
         * round-trip test in this file and fail this one. */
        krb_key_t base;
        base.etype = KRB_ETYPE_AES256_CTS;
        base.len = 32;
        for (int i = 0; i < 32; i++) base.data[i] = (uint8_t)i;

        uint8_t kc[32], ke[32], ki[32], ke2[32];
        krb_derive(&base, KRB_KU_AS_REP_ENCPART, KRB_DERIVE_CKSUM, kc);
        krb_derive(&base, KRB_KU_AS_REP_ENCPART, KRB_DERIVE_ENC,   ke);
        krb_derive(&base, KRB_KU_AS_REP_ENCPART, KRB_DERIVE_INT,   ki);
        krb_derive(&base, KRB_KU_TGS_REP_ENCPART, KRB_DERIVE_ENC,  ke2);

        checks++;
        if (memcmp(kc, ke, 32) && memcmp(ke, ki, 32) && memcmp(kc, ki, 32) &&
            memcmp(ke, ke2, 32))
            printf("  ok   Kc, Ke, Ki and a second usage are four distinct keys\n");
        else { fails++; printf("  FAIL derived keys collide\n"); }
    }

    printf("\nencrypt and decrypt are inverses\n");
    {
        krb_key_t key;
        krb_string_to_key(KRB_ETYPE_AES256_CTS, "hunter2",
                          (const uint8_t *)"VEXTRO.TESTada", 14, 4096, &key);

        /* Lengths chosen around the block boundary, because ciphertext
         * stealing is where a length-dependent bug would live. */
        static const int lens[] = { 1, 15, 16, 17, 31, 32, 33, 100 };
        for (unsigned i = 0; i < sizeof lens / sizeof lens[0]; i++) {
            uint8_t plain[128], ct[256], back[256];
            for (int j = 0; j < lens[i]; j++) plain[j] = (uint8_t)(j * 7 + 3);

            int cl = krb_encrypt(&key, KRB_KU_AS_REP_ENCPART, plain,
                                 (uint32_t)lens[i], ct, sizeof ct);
            int pl = cl < 0 ? -1
                   : krb_decrypt(&key, KRB_KU_AS_REP_ENCPART, ct, (uint32_t)cl,
                                 back, sizeof back);
            checks++;
            if (cl == lens[i] + KRB_CONFOUNDER + KRB_MACLEN &&
                pl == lens[i] && !memcmp(plain, back, (size_t)lens[i]))
                printf("  ok   %d bytes -> %d and back\n", lens[i], cl);
            else {
                fails++;
                printf("  FAIL %d bytes: encrypted %d, decrypted %d\n",
                       lens[i], cl, pl);
            }
        }

        /* The tag has to actually be checked. Flipping one bit of the
         * ciphertext must be refused, not decrypted into rubbish. */
        uint8_t plain[32], ct[128], back[128];
        for (int j = 0; j < 32; j++) plain[j] = (uint8_t)j;
        int cl = krb_encrypt(&key, KRB_KU_AS_REP_ENCPART, plain, 32, ct, sizeof ct);
        ct[3] ^= 0x01;
        checks++;
        if (krb_decrypt(&key, KRB_KU_AS_REP_ENCPART, ct, (uint32_t)cl,
                        back, sizeof back) < 0)
            printf("  ok   a flipped ciphertext bit is refused\n");
        else { fails++; printf("  FAIL a corrupted message decrypted\n"); }

        /* And the usage must be part of the key, not decoration. */
        ct[3] ^= 0x01;
        checks++;
        if (krb_decrypt(&key, KRB_KU_TGS_REP_ENCPART, ct, (uint32_t)cl,
                        back, sizeof back) < 0)
            printf("  ok   the wrong key usage does not decrypt\n");
        else { fails++; printf("  FAIL decrypted under the wrong usage\n"); }
    }

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
