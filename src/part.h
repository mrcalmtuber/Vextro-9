#ifndef PART_H
#define PART_H

/*
 * src/part.h — how a disk says what is on it.
 *
 * There were two ways of finding a filesystem here, and both were
 * hard-coded into the filesystem drivers: try the whole device, then try
 * the four entries of an MBR. That works on a disk partitioned before
 * about 2010 and on nothing else. A GUID partition table describes up to
 * 128 partitions with 64-bit start and length, names them, types them by
 * UUID, and carries a copy of itself at the far end of the disk in case
 * the front is damaged. Every machine sold this decade uses one.
 *
 * Both schemes are parsed here, into one list, so the filesystems can
 * stop guessing. A GPT disk is recognised by the protective MBR that
 * precedes it — a single entry of type 0xEE covering the whole disk,
 * which exists precisely so that a tool that only understands MBRs sees
 * one full partition rather than empty space it might feel free to use.
 */

#include <stdint.h>
#include "blk.h"

#define PART_MAX      64
#define PART_NAME_LEN 40

typedef struct {
    uint64_t start;              /* first LBA                       */
    uint64_t sectors;
    uint8_t  mbr_type;           /* 0 when the entry came from GPT  */
    uint8_t  from_gpt;
    uint8_t  bootable;
    char     name[PART_NAME_LEN];
    uint8_t  type_guid[16];
} partition_t;

static partition_t part_table[PART_MAX];
static int         part_count = 0;
static int         part_scheme = 0;     /* 0 none, 1 MBR, 2 GPT */

/* The type GUIDs worth naming. Everything else is reported as its raw
 * value, because a partition this system cannot use is still a
 * partition and saying "unknown" is more honest than omitting it. */
static const uint8_t GUID_EFI_SYSTEM[16] = {
    0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B
};
static const uint8_t GUID_MS_BASIC[16] = {
    0xA2,0xA0,0xD0,0xEB,0xE5,0xB9,0x33,0x44,0x87,0xC0,0x68,0xB6,0xB7,0x26,0x99,0xC7
};
static const uint8_t GUID_LINUX_FS[16] = {
    0xAF,0x3D,0xC6,0x0F,0x83,0x84,0x72,0x47,0x8E,0x79,0x3D,0x69,0xD8,0x47,0x7D,0xE4
};

