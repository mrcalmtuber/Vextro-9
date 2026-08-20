#ifndef KRB5CRYPTO_H
#define KRB5CRYPTO_H

#include <stdint.h>
#include "aes.h"
#include "ntcrypto.h"

/*
 * src/krb5crypto.h — the encryption profile Kerberos is defined in
 * terms of. RFC 3961 for the shape, RFC 3962 for the AES filling.
 *
 * Kerberos does not encrypt with a cipher; it encrypts with an
 * *encryption type*, which is a cipher plus a checksum plus a rule for
 * turning one key into several. All three have to be right together.
 * A message encrypted with the correct cipher under a key derived with
 * the wrong usage number is indistinguishable from one encrypted with
 * the wrong cipher: the KDC replies KRB-ERROR with a code that means
 * "the password is wrong", because from where it is standing that is
 * exactly what it looks like.
 *
 * ---- which encryption types are offered, and which are refused ----
 *
 * aes256-cts-hmac-sha1-96 and aes128-cts-hmac-sha1-96. Nothing else.
 *
 * In particular *not* rc4-hmac, which this system could implement in
 * about forty lines because src/ntcrypto.h already has every piece of
 * it -- RC4, HMAC-MD5, and the NT hash, which in that encryption type
 * *is* the Kerberos key with no salt and no iteration.
 *
 * That is precisely the reason to refuse it. rc4-hmac makes a user's
 * Kerberos key equal to their NTLM hash, so anyone who steals the hash
 * can mint tickets without ever knowing the password, and any service
 * ticket encrypted under it can be taken offline and attacked at the
 * speed of MD4. It is the mechanism behind Kerberoasting. A client that
 * lists it in its request is telling the KDC that it will accept it,
 * and a KDC that can be persuaded to answer in it has been downgraded.
 *
 * So the request offers 18 and 17, in that order, and if a KDC has
 * nothing but RC4 then this client cannot talk to it. That is a real
 * limitation and it is stated rather than quietly worked around --
 * which is the same call src/ntcrypto.h makes about keeping MD4 out of
 * the TLS configuration, for the same reason.
 *
 * ---- what is derived from what ----
 *
 * The long-term key comes from the password by PBKDF2 with four
 * thousand rounds of HMAC-SHA1, and then -- this is the part that is
 * easy to miss -- one more derivation through DK with the constant
 * "kerberos". Stopping at the PBKDF2 output gives a key that is a
 * perfectly good secret and is not the one the KDC has.
 *
 * From that long-term key, every message derives three more, keyed by
 * what the message is for:
 *
 *     Kc = DK(key, usage || 0x99)     checksums
 *     Ke = DK(key, usage || 0xAA)     encryption
 *     Ki = DK(key, usage || 0x55)     the integrity tag on the ciphertext
 *
 * The usage number is the four-byte big-endian purpose of the message
 * (RFC 4120 section 7.5.1). Separating the keys this way is what stops
 * a blob encrypted for one purpose being replayed into another.
 */

/* Encryption types, from the IANA registry. */
#define KRB_ETYPE_AES128_CTS   17
#define KRB_ETYPE_AES256_CTS   18

/* Checksum types that go with them. */
#define KRB_CKSUM_HMAC_SHA1_96_AES128  15
#define KRB_CKSUM_HMAC_SHA1_96_AES256  16

/* The three derivation suffixes. */
#define KRB_DERIVE_CKSUM  0x99
#define KRB_DERIVE_ENC    0xAA
#define KRB_DERIVE_INT    0x55

/* Key usage numbers, RFC 4120 section 7.5.1. Only the ones this client
 * actually sends or receives are named; a number nobody uses is a
 * number nobody can get wrong. */
#define KRB_KU_PA_ENC_TIMESTAMP     1
#define KRB_KU_AS_REP_ENCPART       3
#define KRB_KU_TGS_REQ_AUTH_CKSUM   6
#define KRB_KU_TGS_REQ_AUTH         7
#define KRB_KU_TGS_REP_ENCPART      8
#define KRB_KU_TGS_REP_SUBKEY       9
#define KRB_KU_AP_REQ_AUTH         11

/* The confounder and the truncated HMAC bracket every message. */
#define KRB_CONFOUNDER  16
#define KRB_MACLEN      12

typedef struct {
    int      etype;
    uint8_t  data[32];
    uint32_t len;              /* 16 for aes128, 32 for aes256 */
} krb_key_t;

