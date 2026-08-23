#ifndef NET_RDP_C
#define NET_RDP_C

/*
 * src/net/rdp.c — the desktop, over a socket.
 *
 * A remote desktop server: a real RDP client connects to port 3389,
 * gets the running Vextro desktop as a stream of bitmap updates, and
 * its mouse and keyboard drive the session.
 *
 * ---- the layer cake ----
 *
 * Connection is a sequence of handshakes, each in a different encoding,
 * and all of them have to complete before a single pixel moves:
 *
 *   1. X.224 connection request / confirm      (rdpwire.h)
 *   2. MCS Connect-Initial / Connect-Response  BER, wrapping GCC in PER
 *   3. MCS Erect Domain, Attach User           compact MCS
 *   4. MCS Channel Join, once per channel
 *   5. Client Info PDU                         the user name, the flags
 *   6. Licensing                               server says "no licence
 *                                              needed" and moves on
 *   7. Demand Active / Confirm Active          capability negotiation
 *   8. Synchronise, Control, Font List/Map     the finishing handshake
 *   9. bitmap updates out, input events in     forever
 *
 * Steps 1 through 8 are about four hundred bytes in each direction and
 * every one of them can fail silently: an RDP client that dislikes a
 * PDU closes the socket without saying why. That is why the wire
 * encoders live in rdpwire.h with their own tests, and why this file
 * logs each state transition.
 *
 * ---- security, stated plainly ----
 *
 * This server negotiates PROTOCOL_RDP with ENCRYPTION_METHOD_NONE.
 * That is a legal, specified configuration and real clients support it
 * (FreeRDP with `/sec:rdp`, mstsc with the matching policy), but it
 * means **the session is plaintext on the wire** -- keystrokes, the
 * password typed into the login panel, and the contents of the screen.
 * There is no TLS here and no CredSSP, so there is also no server
 * authentication: anything that can reach the port can be the desktop
 * as far as a client is concerned, and anything on the path can read
 * and modify the session.
 *
 * That is a deliberate scope decision -- RDP's own security layer needs
 * a server certificate and RSA key exchange, and TLS would need the
 * TLS *server* half that tlsglue.c does not have -- but it is not a
 * detail to be discovered later. rdp_is_encrypted() returns 0 and the
 * terminal command says so before anyone starts the service.
 *
 * ---- what runs ----
 *
 * Unlike the wireless and video work in this tree, all of this executes
 * on the machines it is developed on: it is software over TCP and the
 * network stack is up in QEMU. tools/rdp_probe.py drives a real
 * connection through the handshake from the host and checks each
 * response, and tools/rdp_test.c checks the encoders against the bytes
 * the specification prescribes.
 */

#include <stdint.h>
#include "../vxnet.h"
#include "rdpwire.h"

#define RDP_PORT            3389
#define RDP_TILE            32          /* the damage granularity      */
#define RDP_OUT_BUF         (256 * 1024)
#define RDP_IN_BUF          (64 * 1024)
#define RDP_MAX_CHANNELS    16
#define RDP_SHARE_ID        0x000103EA

static void rdp_log(const char *msg) {
    serial_puts("[rdp] ");
    serial_puts(msg);
    serial_putc('\n');
}

static void rdp_log_num(const char *msg, uint32_t v) {
    serial_puts("[rdp] ");
    serial_puts(msg);
    serial_put_dec(v);
    serial_putc('\n');
}

/* ===== the input event ring =====
 *
 * The network thread parses input PDUs; the compositor thread applies
 * them. They must not both touch the desktop's state, so events cross
 * between them through a ring with one producer and one consumer.
 *
 * No lock. On x86 a single-producer single-consumer ring is safe with
 * nothing more than the store ordering the processor already
 * guarantees: the producer writes the slot before advancing the head,
 * the consumer reads the slot after reading the head, and stores are
 * not reordered with other stores. A lock here would mean the network
 * thread could block the compositor, which is exactly the thing a
 * remote session must never do.
 */

#define RDP_EVQ_SIZE    256             /* power of two                */

typedef struct {
    uint8_t  kind;                      /* 1 = key, 2 = mouse          */
    char     ch;                        /* for a key event             */
    int32_t  x, y;
    uint8_t  buttons;
    int32_t  wheel;
} rdp_event_t;

static rdp_event_t      rdp_evq[RDP_EVQ_SIZE];
static volatile uint32_t rdp_evq_head = 0;   /* written by the network */
static volatile uint32_t rdp_evq_tail = 0;   /* written by the desktop */

static void rdp_evq_push(const rdp_event_t *ev) {
    uint32_t head = rdp_evq_head;
    uint32_t next = (head + 1) & (RDP_EVQ_SIZE - 1);

    /* A full queue drops the newest event rather than blocking or
     * overwriting the oldest. Dropping the newest loses one keystroke
     * under extreme load; overwriting the oldest would deliver input
     * out of order, which is far worse than losing it. */
    if (next == rdp_evq_tail) return;

    rdp_evq[head] = *ev;
    __asm__ volatile("" ::: "memory");  /* fill the slot, then publish */
    rdp_evq_head = next;
}

static int rdp_evq_pop(rdp_event_t *out) {
    uint32_t tail = rdp_evq_tail;
    if (tail == rdp_evq_head) return 0;
    *out = rdp_evq[tail];
    __asm__ volatile("" ::: "memory");
    rdp_evq_tail = (tail + 1) & (RDP_EVQ_SIZE - 1);
    return 1;
}

/* ===== connection state ===== */

typedef enum {
    RDP_ST_IDLE = 0,
    RDP_ST_X224,
    RDP_ST_MCS_CONNECT,
    RDP_ST_MCS_ERECT,
    RDP_ST_MCS_ATTACH,
    RDP_ST_MCS_JOIN,
    RDP_ST_CLIENT_INFO,
    RDP_ST_LICENSING,
    RDP_ST_CAPABILITIES,
    RDP_ST_FINALIZATION,
    RDP_ST_ACTIVE,
    RDP_ST_CLOSED
} rdp_state_t;

static const char *rdp_state_name(rdp_state_t s) {
    switch (s) {
    case RDP_ST_IDLE:          return "idle";
    case RDP_ST_X224:          return "X.224 handshake";
    case RDP_ST_MCS_CONNECT:   return "MCS connect";
    case RDP_ST_MCS_ERECT:     return "MCS erect domain";
    case RDP_ST_MCS_ATTACH:    return "MCS attach user";
    case RDP_ST_MCS_JOIN:      return "MCS channel join";
    case RDP_ST_CLIENT_INFO:   return "client info";
    case RDP_ST_LICENSING:     return "licensing";
    case RDP_ST_CAPABILITIES:  return "capability exchange";
    case RDP_ST_FINALIZATION:  return "finalization";
    case RDP_ST_ACTIVE:        return "active";
    case RDP_ST_CLOSED:        return "closed";
    }
    return "?";
}

static struct {
    int         enabled;                /* the service is running      */
    int         listen_sock;
    int         sock;                   /* the current client, or -1   */
    rdp_state_t state;
    uint8_t     peer[4];

    uint16_t    user_id;                /* the MCS channel we are      */
    uint16_t    channels[RDP_MAX_CHANNELS];
    int         channel_count;
    int         channels_joined;

    uint32_t    share_id;
    uint32_t    desktop_w, desktop_h;   /* what the client asked for   */

    /* the screen, as the client last saw it */
    uint16_t   *shadow;
    uint32_t    shadow_w, shadow_h;
    int         shadow_valid;

    /* the compositor's most recent frame */
    volatile uint32_t *frame;
    volatile uint32_t  frame_w, frame_h;
    volatile uint32_t  frame_seq;
    uint32_t           frame_seen;

    uint32_t    frames_sent;
    uint32_t    tiles_sent;
    uint32_t    bytes_sent;
    uint32_t    stalls;
    uint32_t    events_in;
    uint32_t    connections;

