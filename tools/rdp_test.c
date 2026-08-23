/*
 * tools/rdp_test.c — the RDP wire format, checked against the spec.
 *
 * RDP stacks five encodings in one packet and each writes lengths
 * differently: TPKT big-endian, BER variable-width, PER high-bit
 * continuation, RDP little-endian. A length written in the wrong one of
 * those produces a client that closes the socket without a message,
 * which is the least debuggable failure mode a protocol has.
 *
 * So every encoder in src/net/rdpwire.h is checked here against the
 * bytes the specification prescribes, before any of it reaches a
 * socket. tools/rdp_probe.py does the other half: it drives a real
 * connection against the running system and checks the responses.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "net/rdpwire.h"

static int checks = 0;
static int fails  = 0;

static void expect(int cond, const char *what) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

static void expect_eq(uint32_t got, uint32_t want, const char *what) {
    checks++;
    if (got == want) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s (got %u, want %u)\n", what, got, want);
}

static void hexcheck(const char *what, const uint8_t *got, uint32_t n,
                     const char *hex) {
    uint8_t want[64];
    uint32_t i;
    checks++;
    for (i = 0; i < n; i++) {
        int hi, lo;
        const char *h = hex + i * 2;
        hi = (h[0] <= '9') ? h[0] - '0' : (h[0] | 32) - 'a' + 10;
        lo = (h[1] <= '9') ? h[1] - '0' : (h[1] | 32) - 'a' + 10;
        want[i] = (uint8_t)((hi << 4) | lo);
    }
    if (memcmp(got, want, n) == 0) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s\n         got  ", what);
    for (i = 0; i < n; i++) printf("%02x", got[i]);
    printf("\n         want %s\n", hex);
}

/* ===== TPKT and X.224 ===== */

static void test_tpkt_x224(void) {
    uint8_t buf[64];
    rdp_w_t w;
    uint32_t n;

    printf("\nTPKT and X.224\n");

    /* The four-byte TPKT header: version 3, pad, big-endian length
     * that counts itself. */
    rdp_w_init(&w, buf, sizeof(buf));
    tpkt_write(&w, 0x0134);
    hexcheck("vector     TPKT header", buf, 4, "03000134");

    /* The three bytes on the front of every data PDU, forever. */
    rdp_w_init(&w, buf, sizeof(buf));
    x224_data_write(&w);
    hexcheck("vector     X.224 data header", buf, 3, "02f080");

    /* A connection confirm carrying the negotiation response. The
     * length indicator is 14 when the negotiation block is present. */
    n = x224_connection_confirm(buf, sizeof(buf), PROTOCOL_RDP, 1);
    expect_eq(n, 19, "connection confirm with negotiation is 19 bytes");
    hexcheck("vector     connection confirm", buf, 19,
             "0300"   "0013"          /* TPKT: version, pad, length 19 */
             "0e"     "d0"            /* LI 14, CC                     */
             "0000"   "0000"   "00"   /* DST-REF, SRC-REF, class       */
             "02"     "00"            /* RDP_NEG_RSP, flags            */
             "0800"                   /* length 8, little-endian here  */
             "00000000");             /* PROTOCOL_RDP                  */

    /* Without a negotiation request the confirm is the bare 11 bytes. */
    n = x224_connection_confirm(buf, sizeof(buf), PROTOCOL_RDP, 0);
    expect_eq(n, 11, "connection confirm without negotiation is 11 bytes");
    expect_eq(buf[4], 6, "vector     the length indicator drops to 6");

    /* A negotiation failure, so a CredSSP-only client can say why. */
    n = x224_negotiation_failure(buf, sizeof(buf), HYBRID_REQUIRED_BY_SERVER);
    expect_eq(n, 19, "negotiation failure is 19 bytes");
    expect_eq(buf[11], RDP_NEG_FAILURE, "vector     type is NEG_FAILURE");
}

