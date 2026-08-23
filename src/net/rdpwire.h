#ifndef NET_RDPWIRE_H
#define NET_RDPWIRE_H

/*
 * src/net/rdpwire.h — the bytes RDP is made of.
 *
 * RDP is not one protocol, it is five stacked on each other, and each
 * layer was designed by different people in a different decade with a
 * different idea of how to write a number down. From the outside in:
 *
 *   TPKT      RFC 1006     a 4-byte frame around everything
 *   X.224     ITU-T        the connection handshake, then a 3-byte
 *                          data header forever after
 *   MCS       ITU-T T.125  BER for the connect exchange, then a
 *                          compact PER-ish encoding for everything else
 *   GCC       ITU-T T.124  PER, nested inside the MCS connect as an
 *                          octet string
 *   RDP       MS-RDPBCGR   little-endian structures, finally
 *
 * So the same PDU can carry a length written big-endian (TPKT), a
 * length written in BER's variable-width form (MCS connect), a length
 * written in PER's high-bit-continuation form (GCC), and a length
 * written little-endian (RDP) -- in that order, in one packet. Getting
 * one of them wrong produces a client that disconnects with no message.
 * That is the reason this file exists separately from the state machine
 * in rdp.c: every encoder here is a pure function of its arguments and
 * tools/rdp_test.c checks them against bytes captured from the
 * protocol specification.
 *
 * Freestanding: no allocation, no libc, integers only. The caller owns
 * every buffer and every writer is bounds-checked -- a truncated PDU
 * that gets sent anyway is a protocol desync, and desync in RDP means
 * the client hangs rather than errors.
 */

#include <stdint.h>

/* ===== a bounds-checked writer ===== */

typedef struct {
    uint8_t *buf;
    uint32_t cap;
    uint32_t n;
    int      overflow;
} rdp_w_t;

static void rdp_w_init(rdp_w_t *w, uint8_t *buf, uint32_t cap) {
    w->buf = buf; w->cap = cap; w->n = 0; w->overflow = 0;
}

static void rdp_u8(rdp_w_t *w, uint8_t v) {
    if (w->n + 1 > w->cap) { w->overflow = 1; return; }
    w->buf[w->n++] = v;
}

/* little-endian: the RDP layer proper */
static void rdp_u16(rdp_w_t *w, uint16_t v) {
    rdp_u8(w, (uint8_t)(v & 0xFF));
    rdp_u8(w, (uint8_t)(v >> 8));
}

static void rdp_u32(rdp_w_t *w, uint32_t v) {
    rdp_u16(w, (uint16_t)(v & 0xFFFF));
    rdp_u16(w, (uint16_t)(v >> 16));
}

/* big-endian: TPKT, X.224 and every MCS field that is not a length */
static void rdp_u16be(rdp_w_t *w, uint16_t v) {
    rdp_u8(w, (uint8_t)(v >> 8));
    rdp_u8(w, (uint8_t)(v & 0xFF));
}

static void rdp_u32be(rdp_w_t *w, uint32_t v) {
    rdp_u16be(w, (uint16_t)(v >> 16));
    rdp_u16be(w, (uint16_t)(v & 0xFFFF));
}

static void rdp_bytes(rdp_w_t *w, const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) rdp_u8(w, b[i]);
}

static void rdp_fill(rdp_w_t *w, uint8_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) rdp_u8(w, v);
}

/* ===== a bounds-checked reader ===== */

typedef struct {
    const uint8_t *buf;
    uint32_t       len;
    uint32_t       n;
    int            underflow;
} rdp_r_t;

static void rdp_r_init(rdp_r_t *r, const uint8_t *buf, uint32_t len) {
    r->buf = buf; r->len = len; r->n = 0; r->underflow = 0;
}

static uint8_t rdp_r8(rdp_r_t *r) {
    if (r->n + 1 > r->len) { r->underflow = 1; return 0; }
    return r->buf[r->n++];
}

static uint16_t rdp_r16(rdp_r_t *r) {
    uint16_t a = rdp_r8(r);
    return (uint16_t)(a | ((uint16_t)rdp_r8(r) << 8));
}

