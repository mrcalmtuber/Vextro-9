#ifndef NET_WPA2_H
#define NET_WPA2_H

/*
 * src/net/wpa2.h — the 4-way handshake, and the keys it produces.
 *
 * ---- what this is for ----
 *
 * A WPA2 network does not send the password over the air, and does not
 * send the encryption key over the air either. Both ends already know a
 * secret -- the passphrase -- and the handshake's whole job is to turn
 * that shared secret into a *fresh* key for this one association, prove
 * to each side that the other really has the secret, and do it without
 * either side transmitting anything an eavesdropper could replay.
 *
 * The chain is:
 *
 *   passphrase + SSID  --PBKDF2-SHA1, 4096 rounds-->  PMK   (32 bytes)
 *   PMK + both MACs + both nonces  --PRF-SHA1-->      PTK   (48 bytes)
 *   PTK  --split-->  KCK (16) | KEK (16) | TK (16)
 *
 * KCK signs the handshake messages, KEK encrypts the group key the AP
 * sends inside message 3, and TK is the key that actually encrypts data
 * frames with CCMP. The PMK is the only value derived from the
 * password, it is the same for every station on the network, and it is
 * the expensive one -- 4096 iterations of HMAC-SHA1 over two blocks --
 * which is why it is computed once at init and cached.
 *
 * ---- what an attacker gets ----
 *
 * Everything transmitted during the handshake is in the clear: both
 * nonces, both MAC addresses, and the MICs. That is enough to mount an
 * offline dictionary attack -- capture one handshake, guess a
 * passphrase, derive the PMK and PTK, and check whether the MIC on
 * message 2 comes out right. Nothing in this file prevents that and
 * nothing can; it is a property of WPA2-PSK. What the 4096 iterations
 * buy is that each guess costs about a millisecond instead of a
 * microsecond. A passphrase from a word list is found regardless.
 *
 * ---- byte order ----
 *
 * EAPOL is 802.1X, which is big-endian, unlike the 802.11 frames in
 * net/ieee80211.h that carry it. Every multi-byte field below --
 * key info, key length, key data length, the replay counter -- is
 * network order. This is the single most common place to get a WPA2
 * implementation subtly wrong, because a byte-swapped key_info still
 * parses, still has plausible-looking bits set, and fails only at the
 * MIC with no clue as to why.
 */

#include <stdint.h>
#include "aes.h"
#include "ntcrypto.h"
#include "sha256.h"

/* ===== EAPOL ===== */

#define EAPOL_ETHERTYPE         0x888E

#define EAPOL_TYPE_EAP          0
#define EAPOL_TYPE_START        1
#define EAPOL_TYPE_LOGOFF       2
#define EAPOL_TYPE_KEY          3

#define EAPOL_KEY_DESC_RC4      1     /* the original WPA descriptor  */
#define EAPOL_KEY_DESC_RSN      2     /* 802.11i / WPA2               */
#define EAPOL_KEY_DESC_WPA      254   /* WPA1 vendor descriptor       */

/* key_info bits, in host order after the field is byte-swapped */
#define WPA_KEY_INFO_TYPE_MASK      0x0007
#define WPA_KEY_INFO_TYPE_HMAC_MD5  1   /* + RC4 key data             */
#define WPA_KEY_INFO_TYPE_HMAC_SHA1 2   /* + AES key wrap             */
#define WPA_KEY_INFO_TYPE_AES_CMAC  3   /* + AES key wrap             */
#define WPA_KEY_INFO_PAIRWISE       0x0008
#define WPA_KEY_INFO_IDX_MASK       0x0030
#define WPA_KEY_INFO_IDX_SHIFT      4
#define WPA_KEY_INFO_INSTALL        0x0040
#define WPA_KEY_INFO_ACK            0x0080
#define WPA_KEY_INFO_MIC            0x0100
#define WPA_KEY_INFO_SECURE         0x0200
#define WPA_KEY_INFO_ERROR          0x0400
#define WPA_KEY_INFO_REQUEST        0x0800
#define WPA_KEY_INFO_ENCR_KEY_DATA  0x1000
#define WPA_KEY_INFO_SMK_MESSAGE    0x2000

#define WPA_NONCE_LEN           32
#define WPA_PMK_LEN             32
#define WPA_KCK_LEN             16
#define WPA_KEK_LEN             16
#define WPA_TK_LEN_CCMP         16
#define WPA_PTK_LEN_CCMP        48
#define WPA_PTK_LEN_TKIP        64
#define WPA_MIC_LEN             16
#define WPA_REPLAY_LEN          8

/*
 * The EAPOL-Key frame.
 *
 * Laid out as a packed struct because every field is at a fixed offset
 * and the frame is exactly what goes on the wire. The two-byte fields
 * are held as byte pairs rather than uint16_t so that the big-endian
 * conversion is explicit at every use and cannot be forgotten by a
 * compiler that would have byte-swapped for free on one architecture
 * and not on another.
 */
typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t length[2];
    uint8_t descriptor_type;
    uint8_t key_info[2];
    uint8_t key_length[2];
    uint8_t replay_counter[WPA_REPLAY_LEN];
    uint8_t key_nonce[WPA_NONCE_LEN];
    uint8_t key_iv[16];
    uint8_t key_rsc[8];
    uint8_t key_id[8];
    uint8_t key_mic[WPA_MIC_LEN];
    uint8_t key_data_length[2];
    /* key data follows */
} __attribute__((packed)) eapol_key_t;

#define EAPOL_KEY_HDR_LEN   99      /* 4 of 802.1X + 95 of key descriptor */

