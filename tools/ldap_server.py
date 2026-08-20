#!/usr/bin/env python3
"""
tools/ldap_server.py — a directory server, for testing the client.

src/ldap.h writes BER by hand and parses BER by hand, and the failure
mode of both is a message that is perfectly self-consistent and that no
real server accepts. Testing it against something this repository also
wrote would prove only that the two agree with each other.

So this server is written from RFC 4511 rather than from src/ldap.h,
deliberately: it decodes the client's bytes independently and rejects
anything malformed, including the encodings that are *nearly* right --
a non-minimal length, a presence filter encoded as constructed, an
integer with a leading zero it does not need. Those are exactly the
mistakes a hand-written encoder makes, and a lenient server would let
every one of them through.

    python3 tools/ldap_server.py [port]

It holds four entries under dc=vextro,dc=test and accepts one bind:

    cn=admin,dc=vextro,dc=test  /  secret

Anonymous bind succeeds with no credentials. Anything else is rejected
with invalidCredentials, so the client's failure path is reachable too.
"""

import socket
import socketserver
import sys
import threading

BASE = "dc=vextro,dc=test"

ENTRIES = [
    (f"cn=admin,{BASE}", {
        "cn": ["admin"], "objectClass": ["person", "top"],
        "description": ["the account this test binds as"],
    }),
    (f"cn=ada,{BASE}", {
        "cn": ["ada"], "sn": ["Lovelace"], "uid": ["ada"],
        "objectClass": ["person", "top"],
        "mail": ["ada@vextro.test"],
    }),
    (f"cn=alan,{BASE}", {
        "cn": ["alan"], "sn": ["Turing"], "uid": ["alan"],
        "objectClass": ["person", "top"],
        "mail": ["alan@vextro.test"],
    }),
    (f"cn=machines,{BASE}", {
        "cn": ["machines"], "objectClass": ["group", "top"],
        "member": [f"cn=ada,{BASE}", f"cn=alan,{BASE}"],
    }),
]

BIND_DN, BIND_PW = f"cn=admin,{BASE}", "secret"


# ---------- decoding, strictly ----------

class Bad(Exception):
    pass


class Reader:
    def __init__(self, b):
        self.b, self.i = b, 0

    def byte(self):
        if self.i >= len(self.b):
            raise Bad("truncated")
        v = self.b[self.i]
        self.i += 1
        return v

    def tlv(self):
        tag = self.byte()
        n = self.byte()
        if n == 0x80:
            raise Bad("indefinite length is not accepted")
        if n & 0x80:
            k = n & 0x7F
            if k == 0 or k > 4:
                raise Bad("bad length form")
            n = 0
            for _ in range(k):
                n = (n << 8) | self.byte()
            # Minimal encoding: a length under 0x80 must use the short
            # form. A client that always emits the long form is one this
            # would otherwise silently accept.
            if n < 0x80:
                raise Bad("non-minimal length")
        if self.i + n > len(self.b):
            raise Bad("length past end of buffer")
        v = self.b[self.i:self.i + n]
        self.i += n
        return tag, v


def dec_int(b):
    if not b:
        raise Bad("empty integer")
    if len(b) > 1 and b[0] == 0 and not (b[1] & 0x80):
        raise Bad("non-minimal integer")
    v = int.from_bytes(b, "big", signed=True)
    return v


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


def enc_int(v, tag=0x02):
    if v == 0:
        return tlv(tag, b"\x00")
    n = (v.bit_length() + 8) // 8
    return tlv(tag, v.to_bytes(n, "big", signed=True))


def enc_str(s, tag=0x04):
    return tlv(tag, s.encode())


# ---------- the operations ----------

def parse_filter(tag, body):
    """Returns (attr, value) for equality, (attr, None) for presence."""
    if tag == 0xA3:                       # equality, constructed
        r = Reader(body)
        _, a = r.tlv()
        _, v = r.tlv()
        return a.decode(), v.decode()
    if tag == 0x87:                       # presence, primitive
        return body.decode(), None
    if tag == 0x80 | 0x07:
        return body.decode(), None
    raise Bad(f"unsupported filter tag {tag:#x}")