static uint32_t rdp_r32(rdp_r_t *r) {
    uint32_t a = rdp_r16(r);
    return a | ((uint32_t)rdp_r16(r) << 16);
}

static uint16_t rdp_r16be(rdp_r_t *r) {
    uint16_t a = rdp_r8(r);
    return (uint16_t)((a << 8) | rdp_r8(r));
}

static void rdp_skip(rdp_r_t *r, uint32_t n) {
    if (r->n + n > r->len) { r->underflow = 1; r->n = r->len; return; }
    r->n += n;
}

static uint32_t rdp_left(const rdp_r_t *r) {
    return (r->n <= r->len) ? (r->len - r->n) : 0;
}

/* ===== TPKT (RFC 1006) ===== */

#define TPKT_VERSION    3
#define TPKT_HDR_LEN    4

/*
 * TPKT exists because X.224 was designed for a network that delivered
 * message boundaries and TCP does not. The four bytes are a version, a
 * pad, and the total length big-endian -- including the four bytes
 * themselves, which is the detail that costs an afternoon if missed.
 */
static void tpkt_write(rdp_w_t *w, uint16_t total_len) {
    rdp_u8(w, TPKT_VERSION);
    rdp_u8(w, 0);
    rdp_u16be(w, total_len);
}

/* Patch the length in once the PDU is complete, which is the only way
 * to know it without walking the structure twice. */
static void tpkt_patch(uint8_t *buf, uint32_t total_len) {
    buf[2] = (uint8_t)((total_len >> 8) & 0xFF);
    buf[3] = (uint8_t)(total_len & 0xFF);
}

/* ===== X.224 (ITU-T X.224 / T.123) ===== */

#define X224_TPDU_CONNECTION_REQUEST    0xE0
#define X224_TPDU_CONNECTION_CONFIRM    0xD0
#define X224_TPDU_DISCONNECT_REQUEST    0x80
#define X224_TPDU_DATA                  0xF0

#define X224_DATA_HDR_LEN               3

/* The RDP negotiation blob carried inside the X.224 connection
 * exchange, which is how a client says whether it wants plain RDP
 * security, TLS, or CredSSP. */
#define RDP_NEG_REQ                     0x01
#define RDP_NEG_RSP                     0x02
#define RDP_NEG_FAILURE                 0x03

#define PROTOCOL_RDP                    0x00000000
#define PROTOCOL_SSL                    0x00000001
#define PROTOCOL_HYBRID                 0x00000002

#define SSL_REQUIRED_BY_SERVER          0x00000001
#define SSL_NOT_ALLOWED_BY_SERVER       0x00000002
#define HYBRID_REQUIRED_BY_SERVER       0x00000005

/*
 * Every data PDU after the handshake carries this: length indicator 2,
 * the data TPDU code, and an end-of-transmission marker. Three bytes
 * that never change, on every single packet, forever.
 */
static void x224_data_write(rdp_w_t *w) {
    rdp_u8(w, 2);                       /* LI: two bytes follow        */
    rdp_u8(w, X224_TPDU_DATA);
    rdp_u8(w, 0x80);                    /* EOT                         */
}

/*
 * The connection confirm, with the negotiation response the client is
 * waiting for.
 *
 * A client that asked for TLS or CredSSP and is answered with a plain
 * RDP_NEG_RSP will either fall back or disconnect depending on its
 * configuration; answering with RDP_NEG_FAILURE and a reason is what
 * lets it print something useful instead of timing out.
 */
static uint32_t x224_connection_confirm(uint8_t *out, uint32_t cap,
                                        uint32_t selected_protocol,
                                        int negotiation_present) {
    rdp_w_t w;
    rdp_w_init(&w, out, cap);

    tpkt_write(&w, 0);                  /* length patched below        */

    rdp_u8(&w, negotiation_present ? 14 : 6);   /* LI                  */
    rdp_u8(&w, X224_TPDU_CONNECTION_CONFIRM);
    rdp_u16be(&w, 0);                   /* DST-REF                     */
    rdp_u16be(&w, 0);                   /* SRC-REF                     */
    rdp_u8(&w, 0);                      /* class option                */

    if (negotiation_present) {
        rdp_u8(&w, RDP_NEG_RSP);
        rdp_u8(&w, 0);                  /* flags                       */
        rdp_u16(&w, 8);                 /* length, little-endian here  */
        rdp_u32(&w, selected_protocol);
    }

    if (w.overflow) return 0;
    tpkt_patch(out, w.n);
    return w.n;
}