static inline uint16_t wpa_get_be16(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline void wpa_put_be16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
}

static void wpa_memcpy(uint8_t *d, const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static void wpa_memset(uint8_t *d, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) d[i] = v;
}

/*
 * Constant-time comparison.
 *
 * Used for every MIC check. A byte-at-a-time memcmp that returns early
 * leaks, through timing, how many leading bytes of a forged MIC were
 * right -- which turns a 2^128 forgery into sixteen searches of 256.
 * The handshake is not a high-rate oracle, so the practical risk here
 * is small, but the correct comparison costs nothing.
 */
static int wpa_ct_equal(const uint8_t *a, const uint8_t *b, uint32_t n) {
    uint8_t diff = 0;
    for (uint32_t i = 0; i < n; i++) diff = (uint8_t)(diff | (a[i] ^ b[i]));
    return diff == 0;
}

/* ===== PBKDF2-HMAC-SHA1: the passphrase becomes the PMK =====
 *
 * This is the same construction as krb_pbkdf2() in krb5crypto.h and is
 * written out again rather than shared, because pulling the Kerberos
 * encryption profile into the wireless stack to reach fifteen lines of
 * RFC 2898 would make every machine that associates with an access
 * point also carry a KDC client. The two are checked against their own
 * published vectors independently.
 */
static void wpa_pbkdf2_sha1(const uint8_t *pass, uint32_t plen,
                            const uint8_t *salt, uint32_t slen,
                            uint32_t iter, uint8_t *out, uint32_t outlen) {
    uint32_t blocks = (outlen + 19) / 20;
    uint8_t  buf[64 + 4];

    if (slen > 64) slen = 64;

    for (uint32_t b = 1; b <= blocks; b++) {
        uint8_t u[20], t[20];

        for (uint32_t i = 0; i < slen; i++) buf[i] = salt[i];
        buf[slen + 0] = (uint8_t)(b >> 24);
        buf[slen + 1] = (uint8_t)(b >> 16);
        buf[slen + 2] = (uint8_t)(b >> 8);
        buf[slen + 3] = (uint8_t)b;

        hmac_sha1(pass, plen, buf, slen + 4, u);
        for (int i = 0; i < 20; i++) t[i] = u[i];

        for (uint32_t c = 1; c < iter; c++) {
            hmac_sha1(pass, plen, u, 20, u);
            for (int i = 0; i < 20; i++) t[i] = (uint8_t)(t[i] ^ u[i]);
        }

        {
            uint32_t off  = (b - 1) * 20;
            uint32_t take = (outlen - off < 20) ? outlen - off : 20;
            for (uint32_t i = 0; i < take; i++) out[off + i] = t[i];
        }
    }
}

static uint32_t wpa_strlen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

/*
 * PMK = PBKDF2(passphrase, SSID, 4096, 256 bits).
 *
 * The SSID is the salt, which is why two networks with the same
 * password but different names have different PMKs -- and why a
 * precomputed table has to be built per SSID. It is also why the
 * twenty most common network names have public rainbow tables.
 */
static void wpa_pmk_from_passphrase(const char *passphrase,
                                    const uint8_t *ssid, uint32_t ssid_len,
                                    uint8_t pmk[WPA_PMK_LEN]) {
    wpa_pbkdf2_sha1((const uint8_t *)passphrase, wpa_strlen(passphrase),
                    ssid, ssid_len, 4096, pmk, WPA_PMK_LEN);
}

/* ===== the pseudo-random function ===== */

/*
 * PRF-n, as defined in IEEE 802.11i.
 *
 * HMAC-SHA1 over (label || 0x00 || data || counter), counter starting
 * at zero and incrementing per 20-byte block. The single zero byte
 * between the label and the data is not padding: it is what keeps a
 * label that is a prefix of another label from producing a colliding
 * input, and it is omitted by roughly every implementation that gets
 * this wrong on the first try.
 */
static void wpa_prf_sha1(const uint8_t *key, uint32_t key_len,
                         const char *label,
                         const uint8_t *data, uint32_t data_len,
                         uint8_t *out, uint32_t out_len) {
    uint32_t label_len = wpa_strlen(label);
    uint8_t  buf[256];
    uint8_t  hash[20];
    uint8_t  counter = 0;
    uint32_t pos = 0;

    if (label_len + 1 + data_len + 1 > sizeof(buf)) return;

    for (uint32_t i = 0; i < label_len; i++) buf[i] = (uint8_t)label[i];
    buf[label_len] = 0x00;
    for (uint32_t i = 0; i < data_len; i++) buf[label_len + 1 + i] = data[i];

    while (pos < out_len) {
        uint32_t n = out_len - pos;
        buf[label_len + 1 + data_len] = counter;
        hmac_sha1(key, key_len, buf, label_len + 1 + data_len + 1, hash);
        if (n > 20) n = 20;
        for (uint32_t i = 0; i < n; i++) out[pos + i] = hash[i];
        pos += n;
        counter++;
    }
}

/*
 * The SHA-256 KDF from 802.11-2012, used by the SHA256 AKMs.
 *
 * A different construction from the SHA-1 PRF above, not just a
 * different hash: the counter comes first and is two bytes little-
 * endian, and the output length in *bits* is appended. Substituting
 * SHA-256 into the older PRF produces plausible-looking keys that no
 * access point agrees with.
 */
