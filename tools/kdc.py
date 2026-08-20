#!/usr/bin/env python3
"""
tools/kdc.py — a Kerberos key distribution centre, for testing the client.

src/kerberos.h writes DER by hand and implements RFC 3961's encryption
profile by hand. Both have the same failure mode: a message that is
perfectly self-consistent, decrypts under its own code, and that no real
KDC will look at twice. Testing either against something this repository
also wrote for the purpose would prove only that the two agree.

So this is written from RFC 4120, RFC 3961 and RFC 3962 rather than from
src/kerberos.h, and deliberately differs from it wherever there was a
choice:

  * the AES S-box here is built from exp/log tables over GF(2^8); the C
    finds each multiplicative inverse by search. Same table, arrived at
    two ways.
  * n-fold here materialises the replicated bit string and then folds
    it; the C computes each output bit's source on demand. Same
    function, opposite strategy.
  * the DER decoder is strict about things the client is not required to
    care about -- non-minimal lengths, the indefinite form, a context
    tag where a primitive belongs -- because those are exactly the
    mistakes a hand-written encoder makes, and a lenient KDC would let
    every one of them through.

What it checks, and this is the part that matters: in the TGS exchange
it verifies the client's Authenticator checksum over the *bytes it
received* for the request body. A client that re-encodes the body to
compute that checksum, instead of checksumming what it actually sent,
passes every test it can run on itself and fails here.

    python3 tools/kdc.py [port]

Realm VEXTRO.TEST, listening on TCP. One user and two services:

    ada@VEXTRO.TEST            password "hunter2"
    krbtgt/VEXTRO.TEST         the ticket-granting service
    cifs/files.vextro.test     something to ask for a ticket to

Kerberos is TCP port 88, which needs root. The default here is 8088 and
the client is told where to look, so nothing has to run privileged.
"""

import datetime
import hashlib
import hmac
import math
import os
import socketserver
import sys

REALM = "VEXTRO.TEST"
USERS = {"ada": "hunter2"}
SERVICES = {
    ("krbtgt", REALM): "tgt-service-key-passphrase",
    ("cifs", "files.vextro.test"): "cifs-service-key-passphrase",
}
SKEW = 300          # seconds; RFC 4120's usual tolerance


# ======================================================================
# AES, from FIPS-197
# ======================================================================

def _xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else (a << 1) & 0xFF


def _build():
    exp = [0] * 512
    log = [0] * 256
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        x ^= _xtime(x)                  # multiply by the generator 3
    for i in range(255, 512):
        exp[i] = exp[i - 255]

    sbox = [0] * 256
    for i in range(256):
        b = 0 if i == 0 else exp[255 - log[i]]
        s = b
        for _ in range(4):
            b = ((b << 1) | (b >> 7)) & 0xFF
            s ^= b
        sbox[i] = s ^ 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    return sbox, inv


SBOX, INV_SBOX = _build()
RCON = [0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36]


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = _xtime(a)
        b >>= 1
    return r