static uint32_t x224_negotiation_failure(uint8_t *out, uint32_t cap,
                                         uint32_t reason) {
    rdp_w_t w;
    rdp_w_init(&w, out, cap);

    tpkt_write(&w, 0);
    rdp_u8(&w, 14);
    rdp_u8(&w, X224_TPDU_CONNECTION_CONFIRM);
    rdp_u16be(&w, 0);
    rdp_u16be(&w, 0);
    rdp_u8(&w, 0);
    rdp_u8(&w, RDP_NEG_FAILURE);
    rdp_u8(&w, 0);
    rdp_u16(&w, 8);
    rdp_u32(&w, reason);

    if (w.overflow) return 0;
    tpkt_patch(out, w.n);
    return w.n;
}

/*
 * Read the client's connection request.
 *
 * Everything after the fixed header is optional: there may be a routing
 * token, a cookie, an RDP negotiation request, or nothing at all. A
 * client that sends nothing wants plain RDP security, which is the
 * oldest behaviour and still legal.
 */
typedef struct {
    int      has_negotiation;
    uint32_t requested_protocols;
} x224_cr_t;

static int x224_parse_connection_request(const uint8_t *buf, uint32_t len,
                                         x224_cr_t *out) {
    rdp_r_t r;
    uint8_t li, code;
    uint32_t body_end;

    out->has_negotiation = 0;
    out->requested_protocols = PROTOCOL_RDP;

    if (len < TPKT_HDR_LEN + 7) return 0;

    rdp_r_init(&r, buf, len);
    if (rdp_r8(&r) != TPKT_VERSION) return 0;
    rdp_skip(&r, 1);
    {
        uint16_t tp = rdp_r16be(&r);
        if (tp != len) return 0;        /* the frame must be complete  */
    }

    li   = rdp_r8(&r);
    code = rdp_r8(&r);
    if (code != X224_TPDU_CONNECTION_REQUEST) return 0;

    /* The length indicator counts from the byte after itself. */
    body_end = TPKT_HDR_LEN + 1 + li;
    if (body_end > len) return 0;

    rdp_skip(&r, 5);                    /* DST-REF, SRC-REF, class     */

    /*
     * Scan the variable part for the negotiation request. A cookie is
     * "Cookie: mstshash=..." terminated by CR LF, and the negotiation
     * structure follows it if present -- so rather than parsing the
     * cookie's syntax, the first byte that looks like a well-formed
     * RDP_NEG_REQ is taken.
     */
    while (r.n + 8 <= body_end && !r.underflow) {
        if (r.buf[r.n] == RDP_NEG_REQ &&
            r.buf[r.n + 2] == 8 && r.buf[r.n + 3] == 0) {
            rdp_skip(&r, 4);
            out->requested_protocols = rdp_r32(&r);
            out->has_negotiation = 1;
            break;
        }
        r.n++;
    }

    return 1;
}

/* ===== BER, for the MCS connect exchange ===== */

#define BER_TAG_BOOLEAN         0x01
#define BER_TAG_INTEGER         0x02
#define BER_TAG_OCTET_STRING    0x04
#define BER_TAG_ENUMERATED      0x0A
#define BER_TAG_SEQUENCE        0x30

/*
 * BER lengths are variable width: below 128 the length is one byte,
 * otherwise the first byte is 0x80 plus the number of length bytes that
 * follow. Everything here writes the definite form -- the indefinite
 * form is legal BER and no RDP implementation accepts it.
 */
static void ber_length(rdp_w_t *w, uint32_t len) {
    if (len > 0xFF) {
        rdp_u8(w, 0x82);
        rdp_u16be(w, (uint16_t)len);
    } else if (len > 0x7F) {
        rdp_u8(w, 0x81);
        rdp_u8(w, (uint8_t)len);
    } else {
        rdp_u8(w, (uint8_t)len);
    }
}