static void wpa_kdf_sha256(const uint8_t *key, uint32_t key_len,
                           const char *label,
                           const uint8_t *data, uint32_t data_len,
                           uint8_t *out, uint32_t out_len) {
    uint32_t label_len = wpa_strlen(label);
    uint8_t  buf[256];
    uint8_t  hash[32];
    uint16_t counter = 1;
    uint32_t pos = 0;
    uint32_t bits = out_len * 8;

    if (2 + label_len + data_len + 2 > sizeof(buf)) return;

    for (uint32_t i = 0; i < label_len; i++) buf[2 + i] = (uint8_t)label[i];
    for (uint32_t i = 0; i < data_len; i++)
        buf[2 + label_len + i] = data[i];
    buf[2 + label_len + data_len]     = (uint8_t)(bits & 0xFF);
    buf[2 + label_len + data_len + 1] = (uint8_t)(bits >> 8);

    while (pos < out_len) {
        uint32_t n = out_len - pos;
        buf[0] = (uint8_t)(counter & 0xFF);
        buf[1] = (uint8_t)(counter >> 8);
        hmac_sha256(key, key_len, buf, 2 + label_len + data_len + 2, hash);
        if (n > 32) n = 32;
        for (uint32_t i = 0; i < n; i++) out[pos + i] = hash[i];
        pos += n;
        counter++;
    }
}

/*
 * PTK = PRF(PMK, "Pairwise key expansion",
 *           min(AA,SPA) || max(AA,SPA) || min(ANonce,SNonce) || max(...))
 *
 * The min/max ordering is what makes both ends derive the same key
 * without having to agree on who is who: the authenticator sorts the
 * same pair the same way the supplicant does. Sorting is by unsigned
 * byte comparison, and a signed char here silently breaks every
 * network whose AP has a MAC address with the high bit set -- which is
 * half of them.
 */
static int wpa_mem_lt(const uint8_t *a, const uint8_t *b, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] < b[i];
    }
    return 0;
}

static void wpa_derive_ptk(const uint8_t *pmk, uint32_t pmk_len,
                           const uint8_t aa[6], const uint8_t spa[6],
                           const uint8_t anonce[WPA_NONCE_LEN],
                           const uint8_t snonce[WPA_NONCE_LEN],
                           uint8_t *ptk, uint32_t ptk_len,
                           int use_sha256) {
    uint8_t data[2 * 6 + 2 * WPA_NONCE_LEN];
    uint32_t pos = 0;

    if (wpa_mem_lt(aa, spa, 6)) {
        wpa_memcpy(data + pos, aa, 6);  pos += 6;
        wpa_memcpy(data + pos, spa, 6); pos += 6;
    } else {
        wpa_memcpy(data + pos, spa, 6); pos += 6;
        wpa_memcpy(data + pos, aa, 6);  pos += 6;
    }

    if (wpa_mem_lt(anonce, snonce, WPA_NONCE_LEN)) {
        wpa_memcpy(data + pos, anonce, WPA_NONCE_LEN); pos += WPA_NONCE_LEN;
        wpa_memcpy(data + pos, snonce, WPA_NONCE_LEN); pos += WPA_NONCE_LEN;
    } else {
        wpa_memcpy(data + pos, snonce, WPA_NONCE_LEN); pos += WPA_NONCE_LEN;
        wpa_memcpy(data + pos, anonce, WPA_NONCE_LEN); pos += WPA_NONCE_LEN;
    }

    if (use_sha256)
        wpa_kdf_sha256(pmk, pmk_len, "Pairwise key expansion",
                       data, pos, ptk, ptk_len);
    else
        wpa_prf_sha1(pmk, pmk_len, "Pairwise key expansion",
                     data, pos, ptk, ptk_len);
}

/*
 * The PMKID, which message 1 may carry so a station can tell whether
 * the AP already holds a cached PMK for it.
 *
 *   PMKID = HMAC-SHA1-128(PMK, "PMK Name" || AA || SPA)
 */
static void wpa_derive_pmkid(const uint8_t *pmk, uint32_t pmk_len,
                             const uint8_t aa[6], const uint8_t spa[6],
                             uint8_t pmkid[16]) {
    uint8_t buf[8 + 6 + 6];
    uint8_t hash[20];
    const char *label = "PMK Name";

    for (int i = 0; i < 8; i++) buf[i] = (uint8_t)label[i];
    wpa_memcpy(buf + 8, aa, 6);
    wpa_memcpy(buf + 14, spa, 6);

    hmac_sha1(pmk, pmk_len, buf, sizeof(buf), hash);
    wpa_memcpy(pmkid, hash, 16);
}

/* ===== the MIC over an EAPOL-Key frame ===== */

/*
 * Compute the MIC over a complete EAPOL frame with the MIC field
 * zeroed. `frame` points at the 802.1X version byte, not at the key
 * descriptor -- the four bytes of 802.1X header are covered, and
 * leaving them out is another way to produce a MIC that is wrong every
 * time and diagnosable only by comparison with a working supplicant.
 */
static int wpa_eapol_mic(int key_desc_ver,
                         const uint8_t *kck, uint32_t kck_len,
                         const uint8_t *frame, uint32_t frame_len,
                         uint8_t mic[WPA_MIC_LEN]) {
    uint8_t hash[32];

    switch (key_desc_ver) {
    case WPA_KEY_INFO_TYPE_HMAC_MD5:
        hmac_md5(kck, kck_len, frame, frame_len, mic);
        return 0;

    case WPA_KEY_INFO_TYPE_HMAC_SHA1:
        hmac_sha1(kck, kck_len, frame, frame_len, hash);
        wpa_memcpy(mic, hash, WPA_MIC_LEN);   /* truncated to 128 bits */
        return 0;

    case WPA_KEY_INFO_TYPE_AES_CMAC: {
        aes_key_t k;
        if (aes_setkey(&k, kck, 128) != 0) return -1;
        aes_cmac(&k, frame, frame_len, mic);
        return 0;
    }

    default:
        return -1;
    }
}