    int         shift_down, caps_lock;
    const char *status;
} rdp;

static uint8_t rdp_out[RDP_OUT_BUF];
static uint8_t rdp_in[RDP_IN_BUF];

/* ===== reading a whole PDU ===== */

/*
 * Read exactly `n` bytes, or fail.
 *
 * A short read from a socket is normal -- TCP delivers whatever has
 * arrived -- and a PDU parser that treats one as an error disconnects
 * the client at random under load.
 */
static int rdp_read_full(int s, uint8_t *buf, uint32_t n) {
    uint32_t got = 0;
    while (got < n) {
        int r = vxnet_recv(s, buf + got, (int)(n - got));
        if (r <= 0) return -1;
        got += (uint32_t)r;
    }
    return 0;
}

/*
 * Read one TPKT frame. Returns its total length, or -1.
 *
 * The length in the header is attacker-controlled, so it is checked
 * against the buffer before the body is read -- a claimed length of
 * 65535 with a 64 KB buffer is the obvious way in.
 */
static int rdp_read_pdu(int s, uint8_t *buf, uint32_t cap) {
    uint32_t total;

    if (rdp_read_full(s, buf, TPKT_HDR_LEN) != 0) return -1;
    if (buf[0] != TPKT_VERSION) {
        rdp_log("not a TPKT frame - dropping the connection");
        return -1;
    }

    total = ((uint32_t)buf[2] << 8) | buf[3];
    if (total < TPKT_HDR_LEN || total > cap) {
        rdp_log_num("TPKT length out of range: ", total);
        return -1;
    }

    if (total > TPKT_HDR_LEN &&
        rdp_read_full(s, buf + TPKT_HDR_LEN, total - TPKT_HDR_LEN) != 0)
        return -1;

    return (int)total;
}

/*
 * Send a whole PDU, and treat a full transmit buffer as what it is.
 *
 * lwIP is configured with an 11 KB send buffer and a screen refresh is
 * two megabytes, so the buffer fills almost immediately and stays full
 * for as long as the client takes to drain it. A send that cannot fit
 * comes back non-positive -- exactly like a dead peer does, because
 * this socket API has no errno to tell them apart.
 *
 * Treating that as failure closes the session after the second tile,
 * which is the whole of what "the remote screen never appeared" turned
 * out to be. Back-pressure is normal on a bulk stream; what is not
 * normal is a peer that has stopped reading entirely, so the two are
 * separated by how long the stall lasts rather than by its first
 * occurrence.
 */
/*
 * Send a whole PDU.
 *
 *   0   the PDU is on the wire
 *  -1   nothing was written; the caller may safely abandon it
 *  -2   part of it was written and the rest would not go; the stream
 *       is now desynchronised and the session has to end
 *
 * The distinction is the whole point. A transmit buffer that is full is
 * the normal state of a bulk sender whose client is slower than the
 * screen, and the right response is to drop the update and try again
 * with fresher pixels -- not to wait, and certainly not to hang up.
 * But once bytes of a PDU have gone out, the client is mid-frame and
 * abandoning it would leave it parsing the next PDU as a continuation
 * of this one, so from that point the write has to finish or the
 * connection has to go.
 */
static int rdp_send_all(int s, const uint8_t *buf, uint32_t n) {
    uint32_t sent = 0;
    int stalls = 0;

    while (sent < n) {
        int r = vxnet_send(s, buf + sent, (int)(n - sent));

        if (r > 0) {
            sent += (uint32_t)r;
            stalls = 0;
            continue;
        }

        /* Nothing committed yet: the caller can drop this one. */
        if (sent == 0) return -1;

        /* Committed. Give the stack real time to drain before deciding
         * the client is gone rather than merely behind. */
        if (++stalls > 1000) return -2;
        sched_sleep_ms(5);
    }

    rdp.bytes_sent += n;
    return 0;
}

/* ===== 1. X.224 ===== */

static int rdp_do_x224(int s) {
    x224_cr_t cr;
    int len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
    uint32_t n;

    if (len < 0) return -1;
    if (!x224_parse_connection_request(rdp_in, (uint32_t)len, &cr)) {
        rdp_log("malformed X.224 connection request");
        return -1;
    }

    /*
     * The client says which security protocols it will accept. This
     * server only speaks plain RDP, so a client that demands CredSSP
     * gets a failure it can report rather than a confirm it will choke
     * on three PDUs later.
     */
    if (cr.has_negotiation && !(cr.requested_protocols & PROTOCOL_RDP) &&
        (cr.requested_protocols & PROTOCOL_HYBRID)) {
        rdp_log("client requires CredSSP; this server has none");
        n = x224_negotiation_failure(rdp_out, RDP_OUT_BUF,
                                     HYBRID_REQUIRED_BY_SERVER);
        if (n) rdp_send_all(s, rdp_out, n);
        return -1;
    }

    n = x224_connection_confirm(rdp_out, RDP_OUT_BUF, PROTOCOL_RDP,
                                cr.has_negotiation);
    if (!n || rdp_send_all(s, rdp_out, n) != 0) return -1;

    rdp_log("X.224 connected (plain RDP security, no encryption)");
    return 0;
}

/* ===== 2. MCS connect ===== */

/*
 * Find the client's requested desktop size and its virtual channels in
 * the Connect-Initial's GCC user data.
 *
 * Rather than decoding the BER and PER wrappers to reach it, the blocks
 * are located by scanning for their type tags. Each GCC block is a
 * 2-byte type and a 2-byte length that includes the header, so the
 * structure is self-describing and a scan that validates the length is
 * as safe as a full parse -- and immune to the several encodings
 * different clients use for the wrappers around it.
 */
#define CS_CORE     0xC001
#define CS_SECURITY 0xC002
#define CS_NET      0xC003
#define CS_CLUSTER  0xC004

#define SC_CORE     0x0C01
#define SC_SECURITY 0x0C02
#define SC_NET      0x0C03

static void rdp_parse_client_data(const uint8_t *buf, uint32_t len) {
    uint32_t i = 0;

    rdp.channel_count = 0;
    rdp.desktop_w = 0;
    rdp.desktop_h = 0;

    while (i + 4 <= len) {
        uint16_t type = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        uint16_t blen = (uint16_t)(buf[i + 2] | (buf[i + 3] << 8));

        if (blen < 4 || i + blen > len) { i++; continue; }

        if (type == CS_CORE && blen >= 12) {
            rdp.desktop_w = (uint32_t)(buf[i + 8]  | (buf[i + 9]  << 8));
            rdp.desktop_h = (uint32_t)(buf[i + 10] | (buf[i + 11] << 8));
            i += blen;
            continue;
        }

        if (type == CS_NET && blen >= 8) {
            uint32_t count = (uint32_t)buf[i + 4] |
                             ((uint32_t)buf[i + 5] << 8) |
                             ((uint32_t)buf[i + 6] << 16) |
                             ((uint32_t)buf[i + 7] << 24);
            /* Each channel definition is twelve bytes: an eight-byte
             * name and four of flags. The count comes off the wire, so
             * it is checked against the block it claims to describe. */
            if (count > RDP_MAX_CHANNELS - 1) count = RDP_MAX_CHANNELS - 1;
            if (8 + count * 12 > blen) count = (blen - 8) / 12;

            for (uint32_t c = 0; c < count; c++)
                rdp.channels[rdp.channel_count++] =
                    (uint16_t)(MCS_GLOBAL_CHANNEL + 1 + c);
            i += blen;
            continue;
        }

        /*
         * A block whose type is not one of ours advances by one byte,
         * not by its length.
         *
         * The scan starts inside the BER and PER wrappers, where any
         * two bytes can look like a type and the next two like a
         * length. Trusting an unrecognised length lets a coincidence
         * jump the cursor over the block that was actually being looked
         * for -- which is exactly what happened to CS_CORE here, and
         * presented as a client that asked for a 0x0 desktop.
         */
        i++;
    }

    /*
     * Note what is deliberately *not* added here: the I/O channel.
     *
     * SC_NET names it separately, in its own MCSChannelId field, and
     * its channelIdArray carries only the virtual channels. Putting
     * 1003 in the array as well makes the client see one more channel
     * than exists, join it twice, and then wait forever for a confirm
     * the server has no request left to answer -- which presents as the
     * connection hanging just after the domain is bound.
     */
}

