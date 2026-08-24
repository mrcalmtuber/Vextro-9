#!/usr/bin/env python3
"""
tools/mkntfs.py - make an NTFS volume.

Vextro's filesystem has been exFAT since there was a filesystem, and
exFAT is a table of clusters with a directory of fixed records. NTFS is
a different idea entirely: *everything* is a file, including the
structures that describe where files are. The Master File Table is a
file. The free-space bitmap is a file. The boot sector is a file. The
root directory is a file whose contents are a B-tree of names.

That means formatting is mostly a bootstrapping problem -- the MFT has
to contain a record describing the MFT, in a place the MFT's own record
says it is -- and it is the reason this exists rather than shelling out
to mkntfs(8): the build has to run on a Mac, where there is no such
tool, and the layout has to be one the kernel's driver in ntfs_ops.c
and ntfs_ops.c agrees with byte for byte.

What this produces is a real NTFS volume: a Linux or Windows machine
mounts it and reads the files. What it deliberately does *not* produce
is a bootable Windows volume -- there is no $LogFile content, no
security descriptors beyond a default, and no $UsnJrnl until the kernel
creates one. Those are noted where they occur.

Usage:
    python3 tools/mkntfs.py <image> <size-MB> [src[:dest] ...]

`src` alone puts the file in the root under its own basename; `src:dest`
puts it at `dest`, creating directories along the way. That is the same
convention tools/mkexfat.py takes, so the disk.img rule in the Makefile
did not have to change shape when the boot volume moved.
"""

import os
import struct
import sys

SECTOR = 512
SPC = 8                       # sectors per cluster -> 4 KB clusters
CLUSTER = SECTOR * SPC
# 4096-byte MFT records rather than the 1024 Windows uses by default.
#
# A directory here lives entirely in its resident $INDEX_ROOT -- the
# driver does not build $INDEX_ALLOCATION, so a directory holds as many
# entries as fit in one record and then reports ENOSPC. At 1024 bytes
# that is about six entries, which is not a filesystem. At 4096 it is
# about thirty, which is enough for the volume this seeds and for what a
# session adds to it.
#
# 4096 is the ceiling, not a preference: the kernel reads a record into
# a 4096-byte static buffer and ntfs_try() refuses a volume claiming
# more. Raising it further means raising that buffer, and beyond 4096
# the format stops being one other implementations expect.
MFT_REC = 4096                # bytes per MFT record
MFT_RECS_RESERVED = 16        # the system files occupy 0..15

# The pagefile, preallocated here rather than by the kernel.
#
# src/swap.h resolves its backing store to one absolute LBA at boot so
# that no filesystem code runs inside a page fault. That requires the
# file to be exactly one run, and the only place a single run can be
# guaranteed is a formatter laying out empty space in order.
PAGEFILE_NAME = "pagefile.sys"
PAGEFILE_MB = 256

# Attribute types
AT_STANDARD_INFORMATION = 0x10
AT_FILE_NAME = 0x30
AT_DATA = 0x80
AT_INDEX_ROOT = 0x90
AT_INDEX_ALLOCATION = 0xA0
AT_BITMAP = 0xB0
AT_END = 0xFFFFFFFF

FILE_RECORD_IN_USE = 0x0001
FILE_RECORD_IS_DIRECTORY = 0x0002

# Windows FILETIME for an arbitrary fixed date, so a rebuild of the same
# inputs produces the same bytes. A timestamp of "now" would make every
# image differ and make a diff of two builds meaningless.
FIXED_TIME = 132000000000000000


def u8(v):
    return struct.pack("<B", v)


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def u64(v):
    return struct.pack("<Q", v)


def utf16(s):
    return s.encode("utf-16-le")


def align(n, to):
    return (n + to - 1) // to * to


# ---------------------------------------------------------------- runs