/*
 * Verify the MIC on a received frame.
 *
 * The frame is not modified: the MIC field is saved, zeroed in a scratch
 * copy, and restored. Modifying the caller's buffer in place would work
 * for the handshake but makes retransmission handling a trap.
 */
static int wpa_eapol_check_mic(int key_desc_ver,
                               const uint8_t *kck, uint32_t kck_len,
                               uint8_t *frame, uint32_t frame_len) {
    eapol_key_t *k = (eapol_key_t *)frame;
    uint8_t saved[WPA_MIC_LEN];
    uint8_t computed[WPA_MIC_LEN];
    int ok;

    if (frame_len < EAPOL_KEY_HDR_LEN) return 0;

    wpa_memcpy(saved, k->key_mic, WPA_MIC_LEN);
    wpa_memset(k->key_mic, 0, WPA_MIC_LEN);

    if (wpa_eapol_mic(key_desc_ver, kck, kck_len, frame, frame_len,
                      computed) != 0) {
        wpa_memcpy(k->key_mic, saved, WPA_MIC_LEN);
        return 0;
    }

    ok = wpa_ct_equal(saved, computed, WPA_MIC_LEN);
    wpa_memcpy(k->key_mic, saved, WPA_MIC_LEN);
    return ok;
}

/* ===== AES key wrap (RFC 3394) =====
 *
 * How the group key travels inside message 3. The KEK wraps it, and the
 * wrap carries its own integrity check -- the fixed initial value A6
 * repeated -- so a group key that was tampered with in flight fails to
 * unwrap rather than being installed as garbage.
 */

static int aes_key_unwrap(const uint8_t *kek, uint32_t kek_len,
                          const uint8_t *in, uint32_t in_len,
                          uint8_t *out) {
    aes_key_t k;
    uint8_t   a[8];
    uint8_t   b[16];
    uint32_t  n;

    if (in_len < 24 || (in_len % 8) != 0) return -1;
    n = in_len / 8 - 1;

    if (aes_setkey(&k, kek, (int)(kek_len * 8)) != 0) return -1;

    wpa_memcpy(a, in, 8);
    wpa_memcpy(out, in + 8, n * 8);

    for (int j = 5; j >= 0; j--) {
        for (uint32_t i = n; i >= 1; i--) {
            uint32_t t = n * (uint32_t)j + i;
            uint8_t *r = out + (i - 1) * 8;

            wpa_memcpy(b, a, 8);
            /* t is at most 6n and n is small here, so only the low four
             * bytes of the counter can ever be non-zero -- but it is
             * XORed in full width so a large key data field does not
             * silently wrap. */
            b[7] = (uint8_t)(b[7] ^ (t & 0xFF));
            b[6] = (uint8_t)(b[6] ^ ((t >> 8) & 0xFF));
            b[5] = (uint8_t)(b[5] ^ ((t >> 16) & 0xFF));
            b[4] = (uint8_t)(b[4] ^ ((t >> 24) & 0xFF));
            wpa_memcpy(b + 8, r, 8);

            aes_decrypt_block(&k, b, b);

            wpa_memcpy(a, b, 8);
            wpa_memcpy(r, b + 8, 8);
        }
    }

    for (int i = 0; i < 8; i++)
        if (a[i] != 0xA6) return -1;

    return (int)(n * 8);
}

static int aes_key_wrap(const uint8_t *kek, uint32_t kek_len,
                        const uint8_t *in, uint32_t in_len,
                        uint8_t *out) {
    aes_key_t k;
    uint8_t   a[8];
    uint8_t   b[16];
    uint32_t  n;

    if (in_len < 16 || (in_len % 8) != 0) return -1;
    n = in_len / 8;

    if (aes_setkey(&k, kek, (int)(kek_len * 8)) != 0) return -1;

    for (int i = 0; i < 8; i++) a[i] = 0xA6;
    wpa_memcpy(out + 8, in, in_len);

    for (uint32_t j = 0; j <= 5; j++) {
        for (uint32_t i = 1; i <= n; i++) {
            uint32_t t = n * j + i;
            uint8_t *r = out + i * 8;

            wpa_memcpy(b, a, 8);
            wpa_memcpy(b + 8, r, 8);
            aes_encrypt_block(&k, b, b);

            wpa_memcpy(a, b, 8);
            a[7] = (uint8_t)(a[7] ^ (t & 0xFF));
            a[6] = (uint8_t)(a[6] ^ ((t >> 8) & 0xFF));
            a[5] = (uint8_t)(a[5] ^ ((t >> 16) & 0xFF));
            a[4] = (uint8_t)(a[4] ^ ((t >> 24) & 0xFF));
            wpa_memcpy(r, b + 8, 8);
        }
    }

    wpa_memcpy(out, a, 8);
    return (int)(in_len + 8);
}

/* ===== key data elements ===== */

#define WPA_KDE_TYPE_GTK        1
#define WPA_KDE_TYPE_MAC        3
#define WPA_KDE_TYPE_PMKID      4
#define WPA_KDE_TYPE_IGTK       9

/*
 * Pull the group key out of the decrypted key data of message 3.
 *
 * Key data is a chain of the same TLVs an 802.11 frame uses, with
 * vendor-specific (221) entries carrying an OUI and type. The GTK
 * arrives as OUI 00-0F-AC type 1, whose first byte holds the key index
 * in its low two bits. After the AES unwrap the field is padded with a
 * 0xDD followed by zeros, which is not a real element and must not be
 * parsed as one.
 */
