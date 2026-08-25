#!/usr/bin/env python3
"""
tools/ntfsdir.py - list an NTFS directory, without using the kernel's driver.

src/fs/ntfs/ntfs_ops.c reads NTFS and tools/mkntfs.py writes it, and they
agree -- but they were written together, from the same reading of the
same specification, by the same hand. Two implementations that share a
misunderstanding agree perfectly and are both wrong.

So this is a third one: a directory walker written from the on-disk
structures alone, in a different language, that descends $INDEX_ROOT
into $INDEX_ALLOCATION and prints what it finds. It shares no code with
either of the others. When it lists the same three hundred names the
kernel wrote, that is evidence rather than a tautology.

    python3 tools/ntfsdir.py <image> <path>
"""

import struct
import sys


def u16(b, o): return struct.unpack_from("<H", b, o)[0]
def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def unfixup(rec, sector=512):
    """Undo the update sequence: the last two bytes of every sector were
    replaced with a sequence number, and the real values kept in an
    array. A reader that skips this sees corruption every 512 bytes."""
    off, cnt = u16(rec, 4), u16(rec, 6)
    if cnt == 0:
        return rec
    out = bytearray(rec)
    usn = rec[off:off + 2]
    for i in range(1, cnt):
        tail = i * sector - 2
        if tail + 2 > len(out):
            break
        if bytes(out[tail:tail + 2]) != bytes(usn):
            raise SystemExit(f"torn record: sector {i} sequence mismatch")
        out[tail:tail + 2] = rec[off + i * 2: off + i * 2 + 2]
    return bytes(out)


def runs(data):
    """Decode a run list into (lcn, length) pairs. Offsets are signed
    deltas from the previous run, which is the detail that silently
    works until a file is laid out backwards on the volume."""
    out, i, prev = [], 0, 0
    while i < len(data) and data[i] != 0:
        hdr = data[i]
        nlen, olen = hdr & 0x0F, (hdr >> 4) & 0x0F
        i += 1
        length = int.from_bytes(data[i:i + nlen], "little")
        i += nlen
        if olen == 0:
            out.append((None, length))          # sparse
            continue
        delta = int.from_bytes(data[i:i + olen], "little", signed=True)
        i += olen
        prev += delta
        out.append((prev, length))
    return out


class Volume:
    def __init__(self, path):
        self.f = open(path, "rb")
        boot = self.f.read(512)
        if boot[3:11] != b"NTFS    ":
            raise SystemExit("not an NTFS volume")
        self.bps = u16(boot, 0x0B)
        self.spc = boot[0x0D]
        self.cluster = self.bps * self.spc
        cpr = struct.unpack_from("<b", boot, 0x40)[0]
        self.rec = cpr * self.cluster if cpr > 0 else 1 << (-cpr)
        self.mft_lcn = u64(boot, 0x30)
        self.mft_runs = None
        self.mft_runs = self._data_runs(self.record(0))

    def read_clusters(self, lcn, n):
        self.f.seek(lcn * self.cluster)
        return self.f.read(n * self.cluster)

    def record(self, number):
        if self.mft_runs is None:            # bootstrap: $MFT is contiguous
            base = self.mft_lcn * self.cluster + number * self.rec
        else:
            off = number * self.rec
            vcn, within = divmod(off, self.cluster)
            base = None
            at = 0
            for lcn, ln in self.mft_runs:
                if vcn < at + ln:
                    base = (lcn + (vcn - at)) * self.cluster + within
                    break
                at += ln
            if base is None:
                raise SystemExit(f"record {number} is outside $MFT")
        self.f.seek(base)
        r = unfixup(self.f.read(self.rec))
        if r[0:4] != b"FILE":
            raise SystemExit(f"record {number} is not a FILE record")
        return r

    def attrs(self, rec, want, name=None):
        off = u16(rec, 0x14)
        while off + 8 <= len(rec):
            atype = u32(rec, off)
            if atype == 0xFFFFFFFF:
                return
            alen = u32(rec, off + 4)
            if alen < 16 or off + alen > len(rec):
                return
            nlen, noff = rec[off + 9], u16(rec, off + 0x0A)
            aname = rec[off + noff: off + noff + nlen * 2].decode(
                "utf-16-le") if nlen else ""
            if atype == want and (name is None or aname == name):
                yield off, alen, rec[off + 8]
            off += alen

    def _data_runs(self, rec):
        for off, alen, nonres in self.attrs(rec, 0x80):
            if nonres:
                ro = u16(rec, off + 0x20)
                return runs(rec[off + ro: off + alen])
        return None


