#ifndef SMB2_H
#define SMB2_H

#include <stdint.h>
#include "vxnet.h"
#include "ber.h"
#include "sha256.h"
#include "ntlmssp.h"

/*
 * src/smb2.h — reaching a file that is on another machine.
 *
 * SMB is how Windows shares files, and SMB2 is the rewrite that made it
 * bearable: SMB1 had a hundred-odd commands accreted over fifteen
 * years, variable-width structures, and a dialect negotiation that
 * every worm of the 2000s went through. SMB2 has nineteen commands, a
 * fixed 64-byte header, and each request declares its own structure
 * size so a mismatch is caught rather than misparsed.
 *
 * ---- SMB1 is not here, and that is the feature ----
 *
 * This speaks dialect 0x0202 and 0x0210 -- SMB 2.0.2 and 2.1 -- and
 * cannot be talked down to SMB1, because the code to do it does not
 * exist. WannaCry and NotPetya both travelled through SMB1. A client
 * that keeps it "for compatibility" is a client that can be made to use
 * it.
 *
 * ---- every message is signed ----
 *
 * Not encrypted: signed. Each message carries an HMAC-SHA256 over
 * itself, computed with its own signature field zeroed, under a key
 * that only someone who knew the password could have derived. That
 * stops an attacker who can see the connection from modifying a write
 * in flight or injecting a request into an authenticated session --
 * which is the actual SMB attack, far more than eavesdropping is.
 *
 * The contents are still in the clear. SMB 3.0 added encryption and
 * this does not implement it; the honest summary is that this protects
 * integrity and not confidentiality, and anything sensitive should be
 * going over something else.
 *
 * ---- authentication ----
 *
 * NTLMv2 through NTLMSSP (src/ntlmssp.h), wrapped in SPNEGO because
 * that is what the security buffer of a session setup contains. The
 * pieces for Kerberos authentication are all present next door in
 * src/kerberos.h -- an AP-REQ for cifs/<host> is exactly what would go
 * in the same buffer under a different SPNEGO mechanism -- and wiring
 * the two together is not done here.
 */

#define SMB2_PORT      445

#define SMB2_BUF       65536
#define SMB2_MAX_PATH  512

/* Commands. */
#define SMB2_NEGOTIATE        0x0000
#define SMB2_SESSION_SETUP    0x0001
#define SMB2_LOGOFF           0x0002
#define SMB2_TREE_CONNECT     0x0003
#define SMB2_TREE_DISCONNECT  0x0004
#define SMB2_CREATE           0x0005
#define SMB2_CLOSE            0x0006
#define SMB2_READ             0x0008
#define SMB2_WRITE            0x0009
#define SMB2_QUERY_DIRECTORY  0x000E

/* Status codes worth naming. */
#define STATUS_SUCCESS                    0x00000000u
#define STATUS_NO_MORE_FILES              0x80000006u
#define STATUS_MORE_PROCESSING_REQUIRED   0xC0000016u
#define STATUS_OBJECT_NAME_NOT_FOUND      0xC0000034u
#define STATUS_ACCESS_DENIED              0xC0000022u
#define STATUS_LOGON_FAILURE              0xC000006Du
#define STATUS_BAD_NETWORK_NAME           0xC00000CCu

#define SMB2_FLAGS_SERVER_TO_REDIR  0x00000001u
#define SMB2_FLAGS_SIGNED           0x00000008u

#define SMB2_NEGOTIATE_SIGNING_ENABLED  0x0001
#define SMB2_NEGOTIATE_SIGNING_REQUIRED 0x0002

#define SMB2_DIALECT_202  0x0202
#define SMB2_DIALECT_210  0x0210