static int wpa_parse_gtk_kde(const uint8_t *data, uint32_t len,
                             uint8_t *gtk, uint32_t *gtk_len,
                             uint8_t *key_id) {
    uint32_t pos = 0;

    while (pos + 2 <= len) {
        uint8_t eid  = data[pos];
        uint8_t elen = data[pos + 1];

        if (eid == 0xDD && elen == 0) break;          /* padding        */
        if (pos + 2 + (uint32_t)elen > len) break;    /* truncated      */

        if (eid == 221 && elen >= 8 &&
            data[pos + 2] == 0x00 && data[pos + 3] == 0x0F &&
            data[pos + 4] == 0xAC && data[pos + 5] == WPA_KDE_TYPE_GTK) {
            /* The element body is OUI(3) type(1) keyid(1) reserved(1)
             * and then the key itself, so the key is elen-6 bytes and
             * starts eight in from the element's first octet. */
            uint32_t n = (uint32_t)elen - 6;
            if (n > 32) n = 32;
            *key_id = (uint8_t)(data[pos + 6] & 0x03);
            wpa_memcpy(gtk, data + pos + 8, n);
            *gtk_len = n;
            return 1;
        }

        pos += 2 + (uint32_t)elen;
    }
    return 0;
}

/* ===== the supplicant state machine ===== */

typedef enum {
    WPA_SM_IDLE = 0,
    WPA_SM_PTK_START,       /* message 1 seen, message 2 sent */
    WPA_SM_PTK_DONE,        /* message 3 seen, message 4 sent */
    WPA_SM_FAILED
} wpa_sm_state_t;

typedef struct {
    wpa_sm_state_t state;

    uint8_t  pmk[WPA_PMK_LEN];
    uint8_t  ptk[WPA_PTK_LEN_TKIP];
    uint32_t ptk_len;

    const uint8_t *kck;     /* into ptk[0]  */
    const uint8_t *kek;     /* into ptk[16] */
    const uint8_t *tk;      /* into ptk[32] */
    uint32_t tk_len;

    uint8_t  anonce[WPA_NONCE_LEN];
    uint8_t  snonce[WPA_NONCE_LEN];
    uint8_t  aa[6];         /* the AP  */
    uint8_t  spa[6];        /* us      */

    uint8_t  gtk[32];
    uint32_t gtk_len;
    uint8_t  gtk_id;
    int      gtk_valid;

    uint8_t  replay[WPA_REPLAY_LEN];
    int      have_replay;

    int      key_desc_ver;
    int      use_sha256;

    uint8_t  rsn_ie[64];
    uint8_t  rsn_ie_len;

    /* CCMP packet numbers. Transmit counts up from one; receive keeps
     * the highest seen so a replayed frame can be dropped. */
    uint64_t tx_pn;
    uint64_t rx_pn;

    uint32_t failures;
} wpa_sm_t;

/*
 * Set up a supplicant. The PMK is computed here, which costs 8192
 * HMAC-SHA1 operations -- a few milliseconds -- and is why this is not
 * called from a packet path.
 */
static void wpa_sm_init(wpa_sm_t *sm,
                        const char *passphrase,
                        const uint8_t *ssid, uint32_t ssid_len,
                        const uint8_t aa[6], const uint8_t spa[6],
                        const uint8_t *rsn_ie, uint8_t rsn_ie_len) {
    wpa_memset((uint8_t *)sm, 0, sizeof(*sm));

    wpa_pmk_from_passphrase(passphrase, ssid, ssid_len, sm->pmk);

    wpa_memcpy(sm->aa, aa, 6);
    wpa_memcpy(sm->spa, spa, 6);

    if (rsn_ie_len > sizeof(sm->rsn_ie)) rsn_ie_len = sizeof(sm->rsn_ie);
    wpa_memcpy(sm->rsn_ie, rsn_ie, rsn_ie_len);
    sm->rsn_ie_len = rsn_ie_len;

    sm->ptk_len = WPA_PTK_LEN_CCMP;
    sm->tk_len  = WPA_TK_LEN_CCMP;
    sm->kck = sm->ptk;
    sm->kek = sm->ptk + WPA_KCK_LEN;
    sm->tk  = sm->ptk + WPA_KCK_LEN + WPA_KEK_LEN;
    sm->tx_pn = 1;
    sm->state = WPA_SM_IDLE;
}

/* Set the PMK directly, for 802.1X networks where it came from the
 * authentication server rather than a passphrase. */
static void wpa_sm_set_pmk(wpa_sm_t *sm, const uint8_t *pmk, uint32_t len) {
    if (len > WPA_PMK_LEN) len = WPA_PMK_LEN;
    wpa_memcpy(sm->pmk, pmk, len);
}

/*
 * Lay out an EAPOL-Key reply. Returns the total frame length.
 *
 * The MIC is computed last, over the finished frame with the MIC field
 * still zero, which is why it cannot be filled in as the fields are
 * written.
 */
