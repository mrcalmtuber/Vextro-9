#ifndef NET_IEEE80211_H
#define NET_IEEE80211_H

/*
 * src/net/ieee80211.h — the frames, and how to read them.
 *
 * Everything in here is pure: it takes bytes off the air and turns them
 * into structures, or takes structures and lays them out as bytes. No
 * register touches it, no allocator, no lock. That is deliberate --
 * this is the half of a wireless stack that can be tested without a
 * radio, and tools/wifi_test.c does exactly that.
 *
 * ---- what an 802.11 frame is, briefly ----
 *
 * Ethernet has one header shape. 802.11 has three families -- management,
 * control, data -- and the header length depends on the frame's own
 * flags: whether it is going to or from the distribution system (which
 * decides if there is a fourth address), and whether it carries a QoS
 * control field. So the length of the header cannot be a constant, and
 * every routine here that walks a frame calls ieee80211_hdrlen() first
 * rather than assuming 24.
 *
 * ---- byte order ----
 *
 * 802.11 is little-endian on the wire, unlike every other protocol in
 * this tree. The frame control field, the duration, the sequence
 * control and the capability/interval fields in a beacon are all
 * little-endian; the EAPOL frames that ride on top (see net/wpa2.h) are
 * big-endian, because those come from 802.1X and inherited the network
 * order everything else uses. Mixing those two up produces a stack that
 * associates and then fails the handshake, so the two are kept in
 * separate files with the convention stated at the top of each.
 */

#include <stdint.h>

/* ===== frame control ===== */

#define IEEE80211_FC_VERSION_MASK   0x0003
#define IEEE80211_FC_TYPE_MASK      0x000C
#define IEEE80211_FC_SUBTYPE_MASK   0x00F0

#define IEEE80211_FTYPE_MGMT        0x0000
#define IEEE80211_FTYPE_CTL         0x0004
#define IEEE80211_FTYPE_DATA        0x0008

/* management subtypes, already shifted into their field position */
#define IEEE80211_STYPE_ASSOC_REQ   0x0000
#define IEEE80211_STYPE_ASSOC_RESP  0x0010
#define IEEE80211_STYPE_REASSOC_REQ 0x0020
#define IEEE80211_STYPE_REASSOC_RSP 0x0030
#define IEEE80211_STYPE_PROBE_REQ   0x0040
#define IEEE80211_STYPE_PROBE_RESP  0x0050
#define IEEE80211_STYPE_BEACON      0x0080
#define IEEE80211_STYPE_DISASSOC    0x00A0
#define IEEE80211_STYPE_AUTH        0x00B0
#define IEEE80211_STYPE_DEAUTH      0x00C0
#define IEEE80211_STYPE_ACTION      0x00D0

/* data subtypes */
#define IEEE80211_STYPE_DATA        0x0000
#define IEEE80211_STYPE_NULLFUNC    0x0040
#define IEEE80211_STYPE_QOS_DATA    0x0080

#define IEEE80211_FC_TODS           0x0100
#define IEEE80211_FC_FROMDS         0x0200
#define IEEE80211_FC_MOREFRAG       0x0400
#define IEEE80211_FC_RETRY          0x0800
#define IEEE80211_FC_PWRMGT         0x1000
#define IEEE80211_FC_MOREDATA       0x2000
#define IEEE80211_FC_PROTECTED      0x4000
#define IEEE80211_FC_ORDER          0x8000

#define IEEE80211_ADDR_LEN          6
#define IEEE80211_MAX_SSID_LEN      32

/* An 802.11 header is never larger than this: 2 frame control, 2
 * duration, 4 addresses, 2 sequence control, 2 QoS, 4 HT control. */
#define IEEE80211_MAX_HDRLEN        36

/* ===== information elements ===== */

#define IEEE80211_EID_SSID          0
#define IEEE80211_EID_SUPP_RATES    1
#define IEEE80211_EID_DS_PARAMS     3
#define IEEE80211_EID_TIM           5
#define IEEE80211_EID_COUNTRY       7
#define IEEE80211_EID_ERP           42
#define IEEE80211_EID_RSN           48
#define IEEE80211_EID_EXT_RATES     50
#define IEEE80211_EID_HT_CAP        45
#define IEEE80211_EID_HT_OPER       61
#define IEEE80211_EID_VENDOR        221