/* CREATE parameters. */
#define FILE_READ_DATA        0x00000001u
#define FILE_WRITE_DATA       0x00000002u
#define FILE_READ_ATTRIBUTES  0x00000080u
#define FILE_LIST_DIRECTORY   0x00000001u
#define SYNCHRONIZE           0x00100000u
#define FILE_SHARE_READ       0x00000001u
#define FILE_OPEN             0x00000001u
#define FILE_OVERWRITE_IF     0x00000005u
#define FILE_DIRECTORY_FILE   0x00000001u
#define FILE_NON_DIRECTORY_FILE 0x00000040u

#define SMB2_MAX_ENTRIES 64

typedef struct {
    char     name[256];
    uint64_t size;
    uint32_t attributes;
} smb2_entry_t;

typedef struct {
    int      sock;
    int      open;
    int      dialect;
    int      signing;          /* whether the server asked for signatures */
    uint64_t message_id;
    uint64_t session_id;
    uint32_t tree_id;
    ntlm_session_t sess;
    uint32_t last_status;
    char     server[64];
    uint8_t  file_id[16];
    int      file_open;
    uint64_t file_size;
    uint8_t  tx[SMB2_BUF];
    uint8_t  rx[SMB2_BUF];
} smb2_conn_t;

static smb2_conn_t smb2;

/* SMB is little-endian everywhere, unlike the BER next door. */
static void sput16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }
static void sput32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void sput64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (i * 8));
}
static uint16_t sget16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t sget32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static uint64_t sget64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

static const char *smb2_status_name(uint32_t s) {
    switch (s) {
    case STATUS_SUCCESS:                  return "success";
    case STATUS_NO_MORE_FILES:            return "no more files";
    case STATUS_MORE_PROCESSING_REQUIRED: return "more processing required";
    case STATUS_OBJECT_NAME_NOT_FOUND:    return "no such file";
    case STATUS_ACCESS_DENIED:            return "access denied";
    case STATUS_LOGON_FAILURE:            return "logon failure (bad password)";
    case STATUS_BAD_NETWORK_NAME:         return "no such share";
    default:                              return "error";
    }
}

/* ===========================================================
 * SPNEGO
 *
 * The security buffer of a session setup does not contain NTLMSSP
 * directly. It contains a GSS-API token that *names* the mechanism and
 * then carries it -- which is how the same field can hold Kerberos
 * instead, and how a client offers both and lets the server choose.
 *
 * There is exactly one mechanism offered here, so the encoding is
 * nearly constant: only the token inside varies. It is still written
 * through src/ber.h rather than as a byte template, because the two
 * lengths in the header depend on the token's size and a template with
 * a patched length is how off-by-ones get shipped.
 * =========================================================== */

/* 1.3.6.1.5.5.2 */
static const uint8_t oid_spnego[]  = { 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02 };
/* 1.3.6.1.4.1.311.2.2.10 -- Microsoft's arc, which is where NTLM lives */
static const uint8_t oid_ntlmssp[] = { 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82,
                                       0x37, 0x02, 0x02, 0x0a };

#define BER_OID  0x06

static uint32_t spnego_init(const uint8_t *token, uint32_t tlen,
                            uint8_t *out, uint32_t max) {
    ber_w w = { out, max, 0, 0 };
    uint32_t a = ber_open(&w, BER_APP(0));
    ber_bytes(&w, BER_OID, oid_spnego, sizeof oid_spnego);

    uint32_t b = ber_open(&w, BER_CTX(0));
    uint32_t c = ber_open(&w, BER_SEQ);

    uint32_t m = ber_open(&w, BER_CTX(0));
    uint32_t l = ber_open(&w, BER_SEQ);
    ber_bytes(&w, BER_OID, oid_ntlmssp, sizeof oid_ntlmssp);
    ber_close(&w, l);
    ber_close(&w, m);

    uint32_t t = ber_open(&w, BER_CTX(2));
    ber_bytes(&w, BER_OCTET, token, tlen);
    ber_close(&w, t);

    ber_close(&w, c);
    ber_close(&w, b);
    ber_close(&w, a);
    return w.overflow ? 0 : w.n;
}