static uint32_t wpa_build_eapol_key(wpa_sm_t *sm, uint8_t *out, uint32_t max,
                                    uint16_t key_info,
                                    const uint8_t *nonce,
                                    const uint8_t *key_data,
                                    uint16_t key_data_len,
                                    int with_mic) {
    eapol_key_t *k = (eapol_key_t *)out;
    uint32_t total = EAPOL_KEY_HDR_LEN + key_data_len;

    if (total > max) return 0;
    wpa_memset(out, 0, total);

    k->version = 2;                       /* 802.1X-2004 */
    k->type    = EAPOL_TYPE_KEY;
    wpa_put_be16(k->length, (uint16_t)(total - 4));

    k->descriptor_type = EAPOL_KEY_DESC_RSN;
    wpa_put_be16(k->key_info, key_info);
    wpa_put_be16(k->key_length, (uint16_t)sm->tk_len);
    wpa_memcpy(k->replay_counter, sm->replay, WPA_REPLAY_LEN);

    if (nonce) wpa_memcpy(k->key_nonce, nonce, WPA_NONCE_LEN);

    wpa_put_be16(k->key_data_length, key_data_len);
    if (key_data_len && key_data)
        wpa_memcpy(out + EAPOL_KEY_HDR_LEN, key_data, key_data_len);

    if (with_mic)
        wpa_eapol_mic(sm->key_desc_ver, sm->kck, WPA_KCK_LEN,
                      out, total, k->key_mic);

    return total;
}

/*
 * Feed the state machine one received EAPOL-Key frame; it writes the
 * reply into `out` and returns its length, or 0 if there is nothing to
 * send.
 *
 * `frame` must be writable: verifying the MIC requires zeroing the MIC
 * field, and the frame is restored before return.
 *
 * Retransmission is normal here. An AP that does not see message 2
 * resends message 1 with a fresh ANonce, and an AP that does not see
 * message 4 resends message 3 with the same one. Both are handled by
 * treating each message as a function of its contents rather than
 * requiring a strict sequence -- a supplicant that insists on seeing
 * each message exactly once fails on any lossy link.
 */
static uint32_t wpa_sm_rx_eapol(wpa_sm_t *sm,
                                uint8_t *frame, uint32_t len,
                                uint8_t *out, uint32_t max,
                                int (*get_random)(uint8_t *, uint32_t)) {
    eapol_key_t *k = (eapol_key_t *)frame;
    uint16_t key_info, key_data_len;
    const uint8_t *key_data;

    if (len < EAPOL_KEY_HDR_LEN) return 0;
    if (k->type != EAPOL_TYPE_KEY) return 0;
    if (k->descriptor_type != EAPOL_KEY_DESC_RSN &&
        k->descriptor_type != EAPOL_KEY_DESC_WPA) return 0;

    key_info     = wpa_get_be16(k->key_info);
    key_data_len = wpa_get_be16(k->key_data_length);
    key_data     = frame + EAPOL_KEY_HDR_LEN;

    if ((uint32_t)EAPOL_KEY_HDR_LEN + key_data_len > len) return 0;

    sm->key_desc_ver = key_info & WPA_KEY_INFO_TYPE_MASK;
    if (sm->key_desc_ver == WPA_KEY_INFO_TYPE_AES_CMAC) sm->use_sha256 = 1;

    /* A frame that is not about the pairwise key is a group rekey,
     * handled separately below. */
    if (!(key_info & WPA_KEY_INFO_PAIRWISE)) {
        if (!(key_info & WPA_KEY_INFO_MIC)) return 0;
        if (sm->state != WPA_SM_PTK_DONE) return 0;
        if (!wpa_eapol_check_mic(sm->key_desc_ver, sm->kck, WPA_KCK_LEN,
                                 frame, len)) {
            sm->failures++;
            return 0;
        }

        wpa_memcpy(sm->replay, k->replay_counter, WPA_REPLAY_LEN);

        if (key_info & WPA_KEY_INFO_ENCR_KEY_DATA) {
            uint8_t plain[256];
            int n;
            if (key_data_len > sizeof(plain) + 8 || key_data_len < 24)
                return 0;
            n = aes_key_unwrap(sm->kek, WPA_KEK_LEN, key_data,
                               key_data_len, plain);
            if (n < 0) { sm->failures++; return 0; }
            if (wpa_parse_gtk_kde(plain, (uint32_t)n, sm->gtk,
                                  &sm->gtk_len, &sm->gtk_id))
                sm->gtk_valid = 1;
        }

        /* Acknowledge the rekey. */
        return wpa_build_eapol_key(sm, out, max,
                                   (uint16_t)(sm->key_desc_ver |
                                              WPA_KEY_INFO_MIC |
                                              WPA_KEY_INFO_SECURE),
                                   0, 0, 0, 1);
    }

    /* ---- message 1: has Ack, no MIC ---- */
    if ((key_info & WPA_KEY_INFO_ACK) && !(key_info & WPA_KEY_INFO_MIC)) {
        uint8_t mic_flag;

        wpa_memcpy(sm->anonce, k->key_nonce, WPA_NONCE_LEN);
        wpa_memcpy(sm->replay, k->replay_counter, WPA_REPLAY_LEN);
        sm->have_replay = 1;

        /* A fresh SNonce per handshake is what stops an attacker who
         * recorded an old exchange from replaying our message 2 into a
         * new one and getting the same PTK back. */
        if (!get_random || get_random(sm->snonce, WPA_NONCE_LEN) !=
            (int)WPA_NONCE_LEN) {
            sm->state = WPA_SM_FAILED;
            return 0;
        }

        wpa_derive_ptk(sm->pmk, WPA_PMK_LEN, sm->aa, sm->spa,
                       sm->anonce, sm->snonce,
                       sm->ptk, sm->ptk_len, sm->use_sha256);

        sm->state = WPA_SM_PTK_START;
        mic_flag = 1;

        return wpa_build_eapol_key(sm, out, max,
                                   (uint16_t)(sm->key_desc_ver |
                                              WPA_KEY_INFO_PAIRWISE |
                                              WPA_KEY_INFO_MIC),
                                   sm->snonce,
                                   sm->rsn_ie, sm->rsn_ie_len, mic_flag);
    }

    /* ---- message 3: Ack, MIC and Install ---- */
    if ((key_info & WPA_KEY_INFO_ACK) && (key_info & WPA_KEY_INFO_MIC)) {
        if (sm->state != WPA_SM_PTK_START && sm->state != WPA_SM_PTK_DONE)
            return 0;

        /* The ANonce must be the one from message 1. An AP that
         * changed it is either confused or is a second party trying to
         * splice two handshakes together. */
        if (!wpa_ct_equal(k->key_nonce, sm->anonce, WPA_NONCE_LEN)) {
            sm->failures++;
            return 0;
        }

        if (!wpa_eapol_check_mic(sm->key_desc_ver, sm->kck, WPA_KCK_LEN,
                                 frame, len)) {
            /* This is the case that means the passphrase is wrong.
             * Nothing else in the handshake distinguishes a bad
             * password from a bad frame, because with the wrong PMK
             * every derived key is wrong and the MIC is the first
             * thing that checks one. */
            sm->failures++;
            sm->state = WPA_SM_FAILED;
            return 0;
        }

        wpa_memcpy(sm->replay, k->replay_counter, WPA_REPLAY_LEN);

        if ((key_info & WPA_KEY_INFO_ENCR_KEY_DATA) && key_data_len >= 24) {
            uint8_t plain[256];
            int n;
            if (key_data_len > sizeof(plain) + 8) return 0;
            n = aes_key_unwrap(sm->kek, WPA_KEK_LEN, key_data,
                               key_data_len, plain);
            if (n < 0) { sm->failures++; sm->state = WPA_SM_FAILED; return 0; }
            if (wpa_parse_gtk_kde(plain, (uint32_t)n, sm->gtk,
                                  &sm->gtk_len, &sm->gtk_id))
                sm->gtk_valid = 1;
        }

        sm->state = WPA_SM_PTK_DONE;
        sm->tx_pn = 1;
        sm->rx_pn = 0;

        /* Message 4 carries no key data: it exists only to tell the AP
         * that this end has the same PTK and may start encrypting. */
        return wpa_build_eapol_key(sm, out, max,
                                   (uint16_t)(sm->key_desc_ver |
                                              WPA_KEY_INFO_PAIRWISE |
                                              WPA_KEY_INFO_MIC |
                                              WPA_KEY_INFO_SECURE),
                                   0, 0, 0, 1);
    }

    return 0;
}