/* How many bytes ber_length() will emit, needed to compute an enclosing
 * length before writing it. */
static uint32_t ber_length_size(uint32_t len) {
    if (len > 0xFF) return 3;
    if (len > 0x7F) return 2;
    return 1;
}

static void ber_integer(rdp_w_t *w, uint32_t v) {
    rdp_u8(w, BER_TAG_INTEGER);
    if (v <= 0xFF) { rdp_u8(w, 1); rdp_u8(w, (uint8_t)v); }
    else if (v <= 0xFFFF) { rdp_u8(w, 2); rdp_u16be(w, (uint16_t)v); }
    else { rdp_u8(w, 4); rdp_u32be(w, v); }
}

static void ber_enumerated(rdp_w_t *w, uint8_t v) {
    rdp_u8(w, BER_TAG_ENUMERATED);
    rdp_u8(w, 1);
    rdp_u8(w, v);
}

/* Application tags above 30 need the two-byte form: 0x7F then the tag. */
static void ber_application_tag(rdp_w_t *w, uint8_t tag, uint32_t len) {
    rdp_u8(w, 0x7F);
    rdp_u8(w, tag);
    ber_length(w, len);
}

static void ber_octet_string_tag(rdp_w_t *w, uint32_t len) {
    rdp_u8(w, BER_TAG_OCTET_STRING);
    ber_length(w, len);
}

/* ===== PER, for GCC inside the MCS connect ===== */

/*
 * PER lengths use the high bit as a continuation marker: under 128 is
 * one byte, otherwise two bytes with 0x8000 set. A different scheme
 * from BER, one layer in, in the same packet.
 */
static void per_length(rdp_w_t *w, uint32_t len) {
    if (len > 0x7F) rdp_u16be(w, (uint16_t)(len | 0x8000u));
    else            rdp_u8(w, (uint8_t)len);
}

static uint32_t per_length_size(uint32_t len) {
    return (len > 0x7F) ? 2u : 1u;
}

/* PER integers are offset from the type's declared minimum, so a
 * channel id of 1004 with a minimum of 1001 goes on the wire as 3. */
static void per_integer16(rdp_w_t *w, uint16_t v, uint16_t min) {
    rdp_u16be(w, (uint16_t)(v - min));
}

/* ===== MCS (ITU-T T.125) ===== */

#define MCS_TYPE_CONNECT_INITIAL        101
#define MCS_TYPE_CONNECT_RESPONSE       102

/* The domain MCSPDU choices, shifted into the top six bits of the
 * first byte with the low two bits carrying option flags. */
#define MCS_ERECT_DOMAIN_REQUEST        1
#define MCS_DISCONNECT_ULTIMATUM        8
#define MCS_ATTACH_USER_REQUEST         10
#define MCS_ATTACH_USER_CONFIRM         11
#define MCS_CHANNEL_JOIN_REQUEST        14
#define MCS_CHANNEL_JOIN_CONFIRM        15
#define MCS_SEND_DATA_REQUEST           25
#define MCS_SEND_DATA_INDICATION        26

#define MCS_BASE_CHANNEL_ID             1001
#define MCS_GLOBAL_CHANNEL              1003

static void mcs_pdu_header(rdp_w_t *w, uint8_t choice, uint8_t options) {
    rdp_u8(w, (uint8_t)((choice << 2) | options));
}

/* Attach User Confirm: the server assigns the client its user channel. */
static uint32_t mcs_attach_user_confirm(uint8_t *out, uint32_t cap,
                                        uint16_t user_id) {
    rdp_w_t w;
    rdp_w_init(&w, out, cap);

    tpkt_write(&w, 0);
    x224_data_write(&w);

    mcs_pdu_header(&w, MCS_ATTACH_USER_CONFIRM, 2);
    rdp_u8(&w, 0);                                  /* result: success */
    per_integer16(&w, user_id, MCS_BASE_CHANNEL_ID);

    if (w.overflow) return 0;
    tpkt_patch(out, w.n);
    return w.n;
}

/*
 * Channel Join Confirm.
 *
 * The channel id appears twice -- once as the channel that was
 * requested and once as the channel that was joined. They are the same
 * here because this server never redirects a join, but the field is
 * separate in the protocol because MCS in general may.
 */