/* Build the server's GCC user data: what the client is being given. */
static uint32_t rdp_build_server_gcc(uint8_t *out, uint32_t cap,
                                     uint32_t requested_protocols) {
    rdp_w_t w;
    uint32_t net_len;

    rdp_w_init(&w, out, cap);

    /* --- SC_CORE --- */
    rdp_u16(&w, SC_CORE);
    rdp_u16(&w, 12);
    rdp_u32(&w, 0x00080004);            /* RDP 5.0+                    */
    rdp_u32(&w, requested_protocols);

    /* --- SC_NET: the channel ids the client's names were bound to --- */
    net_len = 8 + (uint32_t)rdp.channel_count * 2;
    if (net_len & 3) net_len += 2;      /* padded to four bytes        */

    rdp_u16(&w, SC_NET);
    rdp_u16(&w, (uint16_t)net_len);
    rdp_u16(&w, MCS_GLOBAL_CHANNEL);
    rdp_u16(&w, (uint16_t)rdp.channel_count);
    for (int i = 0; i < rdp.channel_count; i++)
        rdp_u16(&w, rdp.channels[i]);
    if ((8 + (uint32_t)rdp.channel_count * 2) & 3) rdp_u16(&w, 0);

    /* --- SC_SECURITY: none, and it says so --- */
    rdp_u16(&w, SC_SECURITY);
    rdp_u16(&w, 12);
    rdp_u32(&w, 0);                     /* ENCRYPTION_METHOD_NONE      */
    rdp_u32(&w, 0);                     /* ENCRYPTION_LEVEL_NONE       */

    return w.overflow ? 0 : w.n;
}

/*
 * The MCS Connect-Response, with the GCC Conference Create Response
 * nested inside it as an octet string.
 *
 * Lengths here nest four deep and each layer encodes them differently,
 * so the payload is built innermost-first and the wrappers are sized
 * from what they contain. Writing it in wire order would mean patching
 * three lengths of three different formats afterwards.
 */
static uint32_t rdp_build_mcs_connect_response(uint8_t *out, uint32_t cap,
                                               uint32_t requested_protocols) {
    uint8_t  gcc[512];
    uint32_t gcc_len;
    uint32_t ccr_len, cd_len, ud_len, total;
    rdp_w_t  w;

    gcc_len = rdp_build_server_gcc(gcc, sizeof(gcc), requested_protocols);
    if (!gcc_len) return 0;

    /* ConferenceCreateResponse: the fixed PER preamble, the "McDn" key,
     * then the user data as a PER octet string. */
    ccr_len = 1 + 2 + 2 + 1 + 1 + 1 + 1 + 4 +
              per_length_size(gcc_len) + gcc_len;

    /* ConnectData: the t124 object identifier, then the PDU length. */
    cd_len = 7 + per_length_size(ccr_len) + ccr_len;

    /* userData OCTET STRING inside the BER Connect-Response */
    ud_len = cd_len;

    /* result + calledConnectId + domainParameters + userData */
    total = 3 + 3 + 2 + 26 + 1 + ber_length_size(ud_len) + ud_len;

    rdp_w_init(&w, out, cap);
    tpkt_write(&w, 0);
    x224_data_write(&w);

    ber_application_tag(&w, MCS_TYPE_CONNECT_RESPONSE, total);
    ber_enumerated(&w, 0);                          /* rt-successful   */
    ber_integer(&w, 0);                             /* calledConnectId */

    /* domainParameters: the limits this server will honour */
    rdp_u8(&w, BER_TAG_SEQUENCE);
    rdp_u8(&w, 26);
    ber_integer(&w, 34);        /* maxChannelIds                       */
    ber_integer(&w, 3);         /* maxUserIds                          */
    ber_integer(&w, 0);         /* maxTokenIds                         */
    ber_integer(&w, 1);         /* numPriorities                       */
    ber_integer(&w, 0);         /* minThroughput                       */
    ber_integer(&w, 1);         /* maxHeight                           */
    ber_integer(&w, 0xFFF8);    /* maxMCSPDUsize                       */
    ber_integer(&w, 2);         /* protocolVersion                     */

    ber_octet_string_tag(&w, ud_len);

    /* --- ConnectData --- */
    rdp_u8(&w, 0x00); rdp_u8(&w, 0x05); rdp_u8(&w, 0x00);
    rdp_u8(&w, 0x14); rdp_u8(&w, 0x7C); rdp_u8(&w, 0x00); rdp_u8(&w, 0x01);
    per_length(&w, ccr_len);

    /* --- ConferenceCreateResponse --- */
    rdp_u8(&w, 0x14);                   /* choice                      */
    per_integer16(&w, 0x79F3, 1001);    /* nodeID                      */
    rdp_u8(&w, 0x01); rdp_u8(&w, 0x01); /* tag: INTEGER 1              */
    rdp_u8(&w, 0x00);                   /* result: success             */
    rdp_u8(&w, 0x01);                   /* one UserData set            */
    rdp_u8(&w, 0xC0);                   /* value present, h221         */
    rdp_u8(&w, 0x00);                   /* octet string, length - min  */
    rdp_u8(&w, 'M'); rdp_u8(&w, 'c'); rdp_u8(&w, 'D'); rdp_u8(&w, 'n');
    per_length(&w, gcc_len);
    rdp_bytes(&w, gcc, gcc_len);

    if (w.overflow) return 0;
    tpkt_patch(out, w.n);
    return w.n;
}

static int rdp_do_mcs_connect(int s, uint32_t requested_protocols) {
    int len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
    uint32_t n;

    if (len < TPKT_HDR_LEN + X224_DATA_HDR_LEN + 2) return -1;

    /* Connect-Initial is BER application tag 101: 0x7F 0x65. */
    {
        const uint8_t *p = rdp_in + TPKT_HDR_LEN + X224_DATA_HDR_LEN;
        if (p[0] != 0x7F || p[1] != MCS_TYPE_CONNECT_INITIAL) {
            rdp_log("expected an MCS Connect-Initial");
            return -1;
        }
    }

    /* The GCC client data blocks are somewhere inside; find them by
     * their tags rather than unwrapping four layers of length. */
    rdp_parse_client_data(rdp_in + TPKT_HDR_LEN + X224_DATA_HDR_LEN,
                          (uint32_t)len - TPKT_HDR_LEN - X224_DATA_HDR_LEN);

    serial_puts("[rdp] client wants ");
    serial_put_dec(rdp.desktop_w);
    serial_putc('x');
    serial_put_dec(rdp.desktop_h);
    serial_puts(", ");
    serial_put_dec((uint32_t)rdp.channel_count);
    serial_puts(" channels\n");

    n = rdp_build_mcs_connect_response(rdp_out, RDP_OUT_BUF,
                                       requested_protocols);
    if (!n || rdp_send_all(s, rdp_out, n) != 0) return -1;

    rdp_log("MCS domain bound");
    return 0;
}

/* ===== 3-4. erect domain, attach user, channel joins ===== */

static int rdp_mcs_choice(const uint8_t *pdu, int len) {
    if (len < TPKT_HDR_LEN + X224_DATA_HDR_LEN + 1) return -1;
    return pdu[TPKT_HDR_LEN + X224_DATA_HDR_LEN] >> 2;
}