static uint32_t spnego_resp(const uint8_t *token, uint32_t tlen,
                            uint8_t *out, uint32_t max) {
    ber_w w = { out, max, 0, 0 };
    uint32_t a = ber_open(&w, BER_CTX(1));
    uint32_t b = ber_open(&w, BER_SEQ);
    uint32_t t = ber_open(&w, BER_CTX(2));
    ber_bytes(&w, BER_OCTET, token, tlen);
    ber_close(&w, t);
    ber_close(&w, b);
    ber_close(&w, a);
    return w.overflow ? 0 : w.n;
}

/*
 * Dig the mechanism token back out of whatever the server sent.
 *
 * A server may reply with a NegTokenResp, or -- on the first exchange
 * of a session it initiated -- with a NegTokenInit of its own. Rather
 * than model both, this walks the structure looking for the [2] member
 * that holds an OCTET STRING, which is the mechToken in one and the
 * responseToken in the other. The alternative is two nearly identical
 * parsers, and the one that gets tested is the one that runs.
 */
static int spnego_token(const uint8_t *p, uint32_t n, uint8_t *out, uint32_t max) {
    ber_r r = { p, n, 0, 0 };

    int tag = ber_peek(&r);
    if (tag == (int)BER_APP(0)) {
        ber_enter(&r, BER_APP(0));
        ber_skip(&r);                       /* the SPNEGO OID */
        tag = ber_peek(&r);
    }
    if (tag != (int)BER_CTX(0) && tag != (int)BER_CTX(1)) return -1;
    ber_enter(&r, (uint8_t)tag);
    uint32_t end = ber_enter(&r, BER_SEQ);

    while (r.at < end && !r.bad) {
        if (ber_peek(&r) == (int)BER_CTX(2)) {
            ber_enter(&r, BER_CTX(2));
            return ber_read_bytes(&r, out, max);
        }
        ber_skip(&r);
    }
    return -1;
}

/* ===========================================================
 * framing and signing
 * =========================================================== */

/*
 * Over TCP, SMB2 keeps the NetBIOS session header it inherited: a zero
 * byte and then a 24-bit big-endian length. The zero is a message type
 * from a protocol nobody has run in decades, and it is still required.
 */
static int smb2_send(uint32_t len) {
    smb2_conn_t *c = &smb2;
    uint8_t hdr[4];
    hdr[0] = 0;
    hdr[1] = (uint8_t)(len >> 16); hdr[2] = (uint8_t)(len >> 8); hdr[3] = (uint8_t)len;
    if (vxnet_send(c->sock, hdr, 4) != 4) return -1;
    return vxnet_send(c->sock, c->tx, (int)len) == (int)len ? 0 : -1;
}