static int wpa_sm_complete(const wpa_sm_t *sm) {
    return sm->state == WPA_SM_PTK_DONE;
}

/* ===== CCMP =====
 *
 * The cipher the temporal key is for. AES in counter mode with a CBC-MAC
 * over the header, which is exactly AES-CCM with a 13-byte nonce and an
 * 8-byte tag -- so what is written here is the 802.11-specific part:
 * building that nonce and that additional-authenticated-data from the
 * frame header, and the eight-byte CCMP header that carries the packet
 * number.
 *
 * The additional data is what protects the addresses. Without it, an
 * attacker could redirect a frame by rewriting its destination and the
 * payload would still decrypt.
 */

#define CCMP_HDR_LEN    8
#define CCMP_MIC_LEN    8

static void ccmp_pn_to_hdr(uint8_t *hdr, uint64_t pn, uint8_t key_id) {
    hdr[0] = (uint8_t)(pn & 0xFF);
    hdr[1] = (uint8_t)((pn >> 8) & 0xFF);
    hdr[2] = 0;
    hdr[3] = (uint8_t)(0x20 | (key_id << 6));    /* ExtIV always set */
    hdr[4] = (uint8_t)((pn >> 16) & 0xFF);
    hdr[5] = (uint8_t)((pn >> 24) & 0xFF);
    hdr[6] = (uint8_t)((pn >> 32) & 0xFF);
    hdr[7] = (uint8_t)((pn >> 40) & 0xFF);
}

static uint64_t ccmp_hdr_to_pn(const uint8_t *hdr) {
    return (uint64_t)hdr[0]            | ((uint64_t)hdr[1] << 8)  |
           ((uint64_t)hdr[4] << 16)    | ((uint64_t)hdr[5] << 24) |
           ((uint64_t)hdr[6] << 32)    | ((uint64_t)hdr[7] << 40);
}

/*
 * Build the CCM nonce and additional data from an 802.11 header.
 *
 * The masking is prescribed: fields that legitimately change as a frame
 * is retried or forwarded -- retry, power management, more data -- are
 * zeroed so that a retransmission still authenticates, and the
 * protected bit is forced on so that the AAD matches whether it is
 * computed before or after encryption.
 */
static void ccmp_aad_nonce(const uint8_t *hdr, uint32_t hdrlen,
                           uint8_t *aad, uint32_t *aad_len,
                           uint8_t *nonce) {
    uint16_t fc  = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
    uint16_t seq = (uint16_t)(hdr[22] | ((uint16_t)hdr[23] << 8));
    uint32_t pos = 0;
    int qos = 0, addr4 = 0;
    uint8_t tid = 0;

    if ((fc & 0x000C) == 0x0008) {          /* data frame */
        if ((fc & 0x0300) == 0x0300) addr4 = 1;
        if (fc & 0x0080) {                  /* QoS subtype bit */
            qos = 1;
            fc = (uint16_t)(fc & ~0x8000);  /* mask Order on QoS frames */
        }
        fc = (uint16_t)(fc & ~0x0070);      /* mask subtype bits 4-6    */
    }
    fc = (uint16_t)(fc & ~(0x0800 | 0x1000 | 0x2000));  /* retry/pwr/more */
    fc = (uint16_t)(fc | 0x4000);                       /* protected      */

    aad[pos++] = (uint8_t)(fc & 0xFF);
    aad[pos++] = (uint8_t)(fc >> 8);

    for (int i = 0; i < 18; i++) aad[pos++] = hdr[4 + i];   /* A1 A2 A3 */

    seq = (uint16_t)(seq & 0x000F);          /* keep fragment, drop seq */
    aad[pos++] = (uint8_t)(seq & 0xFF);
    aad[pos++] = (uint8_t)(seq >> 8);

    if (addr4) for (int i = 0; i < 6; i++) aad[pos++] = hdr[24 + i];

    if (qos) {
        uint32_t off = addr4 ? 30 : 24;
        tid = (uint8_t)(hdr[off] & 0x0F);
        aad[pos++] = tid;
        aad[pos++] = 0;
    }

    *aad_len = pos;

    nonce[0] = tid;                                    /* priority */
    for (int i = 0; i < 6; i++) nonce[1 + i] = hdr[10 + i];  /* A2 */
    /* the packet number, most significant byte first */
    (void)hdrlen;
}