static int rdp_do_mcs_setup(int s) {
    int len, choice;
    uint32_t n;

    /*
     * Erect Domain Request, then Attach User Request.
     *
     * Some clients send them back to back without waiting, so this
     * loops on whatever arrives rather than requiring a fixed order --
     * an Erect Domain has no reply, so a server that insists on
     * reading it before the attach will read the attach as an erect
     * and lose the connection.
     */
    for (int guard = 0; guard < 8; guard++) {
        len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
        if (len < 0) return -1;

        choice = rdp_mcs_choice(rdp_in, len);
        if (choice == MCS_ERECT_DOMAIN_REQUEST) continue;

        if (choice == MCS_ATTACH_USER_REQUEST) {
            rdp.user_id = 1002;
            n = mcs_attach_user_confirm(rdp_out, RDP_OUT_BUF, rdp.user_id);
            if (!n || rdp_send_all(s, rdp_out, n) != 0) return -1;
            rdp_log_num("MCS user channel assigned: ", rdp.user_id);
            break;
        }

        rdp_log_num("unexpected MCS PDU during setup: ", (uint32_t)choice);
        return -1;
    }

    /*
     * Channel joins. The client joins its own user channel, the I/O
     * channel, and every virtual channel it asked for -- so the count
     * is the number of virtual channels plus two, and each must be
     * confirmed before the client sends the next.
     */
    rdp.channels_joined = 0;
    for (int i = 0; i < rdp.channel_count + 2; i++) {
        uint16_t requested;

        len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
        if (len < 0) return -1;

        choice = rdp_mcs_choice(rdp_in, len);
        if (choice != MCS_CHANNEL_JOIN_REQUEST) {
            rdp_log_num("expected a channel join, got ", (uint32_t)choice);
            return -1;
        }
        if (len < TPKT_HDR_LEN + X224_DATA_HDR_LEN + 5) return -1;

        {
            const uint8_t *p = rdp_in + TPKT_HDR_LEN + X224_DATA_HDR_LEN + 3;
            requested = (uint16_t)((p[0] << 8) | p[1]);
        }

        n = mcs_channel_join_confirm(rdp_out, RDP_OUT_BUF,
                                     rdp.user_id, requested);
        if (!n || rdp_send_all(s, rdp_out, n) != 0) return -1;
        rdp.channels_joined++;
    }

    rdp_log_num("channels joined: ", (uint32_t)rdp.channels_joined);
    return 0;
}

/* ===== 5-6. client info and licensing ===== */

/*
 * The Client Info PDU carries the domain, user name, password and the
 * shell to run. None of it is used here -- there is no per-user remote
 * session to start, the client is joining the console -- but it has to
 * be consumed, and it is worth noting in the log who connected.
 *
 * It is also the packet that would carry a password in plaintext under
 * this configuration. It is deliberately not logged.
 */
static int rdp_do_client_info(int s) {
    int len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
    if (len < 0) return -1;
    rdp_log("client info received");
    return 0;
}

/*
 * Licensing.
 *
 * A server that does not require a licence says so with an error PDU
 * whose error code means success: STATUS_VALID_CLIENT with
 * ST_NO_TRANSITION. That is not a workaround, it is what the protocol
 * specifies and what every server without a licensing role sends.
 */
static int rdp_send_license_ok(int s) {
    rdp_w_t w;
    uint32_t payload = 4 + 4 + 4 + 4 + 4;   /* sec hdr + preamble + msg */
    uint32_t start;

    rdp_w_init(&w, rdp_out, RDP_OUT_BUF);
    mcs_send_data_indication(&w, rdp.user_id, MCS_GLOBAL_CHANNEL, payload);
    start = w.n;

    rdp_u16(&w, SEC_LICENSE_PKT);       /* security header             */
    rdp_u16(&w, 0);

    rdp_u8(&w, 0xFF);                   /* ERROR_ALERT                 */
    rdp_u8(&w, 0x03);                   /* preamble version 3          */
    rdp_u16(&w, 16);                    /* message size                */

    rdp_u32(&w, 0x00000007);            /* STATUS_VALID_CLIENT         */
    rdp_u32(&w, 0x00000002);            /* ST_NO_TRANSITION            */
    rdp_u16(&w, 0x0004);                /* BB_ERROR_BLOB               */
    rdp_u16(&w, 0);                     /* empty                       */

    if (w.overflow) return -1;
    (void)start;
    tpkt_patch(rdp_out, w.n);
    if (rdp_send_all(s, rdp_out, w.n) != 0) return -1;

    rdp_log("licensing: client accepted, no licence required");
    return 0;
}

/* ===== 7. capabilities ===== */

/*
 * The capability sets.
 *
 * This is the server telling the client what it can do and, more
 * importantly, what it will not do. Every set has a 4-byte header of
 * type and length, and the length *includes* that header -- a length
 * that excludes it makes the client walk off the end of the set list
 * and disconnect without a message.
 *
 * Order is a general capability set of its own and has to be sent even
 * though this server draws nothing with orders: a client that receives
 * no order capability assumes the defaults, which include orders this
 * server would then have to implement.
 */
static void rdp_caps_general(rdp_w_t *w) {
    rdp_u16(w, 1);                      /* CAPSTYPE_GENERAL            */
    rdp_u16(w, 24);
    rdp_u16(w, 1);                      /* osMajorType: Windows        */
    rdp_u16(w, 3);                      /* osMinorType: Windows NT     */
    rdp_u16(w, 0x0200);                 /* protocol version            */
    rdp_u16(w, 0);
    rdp_u16(w, 0);                      /* compression types           */
    /* NO_BITMAP_COMPRESSION_HDR: the bitmap updates below are
     * uncompressed, and saying so removes eight bytes of header the
     * client would otherwise expect on every one of them. */
    rdp_u16(w, 0x0400);
    rdp_u16(w, 0);                      /* updateCapability            */
    rdp_u16(w, 0);                      /* remoteUnshare               */
    rdp_u16(w, 0);                      /* compression level           */
    rdp_u8(w, 1);                       /* refreshRect supported       */
    rdp_u8(w, 1);                       /* suppressOutput supported    */
}

static void rdp_caps_bitmap(rdp_w_t *w, uint16_t width, uint16_t height) {
    rdp_u16(w, 2);                      /* CAPSTYPE_BITMAP             */
    rdp_u16(w, 28);
    rdp_u16(w, 16);                     /* preferred bits per pixel    */
    rdp_u16(w, 1);                      /* receive1BitPerPixel         */
    rdp_u16(w, 1);                      /* receive4BitsPerPixel        */
    rdp_u16(w, 1);                      /* receive8BitsPerPixel        */
    rdp_u16(w, width);
    rdp_u16(w, height);
    rdp_u16(w, 0);
    rdp_u16(w, 1);                      /* desktopResizeFlag           */
    rdp_u16(w, 0);                      /* bitmapCompressionFlag       */
    rdp_u8(w, 0);                       /* highColorFlags              */
    rdp_u8(w, 0);                       /* drawingFlags                */
    rdp_u16(w, 1);                      /* multipleRectangleSupport    */
    rdp_u16(w, 0);
}

static void rdp_caps_order(rdp_w_t *w) {
    rdp_u16(w, 3);                      /* CAPSTYPE_ORDER              */
    rdp_u16(w, 88);
    rdp_fill(w, 0, 16);                 /* terminalDescriptor          */
    rdp_u32(w, 0);
    rdp_u16(w, 1);                      /* desktopSaveXGranularity     */
    rdp_u16(w, 20);                     /* desktopSaveYGranularity     */
    rdp_u16(w, 0);
    rdp_u16(w, 1);                      /* maximumOrderLevel           */
    rdp_u16(w, 0);                      /* numberFonts                 */
    /* NEGOTIATEORDERSUPPORT | ZEROBOUNDSDELTASSUPPORT | COLORINDEX */
    rdp_u16(w, 0x002A);
    rdp_fill(w, 0, 32);                 /* orderSupport: none of them  */
    rdp_u16(w, 0);                      /* textFlags                   */
    rdp_u16(w, 0);                      /* orderSupportExFlags         */
    rdp_u32(w, 0);
    rdp_u32(w, 0);                      /* desktopSaveSize             */
    rdp_u16(w, 0);
    rdp_u16(w, 0);
    rdp_u16(w, 0);                      /* textANSICodePage            */
    rdp_u16(w, 0);
}