static void test_x224_parse(void) {
    /* A real connection request: TPKT, X.224 CR, a cookie, then the
     * negotiation request asking for TLS or CredSSP. */
    const uint8_t cr[] = {
        0x03, 0x00, 0x00, 0x27,             /* TPKT, 39 bytes          */
        0x22, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
        'C','o','o','k','i','e',':',' ','m','s','t','s','h','a','s','h',
        '=','a', 0x0d, 0x0a,
        0x01, 0x00, 0x08, 0x00,             /* RDP_NEG_REQ             */
        0x03, 0x00, 0x00, 0x00              /* TLS | CredSSP           */
    };
    x224_cr_t out;

    printf("\nparsing the client's connection request\n");

    expect(x224_parse_connection_request(cr, sizeof(cr), &out) == 1,
           "a connection request with a cookie parses");
    expect(out.has_negotiation, "the negotiation request is found past it");
    expect_eq(out.requested_protocols, 0x00000003,
              "vector     requested protocols are read");

    /* A frame whose TPKT length disagrees with the buffer is a lie. */
    {
        uint8_t bad[sizeof(cr)];
        memcpy(bad, cr, sizeof(cr));
        bad[3] = 0xFF;
        expect(x224_parse_connection_request(bad, sizeof(bad), &out) == 0,
               "refuses    a TPKT length that does not match the frame");
    }

    /* An old client that sends no negotiation block wants plain RDP. */
    {
        const uint8_t bare[] = {
            0x03, 0x00, 0x00, 0x0b,
            0x06, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        expect(x224_parse_connection_request(bare, sizeof(bare), &out) == 1,
               "a bare connection request parses");
        expect(!out.has_negotiation, "and reports no negotiation");
        expect_eq(out.requested_protocols, PROTOCOL_RDP,
                  "vector     defaulting to plain RDP");
    }

    /* Truncation at every length must be refused, not read past. */
    {
        int all_refused = 1;
        for (uint32_t cut = 0; cut < sizeof(cr); cut++) {
            x224_cr_t t;
            if (x224_parse_connection_request(cr, cut, &t) != 0)
                all_refused = 0;
        }
        expect(all_refused, "refuses    every truncated prefix");
    }
}

/* ===== BER and PER lengths ===== */

static void test_lengths(void) {
    uint8_t buf[8];
    rdp_w_t w;

    printf("\nthe three ways RDP writes a length\n");

    /* BER: short form under 128, then 0x81, then 0x82. */
    rdp_w_init(&w, buf, sizeof(buf)); ber_length(&w, 0x7F);
    hexcheck("vector     BER length 127", buf, 1, "7f");
    rdp_w_init(&w, buf, sizeof(buf)); ber_length(&w, 0x80);
    hexcheck("vector     BER length 128", buf, 2, "8180");
    rdp_w_init(&w, buf, sizeof(buf)); ber_length(&w, 0x1234);
    hexcheck("vector     BER length 4660", buf, 3, "821234");

    expect_eq(ber_length_size(0x7F), 1, "BER size prediction, short form");
    expect_eq(ber_length_size(0x80), 2, "BER size prediction, one byte");
    expect_eq(ber_length_size(0x100), 3, "BER size prediction, two bytes");

    /* PER: high bit as a continuation marker, a different scheme one
     * layer inside the BER above. */
    rdp_w_init(&w, buf, sizeof(buf)); per_length(&w, 0x7F);
    hexcheck("vector     PER length 127", buf, 1, "7f");
    rdp_w_init(&w, buf, sizeof(buf)); per_length(&w, 0x80);
    hexcheck("vector     PER length 128", buf, 2, "8080");
    rdp_w_init(&w, buf, sizeof(buf)); per_length(&w, 0x1F4);
    hexcheck("vector     PER length 500", buf, 2, "81f4");

    expect_eq(per_length_size(0x7F), 1, "PER size prediction, one byte");
    expect_eq(per_length_size(0x80), 2, "PER size prediction, two bytes");

    /* PER integers are offsets from the type's minimum. */
    rdp_w_init(&w, buf, sizeof(buf));
    per_integer16(&w, 1004, MCS_BASE_CHANNEL_ID);
    hexcheck("vector     channel 1004 with minimum 1001", buf, 2, "0003");

    rdp_w_init(&w, buf, sizeof(buf));
    per_integer16(&w, 0x79F3, 1001);
    hexcheck("vector     the GCC node id", buf, 2, "760a");

    /* BER integers narrow to the smallest width that fits. */
    rdp_w_init(&w, buf, sizeof(buf)); ber_integer(&w, 34);
    hexcheck("vector     BER integer 34", buf, 3, "020122");
    rdp_w_init(&w, buf, sizeof(buf)); ber_integer(&w, 0xFFF8);
    hexcheck("vector     BER integer 65528", buf, 4, "0202fff8");
}

/* ===== MCS ===== */

static void test_mcs(void) {
    uint8_t buf[64];
    uint32_t n;

    printf("\nMCS\n");

    /* Attach User Confirm: choice 11, options 2, success, then the
     * user id offset from 1001. */
    n = mcs_attach_user_confirm(buf, sizeof(buf), 1002);
    expect_eq(n, 11, "attach user confirm is 11 bytes");
    hexcheck("vector     attach user confirm", buf, 11,
             "0300000b"   /* TPKT, 11                                  */
             "02f080"     /* X.224 data                                */
             "2e"         /* (11 << 2) | 2                             */
             "00"         /* result: success                           */
             "0001");     /* 1002 - 1001                               */

    /* Channel Join Confirm: the channel appears twice, once as
     * requested and once as joined. */
    n = mcs_channel_join_confirm(buf, sizeof(buf), 1002, 1003);
    expect_eq(n, 15, "channel join confirm is 15 bytes");
    hexcheck("vector     channel join confirm", buf, 15,
             "0300000f"
             "02f080"
             "3e"         /* (15 << 2) | 2                             */
             "00"
             "0001"       /* initiator, offset from 1001               */
             "03eb"       /* requested channel, offset 0               */
             "03eb");     /* joined channel                            */

    /* Send Data Indication, and the overhead it costs. A payload under
     * 128 bytes gets a one-byte PER length; over, two. */
    {
        rdp_w_t w;
        rdp_w_init(&w, buf, sizeof(buf));
        mcs_send_data_indication(&w, 1002, MCS_GLOBAL_CHANNEL, 100);
        hexcheck("vector     send data indication, short payload", buf, 14,
                 "03000000"   /* length patched by the caller          */
                 "02f080"
                 "68"         /* 26 << 2                               */
                 "03ea"       /* initiator: the raw user id            */
                 "03eb"       /* channel                               */
                 "70"         /* priority, not segmented               */
                 "64");       /* PER length 100                        */
        expect_eq(w.n, 14, "and is fourteen bytes");
        expect_eq(mcs_send_overhead(100), 14, "the overhead matches");
    }
    {
        rdp_w_t w;
        rdp_w_init(&w, buf, sizeof(buf));
        mcs_send_data_indication(&w, 1002, MCS_GLOBAL_CHANNEL, 4000);
        expect_eq(w.n, 15, "a long payload costs one byte more");
        expect_eq(mcs_send_overhead(4000), 15, "and the prediction agrees");
        expect_eq((uint32_t)((buf[13] << 8) | buf[14]), 0x8FA0u,
                  "vector     PER length 4000 sets the continuation bit");
    }
}

/* ===== share headers ===== */

static void test_share(void) {
    uint8_t buf[32];
    rdp_w_t w;

    printf("\nshare control and share data headers\n");

    rdp_w_init(&w, buf, sizeof(buf));
    share_control_write(&w, 0x1234, PDUTYPE_DEMANDACTIVEPDU, 1002);
    hexcheck("vector     share control header", buf, 6,
             "3412"       /* total length, little-endian               */
             "1100"       /* type 1 with the version nibble            */
             "ea03");     /* source: 1002                              */
    expect_eq(w.n, SHARE_CONTROL_HDR_LEN, "share control header is 6 bytes");

    rdp_w_init(&w, buf, sizeof(buf));
    share_data_write(&w, 0x40, 1002, 0x000103EA, PDUTYPE2_UPDATE, 0x40);
    expect_eq(w.n, SHARE_DATA_HDR_LEN, "share data header is 18 bytes");
    hexcheck("vector     share data header", buf, 18,
             "4000"
             "1700"       /* PDUTYPE_DATAPDU with version              */
             "ea03"
             "ea030100"   /* share id                                  */
             "00"         /* pad                                       */
             "01"         /* STREAM_LOW                                */
             "4000"       /* uncompressed length                       */
             "02"         /* PDUTYPE2_UPDATE                           */
             "00"         /* not compressed                            */
             "0000");
}

/* ===== bitmaps ===== */

static void test_bitmap(void) {
    printf("\nbitmap updates\n");

    /* RGB565 packing, at the extremes where rounding shows. */
    expect_eq(rdp_rgb565(0x000000), 0x0000, "vector     black");
    expect_eq(rdp_rgb565(0xFFFFFF), 0xFFFF, "vector     white");
    expect_eq(rdp_rgb565(0xFF0000), 0xF800, "vector     red");
    expect_eq(rdp_rgb565(0x00FF00), 0x07E0, "vector     green");
    expect_eq(rdp_rgb565(0x0000FF), 0x001F, "vector     blue");
    /* Truncation, not rounding: the low bits are dropped. */
    expect_eq(rdp_rgb565(0x080402), 0x0820, "vector     low bits truncate");

    /* The rectangle header. destRight and destBottom are inclusive,
     * which is the difference between a clean picture and a stale
     * column down the right of every tile. */
    {
        uint8_t buf[32];
        rdp_w_t w;
        rdp_w_init(&w, buf, sizeof(buf));
        rdp_bitmap_rect_header(&w, 64, 128, 64, 64, 16, 8192);
        expect_eq(w.n, 18, "the rectangle header is 18 bytes");
        hexcheck("vector     bitmap rectangle header", buf, 18,
                 "4000"   /* destLeft 64                               */
                 "8000"   /* destTop 128                               */
                 "7f00"   /* destRight 127, inclusive                  */
                 "bf00"   /* destBottom 191, inclusive                 */
                 "4000"   /* width 64                                  */
                 "4000"   /* height 64                                 */
                 "1000"   /* 16 bits per pixel                         */
                 "0000"   /* no compression                            */
                 "0020"); /* 8192 bytes of data                        */
    }

    /*
     * Tile data is bottom-up, the way a Windows DIB is stored. A
     * top-down copy mirrors every tile vertically, which reads as
     * corruption rather than as an inverted image.
     */
    {
        uint32_t src[4 * 2];
        uint8_t  out[64];
        rdp_w_t  w;

        /* two rows: the top row red, the bottom row blue */
        for (int i = 0; i < 4; i++) src[i]     = 0xFF0000;
        for (int i = 0; i < 4; i++) src[4 + i] = 0x0000FF;

        rdp_w_init(&w, out, sizeof(out));
        rdp_tile_rgb565(&w, src, 4, 0, 0, 4, 2);
        expect_eq(w.n, 16, "a 4x2 tile is 16 bytes at 16bpp");

        expect_eq((uint32_t)(out[0] | (out[1] << 8)), 0x001F,
                  "vector     the first row on the wire is the bottom one");
        expect_eq((uint32_t)(out[8] | (out[9] << 8)), 0xF800,
                  "vector     and the last is the top one");
    }

    /* A sub-tile at the right edge must read from the right place. */
    {
        uint32_t src[8 * 8];
        uint8_t  out[64];
        rdp_w_t  w;
        for (int i = 0; i < 64; i++) src[i] = 0x000000;
        src[3 * 8 + 6] = 0xFFFFFF;      /* x=6, y=3                    */

        rdp_w_init(&w, out, sizeof(out));
        rdp_tile_rgb565(&w, src, 8, 4, 2, 4, 4);   /* 4x4 tile at (4,2) */
        expect_eq(w.n, 32, "a 4x4 tile is 32 bytes");
        /* y=3 is the second row of the tile, so bottom-up it is the
         * third row out; x=6 is column 2 of the tile. */
        expect_eq((uint32_t)(out[2 * 8 + 2 * 2] |
                             (out[2 * 8 + 2 * 2 + 1] << 8)), 0xFFFF,
                  "vector     an offset tile reads the right pixel");
    }
}

/* ===== input ===== */

static void test_input(void) {
    printf("\ninput events\n");

    /* One mouse event: twelve bytes, four of which are a timestamp. */
    {
        const uint8_t ev[] = {
            0x00, 0x00, 0x00, 0x00,         /* eventTime               */
            0x01, 0x80,                     /* INPUT_EVENT_MOUSE       */
            0x00, 0x18,                     /* PTRFLAGS_MOVE           */
            0x40, 0x01,                     /* x = 320                 */
            0xF0, 0x00                      /* y = 240                 */
        };
        rdp_r_t r;
        rdp_input_event_t e;

        rdp_r_init(&r, ev, sizeof(ev));
        expect(rdp_parse_input_event(&r, &e) == 1, "a mouse event parses");
        expect_eq(e.type, INPUT_EVENT_MOUSE, "vector     type is mouse");
        expect_eq(e.a, 320, "vector     x is read");
        expect_eq(e.b, 240, "vector     y is read");
        expect(e.flags & PTRFLAGS_MOVE, "vector     the move flag is set");
    }

    /* A truncated event must be refused rather than read past. */
    {
        const uint8_t half[] = { 0,0,0,0, 0x01, 0x80 };
        rdp_r_t r;
        rdp_input_event_t e;
        rdp_r_init(&r, half, sizeof(half));
        expect(rdp_parse_input_event(&r, &e) == 0,
               "refuses    a truncated input event");
    }

    /* Scancodes. These are PC/XT set 1 make codes, the same the PS/2
     * driver decodes, which is why the table is a pass-through. */
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1E, 0, 0, 0), 'a',
              "vector     scancode 0x1E is 'a'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1E, 0, 1, 0), 'A',
              "vector     with shift it is 'A'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1E, 0, 0, 1), 'A',
              "vector     caps lock also gives 'A'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1E, 0, 1, 1), 'a',
              "vector     shift and caps together give 'a'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x02, 0, 0, 0), '1',
              "vector     scancode 0x02 is '1'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x02, 0, 1, 0), '!',
              "vector     with shift it is '!'");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x02, 0, 0, 1), '1',
              "vector     caps lock does not affect digits");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1C, 0, 0, 0), '\n',
              "vector     enter");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x0E, 0, 0, 0), '\b',
              "vector     backspace");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x39, 0, 0, 0), ' ',
              "vector     space");

    /* The arrow cluster arrives extended and shares scancodes with the
     * keypad; without honouring the flag, Up types an '8'. */
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x48, 1, 0, 0), 0x11,
              "vector     extended 0x48 is KEY_UP");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x50, 1, 0, 0), 0x12,
              "vector     extended 0x50 is KEY_DOWN");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x53, 1, 0, 0), 0x19,
              "vector     extended 0x53 is KEY_DEL");
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1C, 1, 0, 0), '\n',
              "vector     keypad enter is still enter");

    /* An out-of-range scancode must not index off the table. */
    expect_eq((uint32_t)(uint8_t)rdp_scancode_to_char(0x1FF, 0, 0, 0), 0,
              "refuses    a scancode past the table");
}

