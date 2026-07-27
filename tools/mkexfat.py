#!/usr/bin/env python3
"""Format an exFAT volume and populate it with files.

Pure stdlib — no mounting, no privileges, no mkfs.exfat.  The output is a
partition-less exFAT "superfloppy" (VBR at sector 0), which macOS, Linux,
Windows and the Socrates kernel all understand.

exFAT is what lifts FAT32's 4 GB per-file ceiling, which is the whole
reason the system volume moved over: a Wikipedia ZIM is far larger than
that.  The image is written sparsely, so an 8 GB volume costs only the
few megabytes actually used until something fills it.

Usage:
    mkexfat.py <out.img> <size_mb> [src[:dest/path]] ...

Everything written here is laid out contiguously and flagged NoFatChain,
which is legal exFAT and keeps the FAT itself empty; the kernel driver
handles both contiguous runs and real FAT chains.
"""
import os
import struct
import sys
import time

SECTOR = 512
SECTOR_SHIFT = 9


# ------------------------------------------------------------ checksums

def boot_checksum(data):
    """VBR checksum over sectors 0..10, skipping the three volatile bytes."""
    csum = 0
    for i, b in enumerate(data):
        if i in (106, 107, 112):        # VolumeFlags (2) and PercentInUse
            continue
        csum = (((csum << 31) | (csum >> 1)) + b) & 0xFFFFFFFF
    return csum


def table_checksum(data):
    """32-bit rotate-add, used for the up-case table."""
    csum = 0
    for b in data:
        csum = (((csum << 31) | (csum >> 1)) + b) & 0xFFFFFFFF
    return csum


def entry_set_checksum(entries):
    """16-bit rotate-add over a directory entry set, skipping the
    checksum field itself (bytes 2 and 3 of the first entry)."""
    csum = 0
    for i, b in enumerate(entries):
        if i in (2, 3):
            continue
        csum = (((csum << 15) | (csum >> 1)) + b) & 0xFFFF
    return csum


def name_hash(name, upcase):
    """Hash of the up-cased name in UTF-16LE, used to skip entry sets
    whose name cannot possibly match."""
    csum = 0
    for ch in name:
        u = upcase_char(ch, upcase)
        for b in struct.pack('<H', u):
            csum = (((csum << 15) | (csum >> 1)) + b) & 0xFFFF
    return csum


# ------------------------------------------------------------- up-case

def build_upcase():
    """A compressed up-case table covering ASCII.

    The compressed form is a run of uint16: the value 0xFFFF introduces a
    run of identity mappings whose length is the next uint16.  So this is
    "0x61 identity entries, then the 26 upper-case letters, then identity
    to the end of the BMP" — 60 bytes instead of 128 KB.
    """
    vals = [0xFFFF, 0x0061]                       # U+0000..U+0060 identity
    vals += list(range(0x0041, 0x005B))           # a-z -> A-Z
    vals += [0xFFFF, 0x10000 - 0x007B]            # U+007B.. identity
    return b''.join(struct.pack('<H', v) for v in vals)


def upcase_char(ch, _upcase):
    o = ord(ch)
    if 0x61 <= o <= 0x7A:
        return o - 0x20
    return o


# ---------------------------------------------------------- timestamps