/*
 * The bitmap cache.
 *
 * Advertised with every cell count zero, which is the way to say "this
 * server will not use the cache" while still answering the capability
 * the client asked about. A non-zero cache the server never populates
 * makes the client hold memory for nothing; omitting the set entirely
 * makes some clients assume a default cache and then receive cache
 * orders that never come.
 */
static void rdp_caps_bitmap_cache(rdp_w_t *w) {
    rdp_u16(w, 4);                      /* CAPSTYPE_BITMAPCACHE        */
    rdp_u16(w, 40);
    rdp_fill(w, 0, 24);                 /* six reserved dwords         */
    rdp_u16(w, 0); rdp_u16(w, 0);       /* cache 0: entries, cell size */
    rdp_u16(w, 0); rdp_u16(w, 0);       /* cache 1                     */
    rdp_u16(w, 0); rdp_u16(w, 0);       /* cache 2                     */
}

static void rdp_caps_pointer(rdp_w_t *w) {
    rdp_u16(w, 8);                      /* CAPSTYPE_POINTER            */
    rdp_u16(w, 10);
    /* Colour pointers are supported by every client since 5.0, but
     * this server never sends a pointer update: the cursor is drawn
     * into the framebuffer by the compositor, so the client's own
     * pointer would be a second one. */
    rdp_u16(w, 0);                      /* colorPointerFlag            */
    rdp_u16(w, 20);                     /* colorPointerCacheSize       */
    rdp_u16(w, 20);                     /* pointerCacheSize            */
}

static void rdp_caps_input(rdp_w_t *w) {
    rdp_u16(w, 13);                     /* CAPSTYPE_INPUT              */
    rdp_u16(w, 88);
    /* INPUT_FLAG_SCANCODES | INPUT_FLAG_MOUSEX | INPUT_FLAG_UNICODE */
    rdp_u16(w, 0x0001 | 0x0004 | 0x0010);
    rdp_u16(w, 0);
    rdp_u32(w, 0x00000409);             /* keyboard layout: US         */
    rdp_u32(w, 4);                      /* keyboard type: IBM enhanced */
    rdp_u32(w, 0);                      /* subtype                     */
    rdp_u32(w, 12);                     /* function keys               */
    rdp_fill(w, 0, 64);                 /* imeFileName                 */
}

static void rdp_caps_share(rdp_w_t *w) {
    rdp_u16(w, 9);                      /* CAPSTYPE_SHARE              */
    rdp_u16(w, 8);
    rdp_u16(w, rdp.user_id);
    rdp_u16(w, 0);
}

static void rdp_caps_colorcache(rdp_w_t *w) {
    rdp_u16(w, 10);                     /* CAPSTYPE_COLORCACHE         */
    rdp_u16(w, 8);
    rdp_u16(w, 6);
    rdp_u16(w, 0);
}

static void rdp_caps_font(rdp_w_t *w) {
    rdp_u16(w, 14);                     /* CAPSTYPE_FONT               */
    rdp_u16(w, 8);
    rdp_u16(w, 1);                      /* FONTSUPPORT_FONTLIST        */
    rdp_u16(w, 0);
}

static void rdp_caps_virtualchannel(rdp_w_t *w) {
    rdp_u16(w, 20);                     /* CAPSTYPE_VIRTUALCHANNEL     */
    rdp_u16(w, 12);
    rdp_u32(w, 0);                      /* no compression              */
    rdp_u32(w, 1600);                   /* chunk size                  */
}

/*
 * The Demand Active PDU: everything above, in one packet, with two
 * lengths that have to be computed after the fact.
 *
 * It is assembled into a scratch buffer first so the capability block's
 * length is known before the header that quotes it is written.
 */
static int rdp_send_demand_active(int s, uint16_t width, uint16_t height) {
    static uint8_t caps[1024];
    rdp_w_t cw, w;
    uint32_t caps_len, body_len, total;

    rdp_w_init(&cw, caps, sizeof(caps));
    rdp_caps_general(&cw);
    rdp_caps_bitmap(&cw, width, height);
    rdp_caps_order(&cw);
    rdp_caps_bitmap_cache(&cw);
    rdp_caps_pointer(&cw);
    rdp_caps_input(&cw);
    rdp_caps_share(&cw);
    rdp_caps_colorcache(&cw);
    rdp_caps_font(&cw);
    rdp_caps_virtualchannel(&cw);
    if (cw.overflow) return -1;
    caps_len = cw.n;

    /* shareId, source descriptor length, capability length, "RDP\0",
     * capability count, pad, the sets themselves, and the session id */
    body_len = 4 + 2 + 2 + 4 + caps_len + 4;
    total    = SHARE_CONTROL_HDR_LEN + body_len;

    rdp_w_init(&w, rdp_out, RDP_OUT_BUF);
    mcs_send_data_indication(&w, rdp.user_id, MCS_GLOBAL_CHANNEL, total);

    share_control_write(&w, (uint16_t)total, PDUTYPE_DEMANDACTIVEPDU,
                        rdp.user_id);
    rdp_u32(&w, rdp.share_id);
    rdp_u16(&w, 4);                             /* source descriptor   */
    rdp_u16(&w, (uint16_t)(caps_len + 4));      /* caps + count + pad  */
    rdp_u8(&w, 'R'); rdp_u8(&w, 'D'); rdp_u8(&w, 'P'); rdp_u8(&w, 0);
    rdp_u16(&w, 10);                            /* number of sets      */
    rdp_u16(&w, 0);
    rdp_bytes(&w, caps, caps_len);
    rdp_u32(&w, 0);                             /* sessionId           */

    if (w.overflow) return -1;
    tpkt_patch(rdp_out, w.n);
    if (rdp_send_all(s, rdp_out, w.n) != 0) return -1;

    rdp_log("demand active sent (10 capability sets)");
    return 0;
}

/*
 * Read the client's Confirm Active and note what it agreed to.
 *
 * The one capability that changes this server's behaviour is the
 * client's preferred colour depth, because a client that will not take
 * 16bpp cannot be sent the bitmap updates below.
 */
static int rdp_read_confirm_active(int s) {
    for (int guard = 0; guard < 8; guard++) {
        int len = rdp_read_pdu(s, rdp_in, RDP_IN_BUF);
        rdp_r_t r;
        uint16_t total, type;

        if (len < 0) return -1;

        /* Step over TPKT, X.224 and the MCS Send Data Request header:
         * one byte of choice, two of initiator, two of channel, one of
         * priority, and a PER length of one or two bytes. */
        {
            uint32_t off = TPKT_HDR_LEN + X224_DATA_HDR_LEN + 1 + 2 + 2 + 1;
            if ((uint32_t)len <= off) continue;
            off += (rdp_in[off] & 0x80) ? 2u : 1u;
            if ((uint32_t)len <= off) continue;
            rdp_r_init(&r, rdp_in + off, (uint32_t)len - off);
        }

        total = rdp_r16(&r);
        type  = rdp_r16(&r);
        (void)total;

        if ((type & 0x0F) == PDUTYPE_CONFIRMACTIVEPDU) {
            rdp_log("confirm active received");
            return 0;
        }
        /* Anything else this early is a PDU the client sent ahead of
         * itself; skip it rather than failing. */
    }
    rdp_log("no confirm active from the client");
    return -1;
}

/* ===== 8. finalization ===== */

