#!/usr/bin/env python3
"""Format a FAT32 "superfloppy" disk image and populate it with files.

Pure stdlib — no mtools, no mounting.  Layout is a partition-less FAT32
volume (VBR at sector 0), which macOS/Linux/mtools and the Socrates
kernel all understand.

Usage:
    mkfat32.py <out.img> <size_mb> [src[:dest/path]] ...

Destination paths may contain subdirectories (created on the fly).
All names must fit 8.3 (the kernel and this tool only create short
names; lowercase is preserved via the NT case flags).
"""
import os
import struct
import sys
import time

SECTOR = 512
RESVD = 32
NFATS = 2
SPC = 1                       # sectors per cluster (512 B clusters)
EOC = 0x0FFFFFFF


def fat_timestamp(mtime):
    t = time.localtime(mtime)
    year = max(t.tm_year, 1980)
    date = ((year - 1980) << 9) | (t.tm_mon << 5) | t.tm_mday
    tim = (t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)
    return date, tim


def short_name(name):
    """Return (11-byte 8.3 field, nt_flags) or raise if it doesn't fit."""
    name = name.strip()
    if name in ('.', '..'):
        raise ValueError(name)
    if '.' in name:
        base, _, ext = name.rpartition('.')
    else:
        base, ext = name, ''
    if not base or len(base) > 8 or len(ext) > 3:
        raise ValueError(f'name does not fit 8.3: {name}')
    for c in base + ext:
        if c in '+,;=[]*?<>|":/\\' or ord(c) < 0x20 or ord(c) > 0x7E:
            raise ValueError(f'bad character in name: {name}')
    nt = 0
    if base and base == base.lower():
        nt |= 0x08
    if ext and ext == ext.lower():
        nt |= 0x10
    if (base != base.lower() and base != base.upper()) or \
       (ext and ext != ext.lower() and ext != ext.upper()):
        raise ValueError(f'mixed-case name needs LFN (unsupported): {name}')
    field = (base.upper().ljust(8) + ext.upper().ljust(3)).encode('ascii')
    return field, nt