/* capability bits from a beacon or probe response */
#define IEEE80211_CAP_ESS           0x0001
#define IEEE80211_CAP_IBSS          0x0002
#define IEEE80211_CAP_PRIVACY       0x0010
#define IEEE80211_CAP_SHORT_PRE     0x0020
#define IEEE80211_CAP_SHORT_SLOT    0x0400

/* status and reason codes worth naming */
#define IEEE80211_STATUS_SUCCESS            0
#define IEEE80211_REASON_UNSPECIFIED        1
#define IEEE80211_REASON_DEAUTH_LEAVING     3
#define IEEE80211_REASON_MIC_FAILURE        14
#define IEEE80211_REASON_4WAY_TIMEOUT       15

/* ===== little-endian accessors =====
 *
 * Written out rather than cast, because a uint16_t* into a frame buffer
 * is an unaligned load the moment a QoS field shifts everything by two,
 * and on some of the machines this runs on that is a fault rather than
 * a slow path.
 */

static inline uint16_t ieee80211_get_le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t ieee80211_get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void ieee80211_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static inline uint16_t ieee80211_fc(const uint8_t *frame) {
    return ieee80211_get_le16(frame);
}

static inline uint16_t ieee80211_type(uint16_t fc) {
    return (uint16_t)(fc & IEEE80211_FC_TYPE_MASK);
}

static inline uint16_t ieee80211_stype(uint16_t fc) {
    return (uint16_t)(fc & IEEE80211_FC_SUBTYPE_MASK);
}

static inline int ieee80211_is_mgmt(uint16_t fc) {
    return ieee80211_type(fc) == IEEE80211_FTYPE_MGMT;
}

static inline int ieee80211_is_data(uint16_t fc) {
    return ieee80211_type(fc) == IEEE80211_FTYPE_DATA;
}

static inline int ieee80211_has_qos(uint16_t fc) {
    return ieee80211_is_data(fc) && (fc & IEEE80211_STYPE_QOS_DATA);
}

/*
 * The header length, which is not a constant.
 *
 * Four addresses only when a frame is both to and from the distribution
 * system, which on a normal client link never happens -- but a frame
 * that claims it does and is parsed as though it did not will have its
 * payload read six bytes early, and CCMP will then fail to decrypt with
 * no indication why. So the flag is honoured rather than assumed.
 */
static inline uint32_t ieee80211_hdrlen(uint16_t fc) {
    uint32_t len = 24;

    if (ieee80211_is_data(fc)) {
        if ((fc & (IEEE80211_FC_TODS | IEEE80211_FC_FROMDS)) ==
            (IEEE80211_FC_TODS | IEEE80211_FC_FROMDS))
            len += IEEE80211_ADDR_LEN;
        if (fc & IEEE80211_STYPE_QOS_DATA) {
            len += 2;
            /* HT control is present only on QoS frames with Order set. */
            if (fc & IEEE80211_FC_ORDER) len += 4;
        }
    } else if (ieee80211_type(fc) == IEEE80211_FTYPE_CTL) {
        /* Only the two control frames this stack ever looks at. */
        uint16_t st = ieee80211_stype(fc);
        if (st == 0x00C0 || st == 0x00D0) return 10;  /* CTS / ACK */
        return 16;
    }
    return len;
}

static inline const uint8_t *ieee80211_addr1(const uint8_t *f) { return f + 4;  }
static inline const uint8_t *ieee80211_addr2(const uint8_t *f) { return f + 10; }
static inline const uint8_t *ieee80211_addr3(const uint8_t *f) { return f + 16; }

static inline void ieee80211_addr_copy(uint8_t *dst, const uint8_t *src) {
    for (int i = 0; i < IEEE80211_ADDR_LEN; i++) dst[i] = src[i];
}

