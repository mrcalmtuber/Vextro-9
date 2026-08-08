#!/usr/bin/env python3
"""Independent reference decoder for the GGUF block formats.

Written from the format definitions rather than from src/llm.c, so that
agreement between the two is evidence and not tautology. Compares against
build/dequant_test, which runs the kernel's own decoder.

    python3 tools/dequant_ref.py assets/qwen2.gguf blk.0.ffn_down.weight 0 256

Stdlib only, like every other tool here.
"""
import struct
import subprocess
import sys

# ---- GGUF container ----------------------------------------------------

BLOCK = {                       # type: (elements, bytes)
    0: (1, 4), 1: (1, 2),
    2: (32, 18), 3: (32, 20),
    6: (32, 22), 7: (32, 24), 8: (32, 34),
    12: (256, 144), 13: (256, 176), 14: (256, 210),
}
NAME = {0: 'F32', 1: 'F16', 2: 'Q4_0', 3: 'Q4_1', 6: 'Q5_0', 7: 'Q5_1',
        8: 'Q8_0', 12: 'Q4_K', 13: 'Q5_K', 14: 'Q6_K'}


class Reader:
    def __init__(self, data):
        self.d = data
        self.p = 0

    def take(self, n):
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u32(self): return struct.unpack('<I', self.take(4))[0]
    def u64(self): return struct.unpack('<Q', self.take(8))[0]

    def string(self):
        return self.take(self.u64()).decode('utf-8', 'replace')

    def value(self, t):
        simple = {0: 'B', 1: 'b', 2: 'H', 3: 'h', 4: 'I', 5: 'i',
                  6: 'f', 7: '?', 10: 'Q', 11: 'q', 12: 'd'}
        if t in simple:
            f = simple[t]
            return struct.unpack('<' + f, self.take(struct.calcsize(f)))[0]
        if t == 8:
            return self.string()
        if t == 9:
            et = self.u32()
            return [self.value(et) for _ in range(self.u64())]
        raise ValueError('metadata type %d' % t)


def parse(path):
    # Header only -- but "header" includes the whole vocabulary, and
    # 151936 tokens of it do not fit in a few megabytes.
    data = open(path, 'rb').read(64 << 20)
    r = Reader(data)
    assert r.take(4) == b'GGUF'
    r.u32()
    ntensor, nkv = r.u64(), r.u64()
    for _ in range(nkv):
        r.string()
        r.value(r.u32())
    tensors = {}
    for _ in range(ntensor):
        name = r.string()
        dims = [r.u64() for _ in range(r.u32())]
        tensors[name] = (r.u32(), r.u64(), dims)   # type, offset, dims
    data_start = (r.p + 31) & ~31
    return tensors, data_start


# ---- block decoders ----------------------------------------------------

def f16(b):
    return struct.unpack('<e', b)[0]


def q4_0(b):
    d = f16(b[0:2])
    return ([((b[2 + j] & 0xF) - 8) * d for j in range(16)] +
            [((b[2 + j] >> 4) - 8) * d for j in range(16)])


def q4_1(b):
    d, m = f16(b[0:2]), f16(b[2:4])
    return ([(b[4 + j] & 0xF) * d + m for j in range(16)] +
            [(b[4 + j] >> 4) * d + m for j in range(16)])


