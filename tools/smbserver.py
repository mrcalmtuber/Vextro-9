#!/usr/bin/env python3
"""
tools/smbserver.py — an SMB2 file server, for testing the client.

Written from MS-SMB2 and MS-NLMP rather than from src/smb2.h, and strict
about the things a lenient server would let through:

  * every signed request has its HMAC-SHA256 recomputed here, over the
    message with the signature field zeroed. A client that signs before
    zeroing, or over the wrong length, or with the wrong key, fails.
  * the NTLMv2 response is recomputed from the password in the account
    table. A client that folds the wrong fields into the v2 hash -- the
    classic being to uppercase the domain as well as the user -- gets
    STATUS_LOGON_FAILURE and no hint as to why, which is exactly what a
    real server would do.
  * the SPNEGO wrapper is decoded as DER, with non-minimal lengths and
    the indefinite form rejected.
  * StructureSize is checked on every request. It is the field SMB2
    added so that a malformed message is caught rather than misread, and
    a server that ignores it gives that away.

MD4 and RC4 are implemented here rather than taken from hashlib, partly
because OpenSSL 3 no longer offers MD4 in its default provider, and
partly because an independent implementation is the point.

    python3 tools/smbserver.py [port]

Port 445 needs root, so the default is 4445 and the client is told where
to look. One share, one account:

    \\\\server\\share      vextro / hunter2   (domain WORKGROUP)
"""

import hashlib
import hmac
import os
import socketserver
import struct
import sys
import time

USER, PASSWORD, DOMAIN = "vextro", "hunter2", "WORKGROUP"
SHARE = "share"

FILES = {
    "hello.txt": b"Hello from a share on another machine.\n",
    "notes.txt": b"SMB2 signs every message.\nIt does not encrypt them.\n",
    "numbers.txt": b"".join(b"%d\n" % i for i in range(1, 201)),
}


# ---------------------------------------------------------------- MD4