def encode_runlist(runs):
    """
    Encode (lcn, length) pairs into NTFS's data run format.

    Each run is a header byte packing two nibble widths, then the length,
    then the offset as a *signed difference* from the previous run's
    start. The signedness is the part that bites: a run placed before its
    predecessor has a negative delta, and writing it unsigned produces a
    cluster number in the exabytes.
    """
    out = b""
    prev = 0
    for lcn, length in runs:
        delta = lcn - prev
        prev = lcn

        lbytes = b""
        v = length
        while v:
            lbytes += u8(v & 0xFF)
            v >>= 8
        if not lbytes:
            lbytes = b"\x00"

        # two's complement, minimum width that keeps the sign
        obytes = b""
        v = delta
        if v == 0:
            obytes = b"\x00"
        else:
            neg = v < 0
            tmp = v & ((1 << 64) - 1)
            while True:
                obytes += u8(tmp & 0xFF)
                tmp >>= 8
                if not neg and tmp == 0:
                    if obytes[-1] & 0x80:
                        obytes += b"\x00"   # keep it positive
                    break
                if neg and tmp == (1 << (64 - 8 * len(obytes))) - 1:
                    if not (obytes[-1] & 0x80):
                        obytes += b"\xFF"   # keep it negative
                    break
                if len(obytes) >= 8:
                    break

        out += u8((len(obytes) << 4) | len(lbytes)) + lbytes + obytes
    return out + b"\x00"


# --------------------------------------------------------- attributes

def attr_resident(atype, value, name="", flags=0, indexed=0):
    name_b = utf16(name)
    name_off = 0x18
    value_off = align(name_off + len(name_b), 8)
    length = align(value_off + len(value), 8)

    a = struct.pack("<IIBBHHH", atype, length, 0, len(name), name_off,
                    flags, 0)
    a += struct.pack("<IHBB", len(value), value_off, indexed, 0)
    a += name_b
    a += b"\x00" * (value_off - len(a))
    a += value
    a += b"\x00" * (length - len(a))
    return a


def attr_nonresident(atype, runs, real_size, alloc_size, name="",
                     start_vcn=0, last_vcn=None):
    name_b = utf16(name)
    name_off = 0x40
    runlist = encode_runlist(runs)
    run_off = align(name_off + len(name_b), 8)
    length = align(run_off + len(runlist), 8)

    if last_vcn is None:
        last_vcn = alloc_size // CLUSTER - 1

    a = struct.pack("<IIBBHHH", atype, length, 1, len(name), name_off, 0, 0)
    a += struct.pack("<QQHHI", start_vcn, last_vcn, run_off, 0, 0)
    a += struct.pack("<QQQ", alloc_size, real_size, real_size)
    a += name_b
    a += b"\x00" * (run_off - len(a))
    a += runlist
    a += b"\x00" * (length - len(a))
    return a


def standard_information():
    return attr_resident(AT_STANDARD_INFORMATION,
                         struct.pack("<QQQQIIIIQQQQ",
                                     FIXED_TIME, FIXED_TIME,
                                     FIXED_TIME, FIXED_TIME,
                                     0x06,      # FILE_ATTRIBUTE_HIDDEN|SYSTEM
                                     0, 0, 0, 0, 0, 0, 0))


def file_name_attr(parent_ref, name, is_dir, real_size=0, alloc_size=0):
    flags = 0x10000000 if is_dir else 0x06
    v = struct.pack("<Q", parent_ref)
    v += struct.pack("<QQQQ", FIXED_TIME, FIXED_TIME, FIXED_TIME, FIXED_TIME)
    v += struct.pack("<QQ", alloc_size, real_size)
    v += struct.pack("<II", flags, 0)
    v += struct.pack("<BB", len(name), 0)      # length, POSIX namespace
    v += utf16(name)
    return attr_resident(AT_FILE_NAME, v, indexed=1)


# ------------------------------------------------------- index entries

def index_entry(mft_ref, name, is_dir, real_size, alloc_size):
    """One $FILE_NAME entry inside a directory index."""
    fn = struct.pack("<Q", 5 | (5 << 48))      # parent: root, seq 5
    fn += struct.pack("<QQQQ", FIXED_TIME, FIXED_TIME, FIXED_TIME, FIXED_TIME)
    fn += struct.pack("<QQ", alloc_size, real_size)
    fn += struct.pack("<II", 0x10000000 if is_dir else 0x06, 0)
    fn += struct.pack("<BB", len(name), 0)
    fn += utf16(name)

    entry_len = align(0x10 + len(fn), 8)
    e = struct.pack("<QHHI", mft_ref, entry_len, len(fn), 0)
    e += fn
    e += b"\x00" * (entry_len - len(e))
    return e