static int rdp_send_data_pdu(int s, uint8_t pdu_type2,
                             const uint8_t *body, uint32_t body_len) {
    rdp_w_t w;
    uint32_t total = SHARE_DATA_HDR_LEN + body_len;

    rdp_w_init(&w, rdp_out, RDP_OUT_BUF);
    mcs_send_data_indication(&w, rdp.user_id, MCS_GLOBAL_CHANNEL, total);
    share_data_write(&w, (uint16_t)total, rdp.user_id, rdp.share_id,
                     pdu_type2, (uint16_t)(body_len + SHARE_DATA_HDR_LEN));
    if (body_len) rdp_bytes(&w, body, body_len);

    if (w.overflow) return -1;
    tpkt_patch(rdp_out, w.n);
    return rdp_send_all(s, rdp_out, w.n);
}

/*
 * The four PDUs that finish a connection.
 *
 * The client sends its synchronise, cooperate, request-control and font
 * list; the server answers with synchronise, cooperate, granted-control
 * and font map. The order the server sends them in is fixed and a
 * client that receives the font map before the control grant will wait
 * forever for a grant it has already been given.
 */
static int rdp_do_finalization(int s) {
    uint8_t body[8];
    rdp_w_t w;

    /* Synchronize */
    rdp_w_init(&w, body, sizeof(body));
    rdp_u16(&w, 1);                     /* SYNCMSGTYPE_SYNC            */
    rdp_u16(&w, 1002);                  /* target user                 */
    if (rdp_send_data_pdu(s, PDUTYPE2_SYNCHRONIZE, body, w.n) != 0) return -1;

    /* Control: cooperate */
    rdp_w_init(&w, body, sizeof(body));
    rdp_u16(&w, CTRLACTION_COOPERATE);
    rdp_u16(&w, 0);                     /* grantId                     */
    rdp_u32(&w, 0);                     /* controlId                   */
    if (rdp_send_data_pdu(s, PDUTYPE2_CONTROL, body, w.n) != 0) return -1;

    /* Control: granted */
    rdp_w_init(&w, body, sizeof(body));
    rdp_u16(&w, CTRLACTION_GRANTED_CONTROL);
    rdp_u16(&w, rdp.user_id);
    rdp_u32(&w, 0x000003EA);            /* the server's control id     */
    if (rdp_send_data_pdu(s, PDUTYPE2_CONTROL, body, w.n) != 0) return -1;

    /* Font map: no fonts, one page, and it is the last one */
    rdp_w_init(&w, body, sizeof(body));
    rdp_u16(&w, 0);                     /* numberEntries               */
    rdp_u16(&w, 0);                     /* totalNumEntries             */
    rdp_u16(&w, 0x0003);                /* FIRST | LAST                */
    rdp_u16(&w, 4);                     /* entrySize                   */
    if (rdp_send_data_pdu(s, PDUTYPE2_FONTMAP, body, w.n) != 0) return -1;

    rdp_log("finalization complete - session is live");
    return 0;
}

/* ===== the screen ===== */

/*
 * The compositor's hook.
 *
 * Called from the render loop in kernel.c with the frame it has just
 * finished composing. This does almost nothing on purpose: it records
 * where the frame is and bumps a counter. Diffing and encoding happen
 * on the network thread, because a remote client on a slow link must
 * never be able to slow the local display down -- and doing the work
 * here would do exactly that.
 *
 * The frame pointer is the live back buffer rather than a copy. The
 * network thread may therefore read it while the compositor is writing
 * the next frame, and a tile can be captured half-updated. That is
 * deliberate: the alternative is a full-screen copy every frame -- 3 MB
 * at 60 Hz -- to avoid an artefact that the next diff corrects
 * automatically, because a torn tile differs from the shadow and is
 * simply sent again.
 */
static void rdp_frame_ready(uint32_t *fb, uint32_t w, uint32_t h) {
    if (!rdp.enabled) return;
    rdp.frame   = fb;
    rdp.frame_w = w;
    rdp.frame_h = h;
    rdp.frame_seq++;
}

/*
 * Drain the input queue into the desktop.
 *
 * Also called from the compositor loop, and this is the half that
 * actually touches the desktop's state -- on the thread that owns it,
 * which is the whole reason the ring exists.
 */
static void rdp_pump_input(void) {
    rdp_event_t ev;
    int n = 0;

    /* Bounded: a client that floods input must not be able to keep the
     * compositor inside this loop and stall the frame. */
    while (n++ < 64 && rdp_evq_pop(&ev)) {
        if (ev.kind == 1) {
            if (ev.ch) kb_push(ev.ch);
        } else if (ev.kind == 2) {
            mouse_x = ev.x;
            mouse_y = ev.y;
            mouse_buttons = ev.buttons;
            if (ev.wheel) mouse_wheel += ev.wheel;
        }
    }
}

/*
 * Send every tile that changed.
 *
 * The screen is divided into 64x64 tiles and each is compared against
 * the copy the client last received. Only tiles that differ are
 * encoded, which is what makes this usable: a desktop with a blinking
 * cursor and a clock changes two tiles a second, not fifteen hundred.
 *
 * Several tiles are packed into one bitmap update, because the per-PDU
 * cost -- MCS, share control, share data, a round trip's worth of
 * latency -- dwarfs a 64x64 tile at 8 KB.
 */
static int rdp_send_dirty_tiles(int s) {
    uint32_t w = rdp.frame_w, h = rdp.frame_h;
    const uint32_t *fb = (const uint32_t *)rdp.frame;
    uint32_t tiles_x, tiles_y;
    uint32_t sent_this_pass = 0;

    if (!fb || !w || !h || !rdp.shadow) return 0;
    if (w != rdp.shadow_w || h != rdp.shadow_h) return 0;

    tiles_x = (w + RDP_TILE - 1) / RDP_TILE;
    tiles_y = (h + RDP_TILE - 1) / RDP_TILE;

    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t x0 = tx * RDP_TILE, y0 = ty * RDP_TILE;
            uint32_t tw = (x0 + RDP_TILE <= w) ? RDP_TILE : w - x0;
            uint32_t th = (y0 + RDP_TILE <= h) ? RDP_TILE : h - y0;
            int changed = 0;

            /* Compare against what the client already has. */
            for (uint32_t row = 0; row < th && !changed; row++) {
                const uint32_t *src = fb + (uint64_t)(y0 + row) * w + x0;
                uint16_t *dst = rdp.shadow + (uint64_t)(y0 + row) * w + x0;
                for (uint32_t col = 0; col < tw; col++) {
                    if (dst[col] != rdp_rgb565(src[col])) { changed = 1; break; }
                }
            }
            if (!changed && rdp.shadow_valid) continue;

            /* One tile, one bitmap update. Packing more per PDU would
             * need the rectangle count written before the tiles are
             * known, so each is sent as it is found -- the cost is a
             * header per tile, which at 8 KB of payload is under one
             * percent. */
            {
                rdp_w_t w2;
                uint32_t data_len = tw * th * 2;
                uint32_t body = 2 + 2 + 18 + data_len;
                uint32_t total = SHARE_DATA_HDR_LEN + body;

                if (total + 64 > RDP_OUT_BUF) continue;

                rdp_w_init(&w2, rdp_out, RDP_OUT_BUF);
                mcs_send_data_indication(&w2, rdp.user_id,
                                         MCS_GLOBAL_CHANNEL, total);
                share_data_write(&w2, (uint16_t)total, rdp.user_id,
                                 rdp.share_id, PDUTYPE2_UPDATE,
                                 (uint16_t)total);
                rdp_u16(&w2, UPDATETYPE_BITMAP);
                rdp_u16(&w2, 1);                /* one rectangle       */
                rdp_bitmap_rect_header(&w2, (uint16_t)x0, (uint16_t)y0,
                                       (uint16_t)tw, (uint16_t)th,
                                       16, (uint16_t)data_len);
                rdp_tile_rgb565(&w2, fb, w, x0, y0, tw, th);

                if (w2.overflow) continue;
                tpkt_patch(rdp_out, w2.n);

                {
                    int rc = rdp_send_all(s, rdp_out, w2.n);

                    if (rc == -2) return -1;    /* stream desynced      */

                    if (rc == -1) {
                        /*
                         * The transmit buffer is full. Leave the shadow
                         * alone so this tile is still dirty, and stop
                         * the pass -- the rest of the screen is just as
                         * stuck, and the next frame will resend from
                         * here with newer pixels than these.
                         *
                         * This is why the shadow is updated after the
                         * send rather than before it: an abandoned tile
                         * whose shadow had already advanced would be
                         * considered delivered and never sent again,
                         * leaving a permanently stale square on the
                         * client's screen.
                         */
                        rdp.stalls++;
                        rdp.shadow_valid = 1;
                        return 0;
                    }
                }

                /* Now it is really the client's. */
                for (uint32_t row = 0; row < th; row++) {
                    const uint32_t *src = fb + (uint64_t)(y0 + row) * w + x0;
                    uint16_t *dst = rdp.shadow + (uint64_t)(y0 + row) * w + x0;
                    for (uint32_t col = 0; col < tw; col++)
                        dst[col] = rdp_rgb565(src[col]);
                }

                /* Let the stack run: a refresh is hundreds of these
                 * back to back, and a tight loop keeps the transmit
                 * buffer at its limit with no chance to drain. */
                if ((rdp.tiles_sent & 3) == 0) sched_yield();

                rdp.tiles_sent++;
                sent_this_pass++;
            }
        }
    }

    rdp.shadow_valid = 1;
    if (sent_this_pass) rdp.frames_sent++;
    return 0;
}