def q5_0(b):
    d = f16(b[0:2])
    qh = struct.unpack('<I', b[2:6])[0]
    lo = [(((b[6 + j] & 0xF) | (((qh >> j) & 1) << 4)) - 16) * d
          for j in range(16)]
    hi = [(((b[6 + j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) - 16) * d
          for j in range(16)]
    return lo + hi


def q5_1(b):
    d, m = f16(b[0:2]), f16(b[2:4])
    qh = struct.unpack('<I', b[4:8])[0]
    lo = [((b[8 + j] & 0xF) | (((qh >> j) & 1) << 4)) * d + m
          for j in range(16)]
    hi = [((b[8 + j] >> 4) | (((qh >> (j + 16)) & 1) << 4)) * d + m
          for j in range(16)]
    return lo + hi


def q8_0(b):
    d = f16(b[0:2])
    return [struct.unpack('<b', b[2 + j:3 + j])[0] * d for j in range(32)]


def k_scale_min(j, sc):
    """The 6-bit scale/min pairs packed into 12 bytes, as K-quants do."""
    if j < 4:
        return sc[j] & 63, sc[j + 4] & 63
    return ((sc[j + 4] & 0xF) | ((sc[j - 4] >> 6) << 4),
            (sc[j + 4] >> 4) | ((sc[j] >> 6) << 4))


def q4_k(b):
    d, dmin = f16(b[0:2]), f16(b[2:4])
    sc, q = b[4:16], b[16:144]
    out, is_ = [], 0
    for n in range(0, 256, 64):
        s1, m1 = k_scale_min(is_, sc)
        s2, m2 = k_scale_min(is_ + 1, sc)
        base = (n // 64) * 32
        out += [d * s1 * (q[base + l] & 0xF) - dmin * m1 for l in range(32)]
        out += [d * s2 * (q[base + l] >> 4) - dmin * m2 for l in range(32)]
        is_ += 2
    return out


def q5_k(b):
    d, dmin = f16(b[0:2]), f16(b[2:4])
    sc, qh, ql = b[4:16], b[16:48], b[48:176]
    out, is_, u1, u2 = [], 0, 1, 2
    for n in range(0, 256, 64):
        s1, m1 = k_scale_min(is_, sc)
        s2, m2 = k_scale_min(is_ + 1, sc)
        base = (n // 64) * 32
        out += [d * s1 * ((ql[base + l] & 0xF) + (16 if qh[l] & u1 else 0))
                - dmin * m1 for l in range(32)]
        out += [d * s2 * ((ql[base + l] >> 4) + (16 if qh[l] & u2 else 0))
                - dmin * m2 for l in range(32)]
        is_ += 2
        u1 = (u1 << 2) & 0xFF
        u2 = (u2 << 2) & 0xFF
    return out


def q6_k(b):
    ql, qh, sc = b[0:128], b[128:192], b[192:208]
    sc = [x - 256 if x > 127 else x for x in sc]
    d = f16(b[208:210])
    out = [0.0] * 256
    for n in range(0, 256, 128):
        lo, hi, s = n // 2, n // 4, (n // 128) * 8
        for l in range(32):
            i = l // 16
            q1 = ((ql[lo + l] & 0xF) | (((qh[hi + l] >> 0) & 3) << 4)) - 32
            q2 = ((ql[lo + l + 32] & 0xF) | (((qh[hi + l] >> 2) & 3) << 4)) - 32
            q3 = ((ql[lo + l] >> 4) | (((qh[hi + l] >> 4) & 3) << 4)) - 32
            q4 = ((ql[lo + l + 32] >> 4) | (((qh[hi + l] >> 6) & 3) << 4)) - 32
            out[n + l] = d * sc[s + 0] * q1
            out[n + l + 32] = d * sc[s + 2] * q2
            out[n + l + 64] = d * sc[s + 4] * q3
            out[n + l + 96] = d * sc[s + 6] * q4
    return out


DECODE = {0: lambda b: [struct.unpack('<f', b[0:4])[0]],
          1: lambda b: [f16(b[0:2])],
          2: q4_0, 3: q4_1, 6: q5_0, 7: q5_1, 8: q8_0,
          12: q4_k, 13: q5_k, 14: q6_k}


def decode(path, name, first, count):
    tensors, data_start = parse(path)
    ttype, off, _ = tensors[name]
    be, bb = BLOCK[ttype]
    fh = open(path, 'rb')
    out = []
    e = first
    while len(out) < count:
        bi, within = divmod(e, be)
        fh.seek(data_start + off + bi * bb)
        blk = DECODE[ttype](fh.read(bb))
        while within < be and len(out) < count:
            out.append(blk[within])
            within += 1
            e += 1
    return ttype, out


def main():
    path, name = sys.argv[1], sys.argv[2]
    first, count = int(sys.argv[3]), int(sys.argv[4])

    ttype, ref = decode(path, name, first, count)

    got = subprocess.run(['build/dequant_test', path, name,
                          str(first), str(count)],
                         capture_output=True, text=True)
    if got.returncode != 0:
        print('kernel decoder failed:', got.stderr.strip())
        return 1
    mine = [int(l) for l in got.stdout.splitlines() if not l.startswith('#')]

    worst, at = 0, -1
    for i, (a, b) in enumerate(zip(mine, ref)):
        # the C side reports values scaled by 1e6 and truncated
        diff = abs(a - int(b * 1e6))
        if diff > worst:
            worst, at = diff, i
    scale = max(abs(v) for v in ref) or 1.0
    print('%-28s %-6s n=%d  max |kernel-reference| = %d/1e6 at [%d]'
          % (name, NAME.get(ttype, ttype), len(mine), worst, at))
    print('   reference range [%+.6f, %+.6f]' % (min(ref), max(ref)))
    ok = worst <= 2                      # 1 ulp of the 1e6 truncation
    print('   %s' % ('MATCH' if ok else 'MISMATCH'))
    if not ok:
        for i in range(max(0, at - 2), min(len(ref), at + 3)):
            print('     [%d] kernel=%+.6f reference=%+.6f'
                  % (i, mine[i] / 1e6, ref[i]))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
