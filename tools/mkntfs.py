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
    python3 tools/mkntfs.py <image> <size-MB> [name=path ...]
"""

import os
import struct
import sys

SECTOR = 512
SPC = 8                       # sectors per cluster -> 4 KB clusters
CLUSTER = SECTOR * SPC
MFT_REC = 1024                # bytes per MFT record
MFT_RECS_RESERVED = 16        # the system files occupy 0..15

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

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1

    path = sys.argv[1]
    size_mb = int(sys.argv[2])
    files = []
    for spec in sys.argv[3:]:
        if "=" not in spec:
            print(f"  skipping {spec!r}: expected name=path")
            continue
        name, src = spec.split("=", 1)
        if not os.path.isfile(src):
            print(f"  skipping {name}: {src} not found")
            continue
        files.append((name, src))

    total_sectors = size_mb * 1024 * 1024 // SECTOR
    total_clusters = total_sectors // SPC

    # --- layout ---
    # cluster 0        boot
    # cluster 4        $MFT   (grows upward)
    # then             file data
    mft_lcn = 4
    mft_clusters = 64                       # 64 KB of records: 256 records
    mftmirr_lcn = mft_lcn + mft_clusters
    data_start = mftmirr_lcn + 1

    # Place each file's data.
    placed = []
    lcn = data_start
    for name, src in files:
        size = os.path.getsize(src)
        n = max(1, align(size, CLUSTER) // CLUSTER)
        placed.append((name, src, size, lcn, n))
        lcn += n
    data_end = lcn

    if data_end >= total_clusters:
        raise SystemExit("content does not fit in the requested volume")

    # --- the bitmap: one bit per cluster ---
    bitmap = bytearray(align(total_clusters, 8) // 8)

    def mark(start, count):
        for c in range(start, start + count):
            bitmap[c >> 3] |= 1 << (c & 7)

    mark(0, data_end)                       # everything laid out above
    bitmap_bytes = bytes(bitmap)
    bitmap_clusters = max(1, align(len(bitmap_bytes), CLUSTER) // CLUSTER)
    bitmap_lcn = data_end
    mark(bitmap_lcn, bitmap_clusters)
    bitmap_bytes = bytes(bitmap)             # re-read after marking itself
    volume_end = bitmap_lcn + bitmap_clusters

    # --- MFT records ---
    records = {}

    # 0: $MFT
    records[0] = mft_record(0, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$MFT", False),
        attr_nonresident(AT_DATA, [(mft_lcn, mft_clusters)],
                         mft_clusters * CLUSTER, mft_clusters * CLUSTER),
    ])

    # 1: $MFTMirr -- the first four records, duplicated
    records[1] = mft_record(1, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$MFTMirr", False),
        attr_nonresident(AT_DATA, [(mftmirr_lcn, 1)], CLUSTER, CLUSTER),
    ])

    # 2: $LogFile.
    #
    # Allocated and zeroed, not populated. NTFS's log is an LFS
    # transaction journal whose restart and redo record formats are not
    # publicly specified, so what a real one contains cannot be written
    # from documentation. The kernel journals its own metadata writes
    # instead -- see ntfs_journal_* in ntfs_ops.c -- and this file
    # exists so that a Windows chkdsk sees a well-formed volume rather
    # than a missing system file.
    records[2] = mft_record(2, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$LogFile", False),
        attr_resident(AT_DATA, b""),
    ])

    # 3: $Volume
    vol_name = attr_resident(0x60, utf16("Vextro"))
    vol_info = attr_resident(0x70, struct.pack("<QBBH", 0, 3, 1, 0))
    records[3] = mft_record(3, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$Volume", False),
        vol_name, vol_info,
        attr_resident(AT_DATA, b""),
    ])

    # 4: $AttrDef
    records[4] = mft_record(4, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$AttrDef", False),
        attr_resident(AT_DATA, b""),
    ])

    # 5: the root directory
    entries = []
    for i, (name, src, size, flcn, fclu) in enumerate(placed):
        ref = (MFT_RECS_RESERVED + i) | (1 << 48)
        entries.append(index_entry(ref, name, False, size, fclu * CLUSTER))
    records[5] = mft_record(5, 5, [
        standard_information(),
        file_name_attr(5 | (5 << 48), ".", True),
        attr_resident(AT_INDEX_ROOT, index_root(entries), name="$I30"),
    ], is_dir=True, link_count=1)

    # 6: $Bitmap
    records[6] = mft_record(6, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$Bitmap", False),
        attr_nonresident(AT_DATA, [(bitmap_lcn, bitmap_clusters)],
                         len(bitmap_bytes), bitmap_clusters * CLUSTER),
    ])

    # 7: $Boot
    records[7] = mft_record(7, 1, [
        standard_information(),
        file_name_attr(5 | (5 << 48), "$Boot", False),
        attr_nonresident(AT_DATA, [(0, 1)], CLUSTER, CLUSTER),
    ])

    # 8..15: the remaining reserved records, present but empty
    for n, nm in ((8, "$BadClus"), (9, "$Secure"), (10, "$UpCase"),
                  (11, "$Extend"), (12, ""), (13, ""), (14, ""), (15, "")):
        attrs = [standard_information()]
        if nm:
            attrs.append(file_name_attr(5 | (5 << 48), nm, nm == "$Extend"))
        attrs.append(attr_resident(AT_DATA, b""))
        records[n] = mft_record(n, 1, attrs, is_dir=(nm == "$Extend"))

    # 16..: the packed files
    for i, (name, src, size, flcn, fclu) in enumerate(placed):
        n = MFT_RECS_RESERVED + i
        records[n] = mft_record(n, 1, [
            standard_information(),
            file_name_attr(5 | (5 << 48), name, False, size, fclu * CLUSTER),
            attr_nonresident(AT_DATA, [(flcn, fclu)], size, fclu * CLUSTER),
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
    # A negative "clusters per record" means 2^-n bytes, which is how a
    # 1024-byte record is expressed on a volume with 4096-byte clusters.
    struct.pack_into("<b", boot, 0x40, -10)
    struct.pack_into("<b", boot, 0x44, 1)
    struct.pack_into("<Q", boot, 0x48, 0x56455854524F0009)
    struct.pack_into("<H", boot, 0x1FE, 0xAA55)

    # --- write it ---
    with open(path, "wb") as f:
        f.truncate(size_mb * 1024 * 1024)

        f.seek(0)
        f.write(boot)

        f.seek(mft_lcn * CLUSTER)
        for n in range(highest + 1):
            f.seek(mft_lcn * CLUSTER + n * MFT_REC)
            f.write(records.get(n, bytes(MFT_REC)))

        # the mirror: the first four records
        f.seek(mftmirr_lcn * CLUSTER)
        for n in range(4):
            f.write(records[n])

        for name, src, size, flcn, fclu in placed:
            f.seek(flcn * CLUSTER)
            with open(src, "rb") as g:
                while True:
                    chunk = g.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)

        f.seek(bitmap_lcn * CLUSTER)
        f.write(bitmap_bytes)

        # the backup boot sector, which lives in the last sector
        f.seek((total_sectors - 1) * SECTOR)
        f.write(boot)

    used_mb = volume_end * CLUSTER // (1024 * 1024)
    print(f"  NTFS   {path}: {size_mb} MB volume, {total_clusters} clusters "
          f"of {CLUSTER}")
    print(f"         $MFT at cluster {mft_lcn}, {highest + 1} records, "
          f"{len(placed)} files, {used_mb} MB used")
    return 0


if __name__ == "__main__":
    sys.exit(main())