class Volume:
    def __init__(self, size_mb):
        self.total = size_mb * 1024 * 1024 // SECTOR
        # iterate FAT size: clusters = (total - resvd - nfats*fatsz) / spc
        fatsz = 1
        for _ in range(64):
            clusters = (self.total - RESVD - NFATS * fatsz) // SPC
            need = (clusters + 2) * 4
            new_fatsz = (need + SECTOR - 1) // SECTOR
            if new_fatsz == fatsz:
                break
            fatsz = new_fatsz
        self.fatsz = fatsz
        self.nclusters = (self.total - RESVD - NFATS * fatsz) // SPC
        # the FAT must be able to map every data cluster
        self.nclusters = min(self.nclusters, fatsz * SECTOR // 4 - 2)
        if self.nclusters < 65525:
            raise SystemExit(f'volume too small for FAT32 '
                             f'({self.nclusters} clusters < 65525); '
                             f'use a bigger size')
        self.data_lba = RESVD + NFATS * fatsz
        self.img = bytearray(self.total * SECTOR)
        self.fat = [0] * (self.nclusters + 2)
        self.fat[0] = 0x0FFFFFF8
        self.fat[1] = EOC
        self.fat[2] = EOC                       # root directory
        self.next_free = 3
        # dir tree: cluster -> bytearray of entries
        self.dirs = {2: bytearray()}
        self.dirs[2] += struct.pack('<11sB', b'SOCRATES   ', 0x08) + bytes(20)

    def alloc_chain(self, nclus):
        chain = []
        for _ in range(nclus):
            c = self.next_free
            if c >= len(self.fat):
                raise SystemExit('image full')
            self.fat[c] = EOC
            if chain:
                self.fat[chain[-1]] = c
            chain.append(c)
            self.next_free = c + 1
        return chain

    def write_clusters(self, chain, data):
        cs = SPC * SECTOR
        for i, c in enumerate(chain):
            lba = self.data_lba + (c - 2) * SPC
            piece = data[i * cs:(i + 1) * cs]
            self.img[lba * SECTOR:lba * SECTOR + len(piece)] = piece

    def dirent(self, name, attr, first_clus, size, mtime):
        field, nt = short_name(name)
        date, tim = fat_timestamp(mtime)
        return struct.pack('<11sBBBHHHHHHHI',
                           field, attr, nt,
                           0, tim, date,          # create (tenths, time, date)
                           date,                  # access date
                           (first_clus >> 16) & 0xFFFF,
                           tim, date,             # write time/date
                           first_clus & 0xFFFF,
                           size)

    def ensure_dir(self, path, mtime):
        """path is a list of components; returns dir cluster."""
        cur = 2
        for comp in path:
            found = None
            d = self.dirs[cur]
            for off in range(0, len(d), 32):
                ent = d[off:off + 32]
                if ent[11] == 0x10:
                    nm = ent[0:8].decode().rstrip()
                    ex = ent[8:11].decode().rstrip()
                    full = nm + ('.' + ex if ex else '')
                    if full.lower() == comp.lower():
                        found = ((struct.unpack('<H', ent[20:22])[0] << 16) |
                                 struct.unpack('<H', ent[26:28])[0])
                        break
            if found is None:
                chain = self.alloc_chain(1)
                clus = chain[0]
                self.dirs[cur] += self.dirent(comp, 0x10, clus, 0, mtime)
                dot = self.dirent('A', 0x10, clus, 0, mtime)
                dot = b'.' + b' ' * 10 + dot[11:20] + dot[20:]
                dotdot_clus = 0 if cur == 2 else cur
                dd = self.dirent('A', 0x10, dotdot_clus, 0, mtime)
                dd = b'..' + b' ' * 9 + dd[11:20] + dd[20:]
                self.dirs[clus] = bytearray(dot + dd)
                found = clus
            cur = found
        return cur

    def add_file(self, src, dest):
        with open(src, 'rb') as f:
            data = f.read()
        mtime = os.path.getmtime(src)
        parts = [p for p in dest.split('/') if p]
        dirname, fname = parts[:-1], parts[-1]
        dclus = self.ensure_dir(dirname, mtime)
        nclus = (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR)
        first = 0
        if nclus:
            chain = self.alloc_chain(nclus)
            self.write_clusters(chain, data)
            first = chain[0]
        self.dirs[dclus] += self.dirent(fname, 0x20, first, len(data), mtime)
        print(f'  + {dest}  ({len(data)} bytes)')

    def finish(self, out_path):
        # directory clusters (may grow beyond 1 cluster)
        cs = SPC * SECTOR
        for clus, data in sorted(self.dirs.items()):
            need = max(1, (len(data) + cs - 1) // cs)
            chain = [clus]
            while len(chain) < need:
                nxt = self.alloc_chain(1)[0]
                self.fat[chain[-1]] = nxt
                chain.append(nxt)
            self.write_clusters(chain, bytes(data))

        # boot sector
        bpb = bytearray(SECTOR)
        bpb[0:3] = b'\xEB\x58\x90'
        bpb[3:11] = b'SOCRATES'
        struct.pack_into('<H', bpb, 11, SECTOR)          # bytes/sector
        bpb[13] = SPC
        struct.pack_into('<H', bpb, 14, RESVD)
        bpb[16] = NFATS
        struct.pack_into('<H', bpb, 17, 0)               # root entries (0)
        struct.pack_into('<H', bpb, 19, 0)               # totsec16
        bpb[21] = 0xF8                                   # media
        struct.pack_into('<H', bpb, 22, 0)               # fatsz16
        struct.pack_into('<H', bpb, 24, 63)              # sec/track
        struct.pack_into('<H', bpb, 26, 255)             # heads
        struct.pack_into('<I', bpb, 28, 0)               # hidden
        struct.pack_into('<I', bpb, 32, self.total)      # totsec32
        struct.pack_into('<I', bpb, 36, self.fatsz)      # fatsz32
        struct.pack_into('<H', bpb, 40, 0)               # ext flags
        struct.pack_into('<H', bpb, 42, 0)               # fs version
        struct.pack_into('<I', bpb, 44, 2)               # root cluster
        struct.pack_into('<H', bpb, 48, 1)               # fsinfo sector
        struct.pack_into('<H', bpb, 50, 6)               # backup boot
        bpb[64] = 0x80                                   # drive number
        bpb[66] = 0x29                                   # boot signature
        struct.pack_into('<I', bpb, 67, int(time.time()) & 0xFFFFFFFF)
        bpb[71:82] = b'SOCRATES   '
        bpb[82:90] = b'FAT32   '
        bpb[510] = 0x55
        bpb[511] = 0xAA
        self.img[0:SECTOR] = bpb
        self.img[6 * SECTOR:7 * SECTOR] = bpb            # backup

        # FSInfo
        used = sum(1 for i in range(2, self.nclusters + 2) if self.fat[i])
        fsi = bytearray(SECTOR)
        struct.pack_into('<I', fsi, 0, 0x41615252)
        struct.pack_into('<I', fsi, 484, 0x61417272)
        struct.pack_into('<I', fsi, 488, self.nclusters - used)
        struct.pack_into('<I', fsi, 492, self.next_free)
        fsi[510] = 0x55
        fsi[511] = 0xAA
        self.img[1 * SECTOR:2 * SECTOR] = fsi
        self.img[7 * SECTOR:8 * SECTOR] = fsi            # backup

        # FATs
        fat_bytes = bytearray(self.fatsz * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into('<I', fat_bytes, i * 4, v & 0x0FFFFFFF)
        for n in range(NFATS):
            base = (RESVD + n * self.fatsz) * SECTOR
            self.img[base:base + len(fat_bytes)] = fat_bytes

        with open(out_path, 'wb') as f:
            f.write(self.img)
        free_mb = (self.nclusters - used) * SPC * SECTOR // (1024 * 1024)
        print(f'{out_path}: FAT32, {self.total * SECTOR // (1024*1024)} MB, '
              f'{self.nclusters} clusters, {free_mb} MB free')


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    out, size_mb = sys.argv[1], int(sys.argv[2])
    vol = Volume(size_mb)
    for spec in sys.argv[3:]:
        if ':' in spec:
            src, dest = spec.split(':', 1)
        else:
            src, dest = spec, os.path.basename(spec)
        vol.add_file(src, dest)
    vol.finish(out)


if __name__ == '__main__':
    main()
