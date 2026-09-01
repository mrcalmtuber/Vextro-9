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
# This used to be the directory ceiling: a directory lived entirely in
# its resident $INDEX_ROOT, so the record size *was* how many names it
# could hold. The driver builds $INDEX_ALLOCATION now and directories
# grow into a B-tree, so what the record size sets is the fan-out of the
# root node -- about thirty children before the root itself splits --
# and how much of a small directory stays in the record instead of
# costing a cluster.
#
# 4096 is the ceiling rather than a preference: the kernel reads a
# record into a 4096-byte static buffer and ntfs_try() refuses a volume
# claiming more.
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

# The root directory: MFT record 5, sequence 5. Hoisted out of main()
# because dir_index_capacity() needs it to size a $FILE_NAME attribute.
ROOT_REF = 5 | (5 << 48)

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

IE_HAS_CHILD = 0x01
IE_LAST      = 0x02
IX_LARGE     = 0x01           # this node has children

# One index block, and the unit $INDEX_ALLOCATION is divided into. Equal
# to the cluster size, which makes "clusters per index block" 1 -- the
# value written into the $INDEX_ROOT header below and the divisor
# ix_block_read() uses to turn a VCN into a cluster.
INDX_SIZE = CLUSTER


def collation_key(name):
    """
    Order two names the way ntfs_name_cmp() in src/fs/ntfs/ntfs_ops.c
    orders them, which is the order a reader that binary-searches
    depends on.

    That function upcases only a-z and then compares code unit by code
    unit, with the shorter name first when one is a prefix of the other.
    `str.upper()` agrees for ASCII and does not in general -- it maps
    'i' differently under some locales' rules and expands 'ss' -- so the
    fold is written out rather than borrowed. Every name on this volume
    is ASCII today; the point is that the sort does not quietly stop
    matching the kernel if one is not.
    """
    return [ord(c) - 32 if 'a' <= c <= 'z' else ord(c) for c in name]


def index_entry(mft_ref, name, is_dir, real_size, alloc_size,
                child_vcn=None):
    """
    One $FILE_NAME entry inside a directory index.

    With child_vcn, the entry also carries a downlink: the IE_HAS_CHILD
    flag and eight more bytes at the *end* of the entry holding the VCN
    of the subtree whose names all sort before this one. The VCN goes
    last, after the $FILE_NAME value and its padding, which is why it is
    appended rather than packed -- writing it immediately after the
    header would put it inside the name.
    """
    fn = struct.pack("<Q", 5 | (5 << 48))      # parent: root, seq 5
    fn += struct.pack("<QQQQ", FIXED_TIME, FIXED_TIME, FIXED_TIME, FIXED_TIME)
    fn += struct.pack("<QQ", alloc_size, real_size)
    fn += struct.pack("<II", 0x10000000 if is_dir else 0x06, 0)
    fn += struct.pack("<BB", len(name), 0)
    fn += utf16(name)

    entry_len = align(0x10 + len(fn), 8)
    flags = 0
    if child_vcn is not None:
        entry_len += 8
        flags = IE_HAS_CHILD

    e = struct.pack("<QHHI", mft_ref, entry_len, len(fn), flags)
    e += fn
    if child_vcn is not None:
        e += b"\x00" * (entry_len - 8 - len(e))
        e += struct.pack("<Q", child_vcn)
    else:
        e += b"\x00" * (entry_len - len(e))
    return e


def index_end_entry(child_vcn=None):
    """
    The terminator: no name, IE_LAST.

    It carries a downlink like any other entry when the node has
    children, and that downlink is the subtree holding every name after
    the last separator. Omitting it is the mistake ntfs_ops.c warns
    about at length -- it loses the rightmost subtree of every node, so
    the directory reads correctly right up to its last few names.
    """
    if child_vcn is None:
        return struct.pack("<QHHI", 0, 0x10, 0, IE_LAST)
    return (struct.pack("<QHHI", 0, 0x18, 0, IE_LAST | IE_HAS_CHILD)
            + struct.pack("<Q", child_vcn))