/* ===== input, from the client ===== */

static void rdp_handle_input_pdu(rdp_r_t *r) {
    uint16_t count = rdp_r16(r);
    rdp_skip(r, 2);                     /* pad                         */

    if (count > 64) count = 64;         /* the wire said so; bound it  */

    for (uint16_t i = 0; i < count; i++) {
        rdp_input_event_t ev;
        rdp_event_t out;

        if (!rdp_parse_input_event(r, &ev)) break;
        rdp.events_in++;

        if (ev.type == INPUT_EVENT_SCANCODE) {
            int down = !(ev.flags & KBDFLAGS_RELEASE);
            int ext  = (ev.flags & KBDFLAGS_EXTENDED) ? 1 : 0;
            uint16_t sc = ev.a;

            /* Shift and caps are state, not characters. */
            if (sc == 0x2A || sc == 0x36) { rdp.shift_down = down; continue; }
            if (sc == 0x3A) { if (down) rdp.caps_lock = !rdp.caps_lock; continue; }
            if (!down) continue;        /* only make codes type        */

            out.kind = 1;
            out.ch = rdp_scancode_to_char(sc, ext, rdp.shift_down,
                                          rdp.caps_lock);
            if (out.ch) rdp_evq_push(&out);

        } else if (ev.type == INPUT_EVENT_UNICODE) {
            if (!(ev.flags & KBDFLAGS_RELEASE) && ev.a >= 0x20 && ev.a < 0x7F) {
                out.kind = 1;
                out.ch = (char)ev.a;
                rdp_evq_push(&out);
            }

        } else if (ev.type == INPUT_EVENT_MOUSE) {
            uint16_t flags = ev.flags;
            out.kind    = 2;
            out.x       = (int32_t)ev.a;
            out.y       = (int32_t)ev.b;
            out.wheel   = 0;
            out.buttons = 0;

            if (flags & PTRFLAGS_DOWN) {
                if (flags & PTRFLAGS_BUTTON1) out.buttons |= 1;
                if (flags & PTRFLAGS_BUTTON2) out.buttons |= 2;
                if (flags & PTRFLAGS_BUTTON3) out.buttons |= 4;
            }

            /*
             * The wheel carries a signed delta in the low byte and its
             * sign in a separate flag, which is not the same as the
             * byte's own sign bit -- a notch of -120 arrives as 0x78
             * with PTRFLAGS_WHEEL_NEGATIVE set.
             */
            if (flags & PTRFLAGS_WHEEL) {
                int32_t delta = (int32_t)(flags & 0xFF);
                if (flags & PTRFLAGS_WHEEL_NEGATIVE) delta = -(256 - delta);
                out.wheel = delta / 120;
                if (out.wheel == 0) out.wheel = (delta < 0) ? -1 : 1;
            }

            rdp_evq_push(&out);
        }
    }
}

/*
 * One PDU from the client, once the session is live.
 *
 * Everything now arrives inside an MCS Send Data Request wrapping a
 * share data header. The only types this server acts on are input and
 * the two that ask for a repaint; the rest are acknowledged by being
 * ignored, which is what the protocol expects.
 */
static int rdp_handle_active_pdu(const uint8_t *pdu, uint32_t len) {
    rdp_r_t r;
    uint32_t off = TPKT_HDR_LEN + X224_DATA_HDR_LEN;
    uint16_t total, type;

    if (len <= off + 1) return 0;

    /* A disconnect ultimatum is the client leaving politely. */
    if ((pdu[off] >> 2) == MCS_DISCONNECT_ULTIMATUM) {
        rdp_log("client disconnected");
        return -1;
    }
    if ((pdu[off] >> 2) != MCS_SEND_DATA_REQUEST) return 0;

    off += 1 + 2 + 2 + 1;
    if (len <= off) return 0;
    off += (pdu[off] & 0x80) ? 2u : 1u;
    if (len <= off) return 0;

    rdp_r_init(&r, pdu + off, len - off);
    total = rdp_r16(&r);
    type  = rdp_r16(&r);
    (void)total;
    rdp_skip(&r, 2);                    /* PDUSource                   */

    if ((type & 0x0F) != PDUTYPE_DATAPDU) return 0;

    rdp_skip(&r, 4);                    /* shareId                     */
    rdp_skip(&r, 1);                    /* pad                         */
    rdp_skip(&r, 1);                    /* streamId                    */
    rdp_skip(&r, 2);                    /* uncompressedLength          */
    {
        uint8_t pdu_type2 = rdp_r8(&r);
        rdp_skip(&r, 1);                /* compressedType              */
        rdp_skip(&r, 2);                /* compressedLength            */

        if (r.underflow) return 0;

        switch (pdu_type2) {
        case PDUTYPE2_INPUT:
            rdp_handle_input_pdu(&r);
            break;

        case PDUTYPE2_REFRESH_RECT:
        case PDUTYPE2_SUPPRESS_OUTPUT:
            /* The client wants the screen again. Invalidating the
             * shadow makes the next pass send every tile. */
            rdp.shadow_valid = 0;
            break;

        case PDUTYPE2_SHUTDOWN_REQUEST:
            /* Refuse: a remote client may not shut the machine down. */
            rdp_send_data_pdu(rdp.sock, PDUTYPE2_SHUTDOWN_DENIED, 0, 0);
            break;

        default:
            break;
        }
    }
    return 0;
}

/* ===== the session ===== */

static uint16_t *rdp_shadow_alloc(uint32_t w, uint32_t h) {
    uint64_t bytes = (uint64_t)w * h * 2;
    uint32_t pages = (uint32_t)((bytes + 4095) / 4096);
    uint64_t phys;
    void *p;

    p = kmalloc_pages(pages, &phys);
    if (!p) return 0;

    /* Not the framebuffer's contents: a shadow that starts equal to the
     * screen would make the first pass send nothing and the client
     * would sit on a blank window until something moved. */
    {
        uint16_t *s = (uint16_t *)p;
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) s[i] = 0;
    }
    return (uint16_t *)p;
}

/*
 * One client, start to finish.
 *
 * Every step is sequential and every failure closes the socket: there
 * is no recovery path in an RDP handshake, because a client that got a
 * PDU it did not expect has already stopped listening.
 */