def exfat_timestamp(mtime):
    t = time.localtime(mtime)
    year = max(t.tm_year, 1980)
    date = ((year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tim = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return (date << 16) | tim


# -------------------------------------------------------------- volume

class Volume:
    def __init__(self, size_mb):
        self.total = size_mb * 1024 * 1024 // SECTOR
        if self.total < 2048:
            raise SystemExit('volume too small (need at least 1 MB)')

        # 4 KB clusters for small volumes, 32 KB once it is worth it
        self.spc_shift = 3 if size_mb < 512 else 6
        self.spc = 1 << self.spc_shift

        # The boot regions occupy sectors 0..23; start the FAT after them
        # on a sensible alignment.
        self.fat_offset = 128

        # Converge FAT length and cluster count: each depends on the other
        cluster_count = (self.total - self.fat_offset) // self.spc
        for _ in range(64):
            fat_len = ((cluster_count + 2) * 4 + SECTOR - 1) // SECTOR
            fat_len = (fat_len + self.spc - 1) // self.spc * self.spc
            heap = self.fat_offset + fat_len
            heap = (heap + self.spc - 1) // self.spc * self.spc
            new_count = (self.total - heap) // self.spc
            if new_count == cluster_count:
                break
            cluster_count = new_count

        self.fat_length = fat_len
        self.heap_offset = heap
        self.cluster_count = cluster_count
        if self.cluster_count < 16:
            raise SystemExit('volume too small for exFAT')

        self.cluster_bytes = self.spc * SECTOR
        self.next_free = 2               # first allocatable cluster
        self.allocated = set()
        self.f = None

    # -- raw access ------------------------------------------------
    def write_sectors(self, lba, data):
        self.f.seek(lba * SECTOR)
        self.f.write(data)

    def cluster_lba(self, clus):
        return self.heap_offset + (clus - 2) * self.spc

    def alloc(self, nclusters):
        """Contiguous allocation; the image is built once, so there is
        never any fragmentation to work around."""
        first = self.next_free
        if first + nclusters - 2 > self.cluster_count:
            raise SystemExit('image is full')
        for c in range(first, first + nclusters):
            self.allocated.add(c)
        self.next_free += nclusters
        return first

    def write_cluster_data(self, first, data):
        self.f.seek(self.cluster_lba(first) * SECTOR)
        self.f.write(data)

    # -- structures ------------------------------------------------
    def boot_region(self, serial):
        vbr = bytearray(SECTOR)
        vbr[0:3] = b'\xEB\x76\x90'
        vbr[3:11] = b'EXFAT   '
        # 0x0B..0x3F MustBeZero
        struct.pack_into('<Q', vbr, 0x40, 0)                  # PartitionOffset
        struct.pack_into('<Q', vbr, 0x48, self.total)         # VolumeLength
        struct.pack_into('<I', vbr, 0x50, self.fat_offset)
        struct.pack_into('<I', vbr, 0x54, self.fat_length)
        struct.pack_into('<I', vbr, 0x58, self.heap_offset)
        struct.pack_into('<I', vbr, 0x5C, self.cluster_count)
        struct.pack_into('<I', vbr, 0x60, self.root_cluster)
        struct.pack_into('<I', vbr, 0x64, serial)
        struct.pack_into('<H', vbr, 0x68, 0x0100)             # revision 1.00
        struct.pack_into('<H', vbr, 0x6A, 0)                  # VolumeFlags
        vbr[0x6C] = SECTOR_SHIFT
        vbr[0x6D] = self.spc_shift
        vbr[0x6E] = 1                                         # NumberOfFats
        vbr[0x6F] = 0x80                                      # DriveSelect
        used = len(self.allocated) * 100 // max(self.cluster_count, 1)
        vbr[0x70] = min(used, 100)                            # PercentInUse
        vbr[0x1FE] = 0x55
        vbr[0x1FF] = 0xAA

        ext = bytearray(SECTOR)
        struct.pack_into('<I', ext, SECTOR - 4, 0xAA550000)

        region = bytes(vbr) + bytes(ext) * 8          # 0, 1..8
        region += bytes(SECTOR)                       # 9  OEM parameters
        region += bytes(SECTOR)                       # 10 reserved

        csum = boot_checksum(region)
        region += struct.pack('<I', csum) * (SECTOR // 4)     # 11 checksum
        return region


def dirent_file(attrs, secondary_count, mtime):
    e = bytearray(32)
    e[0] = 0x85
    e[1] = secondary_count
    struct.pack_into('<H', e, 4, attrs)
    ts = exfat_timestamp(mtime)
    struct.pack_into('<I', e, 8, ts)      # create
    struct.pack_into('<I', e, 12, ts)     # last modified
    struct.pack_into('<I', e, 16, ts)     # last accessed
    return e


def dirent_stream(name_len, hash_, first_cluster, length, contiguous=True):
    e = bytearray(32)
    e[0] = 0xC0
    e[1] = 0x01 | (0x02 if contiguous else 0)   # AllocationPossible | NoFatChain
    e[3] = name_len
    struct.pack_into('<H', e, 4, hash_)
    struct.pack_into('<Q', e, 8, length)        # ValidDataLength
    struct.pack_into('<I', e, 20, first_cluster)
    struct.pack_into('<Q', e, 24, length)       # DataLength
    return e


def dirent_name(chunk):
    e = bytearray(32)
    e[0] = 0xC1
    enc = chunk.encode('utf-16-le')
    e[2:2 + len(enc)] = enc
    return e


def make_entry_set(name, attrs, first_cluster, length, mtime, upcase,
                   contiguous=True):
    chunks = [name[i:i + 15] for i in range(0, len(name), 15)]
    entries = dirent_file(attrs, 1 + len(chunks), mtime)
    entries += dirent_stream(len(name), name_hash(name, upcase),
                             first_cluster, length, contiguous)
    for c in chunks:
        entries += dirent_name(c)
    csum = entry_set_checksum(entries)
    struct.pack_into('<H', entries, 2, csum)
    return bytes(entries)


class Dir:
    """A directory being built: a list of entry sets plus subdirectories."""

    def __init__(self, name=None):
        self.name = name
        self.children = {}          # name -> Dir
        self.files = []             # (name, bytes, mtime)
        self.cluster = None
        self.size = 0

    def child(self, name):
        if name not in self.children:
            self.children[name] = Dir(name)
        return self.children[name]


def layout(vol, d, upcase, is_root=False, label='SOCRATES'):
    """Depth-first: place every file and subdirectory, then build this
    directory's own entry stream.  Sizes are known before the parent
    writes its entry set because children are laid out first."""
    body = bytearray()

    if is_root:
        # volume label
        e = bytearray(32)
        e[0] = 0x83
        enc = label.encode('utf-16-le')
        e[1] = len(label)
        e[2:2 + len(enc)] = enc
        body += e
        # allocation bitmap + up-case table entries are patched in later
        body += bytes(32)      # placeholder: bitmap
        body += bytes(32)      # placeholder: up-case

    for name, data, mtime in d.files:
        nclus = max(1, (len(data) + vol.cluster_bytes - 1) // vol.cluster_bytes)
        first = vol.alloc(nclus)
        padded = data + bytes(nclus * vol.cluster_bytes - len(data))
        vol.write_cluster_data(first, padded)
        body += make_entry_set(name, 0x20, first, len(data), mtime, upcase)

    for name, sub in d.children.items():
        layout(vol, sub, upcase)
        body += make_entry_set(name, 0x10, sub.cluster, sub.size,
                               time.time(), upcase)

    # round the directory stream up to whole clusters
    nclus = max(1, (len(body) + vol.cluster_bytes - 1) // vol.cluster_bytes)
    d.cluster = vol.alloc(nclus)
    d.size = nclus * vol.cluster_bytes
    d.body = bytes(body) + bytes(d.size - len(body))
    return d


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2

    out_path = sys.argv[1]
    size_mb = int(sys.argv[2])
    specs = sys.argv[3:]

    vol = Volume(size_mb)
    upcase_data = build_upcase()

    root = Dir()
    for spec in specs:
        src, _, dest = spec.partition(':')
        if not dest:
            dest = os.path.basename(src)
        data = open(src, 'rb').read()
        mtime = os.path.getmtime(src)
        parts = dest.strip('/').split('/')
        d = root
        for p in parts[:-1]:
            d = d.child(p)
        d.files.append((parts[-1], data, mtime))

    with open(out_path, 'wb') as f:
        vol.f = f
        # sparse: set the length without writing 8 GB of zeros
        f.truncate(vol.total * SECTOR)

        # cluster 2 = allocation bitmap, then up-case, then the tree
        bitmap_bytes = (vol.cluster_count + 7) // 8
        bitmap_clusters = max(1, (bitmap_bytes + vol.cluster_bytes - 1)
                              // vol.cluster_bytes)
        bitmap_first = vol.alloc(bitmap_clusters)

        upcase_clusters = max(1, (len(upcase_data) + vol.cluster_bytes - 1)
                              // vol.cluster_bytes)
        upcase_first = vol.alloc(upcase_clusters)
        vol.write_cluster_data(
            upcase_first,
            upcase_data + bytes(upcase_clusters * vol.cluster_bytes
                                - len(upcase_data)))

        layout(vol, root, upcase_data, is_root=True)
        vol.root_cluster = root.cluster

        # patch the bitmap and up-case entries into the root stream
        body = bytearray(root.body)
        e = bytearray(32)
        e[0] = 0x81                                   # allocation bitmap
        struct.pack_into('<I', e, 20, bitmap_first)
        struct.pack_into('<Q', e, 24, bitmap_bytes)
        body[32:64] = e

        e = bytearray(32)
        e[0] = 0x82                                   # up-case table
        struct.pack_into('<I', e, 4, table_checksum(upcase_data))
        struct.pack_into('<I', e, 20, upcase_first)
        struct.pack_into('<Q', e, 24, len(upcase_data))
        body[64:96] = e
        root.body = bytes(body)

        vol.write_cluster_data(root.cluster, root.body)

        def write_dirs(d):
            if d is not root:
                vol.write_cluster_data(d.cluster, d.body)
            for sub in d.children.values():
                write_dirs(sub)
        write_dirs(root)

        # allocation bitmap: one bit per cluster from cluster 2
        bitmap = bytearray(bitmap_clusters * vol.cluster_bytes)
        for c in vol.allocated:
            idx = c - 2
            bitmap[idx >> 3] |= 1 << (idx & 7)
        vol.write_cluster_data(bitmap_first, bitmap)

        # FAT: reserved entries only — every file above is contiguous and
        # flagged NoFatChain, so no chains exist to record.
        fat = bytearray(SECTOR)
        struct.pack_into('<I', fat, 0, 0xFFFFFFF8)
        struct.pack_into('<I', fat, 4, 0xFFFFFFFF)
        vol.write_sectors(vol.fat_offset, bytes(fat))

        serial = int(time.time()) & 0xFFFFFFFF
        region = vol.boot_region(serial)
        vol.write_sectors(0, region)          # main boot region
        vol.write_sectors(12, region)         # backup boot region

    used_mb = len(vol.allocated) * vol.cluster_bytes / (1024 * 1024)
    for spec in specs:
        src, _, dest = spec.partition(':')
        print(f'  + {dest or os.path.basename(src)}  '
              f'({os.path.getsize(src)} bytes)')
    print(f'{out_path}: exFAT, {size_mb} MB, {vol.cluster_count} clusters of '
          f'{vol.cluster_bytes // 1024} KB, {used_mb:.1f} MB used')
    return 0


if __name__ == '__main__':
    sys.exit(main())
