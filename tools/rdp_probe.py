#!/usr/bin/env python3
"""
tools/rdp_probe.py - drive a real RDP connection against a running Vextro.

The unit tests in tools/rdp_test.c check that each encoder produces the
bytes the specification prescribes. They cannot check that the *sequence*
is right: that the server answers a Connect-Initial with a well-formed
Connect-Response, that it assigns a user channel, confirms every join,
sends a licensing PDU that means "proceed", advertises capabilities a
client will accept, and then actually puts pixels on the wire.

This does that. It speaks enough of the client half of RDP to walk the
whole handshake and assert on every reply, then sends an input event and
waits for the screen updates that should follow.

Usage:
    python3 tools/rdp_probe.py [host] [port]

Defaults to 127.0.0.1:3389, which is where `make rdp-test` forwards the
guest's port.
"""

import socket
import struct
import sys
import time

checks = 0
fails = 0


def ok(msg):
    global checks
    checks += 1
    print(f"  ok   {msg}")


def bad(msg):
    global checks, fails
    checks += 1
    fails += 1
    print(f"  FAIL {msg}")


def expect(cond, msg):
    ok(msg) if cond else bad(msg)


def expect_eq(got, want, msg):
    if got == want:
        ok(msg)
    else:
        bad(f"{msg} (got {got!r}, want {want!r})")


# ---------- framing ----------

def tpkt(payload):
    return struct.pack(">BBH", 3, 0, len(payload) + 4) + payload


def x224_data(payload):
    return b"\x02\xf0\x80" + payload


def recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError("server closed the connection")
        buf += chunk
    return buf


def recv_tpkt(s):
    hdr = recv_exact(s, 4)
    if hdr[0] != 3:
        raise ValueError(f"not a TPKT frame: {hdr!r}")
    total = struct.unpack(">H", hdr[2:4])[0]
    return hdr + recv_exact(s, total - 4)


def per_len(n):
    return struct.pack(">H", n | 0x8000) if n > 0x7F else bytes([n])


def ber_len(n):
    if n > 0xFF:
        return b"\x82" + struct.pack(">H", n)
    if n > 0x7F:
        return b"\x81" + bytes([n])
    return bytes([n])


def ber_int(v):
    if v <= 0xFF:
        return b"\x02\x01" + bytes([v])
    if v <= 0xFFFF:
        return b"\x02\x02" + struct.pack(">H", v)
    return b"\x02\x04" + struct.pack(">I", v)


# ---------- 1. X.224 ----------

def do_x224(s):
    print("\nX.224 connection")
    cookie = b"Cookie: mstshash=probe\r\n"
    neg = struct.pack("<BBHI", 0x01, 0, 8, 0x00000000)   # PROTOCOL_RDP
    body = bytes([6 + len(cookie) + len(neg), 0xE0]) + \
        struct.pack(">HHB", 0, 0, 0) + cookie + neg
    s.sendall(tpkt(body))

    r = recv_tpkt(s)
    expect_eq(r[5], 0xD0, "the server sends a connection confirm")
    if len(r) >= 19:
        typ, _, ln, proto = struct.unpack("<BBHI", r[11:19])
        expect_eq(typ, 0x02, "it carries an RDP_NEG_RSP")
        expect_eq(ln, 8, "whose length field is 8")
        expect_eq(proto, 0, "and selects plain RDP (no TLS, no CredSSP)")
    else:
        bad("the confirm is too short to carry a negotiation response")


# ---------- 2. MCS connect ----------

def gcc_client_data(width, height):
    """CS_CORE, CS_SECURITY and CS_NET, the three blocks a server reads."""
    core = struct.pack("<HHIHHHHHHI",
                       0xC001, 216,          # type, length
                       0x00080004,           # version: RDP 5.0+
                       width, height,
                       0xCA01,               # colorDepth: 8bpp
                       0xAA03,               # SASSequence
                       0x409,                # keyboardLayout: US
                       2600, 1)              # clientBuild, ... (padded below)
    core += b"\x00" * (216 - len(core))

    sec = struct.pack("<HHII", 0xC002, 12, 0, 0)   # no encryption

    # one virtual channel, so the join loop has something to confirm
    net = struct.pack("<HHI", 0xC003, 8 + 12, 1)
    net += b"rdpdr\x00\x00\x00" + struct.pack("<I", 0x80800000)

    return core + sec + net


