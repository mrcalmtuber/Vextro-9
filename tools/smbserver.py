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
SMB3 = True                    # overridden by --no-smb3 on the command line
SHARES = ("share", "sysvol")   # SYSVOL is where Group Policy lives

def preg(entries):
    """MS-GPREG: "PReg", version 1, then [key;value;type;size;data] with
    every delimiter a UTF-16 character rather than a byte."""
    out = bytearray(b"PReg" + struct.pack("<I", 1))
    for key, name, typ, data in entries:
        out += "[".encode("utf-16-le")
        out += key.encode("utf-16-le") + b"\x00\x00"
        out += ";".encode("utf-16-le")
        out += name.encode("utf-16-le") + b"\x00\x00"
        out += ";".encode("utf-16-le")
        out += struct.pack("<I", typ)
        out += ";".encode("utf-16-le")
        out += struct.pack("<I", len(data))
        out += ";".encode("utf-16-le")
        out += data
        out += "]".encode("utf-16-le")
    return bytes(out)


GPO = "{31B2F340-016D-11D2-945F-00C04FB984F9}"
POLDIR = "vextro.test\\Policies\\" + GPO

FILES = {
    "hello.txt": b"Hello from a share on another machine.\n",
    "notes.txt": b"SMB2 signs every message.\nIt does not encrypt them.\n",
    "numbers.txt": b"".join(b"%d\n" % i for i in range(1, 201)),

    # A domain policy, laid out the way a real SYSVOL is.
    POLDIR + "\\GPT.INI":
        b"[General]\r\nVersion=65539\r\ndisplayName=Default Domain Policy\r\n",
    POLDIR + "\\Machine\\Registry.pol": preg([
        ("Software\\Policies\\Vextro\\Desktop", "ScreenSaverTimeout", 4,
         struct.pack("<I", 600)),
        ("Software\\Policies\\Vextro\\Desktop", "Wallpaper", 1,
         "starfield".encode("utf-16-le") + b"\x00\x00"),
        ("Software\\Policies\\Vextro\\Security", "RequireSigning", 4,
         struct.pack("<I", 1)),
        ("Software\\Policies\\Vextro\\Security", "LegalNotice", 1,
         "Authorised users only".encode("utf-16-le") + b"\x00\x00"),
    ]),
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


# ------------------------------------------------------- AES, CMAC, CCM
#
# For SMB 3.0. Written here for the same reason MD4 and RC4 are: an
# independent implementation is the point. A test server that shared the
# client's AES would confirm the two agree and nothing else -- and the
# ways to get CCM wrong (the B0 flags byte, the counter's starting
# value, whether the length prefix on the associated data counts toward
# the padding) are exactly the ways that produce a self-consistent
# implementation that interoperates with nothing.
#
# Pure Python and no external package, so the server runs anywhere
# python3 does. Slow, and it does not matter: this moves kilobytes.


def _gf_mul(a, b):
    p = 0
    for _ in range(8):
        if b & 1:
            p ^= a
        hi = a & 0x80
        a = (a << 1) & 0xFF
        if hi:
            a ^= 0x1B                     # x^8 + x^4 + x^3 + x + 1
        b >>= 1
    return p


def _build_sbox():
    """The S-box, computed rather than tabulated: multiplicative inverse
    in GF(2^8) followed by the affine transform. Typing 256 constants is
    the other option and it is the one with a typo in it."""
    inv = [0] * 256
    for i in range(1, 256):
        for j in range(1, 256):
            if _gf_mul(i, j) == 1:
                inv[i] = j
                break
    box = [0] * 256
    for i in range(256):
        x = inv[i]
        s = x
        for _ in range(4):
            x = ((x << 1) | (x >> 7)) & 0xFF
            s ^= x
        box[i] = s ^ 0x63
    return box


SBOX = _build_sbox()


def _aes_expand(key):
    rk = [list(key[i * 4:i * 4 + 4]) for i in range(4)]
    rcon = 1
    for i in range(4, 44):
        t = list(rk[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = _gf_mul(rcon, 2)
        rk.append([rk[i - 4][j] ^ t[j] for j in range(4)])
    return rk


def _aes_encrypt_block(rk, blk):
    # State is column-major: s[4*c + r] is row r of column c, and the
    # input maps straight onto it.
    s = list(blk)

    def add_round_key(rnd):
        for c in range(4):
            for r in range(4):
                s[4 * c + r] ^= rk[4 * rnd + c][r]

    add_round_key(0)
    for rnd in range(1, 11):
        for i in range(16):
            s[i] = SBOX[s[i]]
        t = list(s)
        for r in range(1, 4):
            for c in range(4):
                s[4 * c + r] = t[4 * ((c + r) % 4) + r]
        if rnd != 10:
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                s[4 * c + 0] = _gf_mul(a[0], 2) ^ _gf_mul(a[1], 3) ^ a[2] ^ a[3]
                s[4 * c + 1] = a[0] ^ _gf_mul(a[1], 2) ^ _gf_mul(a[2], 3) ^ a[3]
                s[4 * c + 2] = a[0] ^ a[1] ^ _gf_mul(a[2], 2) ^ _gf_mul(a[3], 3)
                s[4 * c + 3] = _gf_mul(a[0], 3) ^ a[1] ^ a[2] ^ _gf_mul(a[3], 2)
        add_round_key(rnd)
    return bytes(s)


def aes_encrypt_block(key, blk):
    return _aes_encrypt_block(_aes_expand(key), blk)


def _xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def aes_cmac(key, msg):
    rk = _aes_expand(key)
    l = _aes_encrypt_block(rk, b"\x00" * 16)

    def dbl(b):
        n = int.from_bytes(b, "big") << 1
        if b[0] & 0x80:
            n ^= 0x87
        return (n & ((1 << 128) - 1)).to_bytes(16, "big")

    k1 = dbl(l)
    k2 = dbl(k1)

    if len(msg) and len(msg) % 16 == 0:
        blocks, last = msg[:-16], _xor(msg[-16:], k1)
    else:
        pad = msg[len(msg) - len(msg) % 16:]
        pad = pad + b"\x80" + b"\x00" * (15 - len(pad))
        blocks, last = msg[:len(msg) - len(msg) % 16], _xor(pad, k2)

    x = b"\x00" * 16
    for i in range(0, len(blocks), 16):
        x = _aes_encrypt_block(rk, _xor(x, blocks[i:i + 16]))
    return _aes_encrypt_block(rk, _xor(x, last))


def _ccm_mac(rk, nonce, aad, pt, taglen):
    L = 15 - len(nonce)
    flags = (0x40 if aad else 0) | (((taglen - 2) // 2) << 3) | (L - 1)
    b0 = bytes([flags]) + nonce + len(pt).to_bytes(L, "big")
    x = _aes_encrypt_block(rk, b0)

    if aad:
        if len(aad) < 0xFF00:
            hdr = len(aad).to_bytes(2, "big")
        else:
            hdr = b"\xff\xfe" + len(aad).to_bytes(4, "big")
        blob = hdr + aad
        blob += b"\x00" * ((-len(blob)) % 16)
        for i in range(0, len(blob), 16):
            x = _aes_encrypt_block(rk, _xor(x, blob[i:i + 16]))

    if pt:
        blob = pt + b"\x00" * ((-len(pt)) % 16)
        for i in range(0, len(blob), 16):
            x = _aes_encrypt_block(rk, _xor(x, blob[i:i + 16]))
    return x


def _ccm_ctr(rk, nonce, data, start):
    L = 15 - len(nonce)
    out = bytearray()
    counter = start
    for i in range(0, len(data), 16):
        a = bytes([L - 1]) + nonce + counter.to_bytes(L, "big")
        s = _aes_encrypt_block(rk, a)
        chunk = data[i:i + 16]
        out += _xor(chunk, s[:len(chunk)])
        counter += 1
    return bytes(out)


def aes_ccm_encrypt(key, nonce, aad, pt, taglen=16):
    rk = _aes_expand(key)
    mac = _ccm_mac(rk, nonce, aad, pt, taglen)
    s0 = _ccm_ctr(rk, nonce, b"\x00" * 16, 0)
    tag = _xor(mac, s0)[:taglen]
    return _ccm_ctr(rk, nonce, pt, 1), tag


def aes_ccm_decrypt(key, nonce, aad, ct, tag, taglen=16):
    rk = _aes_expand(key)
    pt = _ccm_ctr(rk, nonce, ct, 1)
    mac = _ccm_mac(rk, nonce, aad, pt, taglen)
    s0 = _ccm_ctr(rk, nonce, b"\x00" * 16, 0)
    if not hmac.compare_digest(_xor(mac, s0)[:taglen], tag):
        return None
    return pt


def smb3_kdf(ki, label, context):
    """SP 800-108 counter mode, HMAC-SHA256, 128 bits out.

    The label and the context each carry their own terminating NUL, and
    a further zero separates them -- so there are two zero bytes between
    "SMB2AESCCM" and "ServerIn ". That is what interoperates."""
    data = (b"\x00\x00\x00\x01" + label + b"\x00" + b"\x00" + context
            + b"\x00" + b"\x00\x00\x00\x80")
    return hmac.new(ki, data, hashlib.sha256).digest()[:16]


def _aes_selftest():
    """FIPS-197 appendix C.1. A wrong AES here would look like a client
    bug, and this is much cheaper to read than that misunderstanding."""
    key = bytes(range(16))
    pt = bytes.fromhex("00112233445566778899aabbccddeeff")
    want = bytes.fromhex("69c4e0d86a7b0430d8cdb78070b4c55a")
    got = aes_encrypt_block(key, pt)
    if got != want:
        raise SystemExit("smbserver: AES is wrong: %s" % got.hex())
    # RFC 4493 test vector 2, to catch a CMAC subkey mistake.
    k = bytes.fromhex("2b7e151628aed2a6abf7158809cf4f3c")
    m = bytes.fromhex("6bc1bee22e409f96e93d7e117393172a")
    if aes_cmac(k, m).hex() != "070a16b46b4d4144f79bdd9dd04a287c":
        raise SystemExit("smbserver: AES-CMAC is wrong")
    # RFC 3610 packet vector #1, to catch a CCM formatting mistake.
    ct, tag = aes_ccm_encrypt(
        bytes.fromhex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf"),
        bytes.fromhex("00000003020100a0a1a2a3a4a5"),
        bytes(range(8)), bytes(range(8, 31)), 8)
    if (ct + tag).hex() != ("588c979a61c663d2f066d0c2c0f989806d5f6b61dac384"
                            "17e8d12cfdf926e0"):
        raise SystemExit("smbserver: AES-CCM is wrong")


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
        # SMB 3.0. All unset on a 2.x connection, and every branch that
        # reaches for one of them is guarded by the dialect.
        self.dialect = 0
        self.encrypt = False
        self.encrypt_next = False
        self.encrypted_in = False
        self.sign_key = None
        self.enc_key = None          # server -> client
        self.dec_key = None          # client -> server
        self.nonce_seen = set()

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
                    self.encrypted_in = msg[:4] == b"\xfdSMB"
                    if self.encrypted_in:
                        msg = self.unwrap_transform(msg)
                    elif self.encrypt:
                        raise Bad("cleartext message on an encrypted session")
                    reply = self.dispatch(msg)
                except Bad as e:
                    self.log("  rejected: %s" % e)
                    return
                if reply is None:
                    return
                if self.encrypt:
                    reply = self.wrap_transform(reply)
                self.request.sendall(len(reply).to_bytes(4, "big") + reply)
                if self.encrypt_next:
                    self.encrypt = True
                    self.encrypt_next = False

    # ---- SMB 3.0 transform frames ----

    def unwrap_transform(self, msg):
        if not self.dec_key:
            raise Bad("a transform frame arrived before any key was derived")
        if len(msg) <= 52:
            raise Bad("transform frame is too short")
        tag = msg[4:20]
        nonce16 = msg[20:36]
        plain_len = struct.unpack_from("<I", msg, 36)[0]
        sid = struct.unpack_from("<Q", msg, 44)[0]
        if sid != self.session_id:
            raise Bad("transform frame is for another session")
        if plain_len != len(msg) - 52:
            raise Bad("transform OriginalMessageSize disagrees with the frame")

        # A repeated nonce under one key is a keystream reuse, and a
        # client that gets its counter wrong is exactly what this server
        # exists to catch.
        if nonce16 in self.nonce_seen:
            raise Bad("transform nonce repeated")
        self.nonce_seen.add(nonce16)
        if nonce16[11:] != b"\x00" * 5:
            raise Bad("CCM nonce must be 11 bytes, the rest zero")

        pt = aes_ccm_decrypt(self.dec_key, nonce16[:11], msg[20:52],
                             msg[52:], tag)
        if pt is None:
            raise Bad("transform frame does not authenticate")
        self.log("  [encrypted %d bytes in, tag verified]" % len(pt))
        return pt

    def wrap_transform(self, msg):
        nonce = os.urandom(11)
        h = bytearray(52)
        h[0:4] = b"\xfdSMB"
        h[20:31] = nonce
        struct.pack_into("<I", h, 36, len(msg))
        struct.pack_into("<H", h, 42, 0x0001)        # encrypted
        struct.pack_into("<Q", h, 44, self.session_id)
        ct, tag = aes_ccm_encrypt(self.enc_key, nonce, bytes(h[20:52]), msg)
        h[4:20] = tag
        return bytes(h) + ct

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
        elif self.encrypted_in:
            # An encrypted message carries no signature and must not set
            # the flag: the AEAD tag over the transform frame has
            # already authenticated it, and it covered the header
            # fields a signature does not reach. Demanding a signature
            # here as well would reject every correct 3.0 client.
            pass
        elif self.session_key and cmd not in (NEGOTIATE, SESSION_SETUP):
            raise Bad("command 0x%02x arrived unsigned on a signed session" % cmd)
        if self.encrypted_in and (flags & 0x08):
            raise Bad("an encrypted message must not also claim to be signed")

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

    def mac(self, zeroed):
        """The MAC a message of this dialect carries. 3.0 moved from
        HMAC-SHA256 under the session key to AES-CMAC under a derived
        one, and a client that misses that signs perfectly valid 2.1
        signatures onto a 3.0 session."""
        if self.dialect == 0x0300:
            return aes_cmac(self.sign_key, zeroed)
        return hmac.new(self.session_key, zeroed, hashlib.sha256).digest()[:16]

    def check_signature(self, msg):
        if not self.session_key:
            raise Bad("a message is signed before a session key exists")
        got = msg[48:64]
        zeroed = msg[:48] + b"\x00" * 16 + msg[64:]
        want = self.mac(zeroed)
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
        # An encrypted message carries no signature: the AEAD tag has
        # already authenticated it, over more of the message than the
        # signature covers.
        if self.session_key and self.signing and not self.encrypt:
            f = struct.unpack_from("<I", msg, 16)[0] | 0x08
            msg = msg[:16] + struct.pack("<I", f) + msg[20:]
            mac = self.mac(msg[:48] + b"\x00" * 16 + msg[64:])
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
        pick = 0x0300 if (SMB3 and 0x0300 in dialects) else (
               0x0210 if 0x0210 in dialects else (
               0x0202 if 0x0202 in dialects else None))
        if pick is None:
            raise Bad("no dialect in common")
        self.dialect = pick
        caps = 0x00000040 if pick == 0x0300 else 0     # ENCRYPTION
        self.log("     -> %s, signing enabled%s"
                 % (hex(pick), ", encryption offered" if caps else ""))

        out = bytearray(64)
        struct.pack_into("<H", out, 0, 65)
        struct.pack_into("<H", out, 2, 0x0001)          # signing enabled
        struct.pack_into("<H", out, 4, pick)
        out[8:24] = os.urandom(16)
        struct.pack_into("<I", out, 24, caps)
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
            # On 3.0 the three session keys exist from this moment, and
            # the AUTHENTICATE message below is already signed with the
            # derived signing key rather than with the session key.
            if self.dialect == 0x0300:
                self.sign_key = smb3_kdf(key, b"SMB2AESCMAC", b"SmbSign")
                self.dec_key = smb3_kdf(key, b"SMB2AESCCM", b"ServerIn ")
                self.enc_key = smb3_kdf(key, b"SMB2AESCCM", b"ServerOut")
                self.log("     3.0 keys derived (sign/ServerIn/ServerOut)")

            # The message that carried the proof is itself signed, and
            # verifying it is what makes a relayed blob useless.
            self.check_signature(self.cur_msg)
            self.log("     the AUTHENTICATE message's own signature verifies")

            # SessionFlags = ENCRYPT_DATA tells the client this session
            # is encrypted from here on. The reply carrying it is still
            # signed rather than encrypted -- it is the message that
            # establishes the fact, so it cannot depend on it.
            sflags = 0x0004 if self.dialect == 0x0300 else 0
            body = struct.pack("<HHHH", 9, sflags, 0, 0)
            if sflags:
                # Armed, not set: this very reply must still go out
                # signed and in clear, because it is the message that
                # tells the client to start encrypting. handle() flips
                # it once the reply is on the wire.
                self.encrypt_next = True
                self.log("     session encrypted from the next message on")
            return ST_SUCCESS, body

        raise Bad("unexpected NTLMSSP message type %d" % mtype)

    def tree_connect(self, b):
        if struct.unpack_from("<H", b, 0)[0] != 9:
            raise Bad("TREE_CONNECT StructureSize is not 9")
        off = struct.unpack_from("<H", b, 4)[0] - 64
        ln = struct.unpack_from("<H", b, 6)[0]
        path = b[off:off + ln].decode("utf-16-le")
        self.log("  TREE_CONNECT %s" % path)
        if not any(path.lower().endswith("\\" + sh) for sh in SHARES):
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
            if name not in ("", ".") and not any(
                    f.startswith(name + "\\") for f in FILES):
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
        fid = self._fid(b, 8)
        base = self.handles[fid][0]
        if base in (".",):
            base = ""
        if not (flags & 0x01) and fid in getattr(self, "listed", set()):
            self.log("  QUERY_DIRECTORY -> no more files")
            return ST_NO_MORE_FILES, struct.pack("<HHI", 9, 0, 0)
        self.listed = getattr(self, "listed", set()) | {fid}

        # Immediate children only. A share is a tree, and a server that
        # returns every path under the handle makes a client's recursive
        # walk visit each file as many times as it has ancestors.
        prefix = (base + "\\") if base else ""
        names, dirs = [], set()
        for f in FILES:
            if not f.startswith(prefix):
                continue
            rest = f[len(prefix):]
            if "\\" in rest:
                dirs.add(rest.split("\\", 1)[0])
            else:
                names.append(rest)
        names = sorted(names)
        dirnames = sorted(dirs)

        buf = bytearray()
        rows = ([(d, 0, 0x10) for d in dirnames] +
                [(n, len(FILES[prefix + n]), 0x80) for n in names])
        for i, (name, size, attr) in enumerate(rows):
            wide = name.encode("utf-16-le")
            ent = bytearray(64)
            struct.pack_into("<I", ent, 4, i)
            for o in (8, 16, 24, 32):
                struct.pack_into("<Q", ent, o, filetime())
            struct.pack_into("<Q", ent, 40, size)
            struct.pack_into("<Q", ent, 48, size)
            struct.pack_into("<I", ent, 56, attr)
            struct.pack_into("<I", ent, 60, len(wide))
            ent += wide
            while len(ent) % 8:
                ent.append(0)
            if i != len(rows) - 1:
                struct.pack_into("<I", ent, 0, len(ent))
            buf += ent
        self.log("  QUERY_DIRECTORY %r -> %d entries" % (base, len(rows)))

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
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    # SMB 3.0 is offered unless told not to. `--no-smb3` makes this a
    # 2.1-only server again, which is how the client's fallback is
    # tested rather than assumed: a client that has learned to encrypt
    # must still talk to everything it could talk to before.
    SMB3 = "--no-smb3" not in sys.argv
    port = int(args[0]) if args else 4445
    _aes_selftest()
    print("smb2 server on port %d, shares %s%s"
          % (port, ", ".join(SHARES),
             ", SMB 3.0 encryption offered" if SMB3 else ", SMB 2.1 only"),
          flush=True)
    print("  account %s\\%s, %d files" % (DOMAIN, USER, len(FILES)), flush=True)
    Server(("0.0.0.0", port), Handler).serve_forever()