static uint32_t mcs_channel_join_confirm(uint8_t *out, uint32_t cap,
                                         uint16_t user_id,
                                         uint16_t channel_id) {
    rdp_w_t w;
    rdp_w_init(&w, out, cap);

    tpkt_write(&w, 0);
    x224_data_write(&w);

    mcs_pdu_header(&w, MCS_CHANNEL_JOIN_CONFIRM, 2);
    rdp_u8(&w, 0);                                  /* result: success */
    per_integer16(&w, user_id, MCS_BASE_CHANNEL_ID);
    per_integer16(&w, channel_id, 0);
    rdp_u16be(&w, channel_id);

    if (w.overflow) return 0;
    tpkt_patch(out, w.n);
    return w.n;
}

/*
 * Open a Send Data Indication and leave the writer positioned for the
 * payload. The length is a PER length whose width depends on the
 * payload size, so the caller must say how big the payload will be
 * before writing it -- there is no room to patch a one-byte length into
 * two bytes afterwards.
 */
static void mcs_send_data_indication(rdp_w_t *w, uint16_t user_id,
                                     uint16_t channel_id,
                                     uint32_t payload_len) {
    tpkt_write(w, 0);
    x224_data_write(w);
    mcs_pdu_header(w, MCS_SEND_DATA_INDICATION, 0);
    rdp_u16be(w, user_id);
    rdp_u16be(w, channel_id);
    rdp_u8(w, 0x70);                    /* high priority, not segmented */
    per_length(w, payload_len);
}

/* The fixed cost of wrapping a payload: TPKT, X.224 data, the MCS
 * header and its length field. */
static uint32_t mcs_send_overhead(uint32_t payload_len) {
    return TPKT_HDR_LEN + X224_DATA_HDR_LEN + 1 + 2 + 2 + 1 +
           per_length_size(payload_len);
}

/* ===== the RDP layer proper ===== */

/* Security header flags */
#define SEC_EXCHANGE_PKT        0x0001
#define SEC_ENCRYPT             0x0008
#define SEC_INFO_PKT            0x0040
#define SEC_LICENSE_PKT         0x0080

/* Share control PDU types; the version nibble is always 1 */
#define PDUTYPE_DEMANDACTIVEPDU 0x1
#define PDUTYPE_CONFIRMACTIVEPDU 0x3
#define PDUTYPE_DEACTIVATEALLPDU 0x6
#define PDUTYPE_DATAPDU         0x7
#define PDUTYPE_SERVER_REDIR    0xA
#define PDUTYPE_VERSION         0x10

/* Share data PDU types */
#define PDUTYPE2_UPDATE                 2
#define PDUTYPE2_CONTROL                20
#define PDUTYPE2_POINTER                27
#define PDUTYPE2_INPUT                  28
#define PDUTYPE2_SYNCHRONIZE            31
#define PDUTYPE2_REFRESH_RECT           33
#define PDUTYPE2_SUPPRESS_OUTPUT        35
#define PDUTYPE2_SHUTDOWN_REQUEST       36
#define PDUTYPE2_SHUTDOWN_DENIED        37
#define PDUTYPE2_FONTLIST               39
#define PDUTYPE2_FONTMAP                40
#define PDUTYPE2_ERROR_INFO             47

#define STREAM_LOW      1
#define STREAM_MED      2
#define STREAM_HI       4

#define CTRLACTION_REQUEST_CONTROL      0x0001
#define CTRLACTION_GRANTED_CONTROL      0x0002
#define CTRLACTION_DETACH               0x0003
#define CTRLACTION_COOPERATE            0x0004

#define UPDATETYPE_ORDERS       0x0000
#define UPDATETYPE_BITMAP       0x0001
#define UPDATETYPE_PALETTE      0x0002
#define UPDATETYPE_SYNCHRONIZE  0x0003

#define SHARE_CONTROL_HDR_LEN   6
#define SHARE_DATA_HDR_LEN      18      /* control header plus 12      */

/*
 * The share control header: a total length, a type with a version
 * nibble, and the channel that sent it.
 *
 * The version nibble is 1 on every PDU any current client will accept,
 * and a zero there is the classic reason a connection dies immediately
 * after the demand-active.
 */