def index_root(entries, last_child=None):
    """
    The resident $INDEX_ROOT: either the whole directory, or the root
    node of a tree whose other nodes live in $INDEX_ALLOCATION.

    This formatter used to only ever produce the first of those, and
    said so here -- the note read that it "keeps every directory small
    enough that it does not have to" spill. That stopped being true the
    first time a directory had two hundred names in it: xkeyboard-config
    stages 195 files into /etc/xkb/symbols and the record overflowed at
    12984 bytes against a 4096-byte ceiling. The kernel had read and
    written B-trees since directories became B-trees; the formatter was
    the half that could not make one.
    """
    body = b"".join(entries) + index_end_entry(last_child)
    flags = IX_LARGE if last_child is not None else 0
    # type, collation 1 (upcased $FILE_NAME), bytes per index block,
    # clusters per index block.
    header = struct.pack("<IIII", AT_FILE_NAME, 1, INDX_SIZE,
                         INDX_SIZE // CLUSTER)
    node = struct.pack("<IIII", 0x10, 0x10 + len(body), 0x10 + len(body),
                       flags)
    return header + node + body


def indx_block(vcn, entries, last_child=None):
    """
    One "INDX" block of $INDEX_ALLOCATION.

    The layout, and the trap in it, are documented in ntfs_ops.c: the
    index header sits at 0x18 and every offset inside it is relative to
    *that*, not to the block; the update sequence array is at 0x28; and
    entries begin at 0x40, after the array rather than on top of it.
    """
    body = b"".join(entries) + index_end_entry(last_child)
    used = 0x28 + len(body)               # relative to the header at 0x18
    if 0x18 + used > INDX_SIZE:
        raise SystemExit("index block overflow -- a node was packed too full")

    blk = bytearray(INDX_SIZE)
    blk[0:4] = b"INDX"
    struct.pack_into("<HH", blk, 0x04, 0x28, INDX_SIZE // SECTOR + 1)
    struct.pack_into("<Q", blk, 0x08, 0)                  # $LogFile LSN
    struct.pack_into("<Q", blk, 0x10, vcn)
    struct.pack_into("<IIII", blk, 0x18,
                     0x28,                                 # entries offset
                     used,
                     INDX_SIZE - 0x18,                     # allocated
                     IX_LARGE if last_child is not None else 0)
    blk[0x40:0x40 + len(body)] = body
    return apply_fixups(bytes(blk), INDX_SIZE, usa_off=0x28)


def with_child(entry, child_vcn):
    """
    Return `entry` carrying a downlink to `child_vcn`, or unchanged when
    there is none.

    The $FILE_NAME value is copied across untouched; only the length
    grows by eight and IE_HAS_CHILD goes on. Rebuilding the entry from
    its fields instead would mean carrying every field -- reference,
    sizes, flags -- through the tree builder for no reason.
    """
    if child_vcn is None:
        return entry
    ref, length, keylen, flags = struct.unpack_from("<QHHI", entry, 0)
    body = entry[0x10:length - 8] if (flags & IE_HAS_CHILD) else entry[0x10:length]
    new_len = align(0x10 + len(body), 8) + 8
    e = struct.pack("<QHHI", ref, new_len, keylen, flags | IE_HAS_CHILD)
    e += body
    e += b"\x00" * (new_len - 8 - len(e))
    e += struct.pack("<Q", child_vcn)
    return e


# Fill a node to a little under the block's capacity. The slack is not
# superstition: the kernel splits a node on the way down whenever it has
# less than one maximum-sized entry free, so a node packed here to the
# last byte would be split by the first name anyone added to that
# directory at run time. Leaving room means the volume this produces
# behaves like one the kernel grew itself.
INDX_CAPACITY = INDX_SIZE - 0x40 - 512


def build_index(entries, root_capacity):
    """
    Turn a directory's entries, already in collation order, into a root
    node plus a list of index blocks.

    Returns (root_entries, root_last_child, blocks), where blocks is
    indexed by VCN and each element is (entries, last_child).

    ---- the shape ----

    An ordinary B-tree, built bottom-up. At each level the items are
    pairs (entry, child) where `child` is the subtree holding every name
    that sorts *before* that entry -- which is exactly how NTFS stores a
    downlink, and why the downlink lives on the entry after it rather
    than before.

    Entries are packed into a node until one does not fit. That entry
    becomes a **separator**: it is promoted to the level above and does
    not appear in any node below, because this is a B-tree rather than a
    B+tree and a name appears exactly once. The node just closed holds
    everything before the separator, so the separator's downlink is that
    node -- and the node's own rightmost child is whatever child the
    separator brought with it.

    The loop stops when what is left fits in the MFT record, which is a
    smaller budget than a block and is why root_capacity is a parameter
    rather than INDX_CAPACITY.
    """
    blocks = []

    def emit(node_entries, last_child):
        blocks.append((node_entries, last_child))
        return len(blocks) - 1

    def pack(items, trailing):
        """Split one level into nodes. Returns (nodes, separators)."""
        nodes, seps, cur, cur_len = [], [], [], 0
        for e, child in items:
            packed = with_child(e, child)
            if cur and cur_len + len(packed) > INDX_CAPACITY:
                # `e` is promoted; the node just closed ends before it,
                # so that node's rightmost child is `e`'s own child.
                nodes.append((cur, child))
                seps.append(e)
                cur, cur_len = [], 0
                continue
            cur.append(packed)
            cur_len += len(packed)
        nodes.append((cur, trailing))
        return nodes, seps

    items = [(e, None) for e in entries]
    trailing = None

    while True:
        root = [with_child(e, c) for e, c in items]
        # 0x10 of node header, plus the terminator: 0x10 bare, 0x18 with
        # a downlink.
        size = sum(len(e) for e in root) + 0x10 + (0x18 if trailing is not None else 0x10)
        if size <= root_capacity:
            return root, trailing, blocks

        nodes, seps = pack(items, trailing)
        if len(nodes) < 2:
            raise SystemExit(
                "a directory entry is too large for an index block")

        items = []
        for i in range(len(nodes) - 1):
            items.append((seps[i], emit(nodes[i][0], nodes[i][1])))
        trailing = emit(nodes[-1][0], nodes[-1][1])



# The room a directory record has for its $INDEX_ROOT *value*, and what
# else has to fit beside it.
#
# A directory record carries $STANDARD_INFORMATION, one $FILE_NAME and
# $INDEX_ROOT; when the index spills it also carries $INDEX_ALLOCATION
# and $BITMAP. Those two are sized here with a generous runlist because
# the run is not encoded until the clusters are allocated, and being
# wrong in this direction only means splitting one entry earlier than
# strictly necessary. mft_record() is still the thing that refuses an
# overflowing record -- this only decides where to aim.
IX_ATTR_SLACK = align(0x40 + 8 + 32, 8) + align(0x18 + 8 + 8, 8)


def dir_index_capacity(name, with_tree):
    attr_off = align(0x38 + 2 * (MFT_REC // SECTOR + 1), 8)
    fixed = (len(standard_information())
             + len(file_name_attr(ROOT_REF, name, True))
             + 8)                                  # the END marker
    ir_header = 0x20                               # attr header + "$I30"
    return (MFT_REC - attr_off - fixed - ir_header
            - (IX_ATTR_SLACK if with_tree else 0))


def plan_index(node):
    """
    Decide how one directory's index is stored, and record the answer on
    the node: ix_root/ix_last_child/ix_blocks.

    Small directories stay entirely resident, which is what every
    directory on this volume did before xkeyboard-config arrived. Large
    ones get a B-tree, and their blocks are allocated by the caller.
    """
    entries = []
    for c in sorted(node.children.values(),
                    key=lambda n: collation_key(n.name)):
        ref = c.record | (1 << 48)
        entries.append(index_entry(ref, c.name, c.is_dir, c.size,
                                   c.clusters * CLUSTER))

    name = "." if node.name == "" else node.name
    flat = sum(len(e) for e in entries) + 0x10 + 0x10
    if flat <= dir_index_capacity(name, False):
        node.ix_root, node.ix_last_child, node.ix_blocks = entries, None, []
        return

    node.ix_root, node.ix_last_child, node.ix_blocks = build_index(
        entries, dir_index_capacity(name, True))


# ----------------------------------------------------------- records

def apply_fixups(rec, size, usa_off=0x30):
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
    # Past the fixed header: 0x30 in an MFT record, 0x28 in an INDX
    # block. The kernel reads the offset out of the header rather than
    # assuming either, so the two only have to be self-consistent.
    off = usa_off
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
        # A directory's index, decided by plan_index(). ix_blocks is
        # empty for every directory small enough to stay resident, which
        # is all but a handful of them.
        self.ix_root = []           # entries of the resident root node
        self.ix_last_child = None   # VCN under the root's terminator
        self.ix_blocks = []         # [(entries, last_child)], by VCN
        self.ix_lcn = 0             # first cluster of $INDEX_ALLOCATION

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
    # The MFT does not grow. ntfs_mft_alloc searches up to $MFT's own
    # allocated size and stops, so the number of spare records set here
    # is the number of files that can ever be created on the volume --
    # a ceiling every bit as real as the directory one $INDEX_ALLOCATION
    # just removed, and a much less obvious one, because it presents as
    # "the disk is full" on a volume with gigabytes free.
    #
    # A record is one cluster, so this is a direct cost in space: one
    # sixty-fourth of the volume, which is 128 MB of an 8 GB disk for
    # 32,768 files. That is 1.6% for a limit nobody reaches by accident.
    # Small scratch images scale down the same way and stay mostly free
    # space.
    runtime_records = min(32768, max(64, total_clusters // 64))
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

    # --- directory indexes, and the ones that need a B-tree ---
    #
    # This has to happen after the loop above, because an index entry
    # quotes its child's allocated size and that is only known once
    # clusters are handed out. Every directory gets its entries built
    # and sorted here; the ones whose entries do not fit in an MFT
    # record get index blocks, and those blocks are clusters like any
    # other file's.
    for n in [root] + dirs:
        plan_index(n)
        if n.ix_blocks:
            n.ix_lcn = lcn
            lcn += len(n.ix_blocks)

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
    def index_attrs(node):
        """$INDEX_ROOT, and the two attributes that only exist when the
        directory needed a tree."""
        attrs = [attr_resident(AT_INDEX_ROOT,
                               index_root(node.ix_root, node.ix_last_child),
                               name="$I30")]
        if node.ix_blocks:
            nblk = len(node.ix_blocks)
            span = nblk * INDX_SIZE
            attrs.append(attr_nonresident(AT_INDEX_ALLOCATION,
                                          [(node.ix_lcn, nblk)],
                                          span, span, name="$I30"))
            # $BITMAP: one bit per index block, all of them in use. The
            # kernel reads this to find a free block when it splits a
            # node, so a volume that claimed none were used would hand
            # out a block that already holds a subtree.
            bits = bytearray(align(max(8, align(nblk, 8) // 8), 8))
            for i in range(nblk):
                bits[i // 8] |= 1 << (i % 8)
            attrs.append(attr_resident(AT_BITMAP, bytes(bits), name="$I30"))
        return attrs

    records[5] = mft_record(5, 5, [
        standard_information(),
        file_name_attr(ROOT_REF, ".", True),
    ] + index_attrs(root), is_dir=True, link_count=1)

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
            ] + index_attrs(n), is_dir=True)
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

        # The index blocks, before the file data purely for tidiness --
        # they are ordinary clusters and were allocated from the same
        # bump pointer.
        for n in [root] + dirs:
            if not n.ix_blocks:
                continue
            for vcn, (node_entries, last_child) in enumerate(n.ix_blocks):
                f.seek((n.ix_lcn + vcn) * CLUSTER)
                f.write(indx_block(vcn, node_entries, last_child))

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