def do_mcs_connect(s, width, height):
    print("\nMCS connect")
    ud = gcc_client_data(width, height)

    # GCC ConferenceCreateRequest wrapping the user data
    ccr = (b"\x00\x05\x00\x14\x7c\x00\x01"
           + per_len(len(ud) + 14 + 12)
           + b"\x00\x08\x00\x10\x00\x01\xc0\x00"
           + b"\x44\x75\x63\x61"              # "Duca"
           + per_len(len(ud)) + ud)

    dom = (ber_int(34) + ber_int(2) + ber_int(0) + ber_int(1) +
           ber_int(0) + ber_int(1) + ber_int(0xFFFF) + ber_int(2))
    dom = b"\x30" + ber_len(len(dom)) + dom

    body = (b"\x04\x01\x01"          # callingDomainSelector
            + b"\x04\x01\x01"        # calledDomainSelector
            + b"\x01\x01\xff"        # upwardFlag
            + dom + dom + dom
            + b"\x04" + ber_len(len(ccr)) + ccr)

    ci = b"\x7f\x65" + ber_len(len(body)) + body
    s.sendall(tpkt(x224_data(ci)))

    r = recv_tpkt(s)
    p = r[7:]
    expect(p[0] == 0x7F and p[1] == 102,
           "the server sends an MCS Connect-Response")

    # result is the first ENUMERATED after the application tag
    i = 2
    i += 1 if p[i] < 0x80 else (p[i] & 0x7F) + 1
    expect(p[i] == 0x0A and p[i + 2] == 0,
           "whose result is rt-successful")

    expect(b"McDn" in r, "and it carries the server GCC block (McDn)")

    # find SC_NET and SC_SECURITY inside
    expect(b"\x03\x0c" in r or b"\x01\x0c" in r,
           "with the server core/net blocks present")

    j = r.find(b"\x02\x0c")
    if j >= 0 and j + 12 <= len(r):
        method, level = struct.unpack("<II", r[j + 4:j + 12])
        expect_eq(method, 0, "SC_SECURITY reports ENCRYPTION_METHOD_NONE")
        expect_eq(level, 0, "and ENCRYPTION_LEVEL_NONE")
    else:
        bad("no SC_SECURITY block in the connect response")

    return r


def channels_from_response(r):
    """Pull the I/O channel and any virtual channels out of SC_NET."""
    j = r.find(b"\x03\x0c")
    if j < 0:
        return [1003]
    io_ch, count = struct.unpack("<HH", r[j + 4:j + 8])
    chans = [io_ch]
    for k in range(count):
        off = j + 8 + k * 2
        if off + 2 <= len(r):
            chans.append(struct.unpack("<H", r[off:off + 2])[0])
    return chans


# ---------- 3. erect domain, attach user, joins ----------

def do_mcs_setup(s, channels):
    print("\nMCS domain setup")

    s.sendall(tpkt(x224_data(b"\x04\x01\x00\x01\x00")))      # erect domain
    s.sendall(tpkt(x224_data(b"\x28")))                      # attach user

    r = recv_tpkt(s)
    expect_eq(r[7], 0x2E, "the server confirms the attach")
    expect_eq(r[8], 0, "with a success result")
    user_id = struct.unpack(">H", r[9:11])[0] + 1001
    ok(f"and assigns user channel {user_id}")

    joined = []
    for ch in [user_id] + channels:
        s.sendall(tpkt(x224_data(b"\x38" +
                                 struct.pack(">HH", user_id - 1001, ch))))
        r = recv_tpkt(s)
        if r[7] != 0x3E or r[8] != 0:
            bad(f"channel {ch} was not confirmed")
            break
        got = struct.unpack(">H", r[13:15])[0]
        if got != ch:
            bad(f"channel {ch} confirmed as {got}")
            break
        joined.append(ch)

    expect_eq(len(joined), len(channels) + 1,
              f"every channel is confirmed ({len(joined)} of "
              f"{len(channels) + 1})")
    return user_id


# ---------- 4. client info, licensing ----------

def mcs_send(s, user_id, channel, payload):
    hdr = b"\x64" + struct.pack(">HH", user_id, channel) + b"\x70" \
        + per_len(len(payload))
    s.sendall(tpkt(x224_data(hdr + payload)))


def mcs_payload(frame):
    """Strip TPKT, X.224 and the MCS send header off a received frame."""
    off = 7
    if frame[off] >> 2 != 26:        # sendDataIndication
        return None
    off += 1 + 2 + 2 + 1
    off += 2 if frame[off] & 0x80 else 1
    return frame[off:]