static void rdp_session(int s) {
    uint32_t requested = PROTOCOL_RDP;
    uint32_t idle = 0;

    rdp.sock = s;
    rdp.share_id = RDP_SHARE_ID;
    rdp.shift_down = 0;
    rdp.caps_lock = 0;
    rdp.shadow_valid = 0;

    vxnet_nodelay(s, 1);

    rdp.state = RDP_ST_X224;
    if (rdp_do_x224(s) != 0) goto done;

    rdp.state = RDP_ST_MCS_CONNECT;
    if (rdp_do_mcs_connect(s, requested) != 0) goto done;

    rdp.state = RDP_ST_MCS_ATTACH;
    if (rdp_do_mcs_setup(s) != 0) goto done;

    rdp.state = RDP_ST_CLIENT_INFO;
    if (rdp_do_client_info(s) != 0) goto done;

    rdp.state = RDP_ST_LICENSING;
    if (rdp_send_license_ok(s) != 0) goto done;

    rdp.state = RDP_ST_CAPABILITIES;
    {
        /* The desktop is whatever the compositor is rendering; the
         * client's requested size is noted but not honoured, because
         * this session is the console rather than a new one. */
        uint32_t w = rdp.frame_w ? rdp.frame_w : 1024;
        uint32_t h = rdp.frame_h ? rdp.frame_h : 768;

        if (rdp_send_demand_active(s, (uint16_t)w, (uint16_t)h) != 0)
            goto done;
        if (rdp_read_confirm_active(s) != 0) goto done;

        rdp.shadow = rdp_shadow_alloc(w, h);
        if (!rdp.shadow) {
            rdp_log("no memory for the screen shadow");
            goto done;
        }
        rdp.shadow_w = w;
        rdp.shadow_h = h;
    }

    rdp.state = RDP_ST_FINALIZATION;
    if (rdp_do_finalization(s) != 0) goto done;

    rdp.state = RDP_ST_ACTIVE;
    rdp.status = "session active";
    rdp.connections++;

    /*
     * The session loop.
     *
     * The socket is polled with a short timeout rather than blocked on,
     * because this thread has two jobs: deliver input as it arrives and
     * push screen updates as they are produced. A blocking read would
     * do the first well and never do the second.
     */
    /*
     * Short on the read, generous on the write.
     *
     * The read timeout is the poll interval: it decides how quickly
     * input is noticed. The write timeout has to cover a whole screen
     * refresh -- the first pass after connecting sends every tile, and
     * at 1280x800 that is over three hundred of them -- so setting both
     * to the poll interval makes the very first update fail to drain
     * and take the session down with it.
     */
    vxnet_timeout(s, 20000);
    vxnet_rcv_timeout(s, 10);

    while (rdp.enabled) {
        int len = -1;

        /*
         * A read that returns nothing is either the ten-millisecond
         * timeout expiring or the peer having closed, and this socket
         * API does not distinguish them -- both come back as a
         * non-positive return with no errno to inspect.
         *
         * So departure is detected where it is unambiguous instead: a
         * send that fails, which happens on the very next screen
         * update, or a minute with nothing arriving at all. Treating a
         * zero here as a close is what makes the session end the
         * instant the handshake finishes, before a single tile is sent.
         */
        {
            int r = vxnet_recv(s, rdp_in, TPKT_HDR_LEN);
            if (r > 0) {
                uint32_t total;
                if (r < TPKT_HDR_LEN &&
                    rdp_read_full(s, rdp_in + r, TPKT_HDR_LEN - r) != 0) break;
                if (rdp_in[0] != TPKT_VERSION) break;
                total = ((uint32_t)rdp_in[2] << 8) | rdp_in[3];
                if (total < TPKT_HDR_LEN || total > RDP_IN_BUF) break;
                if (total > TPKT_HDR_LEN &&
                    rdp_read_full(s, rdp_in + TPKT_HDR_LEN,
                                  total - TPKT_HDR_LEN) != 0) break;
                len = (int)total;
            }
        }

        if (len > 0) {
            idle = 0;
            if (rdp_handle_active_pdu(rdp_in, (uint32_t)len) != 0) break;
        }

        /* Push whatever the compositor has produced since last time. */
        if (rdp.frame_seq != rdp.frame_seen) {
            rdp.frame_seen = rdp.frame_seq;
            if (rdp_send_dirty_tiles(s) != 0) break;
        } else if (len <= 0) {
            if (++idle > 6000) {        /* about a minute of nothing   */
                rdp_log("session idle timeout");
                break;
            }
            sched_sleep_ms(10);
        }
    }

done:
    if (rdp.shadow) {
        kfree_pages(rdp.shadow,
                    (uint32_t)(((uint64_t)rdp.shadow_w * rdp.shadow_h * 2
                                + 4095) / 4096));
        rdp.shadow = 0;
    }
    vxnet_close(s);
    rdp.sock = -1;
    rdp.state = RDP_ST_CLOSED;
    rdp.status = "waiting for a connection";
    rdp_log("session closed");
}

/*
 * The server thread.
 *
 * One connection at a time. A second client would need a second shadow
 * buffer and a second share id, and more to the point two clients
 * driving one mouse is not a session, it is a fight -- so a second
 * connection is accepted and immediately closed rather than left
 * hanging in the backlog.
 */
static void rdp_server_thread(void) {
    rdp.listen_sock = vxnet_listen(RDP_PORT, 2);
    if (rdp.listen_sock < 0) {
        rdp_log("could not listen on 3389");
        rdp.enabled = 0;
        rdp.status = "could not bind port 3389";
        return;
    }

    rdp_log("listening on 3389 (plaintext: there is no encryption)");
    rdp.status = "waiting for a connection";

    while (rdp.enabled) {
        uint8_t peer[4];
        int c = vxnet_accept(rdp.listen_sock, peer);

        if (c < 0) {
            sched_sleep_ms(100);
            continue;
        }

        if (rdp.sock >= 0) {
            rdp_log("refusing a second client - one session at a time");
            vxnet_close(c);
            continue;
        }

        for (int i = 0; i < 4; i++) rdp.peer[i] = peer[i];
        serial_puts("[rdp] client from ");
        for (int i = 0; i < 4; i++) {
            serial_put_dec(peer[i]);
            if (i != 3) serial_putc('.');
        }
        serial_putc('\n');

        rdp_session(c);
    }

    vxnet_close(rdp.listen_sock);
    rdp.listen_sock = -1;
    rdp.status = "stopped";
}

/* ===== the public face ===== */

static int rdp_start(void) {
    if (rdp.enabled) return 0;

    if (!vxnet_up()) {
        rdp_log("no network");
        return -1;
    }

    rdp.enabled = 1;
    rdp.sock = -1;
    rdp.listen_sock = -1;
    rdp.state = RDP_ST_IDLE;
    rdp.status = "starting";

    /*
     * Below the network stack, deliberately.
     *
     * lwIP's own thread runs at 5. A bulk sender at the same priority
     * competes with the stack that has to process the ACKs opening the
     * window it is waiting on -- so a full send buffer never drains,
     * every retry finds it still full, and the session gives up after
     * whatever happened to fit. Screen data is the least urgent thing
     * on this machine and is scheduled that way.
     */
    if (!sched_spawn_kernel(rdp_server_thread, "rdpd", PRIO_NORMAL)) {
        rdp.enabled = 0;
        rdp_log("could not start the server thread");
        return -1;
    }
    return 0;
}

static void rdp_stop(void) {
    rdp.enabled = 0;
    if (rdp.sock >= 0) vxnet_close(rdp.sock);
}

static int rdp_running(void)      { return rdp.enabled; }
static int rdp_connected(void)    { return rdp.state == RDP_ST_ACTIVE; }
static int rdp_is_encrypted(void) { return 0; }
static const char *rdp_status(void) {
    return rdp.status ? rdp.status : "stopped";
}

#endif /* NET_RDP_C */