def index_end_entry():
    # The terminator: no name, flag 0x02 = last entry in the node.
    return struct.pack("<QHHI", 0, 0x10, 0, 0x02)


def index_root(entries):
    """
    A resident $INDEX_ROOT holding every entry.

    Real NTFS spills into $INDEX_ALLOCATION once the root exceeds what
    fits in an MFT record. This formatter keeps every directory small
    enough that it does not have to -- the kernel's writer in
    ntfs_ops.c is the half that has to cope with growth, and it
    reports rather than corrupts when a root fills.
    """
    body = b"".join(entries) + index_end_entry()
    header = struct.pack("<IIII", AT_FILE_NAME, 1, 4096, 1)   # collation 1
    node = struct.pack("<IIII", 0x10, 0x10 + len(body), 0x10 + len(body), 0)
    return header + node + body


# ----------------------------------------------------------- records

def apply_fixups(rec, size):
    """
    Install the update sequence.

    The last two bytes of every sector are replaced with a copy of the
    sequence number and the displaced values kept in the array, so a
    record torn across a sector boundary is detectable: one sector will
    carry the old number. Writing a record without doing this makes the
    kernel's reader reject it as torn, which is exactly what it is for.
    """
    rec = bytearray(rec)
    count = size // SECTOR + 1
    usn = 1
    off = 0x30                      # fixup array, past the fixed header
    struct.pack_into("<HH", rec, 0x04, off, count)
    struct.pack_into("<H", rec, off, usn)
    for i in range(1, count):
        tail = i * SECTOR - 2
        struct.pack_into("<H", rec, off + i * 2, *struct.unpack_from("<H", rec, tail))
        struct.pack_into("<H", rec, tail, usn)
    return bytes(rec)