def matches(attrs, name, value):
    for k, vals in attrs.items():
        if k.lower() != name.lower():
            continue
        if value is None:
            return True
        return any(v.lower() == value.lower() for v in vals)
    return False


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        buf = b""
        while True:
            try:
                chunk = self.request.recv(4096)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk
            while True:
                msg, rest = self.take_one(buf)
                if msg is None:
                    break
                buf = rest
                try:
                    out = self.dispatch(msg)
                except Bad as e:
                    print(f"  rejected: {e}", flush=True)
                    return
                if out is None:
                    return                 # unbind
                self.request.sendall(out)

    @staticmethod
    def take_one(buf):
        if len(buf) < 2:
            return None, buf
        if buf[0] != 0x30:
            raise Bad("message is not a SEQUENCE")
        n = buf[1]
        hdr = 2
        if n & 0x80:
            k = n & 0x7F
            if len(buf) < 2 + k:
                return None, buf
            n = int.from_bytes(buf[2:2 + k], "big")
            hdr = 2 + k
        if len(buf) < hdr + n:
            return None, buf
        return buf[:hdr + n], buf[hdr + n:]

    def dispatch(self, msg):
        r = Reader(msg)
        tag, body = r.tlv()
        inner = Reader(body)
        _, idb = inner.tlv()
        msgid = dec_int(idb)
        op, opbody = inner.tlv()

        if op == 0x42:                     # unbind
            print("  unbind", flush=True)
            return None
        if op == 0x60:
            return self.bind(msgid, opbody)
        if op == 0x63:
            return self.search(msgid, opbody)
        raise Bad(f"unsupported operation {op:#x}")

    def bind(self, msgid, body):
        r = Reader(body)
        _, ver = r.tlv()
        if dec_int(ver) != 3:
            raise Bad("not LDAPv3")
        _, dn = r.tlv()
        tag, cred = r.tlv()
        if tag != 0x80:
            raise Bad("only simple bind is supported")
        dn, cred = dn.decode(), cred.decode()

        if (dn == "" and cred == "") or (dn == BIND_DN and cred == BIND_PW):
            code, diag = 0, ""
            print(f"  bind {dn or '(anonymous)'}: success", flush=True)
        else:
            code, diag = 49, "invalid credentials"
            print(f"  bind {dn or '(anonymous)'}: rejected", flush=True)

        resp = enc_int(code, 0x0A) + enc_str("") + enc_str(diag)
        return tlv(0x30, enc_int(msgid) + tlv(0x61, resp))

    def search(self, msgid, body):
        r = Reader(body)
        _, base = r.tlv()
        _, scope = r.tlv()
        _, _deref = r.tlv()
        _, sizelim = r.tlv()
        _, _timelim = r.tlv()
        _, _typesonly = r.tlv()
        ftag, fbody = r.tlv()
        _, attrsel = r.tlv()

        attr, value = parse_filter(ftag, fbody)
        limit = dec_int(sizelim) or 100
        base = base.decode()
        print(f"  search base={base} scope={dec_int(scope)} "
              f"filter=({attr}={value if value is not None else '*'})",
              flush=True)

        wanted = []
        ar = Reader(attrsel)
        while ar.i < len(attrsel):
            _, a = ar.tlv()
            wanted.append(a.decode())

        out = b""
        n = 0
        for dn, attrs in ENTRIES:
            if not dn.endswith(base):
                continue
            if not matches(attrs, attr, value):
                continue
            if n >= limit:
                break
            parts = b""
            for k, vals in attrs.items():
                if wanted and k not in wanted:
                    continue
                vs = b"".join(enc_str(v) for v in vals)
                parts += tlv(0x30, enc_str(k) + tlv(0x31, vs))
            entry = enc_str(dn) + tlv(0x30, parts)
            out += tlv(0x30, enc_int(msgid) + tlv(0x64, entry))
            n += 1

        print(f"    {n} entries", flush=True)
        done = enc_int(0, 0x0A) + enc_str("") + enc_str("")
        out += tlv(0x30, enc_int(msgid) + tlv(0x65, done))
        return out


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 3890
    print(f"ldap server on {port}, base {BASE}", flush=True)
    Server(("0.0.0.0", port), Handler).serve_forever()