/* Provided by the kernel (src/vxport_impl.h, RDRAND with a hard failure
 * if the processor will not produce entropy) and by the host test,
 * which supplies a deterministic one so that a round trip is
 * reproducible. */
uint32_t vx_random(uint8_t *out, uint32_t len);

/* ===========================================================
 * n-fold (RFC 3961 section 5.1)
 *
 * Spreads a short string over a longer one so that every input bit
 * influences every output bit. The definition is unusual enough to be
 * worth stating plainly, because every implementation of it looks like
 * a mistake:
 *
 *   Replicate the input until its length is the least common multiple
 *   of the input length and the output length, rotating right by 13
 *   bits before each repetition after the first. Then chop the result
 *   into output-length pieces and add them together with end-around
 *   carry.
 *
 * Thirteen is chosen because it is coprime with 8, so the rotation
 * moves bits across byte boundaries rather than permuting whole bytes.
 * One's-complement addition is chosen because it is the one that
 * carries into the low end rather than discarding the overflow.
 *
 * This works a bit at a time rather than materialising the replicated
 * string. That is not an optimisation -- it is that the replicated
 * string for a five-byte constant folded to sixteen bytes is eighty
 * bytes long, and the bookkeeping to build it is where the usual
 * implementation puts its bugs.
 * =========================================================== */

static void krb_ones_add(uint8_t *acc, const uint8_t *v, uint32_t n) {
    int carry = 0;
    for (int i = (int)n - 1; i >= 0; i--) {
        int s = acc[i] + v[i] + carry;
        acc[i] = (uint8_t)s;
        carry = s >> 8;
    }
    /* End-around: a carry off the top comes back in at the bottom, and
     * may carry again, so this settles rather than adding once. */
    while (carry) {
        for (int i = (int)n - 1; i >= 0; i--) {
            int s = acc[i] + carry;
            acc[i] = (uint8_t)s;
            carry = s >> 8;
            if (!carry) break;
        }
    }
}

static void krb_nfold(const uint8_t *in, uint32_t inlen,
                      uint8_t *out, uint32_t outlen) {
    uint32_t L = inlen * 8, n = outlen * 8;

    uint32_t a = L, b = n;
    while (b) { uint32_t t = a % b; a = b; b = t; }
    uint32_t chunks = (L / a);          /* lcm(L,n)/n reduces to L/gcd */

    for (uint32_t i = 0; i < outlen; i++) out[i] = 0;

    uint8_t chunk[64];
    if (outlen > sizeof chunk) return;

    for (uint32_t c = 0; c < chunks; c++) {
        for (uint32_t i = 0; i < outlen; i++) chunk[i] = 0;
        for (uint32_t j = 0; j < n; j++) {
            uint32_t idx = c * n + j;
            uint32_t r   = idx / L;       /* which repetition */
            uint32_t jj  = idx % L;       /* bit within it */
            /* Repetition r is the input rotated right by 13r, so bit jj
             * of it is bit jj-13r of the original. */
            uint32_t src = (jj + L - (13u * r) % L) % L;
            if ((in[src >> 3] >> (7 - (src & 7))) & 1)
                chunk[j >> 3] |= (uint8_t)(1u << (7 - (j & 7)));
        }
        krb_ones_add(out, chunk, outlen);
    }
}

/* ===========================================================
 * DK and DR (RFC 3961 section 5.1)
 *
 * DR runs the cipher on the n-folded constant, then on its own output,
 * until enough bytes exist; DK is DR passed through random-to-key,
 * which for AES is the identity. The cipher is the profile's own
 * encryption with a zero cipher state, and since the input is exactly
 * one block, ciphertext stealing never engages -- it is a single block
 * encryption, and writing it as one is clearer than pretending
 * otherwise.
 * =========================================================== */

static void krb_dk(const uint8_t *key, uint32_t keylen,
                   const uint8_t *constant, uint32_t clen,
                   uint8_t *out, uint32_t outlen) {
    aes_key_t k;
    aes_setkey(&k, key, (int)keylen * 8);

    uint8_t block[16];
    krb_nfold(constant, clen, block, 16);

    uint32_t done = 0;
    while (done < outlen) {
        aes_encrypt_block(&k, block, block);
        uint32_t take = (outlen - done < 16) ? outlen - done : 16;
        for (uint32_t i = 0; i < take; i++) out[done + i] = block[i];
        done += take;
    }
}