def mft_record(number, seq, attrs, is_dir=False, link_count=1):
    attr_bytes = b"".join(attrs) + u32(AT_END) + u32(0)
    attr_off = 0x38 + 2 * (MFT_REC // SECTOR + 1)
    attr_off = align(attr_off, 8)

    used = attr_off + len(attr_bytes)
    if used > MFT_REC:
        raise SystemExit(f"MFT record {number} overflows ({used} > {MFT_REC})")

    flags = FILE_RECORD_IN_USE | (FILE_RECORD_IS_DIRECTORY if is_dir else 0)

    r = bytearray(MFT_REC)
    r[0:4] = b"FILE"
    struct.pack_into("<HH", r, 0x04, 0x30, MFT_REC // SECTOR + 1)
    struct.pack_into("<Q", r, 0x08, 0)          # $LogFile sequence number
    struct.pack_into("<H", r, 0x10, seq)
    struct.pack_into("<H", r, 0x12, link_count)
    struct.pack_into("<H", r, 0x14, attr_off)
    struct.pack_into("<H", r, 0x16, flags)
    struct.pack_into("<I", r, 0x18, align(used, 8))
    struct.pack_into("<I", r, 0x1C, MFT_REC)
    struct.pack_into("<Q", r, 0x20, 0)          # base record
    struct.pack_into("<H", r, 0x28, 8)          # next attribute id
    struct.pack_into("<I", r, 0x2C, number)
    r[attr_off:attr_off + len(attr_bytes)] = attr_bytes
    return apply_fixups(bytes(r), MFT_REC)


# -------------------------------------------------------------- main
#
# The volume is laid out in one pass, in order, and that ordering is
# load-bearing rather than tidy:
#
#   boot | $MFT | $MFTMirr | $Bitmap | pagefile.sys | file data...
#
# Every file gets a single contiguous run because the allocator here
# only ever moves forward. That is not an optimisation. Without
# $ATTRIBUTE_LIST -- which this driver does not write -- a file's whole
# run list has to fit in its own MFT record, and a 937 MB archive broken
# into a few thousand fragments would not. Sequential allocation makes
# every run list exactly one entry, whatever the file's size.


class Node:
    """A file or a directory in the tree being built."""

    def __init__(self, name, src=None):
        self.name = name
        self.src = src              # None for a directory
        self.children = {}          # name -> Node, for directories
        self.record = None          # assigned MFT record number
        self.lcn = 0                # first cluster of the data
        self.clusters = 0
        self.size = 0

    @property
    def is_dir(self):
        return self.src is None and not self.is_pagefile

    is_pagefile = False

    def child(self, name):
        """Get or create a subdirectory, case-insensitively."""
        for key, node in self.children.items():
            if key.lower() == name.lower():
                return node
        node = Node(name)
        self.children[name] = node
        return node


class Pagefile(Node):
    is_pagefile = True

    def __init__(self, name, size):
        Node.__init__(self, name)
        self.size = size

    @property
    def is_dir(self):
        return False


def walk(node):
    """Every node under `node`, parents before children."""
    for child in node.children.values():
        yield child
        if child.is_dir:
            for grandchild in walk(child):
                yield grandchild


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    path = sys.argv[1]
    size_mb = int(sys.argv[2])

    # --- build the tree ---
    root = Node("")
    for spec in sys.argv[3:]:
        src, _, dest = spec.partition(":")
        if not dest:
            dest = os.path.basename(src)
        if not os.path.isfile(src):
            print(f"  skipping {dest}: {src} not found")
            continue
        parts = dest.strip("/").split("/")
        d = root
        for p in parts[:-1]:
            d = d.child(p)
        leaf = Node(parts[-1], src)
        leaf.size = os.path.getsize(src)
        d.children[parts[-1]] = leaf

    # The pagefile is part of the layout rather than content, so it is
    # added here rather than being passed in by the Makefile.
    #
    # Only when the volume has room to spare for it. The scratch images
    # the host tests format are tens of megabytes, and a formatter that
    # insisted on a 256 MB pagefile would refuse to make one -- so the
    # rule is that a volume must be several times the pagefile before it
    # gets one, and a volume without one boots with swap disabled and
    # says so rather than failing.
    pagefile_mb = PAGEFILE_MB if size_mb >= PAGEFILE_MB * 4 else 0
    if pagefile_mb:
        root.children[PAGEFILE_NAME] = Pagefile(PAGEFILE_NAME,
                                                pagefile_mb * 1024 * 1024)

    nodes = list(walk(root))
    dirs = [n for n in nodes if n.is_dir]
    files = [n for n in nodes if not n.is_dir]

    total_sectors = size_mb * 1024 * 1024 // SECTOR
    total_clusters = total_sectors // SPC

    # --- assign MFT records ---
    #
    # Reserved for the system files, then one per node, then headroom.
    # The headroom is what a running system creates into: ntfs_mft_alloc
    # searches up to $MFT's allocated size and stops, so a table sized
    # exactly to the seed would refuse the first file anyone made.
    next_record = MFT_RECS_RESERVED
    for n in nodes:
        n.record = next_record
        next_record += 1

    # Headroom, scaled to the volume.
    #
    # A record is one cluster here, so headroom is a direct cost in
    # space: 2048 spare records is 8 MB, which is nothing on the 8 GB
    # boot volume and half of a 16 MB scratch image. Sizing it against
    # the volume keeps both sensible -- the test images stay mostly
    # free space, and the real one can take a couple of thousand files.
    runtime_records = min(2048, max(32, total_clusters // 64))
    mft_records = next_record + runtime_records
    mft_clusters = max(1, align(mft_records * MFT_REC, CLUSTER) // CLUSTER)

    # --- lay out the volume ---
    mft_lcn = 4
    mftmirr_lcn = mft_lcn + mft_clusters
    lcn = mftmirr_lcn + 1

    bitmap_bytes_len = align(total_clusters, 8) // 8
    bitmap_clusters = max(1, align(bitmap_bytes_len, CLUSTER) // CLUSTER)
    bitmap_lcn = lcn
    lcn += bitmap_clusters

    for n in files:
        n.clusters = max(1, align(n.size, CLUSTER) // CLUSTER)
        n.lcn = lcn
        lcn += n.clusters
    volume_end = lcn

    if volume_end >= total_clusters:
        raise SystemExit(
            f"content does not fit: needs {volume_end} clusters of "
            f"{CLUSTER}, volume has {total_clusters}")

    # --- the bitmap ---
    bitmap = bytearray(bitmap_bytes_len)
    for c in range(volume_end):
        bitmap[c >> 3] |= 1 << (c & 7)
    # Clusters past the end of the volume are marked in use so nothing
    # allocates into the rounding slack at the tail of the last byte.
    for c in range(total_clusters, bitmap_bytes_len * 8):
        bitmap[c >> 3] |= 1 << (c & 7)
    bitmap_bytes = bytes(bitmap)

    # --- MFT records ---
    records = {}
    ROOT_REF = 5 | (5 << 48)

    def sysrec(number, name, attrs, is_dir=False):
        return mft_record(number, 1,
                          [standard_information(),
                           file_name_attr(ROOT_REF, name, is_dir)] + attrs,
                          is_dir=is_dir)

    records[0] = sysrec(0, "$MFT", [
        attr_nonresident(AT_DATA, [(mft_lcn, mft_clusters)],
                         mft_clusters * CLUSTER, mft_clusters * CLUSTER)])
    records[1] = sysrec(1, "$MFTMirr", [
        attr_nonresident(AT_DATA, [(mftmirr_lcn, 1)], CLUSTER, CLUSTER)])
    records[2] = sysrec(2, "$LogFile", [attr_resident(AT_DATA, b"")])

    records[3] = mft_record(3, 1, [
        standard_information(),
        file_name_attr(ROOT_REF, "$Volume", False),
        attr_resident(0x60, utf16("Vextro")),
        attr_resident(0x70, struct.pack("<QBBH", 0, 3, 1, 0)),
        attr_resident(AT_DATA, b""),
    ])
    records[4] = sysrec(4, "$AttrDef", [attr_resident(AT_DATA, b"")])

    # 5: the root directory, whose index lists its own children
    def index_for(node):
        entries = []
        for c in sorted(node.children.values(),
                        key=lambda n: n.name.upper()):
            ref = c.record | (1 << 48)
            entries.append(index_entry(ref, c.name, c.is_dir, c.size,
                                       c.clusters * CLUSTER))
        return index_root(entries)

    records[5] = mft_record(5, 5, [
        standard_information(),
        file_name_attr(ROOT_REF, ".", True),
        attr_resident(AT_INDEX_ROOT, index_for(root), name="$I30"),
    ], is_dir=True, link_count=1)

    records[6] = sysrec(6, "$Bitmap", [
        attr_nonresident(AT_DATA, [(bitmap_lcn, bitmap_clusters)],
                         len(bitmap_bytes), bitmap_clusters * CLUSTER)])
    records[7] = sysrec(7, "$Boot", [
        attr_nonresident(AT_DATA, [(0, 1)], CLUSTER, CLUSTER)])

    for n, nm in ((8, "$BadClus"), (9, "$Secure"), (10, "$UpCase"),
                  (11, "$Extend"), (12, ""), (13, ""), (14, ""), (15, "")):
        attrs = [standard_information()]
        if nm:
            attrs.append(file_name_attr(ROOT_REF, nm, nm == "$Extend"))
        attrs.append(attr_resident(AT_DATA, b""))
        records[n] = mft_record(n, 1, attrs, is_dir=(nm == "$Extend"))

    # --- the tree ---
    #
    # A parent reference is the record number and the sequence number
    # together, and getting it wrong is invisible until something walks
    # back up: the entry reads correctly, and the file claims to live in
    # a directory that does not contain it.
    parent_of = {}
    for parent in [root] + dirs:
        pref = ROOT_REF if parent is root else (parent.record | (1 << 48))
        for c in parent.children.values():
            parent_of[id(c)] = pref

    for n in nodes:
        pref = parent_of[id(n)]
        if n.is_dir:
            records[n.record] = mft_record(n.record, 1, [
                standard_information(),
                file_name_attr(pref, n.name, True),
                attr_resident(AT_INDEX_ROOT, index_for(n), name="$I30"),
            ], is_dir=True)
        else:
            records[n.record] = mft_record(n.record, 1, [
                standard_information(),
                file_name_attr(pref, n.name, False, n.size,
                               n.clusters * CLUSTER),
                attr_nonresident(AT_DATA, [(n.lcn, n.clusters)], n.size,
                                 n.clusters * CLUSTER),
            ])

    highest = max(records)
    if (highest + 1) * MFT_REC > mft_clusters * CLUSTER:
        raise SystemExit("too many files for the reserved MFT size")

    # --- boot sector ---
    boot = bytearray(SECTOR)
    boot[0:3] = b"\xEB\x52\x90"
    boot[3:11] = b"NTFS    "
    struct.pack_into("<H", boot, 0x0B, SECTOR)
    struct.pack_into("<B", boot, 0x0D, SPC)
    struct.pack_into("<H", boot, 0x0E, 0)
    struct.pack_into("<B", boot, 0x15, 0xF8)
    struct.pack_into("<H", boot, 0x18, 63)
    struct.pack_into("<H", boot, 0x1A, 255)
    struct.pack_into("<I", boot, 0x1C, 0)
    struct.pack_into("<Q", boot, 0x28, total_sectors - 1)
    struct.pack_into("<Q", boot, 0x30, mft_lcn)
    struct.pack_into("<Q", boot, 0x38, mftmirr_lcn)
    # Clusters per MFT record. A positive value is a count of clusters,
    # a negative one is 2^-n bytes. With 4096-byte records on 4096-byte
    # clusters it is exactly 1, and the kernel handles both forms.
    if MFT_REC >= CLUSTER:
        struct.pack_into("<b", boot, 0x40, MFT_REC // CLUSTER)
    else:
        struct.pack_into("<b", boot, 0x40, -(MFT_REC.bit_length() - 1))
    struct.pack_into("<b", boot, 0x44, 1)
    struct.pack_into("<Q", boot, 0x48, 0x56455854524F0009)
    struct.pack_into("<H", boot, 0x1FE, 0xAA55)

    # --- write it ---
    #
    # Sparse: the length is set without writing 8 GB of zeros, and only
    # the clusters that carry something are touched. The pagefile is
    # allocated and never written, so 256 MB of it costs nothing on the
    # host until the kernel pages into it.
    with open(path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

        f.write(boot)

        for n in range(highest + 1):
            f.seek(mft_lcn * CLUSTER + n * MFT_REC)
            f.write(records.get(n, bytes(MFT_REC)))

        f.seek(mftmirr_lcn * CLUSTER)
        for n in range(4):
            f.write(records[n])

        for n in files:
            if n.is_pagefile:
                continue
            f.seek(n.lcn * CLUSTER)
            with open(n.src, "rb") as g:
                while True:
                    chunk = g.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)

        f.seek(bitmap_lcn * CLUSTER)
        f.write(bitmap_bytes)

        f.seek((total_sectors - 1) * SECTOR)
        f.write(boot)

    content = sum(n.clusters for n in files if not n.is_pagefile)
    nfiles = len([n for n in files if not n.is_pagefile])
    print(f"  NTFS   {path}: {size_mb} MB, {total_clusters} clusters of "
          f"{CLUSTER}, {MFT_REC}-byte records")
    print(f"         $MFT {mft_clusters} clusters at {mft_lcn} "
          f"({mft_records} records, {runtime_records} spare), "
          f"$Bitmap at {bitmap_lcn}")
    print(f"         {nfiles} files in {len(dirs) + 1} directories, "
          f"{content * CLUSTER // (1024 * 1024)} MB content, "
          + (f"{pagefile_mb} MB pagefile" if pagefile_mb else "no pagefile"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