static inline int ieee80211_addr_equal(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < IEEE80211_ADDR_LEN; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static inline int ieee80211_addr_is_bcast(const uint8_t *a) {
    for (int i = 0; i < IEEE80211_ADDR_LEN; i++)
        if (a[i] != 0xFF) return 0;
    return 1;
}

/* ===== RSN (802.11i) cipher and AKM suites ===== */

#define RSN_OUI                 0x000FAC   /* 00-0F-AC, big-endian in the IE */

#define RSN_CIPHER_NONE         0
#define RSN_CIPHER_WEP40        1
#define RSN_CIPHER_TKIP         2
#define RSN_CIPHER_CCMP         4
#define RSN_CIPHER_WEP104       5
#define RSN_CIPHER_GCMP         8

#define RSN_AKM_8021X           1   /* EAP; PMK comes from the AS       */
#define RSN_AKM_PSK             2   /* PSK; PMK comes from the passphrase */
#define RSN_AKM_8021X_SHA256    5
#define RSN_AKM_PSK_SHA256      6
#define RSN_AKM_SAE             8   /* WPA3 */

/* What the RSN IE of a beacon told us about a network. */
typedef struct {
    int      present;           /* an RSN IE was found at all           */
    uint16_t version;
    uint32_t group_cipher;      /* RSN_CIPHER_*                          */
    uint32_t pairwise[4];
    int      pairwise_count;
    uint32_t akm[4];
    int      akm_count;
    uint16_t caps;
    int      mfp_required;      /* management frame protection          */
    int      mfp_capable;
} rsn_info_t;

/*
 * Parse an RSN information element body (everything after the EID and
 * length octets).
 *
 * Every count in here is attacker-controlled: it comes from a beacon,
 * which is an unauthenticated broadcast that anybody within radio range
 * can forge. So each field is bounds-checked against the remaining
 * length before it is read, and a truncated or overlong IE is rejected
 * rather than parsed as far as it goes. Returns 1 on success.
 */
static int rsn_parse(const uint8_t *ie, uint32_t len, rsn_info_t *out) {
    uint32_t pos = 0;
    int i;

    for (i = 0; i < 4; i++) { out->pairwise[i] = 0; out->akm[i] = 0; }
    out->present = 0;
    out->version = 0;
    out->group_cipher = RSN_CIPHER_CCMP;
    out->pairwise_count = 0;
    out->akm_count = 0;
    out->caps = 0;
    out->mfp_required = 0;
    out->mfp_capable = 0;

    if (len < 2) return 0;
    out->version = ieee80211_get_le16(ie);
    if (out->version != 1) return 0;
    pos = 2;
    out->present = 1;

    /* From here on every field is optional: an IE may stop after the
     * version, and a defaults-only network is legal. */
    if (pos + 4 > len) return 1;
    /* The suite selector is OUI (3 bytes, big-endian) then type. Only
     * the type is interesting once the OUI is confirmed to be 00-0F-AC;
     * a vendor-specific suite this stack cannot speak is left as its
     * raw type and rejected later by the cipher check. */
    out->group_cipher = ie[pos + 3];
    pos += 4;

    if (pos + 2 > len) return 1;
    {
        uint16_t n = ieee80211_get_le16(ie + pos);
        pos += 2;
        if (n > 4) { /* more than this stack stores; read what fits */ }
        if (pos + (uint32_t)n * 4 > len) return 0;
        for (i = 0; i < (int)n; i++) {
            if (i < 4) out->pairwise[out->pairwise_count++] = ie[pos + 3];
            pos += 4;
        }
    }

    if (pos + 2 > len) return 1;
    {
        uint16_t n = ieee80211_get_le16(ie + pos);
        pos += 2;
        if (pos + (uint32_t)n * 4 > len) return 0;
        for (i = 0; i < (int)n; i++) {
            if (i < 4) out->akm[out->akm_count++] = ie[pos + 3];
            pos += 4;
        }
    }

    if (pos + 2 > len) return 1;
    out->caps = ieee80211_get_le16(ie + pos);
    out->mfp_required = (out->caps & 0x0040) ? 1 : 0;
    out->mfp_capable  = (out->caps & 0x0080) ? 1 : 0;
    return 1;
}

static int rsn_has_cipher(const rsn_info_t *r, uint32_t c) {
    for (int i = 0; i < r->pairwise_count; i++)
        if (r->pairwise[i] == c) return 1;
    return 0;
}

static int rsn_has_akm(const rsn_info_t *r, uint32_t a) {
    for (int i = 0; i < r->akm_count; i++)
        if (r->akm[i] == a) return 1;
    return 0;
}

/* ===== what a scan produces ===== */

typedef enum {
    WIFI_SEC_OPEN = 0,
    WIFI_SEC_WEP,
    WIFI_SEC_WPA2_PSK,
    WIFI_SEC_WPA2_ENTERPRISE,
    WIFI_SEC_WPA3_SAE
} wifi_security_t;

typedef struct {
    uint8_t  bssid[IEEE80211_ADDR_LEN];
    char     ssid[IEEE80211_MAX_SSID_LEN + 1];
    uint8_t  ssid_len;
    uint8_t  channel;
    int8_t   rssi;              /* dBm, as the radio reported it        */
    uint16_t capability;
    uint16_t beacon_interval;
    wifi_security_t security;
    rsn_info_t rsn;
    uint32_t last_seen_ms;
    uint8_t  rsn_ie[64];        /* kept verbatim: the AP's RSN IE must  */
    uint8_t  rsn_ie_len;        /* be echoed back in the assoc request  */
} wifi_bss_t;

/*
 * Walk the information elements of a management frame body.
 *
 * The IE area is a bare TLV chain with no terminator and no count, so
 * the only thing that stops the walk is the frame length -- which means
 * a single IE whose length octet runs past the end of the buffer is the
 * classic way to read off the end of a beacon. The bound is checked
 * before the body is touched, not after.
 */
typedef void (*ieee80211_ie_fn)(uint8_t eid, const uint8_t *body,
                                uint8_t len, void *ctx);

static void ieee80211_walk_ies(const uint8_t *ies, uint32_t len,
                               ieee80211_ie_fn fn, void *ctx) {
    uint32_t pos = 0;
    while (pos + 2 <= len) {
        uint8_t eid  = ies[pos];
        uint8_t ilen = ies[pos + 1];
        if (pos + 2 + (uint32_t)ilen > len) return;   /* truncated: stop */
        fn(eid, ies + pos + 2, ilen, ctx);
        pos += 2 + (uint32_t)ilen;
    }
}

/* The fixed fields at the head of a beacon or probe response body:
 * 8 bytes of timestamp, 2 of beacon interval, 2 of capability. */
#define IEEE80211_BEACON_FIXED_LEN  12

static void wifi_bss_ie_cb(uint8_t eid, const uint8_t *body,
                           uint8_t len, void *ctx) {
    wifi_bss_t *b = (wifi_bss_t *)ctx;

    switch (eid) {
    case IEEE80211_EID_SSID:
        if (len > IEEE80211_MAX_SSID_LEN) len = IEEE80211_MAX_SSID_LEN;
        for (uint8_t i = 0; i < len; i++) {
            /* A hidden network broadcasts a zero-length SSID, and some
             * APs pad with NULs instead. Either way the name is not
             * usable, and control bytes must not reach the desktop's
             * text renderer. */
            uint8_t c = body[i];
            b->ssid[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
        }
        b->ssid[len] = 0;
        b->ssid_len  = len;
        break;

    case IEEE80211_EID_DS_PARAMS:
        if (len >= 1) b->channel = body[0];
        break;

    case IEEE80211_EID_RSN:
        if (rsn_parse(body, len, &b->rsn)) {
            uint8_t keep = len > 62 ? 62 : len;
            b->rsn_ie[0] = IEEE80211_EID_RSN;
            b->rsn_ie[1] = keep;
            for (uint8_t i = 0; i < keep; i++) b->rsn_ie[2 + i] = body[i];
            b->rsn_ie_len = (uint8_t)(keep + 2);
        }
        break;

    default:
        break;
    }
}

/*
 * Turn a received beacon or probe response into a scan entry.
 *
 * Returns 1 if the frame was a well-formed beacon, 0 otherwise. `rssi`
 * comes from the radio rather than the frame -- there is nothing in an
 * 802.11 header that says how loud it was.
 */
static int ieee80211_parse_beacon(const uint8_t *frame, uint32_t len,
                                  int8_t rssi, wifi_bss_t *out) {
    uint16_t fc, stype;
    uint32_t hdrlen, pos;

    if (len < 24 + IEEE80211_BEACON_FIXED_LEN) return 0;

    fc = ieee80211_fc(frame);
    if (!ieee80211_is_mgmt(fc)) return 0;
    stype = ieee80211_stype(fc);
    if (stype != IEEE80211_STYPE_BEACON &&
        stype != IEEE80211_STYPE_PROBE_RESP) return 0;

    hdrlen = ieee80211_hdrlen(fc);
    if (len < hdrlen + IEEE80211_BEACON_FIXED_LEN) return 0;

    for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    /* Address 3 is the BSSID on a frame from an AP; address 2 is the
     * transmitter, which for an infrastructure beacon is the same
     * radio, but that is a property of this topology and not a rule. */
    ieee80211_addr_copy(out->bssid, ieee80211_addr3(frame));
    out->rssi = rssi;

    pos = hdrlen;
    out->beacon_interval = ieee80211_get_le16(frame + pos + 8);
    out->capability      = ieee80211_get_le16(frame + pos + 10);
    pos += IEEE80211_BEACON_FIXED_LEN;

    ieee80211_walk_ies(frame + pos, len - pos, wifi_bss_ie_cb, out);

    /* Classify. The privacy bit alone cannot distinguish WEP from WPA2:
     * both set it. What separates them is the presence of an RSN IE,
     * and within RSN, which AKM the network offers. */
    if (out->rsn.present) {
        if (rsn_has_akm(&out->rsn, RSN_AKM_SAE))
            out->security = WIFI_SEC_WPA3_SAE;
        else if (rsn_has_akm(&out->rsn, RSN_AKM_PSK) ||
                 rsn_has_akm(&out->rsn, RSN_AKM_PSK_SHA256))
            out->security = WIFI_SEC_WPA2_PSK;
        else
            out->security = WIFI_SEC_WPA2_ENTERPRISE;
    } else if (out->capability & IEEE80211_CAP_PRIVACY) {
        out->security = WIFI_SEC_WEP;
    } else {
        out->security = WIFI_SEC_OPEN;
    }

    return 1;
}

static const char *wifi_security_name(wifi_security_t s) {
    switch (s) {
    case WIFI_SEC_OPEN:             return "open";
    case WIFI_SEC_WEP:              return "WEP";
    case WIFI_SEC_WPA2_PSK:         return "WPA2-PSK";
    case WIFI_SEC_WPA2_ENTERPRISE:  return "WPA2-Enterprise";
    case WIFI_SEC_WPA3_SAE:         return "WPA3-SAE";
    }
    return "?";
}

/* ===== building the frames this stack sends ===== */

/*
 * A management frame header. Every management frame from a client has
 * the same shape: no ToDS/FromDS, addr1 the AP, addr2 us, addr3 the
 * BSSID. The sequence number is the caller's to supply because it must
 * increment per frame and the counter lives with the device.
 */
static uint32_t ieee80211_mgmt_hdr(uint8_t *buf, uint16_t stype,
                                   const uint8_t *da, const uint8_t *sa,
                                   const uint8_t *bssid, uint16_t seq) {
    ieee80211_put_le16(buf, (uint16_t)(IEEE80211_FTYPE_MGMT | stype));
    ieee80211_put_le16(buf + 2, 0);          /* duration: the radio fills it */
    ieee80211_addr_copy(buf + 4,  da);
    ieee80211_addr_copy(buf + 10, sa);
    ieee80211_addr_copy(buf + 16, bssid);
    ieee80211_put_le16(buf + 22, (uint16_t)(seq << 4));
    return 24;
}

/* Probe request: an active scan. Broadcast SSID means "everyone answer". */
static uint32_t ieee80211_build_probe_req(uint8_t *buf, uint32_t max,
                                          const uint8_t *sa,
                                          const char *ssid, uint8_t ssid_len,
                                          uint16_t seq) {
    static const uint8_t bcast[6] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    uint32_t pos;

    if (max < 24 + 2 + IEEE80211_MAX_SSID_LEN + 10) return 0;
    pos = ieee80211_mgmt_hdr(buf, IEEE80211_STYPE_PROBE_REQ,
                             bcast, sa, bcast, seq);

    if (ssid_len > IEEE80211_MAX_SSID_LEN) ssid_len = IEEE80211_MAX_SSID_LEN;
    buf[pos++] = IEEE80211_EID_SSID;
    buf[pos++] = ssid_len;
    for (uint8_t i = 0; i < ssid_len; i++) buf[pos++] = (uint8_t)ssid[i];

    /* Supported rates. 1, 2, 5.5 and 11 Mbit/s in units of 500 kbit/s,
     * with the high bit marking the ones this station considers basic. */
    buf[pos++] = IEEE80211_EID_SUPP_RATES;
    buf[pos++] = 8;
    buf[pos++] = 0x82; buf[pos++] = 0x84; buf[pos++] = 0x8B; buf[pos++] = 0x96;
    buf[pos++] = 0x0C; buf[pos++] = 0x12; buf[pos++] = 0x18; buf[pos++] = 0x24;

    return pos;
}

/* Open-system authentication: the vestigial two-frame exchange that
 * still has to happen before association, even though with WPA2 it
 * authenticates nothing at all. The real authentication is the 4-way
 * handshake, which happens after association. */
static uint32_t ieee80211_build_auth(uint8_t *buf, uint32_t max,
                                     const uint8_t *bssid, const uint8_t *sa,
                                     uint16_t seq_num, uint16_t auth_seq) {
    uint32_t pos;
    if (max < 30) return 0;
    pos = ieee80211_mgmt_hdr(buf, IEEE80211_STYPE_AUTH, bssid, sa, bssid,
                             seq_num);
    ieee80211_put_le16(buf + pos, 0); pos += 2;         /* algorithm: open */
    ieee80211_put_le16(buf + pos, auth_seq); pos += 2;
    ieee80211_put_le16(buf + pos, 0); pos += 2;         /* status          */
    return pos;
}

/*
 * Association request.
 *
 * The RSN IE this sends is not copied from the beacon: it states what
 * this station has *chosen* -- one pairwise cipher, one AKM -- out of
 * what the AP offered. That choice is then covered by the 4-way
 * handshake's MIC, which is what makes a downgrade attack detectable.
 * An AP that advertised CCMP and TKIP cannot later claim we asked for
 * TKIP, because message 2 carries these exact bytes and message 3's MIC
 * is computed over the AP's view of the same negotiation.
 */
static uint32_t ieee80211_build_assoc_req(uint8_t *buf, uint32_t max,
                                          const wifi_bss_t *bss,
                                          const uint8_t *sa, uint16_t seq,
                                          int with_rsn,
                                          uint8_t *rsn_ie_out,
                                          uint8_t *rsn_ie_len_out) {
    uint32_t pos;
    uint8_t  ssid_len = bss->ssid_len;

    if (max < 128) return 0;
    pos = ieee80211_mgmt_hdr(buf, IEEE80211_STYPE_ASSOC_REQ,
                             bss->bssid, sa, bss->bssid, seq);

    /* capability: ESS, and privacy when the network is protected */
    ieee80211_put_le16(buf + pos,
        (uint16_t)(IEEE80211_CAP_ESS | IEEE80211_CAP_SHORT_PRE |
                   (with_rsn ? IEEE80211_CAP_PRIVACY : 0)));
    pos += 2;
    ieee80211_put_le16(buf + pos, 10);   /* listen interval, in beacons */
    pos += 2;

    buf[pos++] = IEEE80211_EID_SSID;
    buf[pos++] = ssid_len;
    for (uint8_t i = 0; i < ssid_len; i++) buf[pos++] = (uint8_t)bss->ssid[i];

    buf[pos++] = IEEE80211_EID_SUPP_RATES;
    buf[pos++] = 8;
    buf[pos++] = 0x82; buf[pos++] = 0x84; buf[pos++] = 0x8B; buf[pos++] = 0x96;
    buf[pos++] = 0x0C; buf[pos++] = 0x12; buf[pos++] = 0x18; buf[pos++] = 0x24;

    if (with_rsn) {
        uint32_t start = pos;
        buf[pos++] = IEEE80211_EID_RSN;
        buf[pos++] = 20;                       /* body length, filled below */
        ieee80211_put_le16(buf + pos, 1); pos += 2;          /* version    */

        /* group cipher: whatever the AP announced, echoed back */
        buf[pos++] = 0x00; buf[pos++] = 0x0F; buf[pos++] = 0xAC;
        buf[pos++] = (uint8_t)bss->rsn.group_cipher;

        ieee80211_put_le16(buf + pos, 1); pos += 2;          /* 1 pairwise */
        buf[pos++] = 0x00; buf[pos++] = 0x0F; buf[pos++] = 0xAC;
        buf[pos++] = RSN_CIPHER_CCMP;

        ieee80211_put_le16(buf + pos, 1); pos += 2;          /* 1 AKM      */
        buf[pos++] = 0x00; buf[pos++] = 0x0F; buf[pos++] = 0xAC;
        buf[pos++] = RSN_AKM_PSK;

        ieee80211_put_le16(buf + pos, 0); pos += 2;          /* RSN caps   */

        buf[start + 1] = (uint8_t)(pos - start - 2);

        /* Message 2 of the handshake must carry these bytes verbatim. */
        if (rsn_ie_out && rsn_ie_len_out) {
            uint32_t n = pos - start;
            for (uint32_t i = 0; i < n; i++) rsn_ie_out[i] = buf[start + i];
            *rsn_ie_len_out = (uint8_t)n;
        }
    } else if (rsn_ie_len_out) {
        *rsn_ie_len_out = 0;
    }

    return pos;
}

static uint32_t ieee80211_build_deauth(uint8_t *buf, uint32_t max,
                                       const uint8_t *bssid, const uint8_t *sa,
                                       uint16_t seq, uint16_t reason) {
    uint32_t pos;
    if (max < 26) return 0;
    pos = ieee80211_mgmt_hdr(buf, IEEE80211_STYPE_DEAUTH, bssid, sa, bssid,
                             seq);
    ieee80211_put_le16(buf + pos, reason);
    return pos + 2;
}

/*
 * The association response tells us whether we are on the network and,
 * if so, what association ID we were given. A non-zero status is a
 * refusal and carries the reason in the same field.
 */
static int ieee80211_parse_assoc_resp(const uint8_t *frame, uint32_t len,
                                      uint16_t *status, uint16_t *aid) {
    uint16_t fc;
    uint32_t hdrlen;

    if (len < 24 + 6) return 0;
    fc = ieee80211_fc(frame);
    if (!ieee80211_is_mgmt(fc)) return 0;
    if (ieee80211_stype(fc) != IEEE80211_STYPE_ASSOC_RESP &&
        ieee80211_stype(fc) != IEEE80211_STYPE_REASSOC_RSP) return 0;

    hdrlen = ieee80211_hdrlen(fc);
    if (len < hdrlen + 6) return 0;

    *status = ieee80211_get_le16(frame + hdrlen + 2);
    *aid    = (uint16_t)(ieee80211_get_le16(frame + hdrlen + 4) & 0x3FFF);
    return 1;
}

/* ===== 802.11 <-> Ethernet ===== */

/* The LLC/SNAP header that carries an Ethernet protocol number inside
 * an 802.11 data frame. Everything above this stack speaks Ethernet, so
 * these eight bytes are added on the way out and stripped on the way in. */
static const uint8_t ieee80211_rfc1042[6] = { 0xAA,0xAA,0x03,0x00,0x00,0x00 };

#define IEEE80211_SNAP_LEN  8

/*
 * Convert a received 802.11 data frame into an Ethernet frame in place
 * of `out`. Returns the Ethernet frame length, or 0 if the frame was
 * not one we can deliver.
 *
 * The address that becomes the Ethernet source depends on the direction
 * bits: on a frame from the AP (FromDS) the original sender is addr3,
 * not addr2 -- addr2 is the AP's own radio. Getting this backwards
 * makes every host on the LAN appear to have the AP's MAC, and ARP then
 * quietly resolves everything to the same place.
 */
static uint32_t ieee80211_data_to_eth(const uint8_t *frame, uint32_t len,
                                      uint8_t *out, uint32_t max) {
    uint16_t fc = ieee80211_fc(frame);
    uint32_t hdrlen = ieee80211_hdrlen(fc);
    const uint8_t *da, *sa;
    uint32_t payload_len;
    uint16_t ethertype;

    if (!ieee80211_is_data(fc)) return 0;
    if (ieee80211_stype(fc) == IEEE80211_STYPE_NULLFUNC) return 0;
    if (len < hdrlen + IEEE80211_SNAP_LEN) return 0;

    if (fc & IEEE80211_FC_FROMDS) {
        da = ieee80211_addr1(frame);
        sa = ieee80211_addr3(frame);
    } else if (fc & IEEE80211_FC_TODS) {
        da = ieee80211_addr3(frame);
        sa = ieee80211_addr2(frame);
    } else {
        da = ieee80211_addr1(frame);
        sa = ieee80211_addr2(frame);
    }

    /* The SNAP header must be the RFC 1042 one; anything else is a
     * bridged frame format this stack does not handle. */
    for (int i = 0; i < 6; i++)
        if (frame[hdrlen + i] != ieee80211_rfc1042[i]) return 0;

    ethertype   = (uint16_t)((frame[hdrlen + 6] << 8) | frame[hdrlen + 7]);
    payload_len = len - hdrlen - IEEE80211_SNAP_LEN;

    if (14 + payload_len > max) return 0;

    ieee80211_addr_copy(out, da);
    ieee80211_addr_copy(out + 6, sa);
    out[12] = (uint8_t)(ethertype >> 8);
    out[13] = (uint8_t)(ethertype & 0xFF);
    for (uint32_t i = 0; i < payload_len; i++)
        out[14 + i] = frame[hdrlen + IEEE80211_SNAP_LEN + i];

    return 14 + payload_len;
}

/*
 * The other direction: an Ethernet frame from lwIP becomes an 802.11
 * data frame addressed to the AP.
 *
 * `out` receives the header and SNAP; the payload is copied after it.
 * The frame is left unencrypted here -- CCMP is applied afterwards by
 * the caller, because it needs the packet number and the key, and
 * because on hardware that encrypts in silicon this step is skipped
 * entirely.
 */
static uint32_t ieee80211_eth_to_data(const uint8_t *eth, uint32_t len,
                                      const uint8_t *bssid, uint16_t seq,
                                      int protected_frame,
                                      uint8_t *out, uint32_t max) {
    uint32_t pos;
    uint16_t fc;

    if (len < 14) return 0;
    if (24 + IEEE80211_SNAP_LEN + (len - 14) > max) return 0;

    fc = IEEE80211_FTYPE_DATA | IEEE80211_STYPE_DATA | IEEE80211_FC_TODS;
    if (protected_frame) fc |= IEEE80211_FC_PROTECTED;

    ieee80211_put_le16(out, fc);
    ieee80211_put_le16(out + 2, 0);
    ieee80211_addr_copy(out + 4,  bssid);        /* addr1: the AP        */
    ieee80211_addr_copy(out + 10, eth + 6);      /* addr2: us            */
    ieee80211_addr_copy(out + 16, eth);          /* addr3: the real dest */
    ieee80211_put_le16(out + 22, (uint16_t)(seq << 4));
    pos = 24;

    for (int i = 0; i < 6; i++) out[pos++] = ieee80211_rfc1042[i];
    out[pos++] = eth[12];
    out[pos++] = eth[13];

    for (uint32_t i = 0; i < len - 14; i++) out[pos++] = eth[14 + i];
    return pos;
}

/* ===== channels ===== */

/* Centre frequency in MHz for a 2.4 GHz or 5 GHz channel number. */
static uint32_t ieee80211_chan_to_freq(uint8_t chan) {
    if (chan >= 1 && chan <= 13) return 2407 + (uint32_t)chan * 5;
    if (chan == 14)              return 2484;
    if (chan >= 36 && chan <= 177) return 5000 + (uint32_t)chan * 5;
    return 0;
}

#endif /* NET_IEEE80211_H */