static void share_control_write(rdp_w_t *w, uint16_t total_len,
                                uint8_t pdu_type, uint16_t source) {
    rdp_u16(w, total_len);
    rdp_u16(w, (uint16_t)(pdu_type | PDUTYPE_VERSION));
    rdp_u16(w, source);
}

static void share_data_write(rdp_w_t *w, uint16_t total_len, uint16_t source,
                             uint32_t share_id, uint8_t pdu_type2,
                             uint16_t uncompressed_len) {
    share_control_write(w, total_len, PDUTYPE_DATAPDU, source);
    rdp_u32(w, share_id);
    rdp_u8(w, 0);                       /* pad                         */
    rdp_u8(w, STREAM_LOW);
    rdp_u16(w, uncompressed_len);
    rdp_u8(w, pdu_type2);
    rdp_u8(w, 0);                       /* not compressed              */
    rdp_u16(w, 0);                      /* compressed length           */
}

/* ===== colour conversion for bitmap updates ===== */

/*
 * The framebuffer is 32-bit 0x00RRGGBB; the wire is 16-bit RGB565.
 *
 * Sixteen bits per pixel rather than thirty-two is not a compromise
 * here, it is what makes the update bandwidth tolerable: a full 1024x768
 * frame is 1.5 MB at 16bpp and 3 MB at 32bpp, and the tile diffing
 * below only helps when something is static. Every RDP client in
 * existence supports 16bpp; not all of them negotiate 32.
 */