FN_NAME_LEN, FN_NAME, FN_FLAGS = 0x40, 0x42, 0x38


def entries(vol, node, base, out, depth, ia_runs, blocksize):
    """Walk one node in order, descending into children first."""
    if depth > 8:
        raise SystemExit("index deeper than eight levels")
    eoff = u32(node, base + 0x00)
    used = u32(node, base + 0x04)
    p = base + eoff
    end = base + used
    while p < end:
        elen = u16(node, p + 0x08)
        flags = u32(node, p + 0x0C)
        if elen < 16:
            break
        if flags & 0x01:                       # descend first
            vcn = u64(node, p + elen - 8)
            child = read_index_block(vol, ia_runs, vcn, blocksize)
            entries(vol, child, 0x18, out, depth + 1, ia_runs, blocksize)
        if flags & 0x02:                       # the end entry
            break
        klen = u16(node, p + 0x0A)
        if klen >= FN_NAME:
            n = node[p + 0x10 + FN_NAME_LEN]
            nm = node[p + 0x10 + FN_NAME: p + 0x10 + FN_NAME + n * 2]
            ns = node[p + 0x10 + FN_NAME_LEN + 1]
            if ns != 2:                        # skip 8.3 aliases
                out.append(nm.decode("utf-16-le", errors="replace"))
        p += elen


def read_index_block(vol, ia_runs, vcn, blocksize):
    per = max(1, blocksize // vol.cluster)
    want = vcn * per
    at = 0
    for lcn, ln in ia_runs:
        if want < at + ln:
            data = vol.read_clusters(lcn + (want - at), per)
            blk = unfixup(data)
            if blk[0:4] != b"INDX":
                raise SystemExit(f"vcn {vcn} is not an INDX block")
            return blk
        at += ln
    raise SystemExit(f"vcn {vcn} is not mapped by $INDEX_ALLOCATION")


def lookup(vol, path):
    number = 5
    for comp in [c for c in path.strip("/").split("/") if c]:
        found = None
        for nm, ref in listdir(vol, number, with_refs=True):
            if nm.upper() == comp.upper():
                found = ref
                break
        if found is None:
            raise SystemExit(f"no such path component: {comp}")
        number = found
    return number


def listdir(vol, number, with_refs=False):
    rec = vol.record(number)
    root = None
    for off, alen, nonres in vol.attrs(rec, 0x90, "$I30"):
        vo = u16(rec, off + 0x14)
        root = off + vo
    if root is None:
        raise SystemExit("not a directory")
    blocksize = u32(rec, root + 0x08)

    ia_runs = []
    for off, alen, nonres in vol.attrs(rec, 0xA0, "$I30"):
        if nonres:
            ro = u16(rec, off + 0x20)
            ia_runs = runs(rec[off + ro: off + alen])

    if with_refs:
        out = []
        collect_refs(vol, rec, root + 16, out, 0, ia_runs, blocksize)
        return out
    names = []
    entries(vol, rec, root + 16, names, 0, ia_runs, blocksize)
    return names


def collect_refs(vol, node, base, out, depth, ia_runs, blocksize):
    eoff, used = u32(node, base + 0x00), u32(node, base + 0x04)
    p, end = base + eoff, base + used
    while p < end:
        elen = u16(node, p + 0x08)
        flags = u32(node, p + 0x0C)
        if elen < 16:
            break
        if flags & 0x01:
            vcn = u64(node, p + elen - 8)
            child = read_index_block(vol, ia_runs, vcn, blocksize)
            collect_refs(vol, child, 0x18, out, depth + 1, ia_runs, blocksize)
        if flags & 0x02:
            break
        klen = u16(node, p + 0x0A)
        if klen >= FN_NAME:
            n = node[p + 0x10 + FN_NAME_LEN]
            nm = node[p + 0x10 + FN_NAME: p + 0x10 + FN_NAME + n * 2]
            ref = u64(node, p) & 0x0000FFFFFFFFFFFF
            out.append((nm.decode("utf-16-le", errors="replace"), ref))
        p += elen


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    vol = Volume(sys.argv[1])
    number = lookup(vol, sys.argv[2])
    names = listdir(vol, number)

    ordered = all(a.upper() <= b.upper() for a, b in zip(names, names[1:]))
    print(f"{sys.argv[2]}: {len(names)} entries, "
          f"{'ascending' if ordered else 'OUT OF ORDER'}")
    for n in names[:5]:
        print(f"  {n}")
    if len(names) > 10:
        print(f"  ... {len(names) - 10} more ...")
    for n in names[-5:]:
        print(f"  {n}")
    return 0 if ordered else 1


if __name__ == "__main__":
    sys.exit(main())