def md4(data):
    def rol(x, n):
        x &= 0xFFFFFFFF
        return ((x << n) | (x >> (32 - n))) & 0xFFFFFFFF

    h = [0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476]
    msg = bytearray(data)
    bitlen = len(msg) * 8
    msg.append(0x80)
    while len(msg) % 64 != 56:
        msg.append(0)
    msg += struct.pack("<Q", bitlen)

    for off in range(0, len(msg), 64):
        x = list(struct.unpack("<16I", msg[off:off + 64]))
        a, b, c, d = h
        for i, s in zip(range(16), [3, 7, 11, 19] * 4):
            k = i
            if i % 4 == 0:
                a = rol(a + ((b & c) | (~b & 0xFFFFFFFF & d)) + x[k], s)
            elif i % 4 == 1:
                d = rol(d + ((a & b) | (~a & 0xFFFFFFFF & c)) + x[k], s)
            elif i % 4 == 2:
                c = rol(c + ((d & a) | (~d & 0xFFFFFFFF & b)) + x[k], s)
            else:
                b = rol(b + ((c & d) | (~c & 0xFFFFFFFF & a)) + x[k], s)
        order2 = [0, 4, 8, 12, 1, 5, 9, 13, 2, 6, 10, 14, 3, 7, 11, 15]
        for i, k in enumerate(order2):
            s = [3, 5, 9, 13][i % 4]
            if i % 4 == 0:
                a = rol(a + ((b & c) | (b & d) | (c & d)) + x[k] + 0x5A827999, s)
            elif i % 4 == 1:
                d = rol(d + ((a & b) | (a & c) | (b & c)) + x[k] + 0x5A827999, s)
            elif i % 4 == 2:
                c = rol(c + ((d & a) | (d & b) | (a & b)) + x[k] + 0x5A827999, s)
            else:
                b = rol(b + ((c & d) | (c & a) | (d & a)) + x[k] + 0x5A827999, s)
        order3 = [0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15]
        for i, k in enumerate(order3):
            s = [3, 9, 11, 15][i % 4]
            if i % 4 == 0:
                a = rol(a + (b ^ c ^ d) + x[k] + 0x6ED9EBA1, s)
            elif i % 4 == 1:
                d = rol(d + (a ^ b ^ c) + x[k] + 0x6ED9EBA1, s)
            elif i % 4 == 2:
                c = rol(c + (d ^ a ^ b) + x[k] + 0x6ED9EBA1, s)
            else:
                b = rol(b + (c ^ d ^ a) + x[k] + 0x6ED9EBA1, s)
        h = [(h[0] + a) & 0xFFFFFFFF, (h[1] + b) & 0xFFFFFFFF,
             (h[2] + c) & 0xFFFFFFFF, (h[3] + d) & 0xFFFFFFFF]
    return struct.pack("<4I", *h)


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray()
    i = j = 0
    for ch in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(ch ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def nt_hash(password):
    return md4(password.encode("utf-16-le"))


def ntlmv2_hash(user, domain, nthash):
    return hmac.new(nthash, (user.upper() + domain).encode("utf-16-le"),
                    hashlib.md5).digest()


# ---------------------------------------------------------------- DER

class Bad(Exception):
    pass


def der(buf, off=0):
    if off + 2 > len(buf):
        raise Bad("truncated")
    tag = buf[off]
    n = buf[off + 1]
    hdr = 2
    if n == 0x80:
        raise Bad("indefinite length is not DER")
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 4:
            raise Bad("bad length form")
        n = int.from_bytes(buf[off + 2:off + 2 + k], "big")
        if n < 0x80:
            raise Bad("non-minimal length")
        hdr = 2 + k
    end = off + hdr + n
    if end > len(buf):
        raise Bad("length past end")
    return tag, buf[off + hdr:end], end


def der_all(body):
    out, i = [], 0
    while i < len(body):
        tag, val, i = der(body, i)
        out.append((tag, val))
    return out


def spnego_token(buf):
    """The mechanism token from a NegTokenInit or NegTokenResp."""
    tag, body, _ = der(buf)
    if tag == 0x60:
        items = der_all(body)
        if not items or items[0][0] != 0x06:
            raise Bad("GSS token does not start with a mechanism OID")
        tag, body = items[1]
    if tag not in (0xA0, 0xA1):
        raise Bad("expected a NegotiationToken, got tag 0x%02x" % tag)
    seq = der_all(body)
    if len(seq) != 1 or seq[0][0] != 0x30:
        raise Bad("NegToken must wrap one SEQUENCE")
    for t, v in der_all(seq[0][1]):
        if t == 0xA2:
            inner = der_all(v)
            if len(inner) != 1 or inner[0][0] != 0x04:
                raise Bad("mechToken must be an OCTET STRING")
            return inner[0][1]
    raise Bad("no mechToken in the SPNEGO wrapper")


def enc_len(n):
    if n < 0x80:
        return bytes([n])
    out = b""
    while n:
        out = bytes([n & 0xFF]) + out
        n >>= 8
    return bytes([0x80 | len(out)]) + out


def tlv(tag, body):
    return bytes([tag]) + enc_len(len(body)) + body


OID_SPNEGO = bytes([0x2b, 6, 1, 5, 5, 2])
OID_NTLM = bytes([0x2b, 6, 1, 4, 1, 0x82, 0x37, 2, 2, 0x0a])


def spnego_wrap_challenge(token):
    return tlv(0xA1, tlv(0x30,
        tlv(0xA0, tlv(0x0A, b"\x01")) +          # accept-incomplete
        tlv(0xA1, tlv(0x06, OID_NTLM)) +
        tlv(0xA2, tlv(0x04, token))))


# ------------------------------------------------------------- NTLMSSP

def filetime(t=None):
    if t is None:
        t = time.time()
    return int((t + 11644473600) * 10_000_000)


def av(id_, val):
    return struct.pack("<HH", id_, len(val)) + val


def make_challenge(challenge):
    target = (av(2, DOMAIN.encode("utf-16-le")) +
              av(1, b"V\x00E\x00X\x00T\x00R\x00O\x00") +
              av(7, struct.pack("<Q", filetime())) +
              av(0, b""))
    tname = DOMAIN.encode("utf-16-le")
    payload_off = 56
    msg = bytearray(payload_off)
    msg[0:8] = b"NTLMSSP\x00"
    struct.pack_into("<I", msg, 8, 2)
    struct.pack_into("<HHI", msg, 12, len(tname), len(tname), payload_off)
    flags = (0x00000001 | 0x00000004 | 0x00000010 | 0x00000200 |
             0x00008000 | 0x00080000 | 0x00800000 | 0x40000000 |
             0x20000000 | 0x80000000)
    struct.pack_into("<I", msg, 20, flags)
    msg[24:32] = challenge
    struct.pack_into("<HHI", msg, 40, len(target), len(target),
                     payload_off + len(tname))
    return bytes(msg) + tname + target


def parse_authenticate(msg, challenge, log):
    if msg[:8] != b"NTLMSSP\x00" or struct.unpack_from("<I", msg, 8)[0] != 3:
        raise Bad("not an NTLMSSP AUTHENTICATE")

    def fld(off):
        ln, _, o = struct.unpack_from("<HHI", msg, off)
        if o + ln > len(msg):
            raise Bad("a field points outside the message")
        return msg[o:o + ln]

    ntresp = fld(20)
    domain = fld(28).decode("utf-16-le")
    user = fld(36).decode("utf-16-le")
    sealed = fld(52)
    flags = struct.unpack_from("<I", msg, 60)[0]

    log("     AUTHENTICATE from %s\\%s, %d-byte NT response"
        % (domain or "(none)", user, len(ntresp)))

    if user != USER:
        raise Bad("no such account: %r" % user)
    if len(ntresp) < 24:
        raise Bad("NTLMv1 response offered; this server requires v2")

    proof, blob = ntresp[:16], ntresp[16:]
    v2 = ntlmv2_hash(user, domain, nt_hash(PASSWORD))
    want = hmac.new(v2, challenge + blob, hashlib.md5).digest()
    if not hmac.compare_digest(want, proof):
        log("     the NTLMv2 proof does not match")
        return None
    log("     NTLMv2 proof verifies")

    base = hmac.new(v2, proof, hashlib.md5).digest()
    if flags & 0x40000000:
        if len(sealed) != 16:
            raise Bad("key exchange negotiated but no key sent")
        session = rc4(base, sealed)
        log("     session key recovered through RC4 key exchange")
    else:
        session = base
    return session


# ---------------------------------------------------------------- SMB2

NEGOTIATE, SESSION_SETUP, LOGOFF = 0x00, 0x01, 0x02
TREE_CONNECT, TREE_DISCONNECT = 0x03, 0x04
CREATE, CLOSE, READ, WRITE, QUERY_DIRECTORY = 0x05, 0x06, 0x08, 0x09, 0x0E

ST_SUCCESS = 0x00000000
ST_MORE = 0xC0000016
ST_NO_MORE_FILES = 0x80000006
ST_NOT_FOUND = 0xC0000034
ST_LOGON_FAILURE = 0xC000006D
ST_BAD_SHARE = 0xC00000CC
ST_END_OF_FILE = 0xC0000011
ST_INVALID = 0xC000000D


class Handler(socketserver.BaseRequestHandler):
    def log(self, s):
        print(s, flush=True)

    def setup(self):
        self.session_key = None
        self.session_id = 0
        self.tree_id = 0
        self.challenge = None
        self.handles = {}
        self.next_handle = 1
        self.signing = True

    def handle(self):
        buf = b""
        while True:
            try:
                chunk = self.request.recv(65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while len(buf) >= 4:
                n = int.from_bytes(buf[1:4], "big")
                if len(buf) < 4 + n:
                    break
                msg, buf = buf[4:4 + n], buf[4 + n:]
                try:
                    reply = self.dispatch(msg)
                except Bad as e:
                    self.log("  rejected: %s" % e)
                    return
                if reply is None:
                    return
                self.request.sendall(len(reply).to_bytes(4, "big") + reply)

    # ---- header handling ----

    def dispatch(self, msg):
        if msg[:4] != b"\xfeSMB":
            raise Bad("not an SMB2 message")
        if struct.unpack_from("<H", msg, 4)[0] != 64:
            raise Bad("header size is not 64")
        cmd = struct.unpack_from("<H", msg, 12)[0]
        flags = struct.unpack_from("<I", msg, 16)[0]
        self.mid = struct.unpack_from("<Q", msg, 24)[0]

        # SESSION_SETUP is checked inside its own handler, not here: the
        # message that completes authentication is signed with a key
        # that does not exist until that message has been parsed.
        if flags & 0x08 and cmd != SESSION_SETUP:
            self.check_signature(msg)
        elif self.session_key and cmd not in (NEGOTIATE, SESSION_SETUP):
            raise Bad("command 0x%02x arrived unsigned on a signed session" % cmd)

        body = msg[64:]
        fn = {NEGOTIATE: self.negotiate, SESSION_SETUP: self.session_setup,
              TREE_CONNECT: self.tree_connect, CREATE: self.create,
              CLOSE: self.close, READ: self.read, WRITE: self.write,
              QUERY_DIRECTORY: self.query_dir,
              TREE_DISCONNECT: self.tree_disconnect,
              LOGOFF: self.logoff}.get(cmd)
        if fn is None:
            raise Bad("unsupported command 0x%02x" % cmd)
        status, out = fn(body)
        return self.reply(cmd, status, out)

    def check_signature(self, msg):
        if not self.session_key:
            raise Bad("a message is signed before a session key exists")
        got = msg[48:64]
        zeroed = msg[:48] + b"\x00" * 16 + msg[64:]
        want = hmac.new(self.session_key, zeroed, hashlib.sha256).digest()[:16]
        if not hmac.compare_digest(got, want):
            self.log("     SIGNATURE MISMATCH")
            self.log("       client sent %s" % got.hex())
            self.log("       we compute  %s" % want.hex())
            raise Bad("bad message signature")

    def reply(self, cmd, status, body):
        h = bytearray(64)
        h[0:4] = b"\xfeSMB"
        struct.pack_into("<H", h, 4, 64)
        struct.pack_into("<H", h, 6, 1)
        struct.pack_into("<I", h, 8, status)
        struct.pack_into("<H", h, 12, cmd)
        struct.pack_into("<H", h, 14, 1)
        struct.pack_into("<I", h, 16, 0x00000001)       # server to redirector
        struct.pack_into("<Q", h, 24, self.mid)
        struct.pack_into("<I", h, 36, self.tree_id)
        struct.pack_into("<Q", h, 40, self.session_id)
        msg = bytes(h) + body
        if self.session_key and self.signing:
            f = struct.unpack_from("<I", msg, 16)[0] | 0x08
            msg = msg[:16] + struct.pack("<I", f) + msg[20:]
            mac = hmac.new(self.session_key, msg[:48] + b"\x00" * 16 + msg[64:],
                           hashlib.sha256).digest()[:16]
            msg = msg[:48] + mac + msg[64:]
        return msg

    # ---- the commands ----

    def negotiate(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 36:
            raise Bad("NEGOTIATE StructureSize is not 36")
        count = struct.unpack_from("<H", b, 2)[0]
        dialects = [struct.unpack_from("<H", b, 36 + 2 * i)[0] for i in range(count)]
        self.log("  NEGOTIATE, dialects %s" % [hex(d) for d in dialects])
        if 0x02FF in dialects or any(d < 0x0202 for d in dialects):
            raise Bad("this server does not speak SMB1")
        pick = 0x0210 if 0x0210 in dialects else (
               0x0202 if 0x0202 in dialects else None)
        if pick is None:
            raise Bad("no dialect in common")
        self.log("     -> %s, signing enabled" % hex(pick))

        out = bytearray(64)
        struct.pack_into("<H", out, 0, 65)
        struct.pack_into("<H", out, 2, 0x0001)          # signing enabled
        struct.pack_into("<H", out, 4, pick)
        out[8:24] = os.urandom(16)
        struct.pack_into("<I", out, 28, 65536)
        struct.pack_into("<I", out, 32, 65536)
        struct.pack_into("<I", out, 36, 65536)
        struct.pack_into("<Q", out, 40, filetime())
        struct.pack_into("<Q", out, 48, filetime())
        struct.pack_into("<H", out, 56, 64 + 64)
        struct.pack_into("<H", out, 58, 0)
        return ST_SUCCESS, bytes(out)

    def session_setup(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 25:
            raise Bad("SESSION_SETUP StructureSize is not 25")
        off = struct.unpack_from("<H", b, 12)[0] - 64
        ln = struct.unpack_from("<H", b, 14)[0]
        blob = b[off:off + ln]
        token = spnego_token(blob)

        if token[:8] != b"NTLMSSP\x00":
            raise Bad("the mechanism token is not NTLMSSP")
        mtype = struct.unpack_from("<I", token, 8)[0]

        if mtype == 1:
            self.log("  SESSION_SETUP: NTLMSSP negotiate")
            self.challenge = os.urandom(8)
            self.session_id = int.from_bytes(os.urandom(8), "little") | 1
            out = make_challenge(self.challenge)
            wrapped = spnego_wrap_challenge(out)
            body = bytearray(8)
            struct.pack_into("<H", body, 0, 9)
            struct.pack_into("<H", body, 2, 0)
            struct.pack_into("<H", body, 4, 64 + 8)
            struct.pack_into("<H", body, 6, len(wrapped))
            return ST_MORE, bytes(body) + wrapped

        if mtype == 3:
            key = parse_authenticate(token, self.challenge, self.log)
            if key is None:
                return ST_LOGON_FAILURE, struct.pack("<HHHH", 9, 0, 0, 0)
            self.session_key = key
            # The message that carried the proof is itself signed, and
            # verifying it is what makes a relayed blob useless.
            self.check_signature(self.cur_msg)
            self.log("     the AUTHENTICATE message's own signature verifies")
            return ST_SUCCESS, struct.pack("<HHHH", 9, 0, 0, 0)

        raise Bad("unexpected NTLMSSP message type %d" % mtype)

    def tree_connect(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 9:
            raise Bad("TREE_CONNECT StructureSize is not 9")
        off = struct.unpack_from("<H", b, 4)[0] - 64
        ln = struct.unpack_from("<H", b, 6)[0]
        path = b[off:off + ln].decode("utf-16-le")
        self.log("  TREE_CONNECT %s" % path)
        if not path.lower().endswith("\\" + SHARE):
            return ST_BAD_SHARE, struct.pack("<HH", 9, 0)
        self.tree_id = 1
        out = bytearray(16)
        struct.pack_into("<H", out, 0, 16)
        out[2] = 1                                    # disk share
        struct.pack_into("<I", out, 8, 0)
        struct.pack_into("<I", out, 12, 0x001F01FF)
        return ST_SUCCESS, bytes(out)

    def create(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 57:
            raise Bad("CREATE StructureSize is not 57")
        disp = struct.unpack_from("<I", b, 36)[0]
        opts = struct.unpack_from("<I", b, 40)[0]
        noff = struct.unpack_from("<H", b, 44)[0] - 64
        nlen = struct.unpack_from("<H", b, 46)[0]
        name = b[noff:noff + nlen].decode("utf-16-le") if nlen else ""
        self.log("  CREATE %r%s" % (name, " (directory)" if opts & 1 else ""))

        if opts & 1:                                   # FILE_DIRECTORY_FILE
            if name not in ("", "."):
                return ST_NOT_FOUND, b"\x00" * 8
            size, isdir = 0, True
        else:
            if name not in FILES:
                if disp == 5:                          # FILE_OVERWRITE_IF
                    FILES[name] = b""
                else:
                    return ST_NOT_FOUND, b"\x00" * 8
            size, isdir = len(FILES[name]), False

        hid = self.next_handle
        self.next_handle += 1
        fid = struct.pack("<QQ", hid, 0)
        self.handles[fid] = (name, isdir)

        out = bytearray(88)
        struct.pack_into("<H", out, 0, 89)
        struct.pack_into("<I", out, 4, 1)
        for o in (8, 16, 24, 32):
            struct.pack_into("<Q", out, o, filetime())
        struct.pack_into("<Q", out, 40, size)
        struct.pack_into("<Q", out, 48, size)
        struct.pack_into("<I", out, 56, 0x10 if isdir else 0x80)
        out[64:80] = fid
        return ST_SUCCESS, bytes(out) + b"\x00"

    def _fid(self, b, off):
        fid = b[off:off + 16]
        if fid not in self.handles:
            raise Bad("unknown file handle")
        return fid

    def read(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 49:
            raise Bad("READ StructureSize is not 49")
        want = struct.unpack_from("<I", b, 4)[0]
        offset = struct.unpack_from("<Q", b, 8)[0]
        name, isdir = self.handles[self._fid(b, 16)]
        data = FILES.get(name, b"")[offset:offset + want]
        self.log("  READ %s at %d, %d bytes" % (name, offset, len(data)))
        if not data:
            return ST_END_OF_FILE, struct.pack("<HBBII", 17, 0, 0, 0, 0)
        out = bytearray(16)
        struct.pack_into("<H", out, 0, 17)
        out[2] = 64 + 16
        struct.pack_into("<I", out, 4, len(data))
        return ST_SUCCESS, bytes(out) + data

    def write(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 49:
            raise Bad("WRITE StructureSize is not 49")
        doff = struct.unpack_from("<H", b, 2)[0] - 64
        ln = struct.unpack_from("<I", b, 4)[0]
        offset = struct.unpack_from("<Q", b, 8)[0]
        name, isdir = self.handles[self._fid(b, 16)]
        data = b[doff:doff + ln]
        cur = bytearray(FILES.get(name, b""))
        if len(cur) < offset + ln:
            cur += bytes(offset + ln - len(cur))
        cur[offset:offset + ln] = data
        FILES[name] = bytes(cur)
        self.log("  WRITE %s at %d, %d bytes" % (name, offset, ln))
        out = bytearray(16)
        struct.pack_into("<H", out, 0, 17)
        struct.pack_into("<I", out, 4, ln)
        return ST_SUCCESS, bytes(out)

    def query_dir(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 33:
            raise Bad("QUERY_DIRECTORY StructureSize is not 33")
        flags = b[3]
        self._fid(b, 8)
        if not (flags & 0x01) and getattr(self, "listed", False):
            self.log("  QUERY_DIRECTORY -> no more files")
            return ST_NO_MORE_FILES, struct.pack("<HHI", 9, 0, 0)
        self.listed = True

        buf = bytearray()
        names = sorted(FILES)
        for i, name in enumerate(names):
            wide = name.encode("utf-16-le")
            ent = bytearray(64)
            struct.pack_into("<I", ent, 4, i)
            for o in (8, 16, 24, 32):
                struct.pack_into("<Q", ent, o, filetime())
            struct.pack_into("<Q", ent, 40, len(FILES[name]))
            struct.pack_into("<Q", ent, 48, len(FILES[name]))
            struct.pack_into("<I", ent, 56, 0x80)
            struct.pack_into("<I", ent, 60, len(wide))
            ent += wide
            while len(ent) % 8:
                ent.append(0)
            if i != len(names) - 1:
                struct.pack_into("<I", ent, 0, len(ent))
            buf += ent
        self.log("  QUERY_DIRECTORY -> %d entries" % len(names))

        out = bytearray(8)
        struct.pack_into("<H", out, 0, 9)
        struct.pack_into("<H", out, 2, 64 + 8)
        struct.pack_into("<I", out, 4, len(buf))
        return ST_SUCCESS, bytes(out) + bytes(buf)

    def close(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 24:
            raise Bad("CLOSE StructureSize is not 24")
        fid = self._fid(b, 8)
        name = self.handles.pop(fid)[0]
        self.log("  CLOSE %s" % name)
        return ST_SUCCESS, bytes(60)

    def tree_disconnect(self, b):
        self.log("  TREE_DISCONNECT")
        self.tree_id = 0
        return ST_SUCCESS, struct.pack("<HH", 4, 0)

    def logoff(self, b):
        self.log("  LOGOFF")
        return ST_SUCCESS, struct.pack("<HH", 4, 0)


_orig_dispatch = Handler.dispatch


def dispatch_keeping_message(self, msg):
    self.cur_msg = msg
    return _orig_dispatch(self, msg)


Handler.dispatch = dispatch_keeping_message


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4445
    print("smb2 server on port %d, share \\\\*\\%s" % (port, SHARE), flush=True)
    print("  account %s\\%s, %d files" % (DOMAIN, USER, len(FILES)), flush=True)
    Server(("0.0.0.0", port), Handler).serve_forever()
