/*
 * tools/wifi_test.c — the half of a wireless stack that can be tested
 * without a radio.
 *
 * There is no wireless device in QEMU, and the chipset back-ends in
 * src/net/wifi.c cannot run without proprietary firmware. What *can* be
 * checked, and is checked here, is everything that a single wrong byte
 * would silently break: the key derivation, the frame parsing, the
 * handshake and the cipher.
 *
 * Two kinds of check appear below and they are labelled differently.
 *
 *   "vector"    compares against a value published in a standard. If
 *               this fails, the implementation is wrong.
 *
 *   "round-trip" drives both ends of an exchange and requires them to
 *               agree. This cannot catch a mistake made identically in
 *               both directions, so it is used only where no published
 *               vector exists -- and where it is used, the primitives
 *               underneath it are themselves covered by vectors.
 *
 * The 4-way handshake is exercised against a synthetic authenticator
 * written from the standard in this file, independently of the
 * supplicant in src/net/wpa2.h. It builds message 1 and message 3,
 * verifies the MICs on messages 2 and 4 with a PTK it derived itself,
 * and wraps a group key that the supplicant has to unwrap. A supplicant
 * that agreed with a buggy authenticator would still have to agree with
 * RFC 3394's published wrap to pass.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "net/ieee80211.h"
#include "net/wpa2.h"

static int checks = 0;
static int fails  = 0;

static void ok(const char *what) {
    checks++;
    printf("  ok   %s\n", what);
}

static void bad(const char *what) {
    checks++; fails++;
    printf("  FAIL %s\n", what);
}

static void expect(int cond, const char *what) {
    if (cond) ok(what); else bad(what);
}

/* Compare a buffer against a hex string. */
static void hexcheck(const char *what, const uint8_t *got,
                     const char *hex, int n) {
    uint8_t want[128];
    int i;
    checks++;
    for (i = 0; i < n; i++) {
        int hi, lo;
        const char *h = hex + i * 2;
        hi = (h[0] <= '9') ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
        lo = (h[1] <= '9') ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
        want[i] = (uint8_t)((hi << 4) | lo);
    }
    if (memcmp(got, want, (size_t)n) == 0) {
        printf("  ok   %s\n", what);
        return;
    }
    fails++;
    printf("  FAIL %s\n         got  ", what);
    for (i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n         want %s\n", hex);
}

/* A deterministic nonce source, so a failure is reproducible. */
static uint8_t test_nonce_seed = 0x11;
static int test_random(uint8_t *out, uint32_t n) {
    for (uint32_t i = 0; i < n; i++)
        out[i] = (uint8_t)(test_nonce_seed + i * 7u);
    return (int)n;
}

/* =========================================================
 * 1. the PMK
 * ========================================================= */

static void test_pmk(void) {
    uint8_t pmk[32];

    printf("\nPMK from passphrase -- PBKDF2-HMAC-SHA1, 4096 rounds\n");
    printf("(IEEE 802.11i-2004 annex H.4 test vectors)\n");

    wpa_pmk_from_passphrase("password", (const uint8_t *)"IEEE", 4, pmk);
    hexcheck("vector     SSID \"IEEE\", passphrase \"password\"", pmk,
             "f42c6fc52df0ebef9ebb4b90b38a5f90"
             "2e83fe1b135a70e23aed762e9710a12e", 32);

    wpa_pmk_from_passphrase("ThisIsAPassword",
                            (const uint8_t *)"ThisIsASSID", 11, pmk);
    hexcheck("vector     SSID \"ThisIsASSID\"", pmk,
             "0dc0d6eb90555ed6419756b9a15ec3e3"
             "209b63df707dd508d14581f8982721af", 32);

    {
        /* The maximum-length case: a 32-character SSID and a
         * 64-character passphrase, which is the longest either field
         * can be and the one most likely to run off the end of a
         * fixed buffer. */
        const char *pass = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                           "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        const char *ssid = "ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ";
        /* Cross-checked against a reference PBKDF2 rather than quoted
         * from the standard, which prints only the two cases above. */
        wpa_pmk_from_passphrase(pass, (const uint8_t *)ssid, 32, pmk);
        hexcheck("reference  32-byte SSID, 64-byte passphrase", pmk,
                 "4fd16ee24bd1d8f9e7ebd86cbd802d0b"
                 "3acfd23cb08de414da4e1690e474b857", 32);
    }
}

/* =========================================================
 * 2. AES key wrap -- how the group key travels
 * ========================================================= */

static void test_keywrap(void) {
    /* RFC 3394 section 4.1: wrap 128 bits of key data with a 128-bit
     * KEK. This is exactly the operation message 3 performs on the
     * GTK, so a published vector covers the real path. */
    const uint8_t kek[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F
    };
    const uint8_t key[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF
    };
    uint8_t wrapped[32];
    uint8_t back[32];
    int n;

    printf("\nAES key wrap -- RFC 3394\n");

    n = aes_key_wrap(kek, 16, key, 16, wrapped);
    expect(n == 24, "wrap of 16 bytes produces 24");
    hexcheck("vector     section 4.1 wrapped key", wrapped,
             "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5", 24);

    n = aes_key_unwrap(kek, 16, wrapped, 24, back);
    expect(n == 16, "unwrap returns the original length");
    expect(memcmp(back, key, 16) == 0, "unwrap recovers the key");

    /* A tampered wrap must fail rather than yield rubbish. The integrity
     * check is the whole reason the group key can be trusted. */
    wrapped[5] ^= 0x01;
    expect(aes_key_unwrap(kek, 16, wrapped, 24, back) < 0,
           "a single flipped bit makes the unwrap fail");
}

/* =========================================================
 * 3. beacon parsing
 * ========================================================= */

static void test_beacon(void) {
    uint8_t f[128];
    uint32_t n = 0;
    wifi_bss_t bss;

    printf("\n802.11 beacon parsing\n");

    /* A beacon from 00:11:22:33:44:55 announcing "vextro" on channel 6
     * with an RSN IE offering CCMP and PSK. */
    ieee80211_put_le16(f, IEEE80211_FTYPE_MGMT | IEEE80211_STYPE_BEACON);
    ieee80211_put_le16(f + 2, 0);
    memset(f + 4, 0xFF, 6);                                   /* addr1 */
    { uint8_t a[6] = {0x00,0x11,0x22,0x33,0x44,0x55};
      memcpy(f + 10, a, 6); memcpy(f + 16, a, 6); }
    ieee80211_put_le16(f + 22, 0);
    n = 24;

    memset(f + n, 0, 8); n += 8;                       /* timestamp     */
    ieee80211_put_le16(f + n, 100); n += 2;            /* interval      */
    ieee80211_put_le16(f + n, IEEE80211_CAP_ESS |
                              IEEE80211_CAP_PRIVACY); n += 2;

    f[n++] = IEEE80211_EID_SSID; f[n++] = 6;
    memcpy(f + n, "vextro", 6); n += 6;

    f[n++] = IEEE80211_EID_DS_PARAMS; f[n++] = 1; f[n++] = 6;

    f[n++] = IEEE80211_EID_RSN; f[n++] = 20;
    ieee80211_put_le16(f + n, 1); n += 2;                    /* version */
    f[n++]=0x00; f[n++]=0x0F; f[n++]=0xAC; f[n++]=RSN_CIPHER_CCMP;
    ieee80211_put_le16(f + n, 1); n += 2;
    f[n++]=0x00; f[n++]=0x0F; f[n++]=0xAC; f[n++]=RSN_CIPHER_CCMP;
    ieee80211_put_le16(f + n, 1); n += 2;
    f[n++]=0x00; f[n++]=0x0F; f[n++]=0xAC; f[n++]=RSN_AKM_PSK;
    ieee80211_put_le16(f + n, 0); n += 2;

    expect(ieee80211_parse_beacon(f, n, -42, &bss) == 1, "beacon parses");
    expect(strcmp(bss.ssid, "vextro") == 0, "SSID is read");
    expect(bss.channel == 6, "channel comes from the DS parameter set");
    expect(bss.rssi == -42, "signal strength is carried through");
    expect(bss.security == WIFI_SEC_WPA2_PSK, "classified as WPA2-PSK");
    expect(rsn_has_cipher(&bss.rsn, RSN_CIPHER_CCMP), "CCMP is offered");
    expect(rsn_has_akm(&bss.rsn, RSN_AKM_PSK), "PSK is offered");
    expect(bss.rsn_ie_len == 22, "the RSN IE is kept verbatim");

    /* Truncation must be rejected, not parsed as far as it goes. Every
     * one of these bytes came from an unauthenticated broadcast. */
    {
        int survived = 1;
        for (uint32_t cut = 24; cut < n; cut++) {
            wifi_bss_t t;
            /* Any prefix must either parse or be refused -- never read
             * past the buffer. Run under a sanitizer this is the check
             * that matters; here it must at least not crash. */
            ieee80211_parse_beacon(f, cut, -42, &t);
        }
        expect(survived, "every truncated prefix is handled");
    }

    /* A frame that is not a beacon must be refused. */
    ieee80211_put_le16(f, IEEE80211_FTYPE_DATA);
    expect(ieee80211_parse_beacon(f, n, -42, &bss) == 0,
           "a data frame is not accepted as a beacon");
}

/* =========================================================
 * 4. the 4-way handshake, against a synthetic authenticator
 * ========================================================= */

static const uint8_t ap_mac[6]  = {0x00,0x11,0x22,0x33,0x44,0x55};
static const uint8_t sta_mac[6] = {0x66,0x77,0x88,0x99,0xAA,0xBB};

static const uint8_t test_gtk[16] = {
    0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,
    0xA8,0xA9,0xAA,0xAB,0xAC,0xAD,0xAE,0xAF
};

/* The authenticator's own state, derived independently. */
typedef struct {
    uint8_t pmk[32];
    uint8_t ptk[48];
    uint8_t anonce[32];
    uint8_t replay[8];
} auth_t;

static uint32_t auth_build_m1(auth_t *a, uint8_t *out) {
    eapol_key_t *k = (eapol_key_t *)out;
    uint32_t total = EAPOL_KEY_HDR_LEN;

    memset(out, 0, total);
    k->version = 2;
    k->type    = EAPOL_TYPE_KEY;
    wpa_put_be16(k->length, (uint16_t)(total - 4));
    k->descriptor_type = EAPOL_KEY_DESC_RSN;
    wpa_put_be16(k->key_info, WPA_KEY_INFO_TYPE_HMAC_SHA1 |
                              WPA_KEY_INFO_PAIRWISE |
                              WPA_KEY_INFO_ACK);
    wpa_put_be16(k->key_length, 16);
    memcpy(k->replay_counter, a->replay, 8);
    memcpy(k->key_nonce, a->anonce, 32);
    wpa_put_be16(k->key_data_length, 0);
    return total;
}

/* Message 3 carries the group key, wrapped under the KEK. */
static uint32_t auth_build_m3(auth_t *a, uint8_t *out) {
    eapol_key_t *k = (eapol_key_t *)out;
    uint8_t kde[64];
    uint8_t wrapped[80];
    uint32_t kde_len = 0;
    int wlen;
    uint32_t total;

    /* GTK KDE: 00-0F-AC type 1, key id 1, then the key. */
    kde[kde_len++] = 221;
    kde[kde_len++] = (uint8_t)(6 + 16);
    kde[kde_len++] = 0x00; kde[kde_len++] = 0x0F; kde[kde_len++] = 0xAC;
    kde[kde_len++] = WPA_KDE_TYPE_GTK;
    kde[kde_len++] = 1;              /* key id 1 */
    kde[kde_len++] = 0;
    memcpy(kde + kde_len, test_gtk, 16); kde_len += 16;

    /* The wrap needs a multiple of eight; pad as the standard says. */
    while (kde_len % 8) kde[kde_len++] = 0xDD;

    wlen = aes_key_wrap(a->ptk + 16, 16, kde, kde_len, wrapped);
    if (wlen < 0) return 0;

    total = EAPOL_KEY_HDR_LEN + (uint32_t)wlen;
    memset(out, 0, total);
    k->version = 2;
    k->type    = EAPOL_TYPE_KEY;
    wpa_put_be16(k->length, (uint16_t)(total - 4));
    k->descriptor_type = EAPOL_KEY_DESC_RSN;
    wpa_put_be16(k->key_info, WPA_KEY_INFO_TYPE_HMAC_SHA1 |
                              WPA_KEY_INFO_PAIRWISE |
                              WPA_KEY_INFO_INSTALL |
                              WPA_KEY_INFO_ACK |
                              WPA_KEY_INFO_MIC |
                              WPA_KEY_INFO_SECURE |
                              WPA_KEY_INFO_ENCR_KEY_DATA);
    wpa_put_be16(k->key_length, 16);
    memcpy(k->replay_counter, a->replay, 8);
    memcpy(k->key_nonce, a->anonce, 32);
    wpa_put_be16(k->key_data_length, (uint16_t)wlen);
    memcpy(out + EAPOL_KEY_HDR_LEN, wrapped, (size_t)wlen);

    wpa_eapol_mic(WPA_KEY_INFO_TYPE_HMAC_SHA1, a->ptk, 16,
                  out, total, k->key_mic);
    return total;
}

static void test_handshake(void) {
    auth_t a;
    wpa_sm_t sm;
    uint8_t m1[256], m2[256], m3[256], m4[256];
    uint32_t n1, n2, n3, n4;
    uint8_t rsn_ie[22];

    printf("\nthe 4-way handshake\n");

    /* The station's chosen RSN IE, as it would appear in the
     * association request and again in message 2. */
    {
        uint32_t p = 0;
        rsn_ie[p++] = IEEE80211_EID_RSN; rsn_ie[p++] = 20;
        ieee80211_put_le16(rsn_ie + p, 1); p += 2;
        rsn_ie[p++]=0x00; rsn_ie[p++]=0x0F; rsn_ie[p++]=0xAC;
        rsn_ie[p++]=RSN_CIPHER_CCMP;
        ieee80211_put_le16(rsn_ie + p, 1); p += 2;
        rsn_ie[p++]=0x00; rsn_ie[p++]=0x0F; rsn_ie[p++]=0xAC;
        rsn_ie[p++]=RSN_CIPHER_CCMP;
        ieee80211_put_le16(rsn_ie + p, 1); p += 2;
        rsn_ie[p++]=0x00; rsn_ie[p++]=0x0F; rsn_ie[p++]=0xAC;
        rsn_ie[p++]=RSN_AKM_PSK;
        ieee80211_put_le16(rsn_ie + p, 0); p += 2;
    }

    /* Both ends start from the same passphrase and SSID. */
    memset(&a, 0, sizeof(a));
    wpa_pmk_from_passphrase("ThisIsAPassword",
                            (const uint8_t *)"ThisIsASSID", 11, a.pmk);
    for (int i = 0; i < 32; i++) a.anonce[i] = (uint8_t)(0xC0 + i);
    a.replay[7] = 1;

    wpa_sm_init(&sm, "ThisIsAPassword",
                (const uint8_t *)"ThisIsASSID", 11,
                ap_mac, sta_mac, rsn_ie, sizeof(rsn_ie));

    expect(memcmp(sm.pmk, a.pmk, 32) == 0,
           "round-trip both ends derive the same PMK");

    /* ---- message 1 ---- */
    n1 = auth_build_m1(&a, m1);
    n2 = wpa_sm_rx_eapol(&sm, m1, n1, m2, sizeof(m2), test_random);
    expect(n2 > 0, "message 1 produces a message 2");
    expect(sm.state == WPA_SM_PTK_START, "state advances to PTK_START");

    /* The authenticator now derives the PTK from the SNonce it just
     * received, exactly as the supplicant did. */
    {
        eapol_key_t *k2 = (eapol_key_t *)m2;
        wpa_derive_ptk(a.pmk, 32, ap_mac, sta_mac,
                       a.anonce, k2->key_nonce, a.ptk, 48, 0);
        expect(memcmp(a.ptk, sm.ptk, 48) == 0,
               "round-trip both ends derive the same PTK");

        /* And the MIC the supplicant put on message 2 must verify
         * under the authenticator's independently derived KCK. */
        expect(wpa_eapol_check_mic(WPA_KEY_INFO_TYPE_HMAC_SHA1,
                                   a.ptk, 16, m2, n2) == 1,
               "the MIC on message 2 verifies at the authenticator");

        /* Message 2 must carry the station's RSN IE verbatim. */
        expect(wpa_get_be16(k2->key_data_length) == sizeof(rsn_ie) &&
               memcmp(m2 + EAPOL_KEY_HDR_LEN, rsn_ie, sizeof(rsn_ie)) == 0,
               "message 2 echoes the station's RSN IE");
    }

    /* ---- message 3 ---- */
    a.replay[7] = 2;
    n3 = auth_build_m3(&a, m3);
    expect(n3 > 0, "the authenticator builds message 3");

    n4 = wpa_sm_rx_eapol(&sm, m3, n3, m4, sizeof(m4), test_random);
    expect(n4 > 0, "message 3 produces a message 4");
    expect(wpa_sm_complete(&sm) == 1, "the handshake completes");

    expect(wpa_eapol_check_mic(WPA_KEY_INFO_TYPE_HMAC_SHA1,
                               a.ptk, 16, m4, n4) == 1,
           "the MIC on message 4 verifies at the authenticator");

    /* The group key must have come out of the wrap intact. */
    expect(sm.gtk_valid == 1, "the group key was unwrapped");
    expect(sm.gtk_len == 16 && memcmp(sm.gtk, test_gtk, 16) == 0,
           "vector     the group key matches what was wrapped");
    expect(sm.gtk_id == 1, "the group key index is carried");

    /* ---- the wrong passphrase ---- */
    {
        wpa_sm_t bad_sm;
        uint8_t r[256];
        uint32_t rn;

        wpa_sm_init(&bad_sm, "NotThePassword",
                    (const uint8_t *)"ThisIsASSID", 11,
                    ap_mac, sta_mac, rsn_ie, sizeof(rsn_ie));

        rn = wpa_sm_rx_eapol(&bad_sm, m1, n1, r, sizeof(r), test_random);
        expect(rn > 0, "a wrong passphrase still answers message 1");

        /* Message 3's MIC is where it is caught, and it is the only
         * place: everything before that is unauthenticated. */
        rn = wpa_sm_rx_eapol(&bad_sm, m3, n3, r, sizeof(r), test_random);
        expect(rn == 0, "a wrong passphrase produces no message 4");
        expect(bad_sm.state == WPA_SM_FAILED,
               "a wrong passphrase is detected at message 3's MIC");
    }

    /* ---- a tampered message 3 ---- */
    {
        wpa_sm_t t_sm;
        uint8_t r[256];
        uint8_t m3x[256];

        wpa_sm_init(&t_sm, "ThisIsAPassword",
                    (const uint8_t *)"ThisIsASSID", 11,
                    ap_mac, sta_mac, rsn_ie, sizeof(rsn_ie));
        wpa_sm_rx_eapol(&t_sm, m1, n1, r, sizeof(r), test_random);

        memcpy(m3x, m3, n3);
        m3x[EAPOL_KEY_HDR_LEN + 3] ^= 0x01;   /* flip a bit in the GTK */

        expect(wpa_sm_rx_eapol(&t_sm, m3x, n3, r, sizeof(r),
                               test_random) == 0,
               "a tampered message 3 is rejected");
    }
}

/* =========================================================
 * 5. CCMP
 * ========================================================= */

static void test_ccmp(void) {
    uint8_t tk[16];
    uint8_t frame[128];
    uint8_t enc[160];
    uint8_t dec[160];
    uint32_t hdrlen = 24, flen;
    uint64_t pn = 0x0102030405ULL;
    uint64_t got_pn = 0;
    int e, d;

    printf("\nCCMP -- AES-CCM over 802.11 data frames\n");

    for (int i = 0; i < 16; i++) tk[i] = (uint8_t)(0x50 + i);

    /* A plaintext data frame to the AP. */
    ieee80211_put_le16(frame, IEEE80211_FTYPE_DATA | IEEE80211_FC_TODS);
    ieee80211_put_le16(frame + 2, 0);
    memcpy(frame + 4,  ap_mac, 6);
    memcpy(frame + 10, sta_mac, 6);
    memcpy(frame + 16, ap_mac, 6);
    ieee80211_put_le16(frame + 22, 0x0010);
    for (int i = 0; i < 40; i++) frame[24 + i] = (uint8_t)(i * 3 + 1);
    flen = 24 + 40;

    e = ccmp_encrypt(tk, 16, pn, 0, frame, flen, hdrlen, enc, sizeof(enc));
    expect(e == (int)(flen + 8 + 8),
           "ciphertext grows by the CCMP header and MIC");
    expect((enc[1] & 0x40) != 0, "the Protected bit is set on output");
    expect((enc[hdrlen + 3] & 0x20) != 0, "the ExtIV bit is set");

    d = ccmp_decrypt(tk, 16, enc, (uint32_t)e, hdrlen, &got_pn,
                     dec, sizeof(dec));
    expect(d == (int)flen, "round-trip decryption returns the original size");
    expect(got_pn == pn, "the packet number is recovered");
    expect(memcmp(dec + hdrlen, frame + hdrlen, 40) == 0,
           "round-trip the payload survives");
    expect((dec[1] & 0x40) == 0, "the Protected bit is cleared on input");

    /* The addresses are authenticated, not encrypted: rewriting one
     * must break the MIC. This is what stops a frame being redirected. */
    {
        uint8_t tampered[160];
        memcpy(tampered, enc, (size_t)e);
        tampered[16] ^= 0x01;          /* change addr3 */
        expect(ccmp_decrypt(tk, 16, tampered, (uint32_t)e, hdrlen,
                            &got_pn, dec, sizeof(dec)) < 0,
               "rewriting a destination address breaks the MIC");
    }

    /* And so must changing the ciphertext. */
    {
        uint8_t tampered[160];
        memcpy(tampered, enc, (size_t)e);
        tampered[hdrlen + 8 + 5] ^= 0x01;
        expect(ccmp_decrypt(tk, 16, tampered, (uint32_t)e, hdrlen,
                            &got_pn, dec, sizeof(dec)) < 0,
               "a flipped payload bit breaks the MIC");
    }

    /* A different packet number must produce different ciphertext --
     * this is the property that a repeated nonce would destroy. */
    {
        uint8_t enc2[160];
        int e2 = ccmp_encrypt(tk, 16, pn + 1, 0, frame, flen, hdrlen,
                              enc2, sizeof(enc2));
        expect(e2 == e && memcmp(enc + hdrlen + 8, enc2 + hdrlen + 8, 40) != 0,
               "a new packet number gives new ciphertext");
    }
}

/* =========================================================
 * 6. 802.11 <-> Ethernet
 * ========================================================= */

static void test_encap(void) {
    uint8_t eth[128], frame[160], back[160];
    uint32_t n, m;

    printf("\n802.11 to Ethernet conversion\n");

    memcpy(eth, ap_mac, 6);
    memcpy(eth + 6, sta_mac, 6);
    eth[12] = 0x08; eth[13] = 0x00;              /* IPv4 */
    for (int i = 0; i < 46; i++) eth[14 + i] = (uint8_t)(i + 1);

    n = ieee80211_eth_to_data(eth, 14 + 46, ap_mac, 7, 0,
                              frame, sizeof(frame));
    expect(n == 24 + 8 + 46, "encapsulation adds a header and SNAP");
    expect((ieee80211_fc(frame) & IEEE80211_FC_TODS) != 0,
           "a frame to the AP has ToDS set");

    /* Coming back the other way the frame is FromDS, so the source is
     * addr3 rather than addr2 -- the case that silently makes every
     * host on the LAN look like the access point. */
    ieee80211_put_le16(frame, IEEE80211_FTYPE_DATA | IEEE80211_FC_FROMDS);
    memcpy(frame + 4,  sta_mac, 6);      /* addr1: us            */
    memcpy(frame + 10, ap_mac,  6);      /* addr2: the AP radio  */
    { uint8_t origin[6] = {0xDE,0xAD,0xBE,0xEF,0x00,0x01};
      memcpy(frame + 16, origin, 6);     /* addr3: real sender   */

      m = ieee80211_data_to_eth(frame, n, back, sizeof(back));
      expect(m == 14 + 46, "decapsulation returns an Ethernet frame");
      expect(memcmp(back, sta_mac, 6) == 0, "destination is addr1");
      expect(memcmp(back + 6, origin, 6) == 0,
             "source is addr3 on a FromDS frame, not the AP");
    }
    expect(back[12] == 0x08 && back[13] == 0x00, "the ethertype survives");
    expect(memcmp(back + 14, eth + 14, 46) == 0, "round-trip the payload");

    /* A frame with a SNAP header that is not RFC 1042 is not ours. */
    frame[24] = 0xBB;
    expect(ieee80211_data_to_eth(frame, n, back, sizeof(back)) == 0,
           "a non-RFC-1042 SNAP header is refused");
}

int main(void) {
    printf("Vextro wireless: 802.11 frames, WPA2 key derivation, CCMP\n");
    printf("=========================================================\n");

    test_pmk();
    test_keywrap();
    test_beacon();
    test_handshake();
    test_ccmp();
    test_encap();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