/*
 * The per-message key for one purpose. The constant is five bytes: the
 * usage big-endian, then the byte that says which of the three keys
 * this is.
 */
static void krb_derive(const krb_key_t *base, uint32_t usage, uint8_t kind,
                       uint8_t *out) {
    uint8_t c[5];
    c[0] = (uint8_t)(usage >> 24); c[1] = (uint8_t)(usage >> 16);
    c[2] = (uint8_t)(usage >> 8);  c[3] = (uint8_t)usage;
    c[4] = kind;
    krb_dk(base->data, base->len, c, 5, out, base->len);
}

/* ===========================================================
 * PBKDF2-HMAC-SHA1 (RFC 2898) and string-to-key (RFC 3962 section 4)
 * =========================================================== */

static void krb_pbkdf2(const uint8_t *pass, uint32_t plen,
                       const uint8_t *salt, uint32_t slen,
                       uint32_t iter, uint8_t *out, uint32_t outlen) {
    uint32_t blocks = (outlen + 19) / 20;
    uint8_t  buf[128 + 4];
    if (slen > 128) slen = 128;

    for (uint32_t b = 1; b <= blocks; b++) {
        for (uint32_t i = 0; i < slen; i++) buf[i] = salt[i];
        buf[slen + 0] = (uint8_t)(b >> 24); buf[slen + 1] = (uint8_t)(b >> 16);
        buf[slen + 2] = (uint8_t)(b >> 8);  buf[slen + 3] = (uint8_t)b;

        uint8_t u[20], t[20];
        hmac_sha1(pass, plen, buf, slen + 4, u);
        for (int i = 0; i < 20; i++) t[i] = u[i];

        for (uint32_t c = 1; c < iter; c++) {
            hmac_sha1(pass, plen, u, 20, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }

        uint32_t off  = (b - 1) * 20;
        uint32_t take = (outlen - off < 20) ? outlen - off : 20;
        for (uint32_t i = 0; i < take; i++) out[off + i] = t[i];
    }
}

/*
 * A password and a salt become a key.
 *
 * The salt is not a random value the way it is elsewhere: Kerberos
 * derives it from the principal's own name -- realm followed by each
 * name component, concatenated with no separator -- so that the same
 * password in two realms produces two different keys without anything
 * having to be stored. krb_salt() below builds it.
 *
 * The final DK with "kerberos" is the step that is easiest to leave
 * out, and leaving it out fails in the least helpful possible way: the
 * key is the right length, looks entirely random, and every message
 * encrypted under it is rejected as a bad password.
 */
static int krb_string_to_key(int etype, const char *password,
                             const uint8_t *salt, uint32_t slen,
                             uint32_t iter, krb_key_t *out) {
    uint32_t keylen;
    if (etype == KRB_ETYPE_AES256_CTS)      keylen = 32;
    else if (etype == KRB_ETYPE_AES128_CTS) keylen = 16;
    else return -1;

    if (iter == 0) iter = 4096;             /* the RFC 3962 default */

    uint32_t plen = 0;
    while (password[plen]) plen++;

    uint8_t tkey[32];
    krb_pbkdf2((const uint8_t *)password, plen, salt, slen, iter, tkey, keylen);

    out->etype = etype;
    out->len   = keylen;
    krb_dk(tkey, keylen, (const uint8_t *)"kerberos", 8, out->data, keylen);
    return 0;
}

/* realm || name components, concatenated. "ADA" in "VEXTRO.TEST" salts
 * with "VEXTRO.TESTada" -- the realm keeps its case, the name keeps
 * its own, and neither is uppercased on the way in. */
static uint32_t krb_salt(const char *realm, const char *const *name, int nname,
                         uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    for (const char *p = realm; *p && n < max; p++) out[n++] = (uint8_t)*p;
    for (int i = 0; i < nname; i++)
        for (const char *p = name[i]; *p && n < max; p++) out[n++] = (uint8_t)*p;
    return n;
}

/* ===========================================================
 * encryption and decryption, the simplified profile (RFC 3961 5.3)
 * =========================================================== */

static int krb_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t d = 0;
    for (uint32_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

/*
 * Returns the ciphertext length, or -1.
 *
 * The layout is confounder, then plaintext, then a truncated HMAC over
 * both -- and the HMAC is over the *plaintext*, not the ciphertext.
 * That is encrypt-and-MAC, which is not the construction anyone would
 * choose today, and it is what Kerberos specifies. Deciding to be
 * cleverer than the specification here produces a client that is
 * correct and cannot talk to anything.
 *
 * The confounder is what makes two identical messages encrypt
 * differently. Sixteen random bytes, and if the machine cannot produce
 * them this fails rather than proceeding with a buffer that was never
 * written -- the same rule vx_random() enforces for TLS.
 */
static int krb_encrypt(const krb_key_t *key, uint32_t usage,
                       const uint8_t *plain, uint32_t plen,
                       uint8_t *out, uint32_t outmax) {
    if (key->etype != KRB_ETYPE_AES256_CTS && key->etype != KRB_ETYPE_AES128_CTS)
        return -1;
    uint32_t total = KRB_CONFOUNDER + plen;
    if (total + KRB_MACLEN > outmax) return -1;

    static uint8_t buf[8192];
    if (total > sizeof buf) return -1;

    if (vx_random(buf, KRB_CONFOUNDER) != KRB_CONFOUNDER) return -1;
    for (uint32_t i = 0; i < plen; i++) buf[KRB_CONFOUNDER + i] = plain[i];

    uint8_t ke[32], ki[32];
    krb_derive(key, usage, KRB_DERIVE_ENC, ke);
    krb_derive(key, usage, KRB_DERIVE_INT, ki);

    aes_key_t k;
    aes_setkey(&k, ke, (int)key->len * 8);
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = 0;
    if (aes_cts_encrypt(&k, iv, buf, out, total) != 0) return -1;

    uint8_t mac[20];
    hmac_sha1(ki, key->len, buf, total, mac);
    for (int i = 0; i < KRB_MACLEN; i++) out[total + i] = mac[i];

    return (int)(total + KRB_MACLEN);
}

/* Returns the plaintext length, or -1 if the tag does not verify. */
static int krb_decrypt(const krb_key_t *key, uint32_t usage,
                       const uint8_t *cipher, uint32_t clen,
                       uint8_t *out, uint32_t outmax) {
    if (key->etype != KRB_ETYPE_AES256_CTS && key->etype != KRB_ETYPE_AES128_CTS)
        return -1;
    if (clen < KRB_CONFOUNDER + KRB_MACLEN) return -1;

    uint32_t total = clen - KRB_MACLEN;
    static uint8_t buf[8192];
    if (total > sizeof buf) return -1;

    uint8_t ke[32], ki[32];
    krb_derive(key, usage, KRB_DERIVE_ENC, ke);
    krb_derive(key, usage, KRB_DERIVE_INT, ki);

    aes_key_t k;
    aes_setkey(&k, ke, (int)key->len * 8);
    uint8_t iv[16];
    for (int i = 0; i < 16; i++) iv[i] = 0;
    if (aes_cts_decrypt(&k, iv, cipher, buf, total) != 0) return -1;

    uint8_t mac[20];
    hmac_sha1(ki, key->len, buf, total, mac);
    /* Compared in constant time and *before* the plaintext is handed
     * back, so a caller cannot be tempted to look at bytes that have
     * not been authenticated. */
    if (!krb_ct_equal(mac, cipher + total, KRB_MACLEN)) return -1;

    uint32_t plen = total - KRB_CONFOUNDER;
    if (plen > outmax) return -1;
    for (uint32_t i = 0; i < plen; i++) out[i] = buf[KRB_CONFOUNDER + i];
    return (int)plen;
}

/* The keyed checksum an Authenticator carries, hmac-sha1-96-aes*. */
static void krb_checksum(const krb_key_t *key, uint32_t usage,
                         const uint8_t *msg, uint32_t mlen,
                         uint8_t out[KRB_MACLEN]) {
    uint8_t kc[32], mac[20];
    krb_derive(key, usage, KRB_DERIVE_CKSUM, kc);
    hmac_sha1(kc, key->len, msg, mlen, mac);
    for (int i = 0; i < KRB_MACLEN; i++) out[i] = mac[i];
}

static int krb_cksumtype(const krb_key_t *key) {
    return key->etype == KRB_ETYPE_AES256_CTS ? KRB_CKSUM_HMAC_SHA1_96_AES256
                                              : KRB_CKSUM_HMAC_SHA1_96_AES128;
}

#endif /* KRB5CRYPTO_H */