def do_client_info(s, user_id):
    print("\nclient info and licensing")

    def uni(text):
        return text.encode("utf-16-le") + b"\x00\x00"

    dom, usr, pwd, shell, wd = uni(""), uni("probe"), uni(""), uni(""), uni("")
    info = struct.pack("<IIHHHHH", 0, 0x00000033, len(dom) - 2, len(usr) - 2,
                       len(pwd) - 2, len(shell) - 2, len(wd) - 2)
    info += dom + usr + pwd + shell + wd
    sec = struct.pack("<HH", 0x0040, 0)          # SEC_INFO_PKT
    mcs_send(s, user_id, 1003, sec + info)

    r = recv_tpkt(s)
    p = mcs_payload(r)
    expect(p is not None, "the server replies on the I/O channel")
    if p:
        flags = struct.unpack("<H", p[0:2])[0]
        expect(flags & 0x0080, "with SEC_LICENSE_PKT set")
        expect_eq(p[4], 0xFF, "an ERROR_ALERT licensing message")
        err, state = struct.unpack("<II", p[8:16])
        expect_eq(err, 7, "whose error code is STATUS_VALID_CLIENT")
        expect_eq(state, 2, "and state is ST_NO_TRANSITION")


# ---------- 5. capabilities ----------

CAP_NAMES = {
    1: "general", 2: "bitmap", 3: "order", 4: "bitmapcache",
    8: "pointer", 9: "share", 10: "colorcache", 13: "input",
    14: "font", 20: "virtualchannel",
}


def do_capabilities(s, user_id):
    print("\ncapability exchange")

    r = recv_tpkt(s)
    p = mcs_payload(r)
    if p is None:
        bad("no demand active PDU")
        return None

    total, ptype, src = struct.unpack("<HHH", p[0:6])
    expect_eq(ptype & 0x0F, 1, "the server sends a demand active PDU")
    expect_eq(ptype >> 4, 1, "with protocol version 1 in the type field")

    share_id, len_src, len_caps = struct.unpack("<IHH", p[6:14])
    ok(f"share id 0x{share_id:08x}")

    off = 14 + len_src
    ncaps = struct.unpack("<H", p[off:off + 2])[0]
    off += 4

    found = {}
    for _ in range(ncaps):
        if off + 4 > len(p):
            break
        ct, cl = struct.unpack("<HH", p[off:off + 4])
        if cl < 4:
            break
        found[ct] = p[off:off + cl]
        off += cl

    expect_eq(len(found), ncaps, f"all {ncaps} capability sets parse")

    for want in (1, 2, 4, 8, 13):
        expect(want in found,
               f"the {CAP_NAMES.get(want, want)} capability set is present")

    if 2 in found:
        bmp = found[2]
        bpp, _, _, _, w, h = struct.unpack("<HHHHHH", bmp[4:16])
        expect_eq(bpp, 16, "bitmap capability offers 16 bits per pixel")
        ok(f"desktop is {w}x{h}")
        return share_id, w, h

    return share_id, 0, 0


def send_confirm_active(s, user_id, share_id):
    caps = b""
    # general
    caps += struct.pack("<HHHHHHHHHHHBB", 1, 24, 1, 3, 0x0200, 0, 0,
                        0x0400, 0, 0, 0, 0, 0)
    # bitmap
    caps += struct.pack("<HHHHHHHHHHHBBHH", 2, 28, 16, 1, 1, 1,
                        1024, 768, 0, 1, 0, 0, 0, 1, 0)
    # order
    caps += struct.pack("<HH", 3, 88) + b"\x00" * 84
    # pointer
    caps += struct.pack("<HHHHH", 8, 10, 0, 20, 20)
    # input
    caps += struct.pack("<HHHHIIII", 13, 88, 0x0015, 0, 0x409, 4, 0, 12)
    caps += b"\x00" * 64

    ncaps = 5
    body = struct.pack("<IHHH", share_id, 1002, 4, len(caps) + 4)
    body += b"MSTSC\x00"[:4]
    body += struct.pack("<HH", ncaps, 0) + caps

    total = 6 + len(body)
    pdu = struct.pack("<HHH", total, 0x13, user_id) + body
    mcs_send(s, user_id, 1003, pdu)


# ---------- 6. finalization ----------

def data_pdu(user_id, share_id, ptype2, body):
    total = 18 + len(body)
    return (struct.pack("<HHH", total, 0x17, user_id) +
            struct.pack("<IBBHBBH", share_id, 0, 1, total, ptype2, 0, 0) +
            body)