static int smb2_recv(uint32_t *out_len) {
    smb2_conn_t *c = &smb2;
    uint8_t hdr[4];
    uint32_t have = 0;
    while (have < 4) {
        int n = vxnet_recv(c->sock, hdr + have, (int)(4 - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    uint32_t total = ((uint32_t)hdr[1] << 16) | ((uint32_t)hdr[2] << 8) | hdr[3];
    if (total < 64 || total > SMB2_BUF) return -1;

    have = 0;
    while (have < total) {
        int n = vxnet_recv(c->sock, c->rx + have, (int)(total - have));
        if (n <= 0) return -1;
        have += (uint32_t)n;
    }
    *out_len = total;
    return 0;
}

static void smb2_header(uint16_t command, uint32_t tree) {
    smb2_conn_t *c = &smb2;
    uint8_t *h = c->tx;
    for (int i = 0; i < 64; i++) h[i] = 0;
    h[0] = 0xFE; h[1] = 'S'; h[2] = 'M'; h[3] = 'B';
    sput16(h + 4, 64);                    /* header size */
    sput16(h + 6, 1);                     /* credit charge */
    sput16(h + 12, command);
    sput16(h + 14, 1);                    /* credits requested */
    sput64(h + 24, c->message_id++);
    sput32(h + 36, tree);
    sput64(h + 40, c->session_id);
}

/*
 * Sign in place.
 *
 * The signature covers the message including its own signature field,
 * which must therefore be zero while the HMAC is computed. Forgetting
 * to clear it produces a signature over whatever was left in the buffer
 * from the previous message -- stable, reproducible, and rejected.
 */
static void smb2_sign(uint32_t len) {
    smb2_conn_t *c = &smb2;
    if (!c->sess.valid || !c->signing) return;
    uint32_t f = sget32(c->tx + 16);
    sput32(c->tx + 16, f | SMB2_FLAGS_SIGNED);
    for (int i = 0; i < 16; i++) c->tx[48 + i] = 0;
    uint8_t mac[32];
    hmac_sha256(c->sess.session_key, 16, c->tx, len, mac);
    for (int i = 0; i < 16; i++) c->tx[48 + i] = mac[i];
}

/* And check the reply the same way. */
static int smb2_verify(uint32_t len) {
    smb2_conn_t *c = &smb2;
    if (!c->sess.valid || !c->signing) return 0;
    if (!(sget32(c->rx + 16) & SMB2_FLAGS_SIGNED)) return 0;

    uint8_t got[16], mac[32];
    for (int i = 0; i < 16; i++) { got[i] = c->rx[48 + i]; c->rx[48 + i] = 0; }
    hmac_sha256(c->sess.session_key, 16, c->rx, len, mac);
    for (int i = 0; i < 16; i++) c->rx[48 + i] = got[i];
    return ct_equal(got, mac, 16) ? 0 : -1;
}

/* One request, one reply. Returns the reply length, or -1. */
static int smb2_call(uint32_t len) {
    smb2_conn_t *c = &smb2;
    smb2_sign(len);
    if (smb2_send(len) != 0) return -1;

    uint32_t rl;
    if (smb2_recv(&rl) != 0) return -1;
    if (c->rx[0] != 0xFE || c->rx[1] != 'S' || c->rx[2] != 'M' || c->rx[3] != 'B')
        return -1;

    c->last_status = sget32(c->rx + 8);
    if (smb2_verify(rl) != 0) {
        serial_puts("[smb2] the server's signature does not verify; "
                    "dropping the reply\n");
        return -1;
    }
    return (int)rl;
}

/* ===========================================================
 * the exchanges
 * =========================================================== */

static int smb2_negotiate(void) {
    smb2_conn_t *c = &smb2;
    smb2_header(SMB2_NEGOTIATE, 0);
    uint8_t *b = c->tx + 64;

    sput16(b + 0, 36);
    sput16(b + 2, 2);                     /* two dialects offered */
    sput16(b + 4, SMB2_NEGOTIATE_SIGNING_ENABLED);
    sput32(b + 8, 0);                     /* no capabilities claimed */
    if (vx_random(b + 12, 16) != 16) return -1;    /* client GUID */
    sput64(b + 28, 0);
    sput16(b + 36, SMB2_DIALECT_202);
    sput16(b + 38, SMB2_DIALECT_210);

    int rl = smb2_call(64 + 40);
    if (rl < 0 || c->last_status != STATUS_SUCCESS) return -1;
    if (rl < 64 + 64) return -1;

    const uint8_t *r = c->rx + 64;
    c->dialect = sget16(r + 4);
    if (c->dialect != SMB2_DIALECT_202 && c->dialect != SMB2_DIALECT_210) {
        serial_puts("[smb2] the server chose a dialect this client did not "
                    "offer\n");
        return -1;
    }
    /* Signing is turned on if the server will accept it, not only if it
     * insists. A server that merely permits signing and a client that
     * only signs when forced is a connection nobody is protecting. */
    uint16_t mode = sget16(r + 2);
    c->signing = (mode & (SMB2_NEGOTIATE_SIGNING_ENABLED |
                          SMB2_NEGOTIATE_SIGNING_REQUIRED)) ? 1 : 0;
    return 0;
}

static int smb2_session_setup(const char *domain, const char *user,
                              const char *password, uint64_t now) {
    smb2_conn_t *c = &smb2;

    /* ---- one: the client's capabilities, wrapped in SPNEGO ---- */
    uint8_t token[512], wrapped[1024];
    uint32_t tl = ntlm_negotiate(token, sizeof token);
    uint32_t wl = spnego_init(token, tl, wrapped, sizeof wrapped);
    if (!wl) return -1;

    smb2_header(SMB2_SESSION_SETUP, 0);
    uint8_t *b = c->tx + 64;
    sput16(b + 0, 25);
    b[2] = 0;                                       /* flags */
    b[3] = SMB2_NEGOTIATE_SIGNING_ENABLED;
    sput32(b + 4, 0);
    sput32(b + 8, 0);
    sput16(b + 12, 64 + 24);                        /* security buffer offset */
    sput16(b + 14, (uint16_t)wl);
    sput64(b + 16, 0);
    for (uint32_t i = 0; i < wl; i++) b[24 + i] = wrapped[i];

    int rl = smb2_call(64 + 24 + wl);
    if (rl < 0) return -1;
    if (c->last_status != STATUS_MORE_PROCESSING_REQUIRED) {
        serial_puts("[smb2] the server did not challenge: ");
        serial_puts(smb2_status_name(c->last_status));
        serial_puts("\n");
        return -1;
    }

    /* The session id arrives with the challenge and is used from here
     * on, including on the message that completes the authentication. */
    c->session_id = sget64(c->rx + 40);

    uint16_t soff = sget16(c->rx + 64 + 4);
    uint16_t slen = sget16(c->rx + 64 + 6);
    if ((uint32_t)soff + slen > (uint32_t)rl) return -1;

    uint8_t inner[1024];
    int il = spnego_token(c->rx + soff, slen, inner, sizeof inner);
    if (il <= 0) {
        serial_puts("[smb2] no mechanism token in the server's reply\n");
        return -1;
    }

    ntlm_challenge_t ch;
    if (ntlm_parse_challenge(inner, (uint32_t)il, &ch) != 0) {
        serial_puts("[smb2] the challenge is not NTLMSSP\n");
        return -1;
    }

    /* ---- two: the proof ---- */
    uint32_t al = ntlm_authenticate(&ch, domain, user, password, now,
                                    token, sizeof token, &c->sess);
    if (!al) return -1;
    wl = spnego_resp(token, al, wrapped, sizeof wrapped);
    if (!wl) return -1;

    smb2_header(SMB2_SESSION_SETUP, 0);
    b = c->tx + 64;
    sput16(b + 0, 25);
    b[2] = 0;
    b[3] = SMB2_NEGOTIATE_SIGNING_ENABLED;
    sput32(b + 4, 0);
    sput32(b + 8, 0);
    sput16(b + 12, 64 + 24);
    sput16(b + 14, (uint16_t)wl);
    sput64(b + 16, 0);
    for (uint32_t i = 0; i < wl; i++) b[24 + i] = wrapped[i];

    /*
     * This message is signed, and it is the first one that can be: the
     * key was only derived a few lines ago. The server verifies it as
     * proof that the client really holds what it just claimed, so a
     * relay that forwards the blob without the key fails here.
     */
    rl = smb2_call(64 + 24 + wl);
    if (rl < 0) { c->sess.valid = 0; return -1; }
    if (c->last_status != STATUS_SUCCESS) {
        serial_puts("[smb2] authentication refused: ");
        serial_puts(smb2_status_name(c->last_status));
        serial_puts("\n");
        c->sess.valid = 0;
        return -1;
    }
    return 0;
}

static uint32_t smb2_wide(const char *s, uint8_t *out, uint32_t max) {
    uint32_t n = 0;
    while (*s && n + 2 <= max) { out[n++] = (uint8_t)*s++; out[n++] = 0; }
    return n;
}

static int smb2_tree_connect(const char *unc) {
    smb2_conn_t *c = &smb2;
    uint8_t path[SMB2_MAX_PATH];
    uint32_t pl = smb2_wide(unc, path, sizeof path);

    smb2_header(SMB2_TREE_CONNECT, 0);
    uint8_t *b = c->tx + 64;
    sput16(b + 0, 9);
    sput16(b + 2, 0);
    sput16(b + 4, 64 + 8);
    sput16(b + 6, (uint16_t)pl);
    for (uint32_t i = 0; i < pl; i++) b[8 + i] = path[i];

    int rl = smb2_call(64 + 8 + pl);
    if (rl < 0) return -1;
    if (c->last_status != STATUS_SUCCESS) {
        serial_puts("[smb2] tree connect: ");
        serial_puts(smb2_status_name(c->last_status));
        serial_puts("\n");
        return -1;
    }
    c->tree_id = sget32(c->rx + 36);
    return 0;
}

/* Open a file or a directory. */
static int smb2_create(const char *name, int directory, int for_write) {
    smb2_conn_t *c = &smb2;
    uint8_t wname[SMB2_MAX_PATH];
    uint32_t nl = smb2_wide(name, wname, sizeof wname);

    smb2_header(SMB2_CREATE, c->tree_id);
    uint8_t *b = c->tx + 64;
    for (int i = 0; i < 56; i++) b[i] = 0;
    sput16(b + 0, 57);
    sput32(b + 4, 2);                              /* impersonation */
    sput32(b + 24, for_write ? (FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE)
                             : (FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE));
    sput32(b + 28, 0);
    sput32(b + 32, FILE_SHARE_READ);
    sput32(b + 36, for_write ? FILE_OVERWRITE_IF : FILE_OPEN);
    sput32(b + 40, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE);
    sput16(b + 44, (uint16_t)(64 + 56));
    sput16(b + 46, (uint16_t)nl);
    sput32(b + 48, 0);
    sput32(b + 52, 0);
    for (uint32_t i = 0; i < nl; i++) b[56 + i] = wname[i];

    /* A zero-length name is legal -- it opens the share's root -- but
     * the buffer must still be one byte, because a structure size of 57
     * says there is one. */
    uint32_t len = 64 + 56 + (nl ? nl : 1);
    if (!nl) b[56] = 0;

    int rl = smb2_call(len);
    if (rl < 0) return -1;
    if (c->last_status != STATUS_SUCCESS) {
        serial_puts("[smb2] create ");
        serial_puts(name);
        serial_puts(": ");
        serial_puts(smb2_status_name(c->last_status));
        serial_puts("\n");
        return -1;
    }
    if (rl < 64 + 88) return -1;
    for (int i = 0; i < 16; i++) c->file_id[i] = c->rx[64 + 64 + i];
    c->file_size = sget64(c->rx + 64 + 48);
    c->file_open = 1;
    return 0;
}

static int smb2_close_file(void) {
    smb2_conn_t *c = &smb2;
    if (!c->file_open) return 0;

    smb2_header(SMB2_CLOSE, c->tree_id);
    uint8_t *b = c->tx + 64;
    for (int i = 0; i < 24; i++) b[i] = 0;
    sput16(b + 0, 24);
    for (int i = 0; i < 16; i++) b[8 + i] = c->file_id[i];

    int rl = smb2_call(64 + 24);
    c->file_open = 0;
    return (rl < 0 || c->last_status != STATUS_SUCCESS) ? -1 : 0;
}

/* Returns bytes read, 0 at end of file, or -1. */
static int smb2_read(uint64_t offset, uint8_t *out, uint32_t want) {
    smb2_conn_t *c = &smb2;
    if (!c->file_open) return -1;
    if (want > SMB2_BUF - 128) want = SMB2_BUF - 128;

    smb2_header(SMB2_READ, c->tree_id);
    uint8_t *b = c->tx + 64;
    for (int i = 0; i < 49; i++) b[i] = 0;
    sput16(b + 0, 49);
    sput32(b + 4, want);
    sput64(b + 8, offset);
    for (int i = 0; i < 16; i++) b[16 + i] = c->file_id[i];
    sput32(b + 32, 1);                       /* minimum count */

    int rl = smb2_call(64 + 49);
    if (rl < 0) return -1;
    if (c->last_status == STATUS_SUCCESS) {
        uint8_t doff = c->rx[64 + 2];
        uint32_t dlen = sget32(c->rx + 64 + 4);
        /* The offset is a single byte from the network and the length a
         * word; neither is trustworthy until checked against what
         * actually arrived. */
        if ((uint32_t)doff + dlen > (uint32_t)rl || dlen > want) return -1;
        for (uint32_t i = 0; i < dlen; i++) out[i] = c->rx[doff + i];
        return (int)dlen;
    }
    /* End of file is reported as an error, and is not one. */
    if (c->last_status == 0xC0000011u) return 0;      /* STATUS_END_OF_FILE */
    return -1;
}

static int smb2_write(uint64_t offset, const uint8_t *data, uint32_t len) {
    smb2_conn_t *c = &smb2;
    if (!c->file_open) return -1;
    if (len > SMB2_BUF - 256) return -1;

    smb2_header(SMB2_WRITE, c->tree_id);
    uint8_t *b = c->tx + 64;
    for (int i = 0; i < 48; i++) b[i] = 0;
    sput16(b + 0, 49);
    sput16(b + 2, (uint16_t)(64 + 48));
    sput32(b + 4, len);
    sput64(b + 8, offset);
    for (int i = 0; i < 16; i++) b[16 + i] = c->file_id[i];
    for (uint32_t i = 0; i < len; i++) b[48 + i] = data[i];

    int rl = smb2_call(64 + 48 + len);
    if (rl < 0 || c->last_status != STATUS_SUCCESS) {
        serial_puts("[smb2] write: ");
        serial_puts(smb2_status_name(c->last_status));
        serial_puts("\n");
        return -1;
    }
    return (int)sget32(c->rx + 64 + 4);
}

/*
 * List a directory.
 *
 * The handle must already be open on the directory. A real listing may
 * need several round trips -- the server fills its output buffer and
 * says STATUS_NO_MORE_FILES when there is nothing left -- so this loops
 * rather than assuming one reply is the whole directory, which is the
 * bug that makes a share look like it holds twelve files.
 */
static int smb2_list(const char *pattern, smb2_entry_t *out, int max) {
    smb2_conn_t *c = &smb2;
    if (!c->file_open) return -1;

    int found = 0;
    for (int round = 0; round < 64; round++) {
        uint8_t wpat[128];
        uint32_t pl = smb2_wide(pattern, wpat, sizeof wpat);

        smb2_header(SMB2_QUERY_DIRECTORY, c->tree_id);
        uint8_t *b = c->tx + 64;
        for (int i = 0; i < 33; i++) b[i] = 0;
        sput16(b + 0, 33);
        b[2] = 0x01;                       /* FileDirectoryInformation */
        b[3] = (round == 0) ? 0x01 : 0x00; /* restart on the first call */
        sput32(b + 4, 0);
        for (int i = 0; i < 16; i++) b[8 + i] = c->file_id[i];
        sput16(b + 24, (uint16_t)(64 + 32));
        sput16(b + 26, (uint16_t)pl);
        sput32(b + 28, 16384);
        for (uint32_t i = 0; i < pl; i++) b[32 + i] = wpat[i];

        int rl = smb2_call(64 + 32 + (pl ? pl : 1));
        if (rl < 0) return -1;
        if (c->last_status == STATUS_NO_MORE_FILES) return found;
        if (c->last_status != STATUS_SUCCESS) return found ? found : -1;

        uint16_t boff = sget16(c->rx + 64 + 2);
        uint32_t blen = sget32(c->rx + 64 + 4);
        if ((uint32_t)boff + blen > (uint32_t)rl) return -1;

        uint32_t at = boff;
        for (;;) {
            if (at + 64 > boff + blen) break;
            uint32_t next = sget32(c->rx + at);
            uint64_t size = sget64(c->rx + at + 40);
            uint32_t attr = sget32(c->rx + at + 56);
            uint32_t nlen = sget32(c->rx + at + 60);
            if (at + 64 + nlen > boff + blen) break;

            if (found < max) {
                smb2_entry_t *e = &out[found];
                uint32_t k = 0;
                /* UTF-16 back to bytes. Anything outside Latin-1 becomes
                 * a question mark rather than half a character. */
                for (uint32_t i = 0; i + 1 < nlen && k + 1 < sizeof e->name; i += 2) {
                    uint16_t u = (uint16_t)(c->rx[at+64+i] | (c->rx[at+64+i+1] << 8));
                    e->name[k++] = (u < 0x100) ? (char)u : '?';
                }
                e->name[k] = 0;
                e->size = size;
                e->attributes = attr;
                found++;
            }
            if (next == 0) break;
            if (next < 64 || at + next > boff + blen) break;
            at += next;
        }
    }
    return found;
}

static int smb2_tree_disconnect(void) {
    smb2_conn_t *c = &smb2;
    if (!c->tree_id) return 0;
    smb2_header(SMB2_TREE_DISCONNECT, c->tree_id);
    sput16(c->tx + 64, 4);
    sput16(c->tx + 66, 0);
    int rl = smb2_call(64 + 4);
    c->tree_id = 0;
    return rl < 0 ? -1 : 0;
}

static int smb2_logoff(void) {
    smb2_conn_t *c = &smb2;
    if (!c->session_id) return 0;
    smb2_header(SMB2_LOGOFF, 0);
    sput16(c->tx + 64, 4);
    sput16(c->tx + 66, 0);
    int rl = smb2_call(64 + 4);
    c->session_id = 0;
    c->sess.valid = 0;
    return rl < 0 ? -1 : 0;
}

/* ===========================================================
 * the front door
 * =========================================================== */

static int smb2_connect(const char *host, uint16_t port,
                        const char *domain, const char *user,
                        const char *password, uint64_t now) {
    smb2_conn_t *c = &smb2;
    if (c->open) return -1;

    uint8_t ip[4];
    if (!vxnet_resolve(host, ip)) {
        serial_puts("[smb2] cannot resolve ");
        serial_puts(host);
        serial_puts("\n");
        return -1;
    }
    c->sock = vxnet_socket();
    if (c->sock < 0) return -1;
    vxnet_timeout(c->sock, 15000);
    if (vxnet_connect(c->sock, ip, port) != 0) {
        vxnet_close(c->sock);
        serial_puts("[smb2] nothing answering there\n");
        return -1;
    }

    c->open = 1;
    c->message_id = 0;
    c->session_id = 0;
    c->tree_id = 0;
    c->file_open = 0;
    c->sess.valid = 0;
    c->signing = 0;

    if (smb2_negotiate() != 0) { vxnet_close(c->sock); c->open = 0; return -1; }

    serial_puts("[smb2] dialect ");
    serial_puts(c->dialect == SMB2_DIALECT_210 ? "2.1" : "2.0.2");
    serial_puts(c->signing ? ", signed\n" : ", UNSIGNED\n");

    if (smb2_session_setup(domain, user, password, now) != 0) {
        vxnet_close(c->sock);
        c->open = 0;
        return -1;
    }
    serial_puts("[smb2] authenticated as ");
    serial_puts(user);
    serial_puts("\n");
    return 0;
}

static void smb2_disconnect(void) {
    smb2_conn_t *c = &smb2;
    if (!c->open) return;
    smb2_close_file();
    smb2_tree_disconnect();
    smb2_logoff();
    vxnet_close(c->sock);
    c->open = 0;
}

static int smb2_signed(void) { return smb2.signing && smb2.sess.valid; }

#endif /* SMB2_H */