static inline uint16_t rdp_rgb565(uint32_t px) {
    uint32_t r = (px >> 16) & 0xFF;
    uint32_t g = (px >> 8)  & 0xFF;
    uint32_t b =  px        & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/*
 * Write one tile's pixels as an uncompressed RDP bitmap.
 *
 * Bitmap data in a bitmap update is bottom-up: the last row of the
 * rectangle comes first, the way a Windows DIB is stored. A top-down
 * copy produces a picture that is vertically mirrored inside every
 * tile, which looks like corruption rather than like an inverted image
 * and is the single most common bug in a first RDP server.
 */
static void rdp_tile_rgb565(rdp_w_t *w, const uint32_t *src, uint32_t pitch,
                            uint32_t x, uint32_t y, uint32_t tw, uint32_t th) {
    for (uint32_t row = 0; row < th; row++) {
        const uint32_t *line = src + (uint64_t)(y + th - 1 - row) * pitch + x;
        for (uint32_t col = 0; col < tw; col++)
            rdp_u16(w, rdp_rgb565(line[col]));
    }
}

/*
 * A bitmap update carrying one rectangle.
 *
 * destRight and destBottom are inclusive, which is why they are one
 * less than the obvious value. An exclusive right edge makes every tile
 * one pixel too wide and leaves a column of stale pixels down the right
 * of each one.
 */
static void rdp_bitmap_rect_header(rdp_w_t *w, uint16_t x, uint16_t y,
                                   uint16_t tw, uint16_t th,
                                   uint16_t bpp, uint16_t data_len) {
    rdp_u16(w, x);
    rdp_u16(w, y);
    rdp_u16(w, (uint16_t)(x + tw - 1));
    rdp_u16(w, (uint16_t)(y + th - 1));
    rdp_u16(w, tw);
    rdp_u16(w, th);
    rdp_u16(w, bpp);
    rdp_u16(w, 0);                      /* no compression              */
    rdp_u16(w, data_len);
}

/* ===== input events, from the client ===== */

#define INPUT_EVENT_SYNC        0x0000
#define INPUT_EVENT_SCANCODE    0x0004
#define INPUT_EVENT_UNICODE     0x0005
#define INPUT_EVENT_MOUSE       0x8001
#define INPUT_EVENT_MOUSEX      0x8002

#define KBDFLAGS_EXTENDED       0x0100
#define KBDFLAGS_DOWN           0x4000
#define KBDFLAGS_RELEASE        0x8000

#define PTRFLAGS_HWHEEL         0x0400
#define PTRFLAGS_WHEEL          0x0200
#define PTRFLAGS_WHEEL_NEGATIVE 0x0100
#define PTRFLAGS_MOVE           0x0800
#define PTRFLAGS_DOWN           0x8000
#define PTRFLAGS_BUTTON1        0x1000
#define PTRFLAGS_BUTTON2        0x2000
#define PTRFLAGS_BUTTON3        0x4000

typedef struct {
    uint16_t type;
    uint16_t flags;
    uint16_t a, b;
} rdp_input_event_t;

/*
 * Read one input event out of an input PDU body.
 *
 * Every event is twelve bytes: four of timestamp, two of type, six of
 * payload. The timestamp is the client's own tick count and is not
 * useful for anything except ordering, which TCP already guarantees.
 */
static int rdp_parse_input_event(rdp_r_t *r, rdp_input_event_t *ev) {
    if (rdp_left(r) < 12) return 0;
    rdp_skip(r, 4);                     /* eventTime                   */
    ev->type  = rdp_r16(r);
    ev->flags = rdp_r16(r);
    ev->a     = rdp_r16(r);
    ev->b     = rdp_r16(r);
    return !r->underflow;
}

/*
 * An RDP scancode is a PC/XT set 1 make code, which is exactly what the
 * PS/2 driver in this system already decodes -- so the mapping is a
 * table of the codes whose meaning differs, and a pass-through for
 * everything else.
 *
 * Returns the character this system's keyboard layer would have
 * produced, or 0 for a key that has no character.
 */
static char rdp_scancode_to_char(uint16_t scancode, int extended,
                                 int shift, int caps) {
    static const char base[128] = {
        0,   27, '1','2','3','4','5','6','7','8','9','0','-','=', '\b','\t',
        'q','w','e','r','t','y','u','i','o','p','[',']','\n', 0,  'a','s',
        'd','f','g','h','j','k','l',';','\'','`', 0, '\\','z','x','c','v',
        'b','n','m',',','.','/', 0,  '*', 0,  ' ', 0,  0,   0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  '-', 0,  0,  0,  '+', 0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
    };
    static const char shifted[128] = {
        0,   27, '!','@','#','$','%','^','&','*','(',')','_','+', '\b','\t',
        'Q','W','E','R','T','Y','U','I','O','P','{','}','\n', 0,  'A','S',
        'D','F','G','H','J','K','L',':','"','~', 0,  '|','Z','X','C','V',
        'B','N','M','<','>','?', 0,  '*', 0,  ' ', 0,  0,   0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  '-', 0,  0,  0,  '+', 0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
        0,   0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
    };
    char c;

    if (scancode >= 128) return 0;

    /* The arrow and navigation cluster arrives with the extended flag
     * set and shares its scancodes with the numeric keypad. Without
     * honouring the flag, pressing Up types an '8'. */
    if (extended) {
        switch (scancode) {
        case 0x48: return 0x11;         /* KEY_UP    */
        case 0x50: return 0x12;         /* KEY_DOWN  */
        case 0x4B: return 0x13;         /* KEY_LEFT  */
        case 0x4D: return 0x14;         /* KEY_RIGHT */
        case 0x49: return 0x15;         /* KEY_PGUP  */
        case 0x51: return 0x16;         /* KEY_PGDN  */
        case 0x47: return 0x17;         /* KEY_HOME  */
        case 0x4F: return 0x18;         /* KEY_END   */
        case 0x53: return 0x19;         /* KEY_DEL   */
        case 0x1C: return '\n';         /* keypad enter */
        default:   return 0;
        }
    }

    /* The same cluster again, for clients that do not set the flag. */
    switch (scancode) {
    case 0x48: return 0x11;
    case 0x50: return 0x12;
    case 0x4B: return 0x13;
    case 0x4D: return 0x14;
    case 0x49: return 0x15;
    case 0x51: return 0x16;
    case 0x47: return 0x17;
    case 0x4F: return 0x18;
    case 0x53: return 0x19;
    default: break;
    }

    c = shift ? shifted[scancode] : base[scancode];

    /* Caps lock affects letters and nothing else, which is why it
     * cannot simply be folded into the shift flag. */
    if (caps && !shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (caps && shift  && c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');

    return c;
}

#endif /* NET_RDPWIRE_H */