static int part_guid_eq(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 16; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static const char *part_type_name(const partition_t *p) {
    if (!p->from_gpt) {
        switch (p->mbr_type) {
        case 0x07: return "exFAT/NTFS";
        case 0x0B: case 0x0C: return "FAT32";
        case 0x83: return "Linux";
        case 0xEE: return "GPT protective";
        case 0xEF: return "EFI system";
        default:   return "unknown";
        }
    }
    if (part_guid_eq(p->type_guid, GUID_EFI_SYSTEM)) return "EFI system";
    if (part_guid_eq(p->type_guid, GUID_MS_BASIC))   return "basic data";
    if (part_guid_eq(p->type_guid, GUID_LINUX_FS))   return "Linux";
    return "unrecognised type";
}

/*
 * The GPT header, at LBA 1. Its own CRC is computed with the CRC field
 * zeroed, which is why the field is copied out and cleared before the
 * sum rather than skipped inside the loop.
 */
typedef struct {
    char     signature[8];       /* "EFI PART"          */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc;
    uint32_t reserved;
    uint64_t my_lba;
    uint64_t alt_lba;            /* the backup at the far end */
    uint64_t first_usable;
    uint64_t last_usable;
    uint8_t  disk_guid[16];
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entries_crc;
} __attribute__((packed)) gpt_header_t;

typedef struct {
    uint8_t  type_guid[16];
    uint8_t  part_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name[36];           /* UTF-16LE */
} __attribute__((packed)) gpt_entry_t;

/* CRC-32, the ordinary reflected one, table-free. A table would be a
 * kilobyte of .bss to save time on four kilobytes of input read once. */
static uint32_t part_crc32(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

static uint8_t part_sector[512];
static uint8_t part_entries[4096];

static int part_add(uint64_t start, uint64_t sectors) {
    if (part_count >= PART_MAX) return -1;
    partition_t *p = &part_table[part_count];
    for (uint64_t i = 0; i < sizeof(*p); i++) ((uint8_t *)p)[i] = 0;
    p->start   = start;
    p->sectors = sectors;
    part_count++;
    return part_count - 1;
}

static int part_scan_gpt(void) {
    if (blk_read(1, 1, part_sector) != 0) return 0;
    gpt_header_t h;
    for (uint64_t i = 0; i < sizeof(h); i++)
        ((uint8_t *)&h)[i] = part_sector[i];

    const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (h.signature[i] != sig[i]) return 0;
    if (h.header_size < 92 || h.header_size > 512) return 0;

    uint32_t want = h.header_crc;
    ((gpt_header_t *)part_sector)->header_crc = 0;
    uint32_t got = part_crc32(part_sector, h.header_size);
    if (got != want) {
        serial_puts("[part] GPT header CRC mismatch - ignoring it\n");
        return 0;
    }
    if (h.entry_size < sizeof(gpt_entry_t) || h.entry_size > 512) return 0;
    if (h.entry_count == 0 || h.entry_count > 512) return 0;

    part_scheme = 2;
    uint32_t per_sector = 512 / h.entry_size;
    uint32_t remaining  = h.entry_count;
    uint64_t lba = h.entries_lba;

    while (remaining && part_count < PART_MAX) {
        uint32_t batch = remaining > per_sector * 8 ? per_sector * 8 : remaining;
        uint32_t secs  = (batch + per_sector - 1) / per_sector;
        if (secs > 8) secs = 8;
        if (blk_read(lba, secs, part_entries) != 0) break;

        for (uint32_t i = 0; i < batch && part_count < PART_MAX; i++) {
            const gpt_entry_t *e =
                (const gpt_entry_t *)(part_entries + (uint64_t)i * h.entry_size);
            int empty = 1;
            for (int k = 0; k < 16; k++) if (e->type_guid[k]) empty = 0;
            if (empty) continue;
            if (e->last_lba < e->first_lba) continue;

            int idx = part_add(e->first_lba, e->last_lba - e->first_lba + 1);
            if (idx < 0) break;
            partition_t *p = &part_table[idx];
            p->from_gpt = 1;
            for (int k = 0; k < 16; k++) p->type_guid[k] = e->type_guid[k];
            /* The name is UTF-16; anything outside ASCII becomes a
             * question mark rather than half a character. */
            int n = 0;
            for (int k = 0; k < 36 && n < PART_NAME_LEN - 1; k++) {
                uint16_t c = e->name[k];
                if (!c) break;
                p->name[n++] = (c < 0x80) ? (char)c : '?';
            }
            p->name[n] = '\0';
        }
        remaining -= batch;
        lba += secs;
    }
    return part_count;
}

static int part_scan_mbr(void) {
    if (blk_read(0, 1, part_sector) != 0) return 0;
    if (part_sector[510] != 0x55 || part_sector[511] != 0xAA) return 0;

    int found = 0;
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = part_sector + 446 + i * 16;
        uint8_t type = e[4];
        if (!type) continue;
        uint32_t start = (uint32_t)e[8]  | ((uint32_t)e[9] << 8) |
                         ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                         ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);
        if (!count) continue;

        /* A protective entry is not a partition; it is a sign that the
         * real table is a GPT and this one exists to keep old tools out
         * of it. */
        if (type == 0xEE) return -1;

        int idx = part_add(start, count);
        if (idx < 0) break;
        part_table[idx].mbr_type = type;
        part_table[idx].bootable = (e[0] & 0x80) != 0;
        found++;
    }
    if (found) part_scheme = 1;
    return found;
}

static void part_scan(void) {
    part_count = 0;
    part_scheme = 0;
    if (!blk_present()) return;

    int mbr = part_scan_mbr();
    if (mbr == -1) {
        /* Protective MBR: the table is a GPT. */
        part_count = 0;
        if (!part_scan_gpt())
            serial_puts("[part] protective MBR but no usable GPT\n");
    } else if (mbr == 0) {
        /* No MBR at all — still worth looking, since a disk written by
         * a tool that skipped the protective entry is not unheard of. */
        part_scan_gpt();
    }

    serial_puts("[part] ");
    serial_puts(part_scheme == 2 ? "GPT" : part_scheme == 1 ? "MBR" : "no");
    serial_puts(" partition table, ");
    serial_put_dec((uint32_t)part_count);
    serial_puts(" partition(s)\n");

    for (int i = 0; i < part_count && i < 8; i++) {
        partition_t *p = &part_table[i];
        serial_puts("[part]   ");
        serial_put_dec((uint32_t)i);
        serial_puts(": ");
        serial_put_dec((uint32_t)(p->sectors / 2048));
        serial_puts(" MB at LBA ");
        serial_put_dec((uint32_t)p->start);
        serial_puts(", ");
        serial_puts(part_type_name(p));
        if (p->name[0]) {
            serial_puts(" \"");
            serial_puts(p->name);
            serial_puts("\"");
        }
        serial_puts("\n");
    }
}

#endif /* PART_H */