/* ===== the writer's own bounds ===== */

static void test_bounds(void) {
    uint8_t small[4];
    rdp_w_t w;
    rdp_r_t r;

    printf("\nbounds\n");

    rdp_w_init(&w, small, sizeof(small));
    rdp_u32(&w, 0x11223344);
    expect(!w.overflow, "a writer that exactly fills does not overflow");
    rdp_u8(&w, 0xFF);
    expect(w.overflow, "refuses    one byte past the end");
    expect_eq(w.n, 4, "and does not advance");

    {
        const uint8_t two[2] = { 1, 2 };
        rdp_r_init(&r, two, sizeof(two));
        rdp_r16(&r);
        expect(!r.underflow, "a reader that exactly drains does not underflow");
        rdp_r8(&r);
        expect(r.underflow, "refuses    reading past the end");
        expect_eq(rdp_left(&r), 0, "and reports nothing left");
    }

    /* A connection confirm into a buffer too small must report failure
     * rather than emitting a truncated PDU. */
    {
        uint8_t tiny[8];
        expect_eq(x224_connection_confirm(tiny, sizeof(tiny), PROTOCOL_RDP, 1),
                  0, "refuses    a PDU that will not fit");
    }
}

int main(void) {
    printf("Vextro RDP: TPKT, X.224, MCS, share headers, bitmaps, input\n");
    printf("===========================================================\n");

    test_tpkt_x224();
    test_x224_parse();
    test_lengths();
    test_mcs();
    test_share();
    test_bitmap();
    test_input();
    test_bounds();

    printf("\n%d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