/*
 * Encrypt a data frame in place of `out`.
 *
 * Input is a plaintext 802.11 frame; output is header, CCMP header,
 * ciphertext and 8-byte MIC. Returns the output length.
 */
static int ccmp_encrypt(const uint8_t *tk, uint32_t tk_len,
                        uint64_t pn, uint8_t key_id,
                        const uint8_t *frame, uint32_t len,
                        uint32_t hdrlen,
                        uint8_t *out, uint32_t max) {
    uint8_t aad[32], nonce[13], ccmp_hdr[CCMP_HDR_LEN];
    uint32_t aad_len = 0, data_len;

    if (len < hdrlen) return -1;
    data_len = len - hdrlen;
    if (hdrlen + CCMP_HDR_LEN + data_len + CCMP_MIC_LEN > max) return -1;

    ccmp_aad_nonce(frame, hdrlen, aad, &aad_len, nonce);
    nonce[7]  = (uint8_t)((pn >> 40) & 0xFF);
    nonce[8]  = (uint8_t)((pn >> 32) & 0xFF);
    nonce[9]  = (uint8_t)((pn >> 24) & 0xFF);
    nonce[10] = (uint8_t)((pn >> 16) & 0xFF);
    nonce[11] = (uint8_t)((pn >> 8)  & 0xFF);
    nonce[12] = (uint8_t)(pn & 0xFF);

    ccmp_pn_to_hdr(ccmp_hdr, pn, key_id);

    for (uint32_t i = 0; i < hdrlen; i++) out[i] = frame[i];
    out[1] = (uint8_t)(out[1] | 0x40);          /* set Protected */
    for (uint32_t i = 0; i < CCMP_HDR_LEN; i++)
        out[hdrlen + i] = ccmp_hdr[i];

    if (aes_ccm_encrypt(tk, (int)(tk_len * 8), nonce, 13,
                        aad, aad_len,
                        frame + hdrlen, data_len,
                        out + hdrlen + CCMP_HDR_LEN,
                        out + hdrlen + CCMP_HDR_LEN + data_len,
                        CCMP_MIC_LEN) != 0)
        return -1;

    return (int)(hdrlen + CCMP_HDR_LEN + data_len + CCMP_MIC_LEN);
}

/*
 * Decrypt a received data frame. Returns the plaintext frame length
 * (header plus payload, CCMP header removed) or -1.
 *
 * A failure here is not necessarily an attack -- a frame from a station
 * that rekeyed, or one that arrived after a key change, fails the same
 * way -- but it is never something to deliver upward, so the frame is
 * dropped rather than passed on unauthenticated.
 */
static int ccmp_decrypt(const uint8_t *tk, uint32_t tk_len,
                        const uint8_t *frame, uint32_t len,
                        uint32_t hdrlen,
                        uint64_t *pn_out,
                        uint8_t *out, uint32_t max) {
    uint8_t aad[32], nonce[13];
    uint32_t aad_len = 0, data_len;
    uint64_t pn;

    if (len < hdrlen + CCMP_HDR_LEN + CCMP_MIC_LEN) return -1;
    if (!(frame[hdrlen + 3] & 0x20)) return -1;     /* ExtIV must be set */

    data_len = len - hdrlen - CCMP_HDR_LEN - CCMP_MIC_LEN;
    if (hdrlen + data_len > max) return -1;

    pn = ccmp_hdr_to_pn(frame + hdrlen);
    if (pn_out) *pn_out = pn;

    ccmp_aad_nonce(frame, hdrlen, aad, &aad_len, nonce);
    nonce[7]  = (uint8_t)((pn >> 40) & 0xFF);
    nonce[8]  = (uint8_t)((pn >> 32) & 0xFF);
    nonce[9]  = (uint8_t)((pn >> 24) & 0xFF);
    nonce[10] = (uint8_t)((pn >> 16) & 0xFF);
    nonce[11] = (uint8_t)((pn >> 8)  & 0xFF);
    nonce[12] = (uint8_t)(pn & 0xFF);

    for (uint32_t i = 0; i < hdrlen; i++) out[i] = frame[i];

    if (aes_ccm_decrypt(tk, (int)(tk_len * 8), nonce, 13,
                        aad, aad_len,
                        frame + hdrlen + CCMP_HDR_LEN, data_len,
                        frame + hdrlen + CCMP_HDR_LEN + data_len,
                        CCMP_MIC_LEN,
                        out + hdrlen) != 0)
        return -1;

    out[1] = (uint8_t)(out[1] & ~0x40);     /* clear Protected */
    return (int)(hdrlen + data_len);
}

#endif /* NET_WPA2_H */