class AES:
    def __init__(self, key):
        nk = len(key) // 4
        self.nr = nk + 6
        w = [list(key[4 * i:4 * i + 4]) for i in range(nk)]
        for i in range(nk, 4 * (self.nr + 1)):
            t = list(w[i - 1])
            if i % nk == 0:
                t = t[1:] + t[:1]
                t = [SBOX[b] for b in t]
                t[0] ^= RCON[i // nk]
            elif nk > 6 and i % nk == 4:
                t = [SBOX[b] for b in t]
            w.append([w[i - nk][j] ^ t[j] for j in range(4)])
        self.w = w

    def _addkey(self, s, rnd):
        for c in range(4):
            for r in range(4):
                s[4 * c + r] ^= self.w[4 * rnd + c][r]

    def encrypt(self, block):
        s = list(block)
        self._addkey(s, 0)
        for rnd in range(1, self.nr + 1):
            t = [SBOX[s[4 * ((c + r) % 4) + r]] for c in range(4) for r in range(4)]
            if rnd != self.nr:
                s = []
                for c in range(4):
                    a = t[4 * c:4 * c + 4]
                    s += [_mul(a[0], 2) ^ _mul(a[1], 3) ^ a[2] ^ a[3],
                          a[0] ^ _mul(a[1], 2) ^ _mul(a[2], 3) ^ a[3],
                          a[0] ^ a[1] ^ _mul(a[2], 2) ^ _mul(a[3], 3),
                          _mul(a[0], 3) ^ a[1] ^ a[2] ^ _mul(a[3], 2)]
            else:
                s = t
            self._addkey(s, rnd)
        return bytes(s)

    def decrypt(self, block):
        s = list(block)
        self._addkey(s, self.nr)
        for rnd in range(self.nr - 1, -1, -1):
            t = [INV_SBOX[s[4 * ((c - r) % 4) + r]] for c in range(4) for r in range(4)]
            s = t
            self._addkey(s, rnd)
            if rnd != 0:
                out = []
                for c in range(4):
                    a = s[4 * c:4 * c + 4]
                    out += [_mul(a[0], 14) ^ _mul(a[1], 11) ^ _mul(a[2], 13) ^ _mul(a[3], 9),
                            _mul(a[0], 9) ^ _mul(a[1], 14) ^ _mul(a[2], 11) ^ _mul(a[3], 13),
                            _mul(a[0], 13) ^ _mul(a[1], 9) ^ _mul(a[2], 14) ^ _mul(a[3], 11),
                            _mul(a[0], 11) ^ _mul(a[1], 13) ^ _mul(a[2], 9) ^ _mul(a[3], 14)]
                s = out
        return bytes(s)


def _xor(a, b):
    return bytes(x ^ y for x, y in zip(a, b))


def cbc_cts_encrypt(key, iv, data):
    """RFC 3962: CBC with ciphertext stealing, last two blocks swapped."""
    aes = AES(key)
    if len(data) < 16:
        raise Bad("CTS needs at least one block")
    if len(data) == 16:
        return aes.encrypt(_xor(data, iv))

    nb = (len(data) + 15) // 16
    d = len(data) - (nb - 1) * 16
    out = bytearray()
    prev = iv
    for i in range(nb - 2):
        prev = aes.encrypt(_xor(data[i * 16:i * 16 + 16], prev))
        out += prev
    head = (nb - 2) * 16
    cprev = aes.encrypt(_xor(data[head:head + 16], prev))
    tail = data[head + 16:].ljust(16, b"\x00")
    clast = aes.encrypt(_xor(tail, cprev))
    return bytes(out + clast + cprev[:d])


def cbc_cts_decrypt(key, iv, data):
    aes = AES(key)
    if len(data) < 16:
        raise Bad("CTS needs at least one block")
    if len(data) == 16:
        return _xor(aes.decrypt(data), iv)

    nb = (len(data) + 15) // 16
    d = len(data) - (nb - 1) * 16
    out = bytearray()
    prev = iv
    for i in range(nb - 2):
        blk = data[i * 16:i * 16 + 16]
        out += _xor(aes.decrypt(blk), prev)
        prev = blk
    head = (nb - 2) * 16
    clast = data[head:head + 16]
    z = aes.decrypt(clast)
    cprev = data[head + 16:] + z[d:]
    out += _xor(aes.decrypt(cprev), prev)
    out += _xor(z[:d], cprev[:d])
    return bytes(out)


# ======================================================================
# RFC 3961: n-fold, DK, string-to-key, the simplified profile
# ======================================================================

def ones_add(a, b):
    n = len(a)
    out = bytearray(n)
    carry = 0
    for i in range(n - 1, -1, -1):
        s = a[i] + b[i] + carry
        out[i] = s & 0xFF
        carry = s >> 8
    while carry:
        for i in range(n - 1, -1, -1):
            s = out[i] + carry
            out[i] = s & 0xFF
            carry = s >> 8
            if not carry:
                break
    return bytes(out)


def nfold(data, nbits):
    inbits = len(data) * 8
    lcm = inbits * nbits // math.gcd(inbits, nbits)

    bits = []
    for r in range(lcm // inbits):
        rot = (13 * r) % inbits
        for j in range(inbits):
            src = (j - rot) % inbits
            bits.append((data[src // 8] >> (7 - src % 8)) & 1)

    nbytes = nbits // 8
    acc = bytes(nbytes)
    for c in range(lcm // nbits):
        chunk = bytearray(nbytes)
        for j in range(nbits):
            if bits[c * nbits + j]:
                chunk[j // 8] |= 1 << (7 - j % 8)
        acc = ones_add(acc, chunk)
    return acc


def dk(key, constant, outlen):
    aes = AES(key)
    block = nfold(constant, 128)
    out = b""
    while len(out) < outlen:
        block = aes.encrypt(block)
        out += block
    return out[:outlen]


def derive(key, usage, kind):
    c = usage.to_bytes(4, "big") + bytes([kind])
    return dk(key, c, len(key))


def string_to_key(password, salt, keylen, iterations=4096):
    tkey = hashlib.pbkdf2_hmac("sha1", password.encode(), salt, iterations, keylen)
    return dk(tkey, b"kerberos", keylen)


def krb_encrypt(key, usage, plain):
    ke = derive(key, usage, 0xAA)
    ki = derive(key, usage, 0x55)
    body = os.urandom(16) + plain
    ct = cbc_cts_encrypt(ke, bytes(16), body)
    mac = hmac.new(ki, body, hashlib.sha1).digest()[:12]
    return ct + mac


def krb_decrypt(key, usage, cipher):
    if len(cipher) < 28:
        raise Bad("ciphertext too short")
    ke = derive(key, usage, 0xAA)
    ki = derive(key, usage, 0x55)
    body = cbc_cts_decrypt(ke, bytes(16), cipher[:-12])
    want = hmac.new(ki, body, hashlib.sha1).digest()[:12]
    if not hmac.compare_digest(want, cipher[-12:]):
        raise Bad("integrity check failed -- wrong key or damaged message")
    return body[16:]


def krb_checksum(key, usage, msg):
    kc = derive(key, usage, 0x99)
    return hmac.new(kc, msg, hashlib.sha1).digest()[:12]


# ======================================================================
# DER, decoded strictly
# ======================================================================

class Bad(Exception):
    pass


class TLV:
    __slots__ = ("tag", "body", "raw")

    def __init__(self, tag, body, raw):
        self.tag, self.body, self.raw = tag, body, raw

    def __repr__(self):
        return f"TLV(0x{self.tag:02x}, {len(self.body)} bytes)"


def der_decode(buf, off=0):
    """One TLV at `off`. Returns (TLV, next_offset)."""
    if off + 2 > len(buf):
        raise Bad("truncated")
    tag = buf[off]
    if tag & 0x1F == 0x1F:
        raise Bad("multi-byte tags are not used by Kerberos")
    n = buf[off + 1]
    hdr = 2
    if n == 0x80:
        raise Bad("indefinite length is not DER")
    if n & 0x80:
        k = n & 0x7F
        if k == 0 or k > 4:
            raise Bad("bad length form")
        if off + 2 + k > len(buf):
            raise Bad("truncated length")
        n = int.from_bytes(buf[off + 2:off + 2 + k], "big")
        if n < 0x80:
            raise Bad("non-minimal length: %d encoded in %d bytes" % (n, k))
        hdr = 2 + k
    end = off + hdr + n
    if end > len(buf):
        raise Bad("length runs past the end of the message")
    return TLV(tag, buf[off + hdr:end], buf[off:end]), end


def der_children(tlv):
    out, i = [], 0
    while i < len(tlv.body):
        child, i = der_decode(tlv.body, i)
        out.append(child)
    return out


def field(children, ctx):
    """The [ctx] member of a SEQUENCE, or None."""
    for c in children:
        if c.tag == 0xA0 | ctx:
            return c
    return None


def need(children, ctx, what):
    f = field(children, ctx)
    if f is None:
        raise Bad("missing [%d] in %s" % (ctx, what))
    return f


def der_int(tlv):
    if tlv.tag != 0x02:
        raise Bad("expected INTEGER, got tag 0x%02x" % tlv.tag)
    b = tlv.body
    if not b:
        raise Bad("empty INTEGER")
    if len(b) > 1 and b[0] == 0 and not (b[1] & 0x80):
        raise Bad("non-minimal INTEGER")
    if len(b) > 1 and b[0] == 0xFF and (b[1] & 0x80):
        raise Bad("non-minimal negative INTEGER")
    return int.from_bytes(b, "big", signed=True)


def der_str(tlv):
    if tlv.tag not in (0x1B, 0x16, 0x0C, 0x13):
        raise Bad("expected a string, got tag 0x%02x" % tlv.tag)
    return tlv.body.decode()


def der_octets(tlv):
    if tlv.tag != 0x04:
        raise Bad("expected OCTET STRING, got tag 0x%02x" % tlv.tag)
    return tlv.body


def der_time(tlv):
    if tlv.tag != 0x18:
        raise Bad("expected GeneralizedTime, got tag 0x%02x" % tlv.tag)
    s = tlv.body.decode()
    if len(s) != 15 or s[-1] != "Z":
        raise Bad("KerberosTime must be YYYYMMDDHHMMSSZ, got %r" % s)
    return datetime.datetime.strptime(s, "%Y%m%d%H%M%SZ").replace(
        tzinfo=datetime.timezone.utc)


def der_bitstring(tlv):
    if tlv.tag != 0x03:
        raise Bad("expected BIT STRING, got tag 0x%02x" % tlv.tag)
    if len(tlv.body) != 5:
        raise Bad("expected a 32-bit BIT STRING, got %d bytes" % len(tlv.body))
    if tlv.body[0] != 0:
        raise Bad("KDCOptions must have no unused bits, got %d" % tlv.body[0])
    return int.from_bytes(tlv.body[1:], "big")


def inner(tlv):
    """The single element inside an explicit context wrapper."""
    kids = der_children(tlv)
    if len(kids) != 1:
        raise Bad("explicit tag should wrap exactly one element")
    return kids[0]


# ---------- encoding ----------

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


def e_int(v):
    if v == 0:
        return tlv(0x02, b"\x00")
    n = (v.bit_length() + 8) // 8
    return tlv(0x02, v.to_bytes(n, "big", signed=True))


def e_str(s):
    return tlv(0x1B, s.encode())


def e_octets(b):
    return tlv(0x04, b)


def e_time(dt):
    return tlv(0x18, dt.strftime("%Y%m%d%H%M%SZ").encode())


def e_bits(v):
    return tlv(0x03, b"\x00" + v.to_bytes(4, "big"))


def ctx(n, body):
    return tlv(0xA0 | n, body)


def app(n, body):
    return tlv(0x60 | n, body)


def e_principal(ntype, names):
    return tlv(0x30, ctx(0, e_int(ntype)) +
                     ctx(1, tlv(0x30, b"".join(e_str(n) for n in names))))


def e_encdata(etype, cipher):
    return tlv(0x30, ctx(0, e_int(etype)) + ctx(2, e_octets(cipher)))


def e_key(etype, key):
    return tlv(0x30, ctx(0, e_int(etype)) + ctx(1, e_octets(key)))


# ======================================================================
# the realm
# ======================================================================

ETYPE_AES256, ETYPE_AES128 = 18, 17
KEYLEN = {ETYPE_AES256: 32, ETYPE_AES128: 16}


def princ_salt(names):
    return (REALM + "".join(names)).encode()


def user_key(user, etype):
    return string_to_key(USERS[user], princ_salt([user]), KEYLEN[etype])


def service_key(names, etype):
    """A service key is not derived from a principal salt in a real realm
    -- it is a random key in the database. Deriving it from a passphrase
    here keeps this file stateless; the client never sees it either
    way."""
    return string_to_key(SERVICES[tuple(names)],
                         princ_salt(list(names)), KEYLEN[etype])


def now():
    return datetime.datetime.now(datetime.timezone.utc)


def far():
    return now() + datetime.timedelta(hours=10)


def e_error(code, text, edata=None):
    body = (ctx(0, e_int(5)) + ctx(1, e_int(30)) +
            ctx(4, e_time(now())) + ctx(5, e_int(0)) +
            ctx(6, e_int(code)) +
            ctx(9, e_str(REALM)) +
            ctx(10, e_principal(2, ["krbtgt", REALM])))
    if text:
        body += ctx(11, e_str(text))
    if edata:
        body += ctx(12, e_octets(edata))
    return app(30, tlv(0x30, body))


def etype_info2(user, etype):
    entry = tlv(0x30, ctx(0, e_int(etype)) +
                      ctx(1, e_str(princ_salt([user]).decode())))
    padata = tlv(0x30, ctx(1, e_int(19)) + ctx(2, e_octets(tlv(0x30, entry))))
    return tlv(0x30, padata)


def parse_principal(t):
    kids = der_children(inner(t) if t.tag & 0xC0 == 0x80 else t)
    ntype = der_int(inner(need(kids, 0, "PrincipalName")))
    seq = inner(need(kids, 1, "PrincipalName"))
    if seq.tag != 0x30:
        raise Bad("name-string must be a SEQUENCE")
    return ntype, [der_str(c) for c in der_children(seq)]


def parse_encdata(t):
    kids = der_children(inner(t))
    etype = der_int(inner(need(kids, 0, "EncryptedData")))
    cipher = der_octets(inner(need(kids, 2, "EncryptedData")))
    return etype, cipher


def parse_req_body(bodytlv):
    """Returns a dict, and keeps the raw bytes -- the TGS checksum is
    over exactly these and nothing that merely re-encodes to them."""
    seq = inner(bodytlv)
    if seq.tag != 0x30:
        raise Bad("KDC-REQ-BODY must be a SEQUENCE")
    kids = der_children(seq)

    out = {"raw": bodytlv.raw}
    out["options"] = der_bitstring(inner(need(kids, 0, "req-body")))
    cn = field(kids, 1)
    out["cname"] = parse_principal(cn)[1] if cn else None
    out["realm"] = der_str(inner(need(kids, 2, "req-body")))
    out["sname"] = parse_principal(need(kids, 3, "req-body"))[1]
    out["till"] = der_time(inner(need(kids, 5, "req-body")))
    out["nonce"] = der_int(inner(need(kids, 7, "req-body")))
    es = inner(need(kids, 8, "req-body"))
    if es.tag != 0x30:
        raise Bad("etype must be a SEQUENCE OF")
    out["etypes"] = [der_int(c) for c in der_children(es)]
    return out


def pick_etype(offered):
    for e in (ETYPE_AES256, ETYPE_AES128):
        if e in offered:
            return e
    return None


def make_ticket(sname, skey, setype, session, setype_session,
                crealm, cname, authtime, endtime):
    encpart = app(3, tlv(0x30,
        ctx(0, e_bits(0x40000000)) +                  # forwardable
        ctx(1, e_key(setype_session, session)) +
        ctx(2, e_str(crealm)) +
        ctx(3, e_principal(1, cname)) +
        ctx(4, tlv(0x30, ctx(0, e_int(0)) + ctx(1, e_octets(b"")))) +
        ctx(5, e_time(authtime)) +
        ctx(7, e_time(endtime))))
    sealed = krb_encrypt(skey, 2, encpart)
    return app(1, tlv(0x30,
        ctx(0, e_int(5)) +
        ctx(1, e_str(REALM)) +
        ctx(2, e_principal(2, sname)) +
        ctx(3, e_encdata(setype, sealed))))


def parse_ticket(t):
    if t.tag != 0x61:
        raise Bad("expected a Ticket, got tag 0x%02x" % t.tag)
    kids = der_children(inner_seq(t))
    sname = parse_principal(need(kids, 2, "Ticket"))[1]
    etype, cipher = parse_encdata(need(kids, 3, "Ticket"))
    return sname, etype, cipher


def inner_seq(t):
    kids = der_children(t)
    if len(kids) != 1 or kids[0].tag != 0x30:
        raise Bad("expected a SEQUENCE inside the application tag")
    return kids[0]


# ======================================================================
# the exchanges
# ======================================================================

def handle_as_req(msg, log):
    kids = der_children(inner_seq(msg))
    if der_int(inner(need(kids, 1, "AS-REQ"))) != 5:
        raise Bad("not protocol version 5")
    if der_int(inner(need(kids, 2, "AS-REQ"))) != 10:
        raise Bad("msg-type is not AS-REQ")

    body = parse_req_body(need(kids, 4, "AS-REQ"))
    if body["cname"] is None:
        raise Bad("an AS-REQ must name its client")
    user = body["cname"][0]
    log("  AS-REQ for %s@%s -> %s" % (user, body["realm"], "/".join(body["sname"])))
    log("     etypes offered: %s" % body["etypes"])

    if body["realm"] != REALM:
        return e_error(6, "no such realm here")
    if user not in USERS:
        return e_error(6, "client principal unknown")

    etype = pick_etype(body["etypes"])
    if etype is None:
        return e_error(14, "this KDC has only AES")
    if 23 in body["etypes"]:
        log("     NOTE: the client offered rc4-hmac")

    ckey = user_key(user, etype)

    pa = field(kids, 3)
    ts = None
    if pa is not None:
        for p in der_children(inner(pa)):
            pk = der_children(p)
            ptype = der_int(inner(need(pk, 1, "PA-DATA")))
            pval = der_octets(inner(need(pk, 2, "PA-DATA")))
            if ptype == 2:
                enc, _ = der_decode(pval)
                pe, pc = parse_encdata_seq(enc)
                if pe != etype:
                    return e_error(14, "pre-auth used a different etype")
                try:
                    plain = krb_decrypt(ckey, 1, pc)
                except Bad as e:
                    log("     pre-auth rejected: %s" % e)
                    return e_error(24, "pre-authentication failed")
                tsq, _ = der_decode(plain)
                ts = der_time(inner(need(der_children(tsq), 0, "PA-ENC-TS-ENC")))

    if ts is None:
        log("     no pre-authentication; asking for it")
        return e_error(25, "additional pre-authentication required",
                       etype_info2(user, etype))

    drift = abs((now() - ts).total_seconds())
    if drift > SKEW:
        log("     timestamp is %.0f s away from ours" % drift)
        return e_error(37, "clock skew too great")
    log("     pre-authentication accepted, clock within %.0f s" % drift)

    session = os.urandom(KEYLEN[etype])
    authtime, endtime = now(), far()
    skey = service_key(tuple(body["sname"]), etype)
    ticket = make_ticket(body["sname"], skey, etype, session, etype,
                         REALM, [user], authtime, endtime)

    encpart = app(25, tlv(0x30,
        ctx(0, e_key(etype, session)) +
        ctx(1, tlv(0x30, b"")) +
        ctx(2, e_int(body["nonce"])) +
        ctx(4, e_bits(0x40000000)) +
        ctx(5, e_time(authtime)) +
        ctx(7, e_time(endtime)) +
        ctx(9, e_str(REALM)) +
        ctx(10, e_principal(2, body["sname"]))))

    log("     issuing a TGT, session key %d bytes, until %s"
        % (len(session), endtime.strftime("%Y%m%d%H%M%SZ")))
    return app(11, tlv(0x30,
        ctx(0, e_int(5)) + ctx(1, e_int(11)) +
        ctx(3, e_str(REALM)) +
        ctx(4, e_principal(1, [user])) +
        ctx(5, ticket) +
        ctx(6, e_encdata(etype, krb_encrypt(ckey, 3, encpart)))))


def parse_encdata_seq(t):
    """EncryptedData that is not wrapped in a context tag."""
    if t.tag != 0x30:
        raise Bad("EncryptedData must be a SEQUENCE")
    kids = der_children(t)
    return (der_int(inner(need(kids, 0, "EncryptedData"))),
            der_octets(inner(need(kids, 2, "EncryptedData"))))


def handle_tgs_req(msg, log):
    kids = der_children(inner_seq(msg))
    if der_int(inner(need(kids, 2, "TGS-REQ"))) != 12:
        raise Bad("msg-type is not TGS-REQ")

    body = parse_req_body(need(kids, 4, "TGS-REQ"))
    log("  TGS-REQ for %s" % "/".join(body["sname"]))

    pa = need(kids, 3, "TGS-REQ")
    apreq = None
    for p in der_children(inner(pa)):
        pk = der_children(p)
        if der_int(inner(need(pk, 1, "PA-DATA"))) == 1:
            apreq = der_octets(inner(need(pk, 2, "PA-DATA")))
    if apreq is None:
        raise Bad("a TGS-REQ must carry a PA-TGS-REQ")

    ap, _ = der_decode(apreq)
    if ap.tag != 0x6E:
        raise Bad("PA-TGS-REQ must contain an AP-REQ")
    apk = der_children(inner_seq(ap))
    tkt = inner(need(apk, 3, "AP-REQ"))
    sname, tetype, tcipher = parse_ticket(tkt)
    if tuple(sname) != ("krbtgt", REALM):
        raise Bad("that ticket is not for the ticket-granting service")

    tgtkey = service_key(("krbtgt", REALM), tetype)
    encticket = krb_decrypt(tgtkey, 2, tcipher)
    et, _ = der_decode(encticket)
    etk = der_children(inner_seq(et))
    keyk = der_children(inner(need(etk, 1, "EncTicketPart")))
    setype = der_int(inner(need(keyk, 0, "EncryptionKey")))
    session = der_octets(inner(need(keyk, 1, "EncryptionKey")))
    crealm = der_str(inner(need(etk, 2, "EncTicketPart")))
    cname = parse_principal(need(etk, 3, "EncTicketPart"))[1]
    log("     the ticket opens: %s@%s" % ("/".join(cname), crealm))

    aetype, acipher = parse_encdata(need(apk, 4, "AP-REQ"))
    if aetype != setype:
        raise Bad("the authenticator's etype does not match the session key")
    auth = krb_decrypt(session, 7, acipher)
    au, _ = der_decode(auth)
    if au.tag != 0x62:
        raise Bad("expected an Authenticator")
    auk = der_children(inner_seq(au))

    actime = der_time(inner(need(auk, 5, "Authenticator")))
    drift = abs((now() - actime).total_seconds())
    if drift > SKEW:
        return e_error(37, "clock skew too great")

    # The check this whole file exists for.
    ck = field(auk, 3)
    if ck is None:
        raise Bad("a TGS Authenticator must carry a checksum")
    ckk = der_children(inner(ck))
    cktype = der_int(inner(need(ckk, 0, "Checksum")))
    ckval = der_octets(inner(need(ckk, 1, "Checksum")))
    if cktype not in (15, 16):
        raise Bad("checksum type %d is not one of the AES ones" % cktype)
    want = krb_checksum(session, 6, body["raw"])
    if not hmac.compare_digest(want, ckval):
        log("     CHECKSUM MISMATCH over the request body")
        log("       we compute %s" % want.hex())
        log("       client sent %s" % ckval.hex())
        return e_error(31, "the authenticator checksum does not cover "
                           "the request body that was sent")
    log("     authenticator checksum over the request body verifies")

    if tuple(body["sname"]) not in SERVICES:
        return e_error(7, "server principal unknown")

    etype = pick_etype(body["etypes"]) or setype
    newsession = os.urandom(KEYLEN[etype])
    authtime, endtime = now(), far()
    skey = service_key(tuple(body["sname"]), etype)
    ticket = make_ticket(body["sname"], skey, etype, newsession, etype,
                         crealm, cname, authtime, endtime)

    encpart = app(26, tlv(0x30,
        ctx(0, e_key(etype, newsession)) +
        ctx(1, tlv(0x30, b"")) +
        ctx(2, e_int(body["nonce"])) +
        ctx(4, e_bits(0x40000000)) +
        ctx(5, e_time(authtime)) +
        ctx(7, e_time(endtime)) +
        ctx(9, e_str(REALM)) +
        ctx(10, e_principal(2, body["sname"]))))

    log("     issuing a service ticket for %s" % "/".join(body["sname"]))
    return app(13, tlv(0x30,
        ctx(0, e_int(5)) + ctx(1, e_int(13)) +
        ctx(3, e_str(crealm)) +
        ctx(4, e_principal(1, cname)) +
        ctx(5, ticket) +
        ctx(6, e_encdata(setype, krb_encrypt(session, 8, encpart)))))


class Handler(socketserver.BaseRequestHandler):
    def log(self, s):
        print(s, flush=True)

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
                n = int.from_bytes(buf[:4], "big")
                if n > 1 << 20:
                    return
                if len(buf) < 4 + n:
                    break
                msg, buf = buf[4:4 + n], buf[4 + n:]
                try:
                    reply = self.dispatch(msg)
                except Bad as e:
                    self.log("  rejected: %s" % e)
                    reply = e_error(60, "malformed request: %s" % e)
                except Exception as e:                # noqa: BLE001
                    self.log("  internal error: %r" % e)
                    reply = e_error(60, "internal error")
                self.request.sendall(len(reply).to_bytes(4, "big") + reply)

    def dispatch(self, msg):
        m, end = der_decode(msg)
        if end != len(msg):
            raise Bad("trailing bytes after the message")
        if m.tag == 0x6A:
            return handle_as_req(m, self.log)
        if m.tag == 0x6C:
            return handle_tgs_req(m, self.log)
        raise Bad("unsupported message, application tag 0x%02x" % m.tag)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8088
    print("kdc for realm %s on port %d" % (REALM, port), flush=True)
    print("  user ada, services %s"
          % ", ".join("/".join(s) for s in SERVICES), flush=True)
    Server(("0.0.0.0", port), Handler).serve_forever()