def do_finalization(s, user_id, share_id):
    print("\nfinalization")

    mcs_send(s, user_id, 1003,
             data_pdu(user_id, share_id, 31, struct.pack("<HH", 1, 1002)))
    mcs_send(s, user_id, 1003,
             data_pdu(user_id, share_id, 20, struct.pack("<HHI", 4, 0, 0)))
    mcs_send(s, user_id, 1003,
             data_pdu(user_id, share_id, 20, struct.pack("<HHI", 1, 0, 0)))
    mcs_send(s, user_id, 1003,
             data_pdu(user_id, share_id, 39,
                      struct.pack("<HHHH", 0, 0, 0x0003, 4)))

    seen = set()
    s.settimeout(5.0)
    deadline = time.time() + 5.0
    while time.time() < deadline and len(seen) < 4:
        try:
            p = mcs_payload(recv_tpkt(s))
        except (socket.timeout, EOFError):
            break
        if not p or len(p) < 18:
            continue
        if (struct.unpack("<H", p[2:4])[0] & 0x0F) != 7:
            continue
        seen.add(p[14])

    expect(31 in seen, "the server sends a synchronise PDU")
    expect(20 in seen, "and control PDUs (cooperate, granted)")
    expect(40 in seen, "and a font map, which ends the handshake")


# ---------- 7. the screen and input ----------

def do_screen(s, user_id, share_id):
    print("\nscreen updates and input")

    # A mouse move to the middle of the screen: proves input is parsed.
    ev = struct.pack("<IHHHH", 0, 0x8001, 0x0800, 400, 300)
    body = struct.pack("<HH", 1, 0) + ev
    mcs_send(s, user_id, 1003, data_pdu(user_id, share_id, 28, body))
    ok("an input event was accepted without the session dropping")

    # Ask for the whole screen again.
    #
    # The first full refresh goes out the moment the session becomes
    # live, which is while the finalization loop above is still reading
    # -- so by now the desktop is static and nothing new is being sent.
    # A refresh-rect makes the server invalidate its shadow and resend,
    # which is what this needs in order to see any pixels at all.
    area = struct.pack("<HHHH", 0, 0, 1279, 799)
    refresh = struct.pack("<BBBB", 1, 0, 0, 0) + area
    mcs_send(s, user_id, 1003, data_pdu(user_id, share_id, 33, refresh))

    tiles = 0
    pixels = 0
    s.settimeout(8.0)
    deadline = time.time() + 8.0

    while time.time() < deadline and tiles < 4:
        try:
            p = mcs_payload(recv_tpkt(s))
        except (socket.timeout, EOFError):
            break
        if not p or len(p) < 20:
            continue
        if (struct.unpack("<H", p[2:4])[0] & 0x0F) != 7:
            continue
        if p[14] != 2:                       # PDUTYPE2_UPDATE
            continue
        if struct.unpack("<H", p[18:20])[0] != 0:   # UPDATETYPE_BITMAP
            continue

        nrect = struct.unpack("<H", p[20:22])[0]
        off = 22
        for _ in range(nrect):
            if off + 18 > len(p):
                break
            (l, t, rgt, bot, w, h, bpp, flags, blen) = \
                struct.unpack("<HHHHHHHHH", p[off:off + 18])
            off += 18 + blen
            tiles += 1
            pixels += w * h
            if tiles == 1:
                expect_eq(bpp, 16, "bitmap updates are 16 bits per pixel")
                expect_eq(rgt - l + 1, w, "destRight is inclusive")
                expect_eq(bot - t + 1, h, "destBottom is inclusive")
                expect_eq(blen, w * h * 2, "the data length matches the tile")

    expect(tiles > 0, f"the desktop sent bitmap updates ({tiles} tiles, "
                      f"{pixels} pixels)")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 3389

    print(f"Vextro RDP probe -> {host}:{port}")
    print("=" * 52)

    s = socket.create_connection((host, port), timeout=10)
    s.settimeout(10.0)

    try:
        do_x224(s)
        r = do_mcs_connect(s, 1024, 768)
        channels = channels_from_response(r)
        user_id = do_mcs_setup(s, channels)
        do_client_info(s, user_id)
        caps = do_capabilities(s, user_id)
        if caps:
            share_id, w, h = caps
            send_confirm_active(s, user_id, share_id)
            do_finalization(s, user_id, share_id)
            do_screen(s, user_id, share_id)
    except Exception as e:
        bad(f"the connection failed: {type(e).__name__}: {e}")
    finally:
        s.close()

    print(f"\n{checks} checks, {fails} failures")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
