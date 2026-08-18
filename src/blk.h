#ifndef BLK_H
#define BLK_H

#include <stdint.h>
#include "pci.h"
#include "ata.h"     /* legacy PIO, ports 0x1F0                          */
#include "ahci.h"    /* SATA                                             */
#include "nvme.h"
#include "kheap.h"    /* PCIe SSD                                         */

/*
 * The block layer: one disk-shaped hole for three very different drivers
 * to fit into.
 *
 * exfat.h and fat32.h used to call ata_read() by name.  That was true
 * enough while there was exactly one way to reach a disk, and it is the
 * reason adding a second one is a layer rather than an #ifdef: the
 * filesystems want sectors, not a bus.
 *
 * Everything above this line is addressed in 512-byte sectors, always,
 * whatever the hardware underneath thinks a block is.  That is a real
 * commitment and not just a convention — NVMe namespaces with 4 KB
 * blocks exist and are becoming normal, and nvme.h earns the promise
 * with a read-modify-write rather than this file pretending the case
 * away.
 *
 * Multiple disks are enumerated rather than the first one winning,
 * because "the disk" is not a thing on a real machine: a laptop with an
 * NVMe system drive and a SATA data drive is ordinary, and which one
 * holds the volume we want is not knowable until something tries to
 * mount it.  So fs_mount() walks them.
 */

#define BLK_NONE  0
#define BLK_NVME  1
#define BLK_AHCI  2
#define BLK_ATA   3

#define BLK_MAX_DEV 6

typedef struct {
    uint8_t  kind;
    uint8_t  unit;         /* namespace index, AHCI disk index, or 0      */
    uint64_t sectors;      /* 512-byte sectors                            */
    const char *bus;
} blk_dev_t;

static blk_dev_t blk_devs[BLK_MAX_DEV];
static int blk_count = 0;
static int blk_cur = -1;

static void blk_add(uint8_t kind, uint8_t unit, uint64_t sectors, const char *bus) {
    if (blk_count >= BLK_MAX_DEV || sectors == 0) return;
    blk_devs[blk_count].kind    = kind;
    blk_devs[blk_count].unit    = unit;
    blk_devs[blk_count].sectors = sectors;
    blk_devs[blk_count].bus     = bus;
    blk_count++;
}

/*
 * Probe every bus, newest first.
 *
 * The order is not a preference so much as a statement about what a
 * machine that has both is likely doing: a system with an NVMe drive
 * keeps the OS on it, and a SATA disk alongside is storage.  Nothing
 * here depends on being right, since fs_mount() tries them all — it only
 * decides which one gets tried first, and therefore which one an
 * ambiguous machine boots from.
 */
static void blk_init(void) {
    blk_count = 0;
    blk_cur   = -1;

    int n = nvme_init();
    for (int i = 0; i < n; i++)
        blk_add(BLK_NVME, (uint8_t)i, nvme_capacity(i), "NVMe");

    n = ahci_init();
    for (int i = 0; i < n; i++)
        blk_add(BLK_AHCI, (uint8_t)i, ahci_capacity(i), "SATA");

    ata_init();
    if (ata_present)
        blk_add(BLK_ATA, 0, ata_sectors, "IDE");

    if (blk_count > 0) blk_cur = 0;

    serial_puts("[blk] ");
    serial_put_dec((uint32_t)blk_count);
    serial_puts(blk_count == 1 ? " disk\n" : " disks\n");
}

static int blk_select(int idx) {
    if (idx < 0 || idx >= blk_count) return -1;
    blk_cur = idx;
    return 0;
}

static int blk_present(void) { return blk_cur >= 0; }

static uint64_t blk_sectors(void) {
    return blk_cur >= 0 ? blk_devs[blk_cur].sectors : 0;
}

static const char *blk_bus_name(void) {
    return blk_cur >= 0 ? blk_devs[blk_cur].bus : "none";
}

/*
 * The dispatch itself.  A switch rather than a table of function
 * pointers: there are three cases, they are known at compile time, and a
 * function pointer in .bss would be one more thing to get wrong in a
 * kernel that links with -static -pie.
 */
/* ===== BAD BLOCK REMAPPING =====
 *
 * A disk that fails a read does not usually fail everywhere. One sector
 * goes bad, and every access to the file containing it fails forever
 * afterwards -- including, on a filesystem, accesses to the directory
 * that names a hundred other files that are perfectly fine.
 *
 * A remap table is what turns that from fatal into merely lossy: the
 * failing sector is recorded, a spare is assigned from a reserved area
 * at the end of the volume, and every later access to the original goes
 * to the spare instead. The data in the bad sector is gone -- nothing
 * can bring it back -- but the sector itself works again, and a
 * filesystem that rewrites it recovers completely.
 *
 * The table is in memory only. Persisting it needs a place on the disk
 * to persist it to, and reserving one changes the volume layout, which
 * is a bigger decision than this. What it does mean is that the damage
 * is contained for as long as the machine is up and reported when it
 * happens rather than presented as a read that simply failed.
 */
#define BLK_REMAP_MAX 256

static struct {
    uint64_t bad;                 /* the LBA that failed          */
    uint64_t spare;               /* where it goes now            */
    uint32_t failures;            /* how many times before we gave up */
} blk_remap[BLK_REMAP_MAX];
static int      blk_remap_count = 0;
static uint64_t blk_spare_next = 0;
static uint64_t blk_read_errors = 0;

static uint64_t blk_remap_lookup(uint64_t lba) {
    for (int i = 0; i < blk_remap_count; i++)
        if (blk_remap[i].bad == lba) return blk_remap[i].spare;
    return lba;
}

/* Raw access, below the remapping and the cache. */
static int blk_read_raw(uint64_t lba, uint32_t count, void *buf) {
    const blk_dev_t *d = &blk_devs[blk_cur];
    switch (d->kind) {
        case BLK_NVME: return nvme_read(d->unit, lba, count, buf);
        case BLK_AHCI: return ahci_read(d->unit, lba, count, buf);
        case BLK_ATA:  return ata_read(lba, count, buf);
    }
    return -1;
}

static int blk_write_raw(uint64_t lba, uint32_t count, const void *buf) {
    const blk_dev_t *d = &blk_devs[blk_cur];
    switch (d->kind) {
        case BLK_NVME: return nvme_write(d->unit, lba, count, buf);
        case BLK_AHCI: return ahci_write(d->unit, lba, count, buf);
        case BLK_ATA:  return ata_write(lba, count, buf);
    }
    return -1;
}

/*
 * Retry a failed sector, then give up on it and assign a spare.
 *
 * Three attempts, because a marginal sector often reads on the second
 * try and a disk that is merely busy always does. Only after that is the
 * sector written off, and the caller still gets an error for *this*
 * read: the data is gone. What changes is that the next read of the same
 * sector goes somewhere that works.
 */
static int blk_retire_sector(uint64_t lba) {
    static uint8_t scratch[512];
    for (int attempt = 0; attempt < 3; attempt++)
        if (blk_read_raw(lba, 1, scratch) == 0) return 0;

    blk_read_errors++;
    if (blk_remap_count >= BLK_REMAP_MAX) return -1;
    if (!blk_spare_next) {
        /* The last thousand sectors, which no filesystem here places
         * anything in. */
        uint64_t total = blk_devs[blk_cur].sectors;
        if (total < 4096) return -1;
        blk_spare_next = total - 1024;
    }
    blk_remap[blk_remap_count].bad      = lba;
    blk_remap[blk_remap_count].spare    = blk_spare_next++;
    blk_remap[blk_remap_count].failures = 3;
    blk_remap_count++;

    serial_puts("[blk] sector ");
    serial_put_dec((uint32_t)lba);
    serial_puts(" failed three reads - remapped to a spare; "
                "its contents are lost\n");
    return -1;
}

/* ===== READ-AHEAD CACHE =====
 *
 * Filesystems read in sectors and files are laid out in runs, so a read
 * of sector n is very nearly a promise that n+1 is next. Fetching a
 * whole cluster's worth on the first access and serving the rest from
 * memory turns a sequence of single-sector commands -- each one a full
 * round trip to the device -- into one command and a memory copy.
 *
 * The prediction is deliberately simple: a hit at the end of the cached
 * run extends the run forward. Anything cleverer needs history the
 * access pattern does not justify, and a wrong prediction costs a read
 * that was not needed.
 */
#define BLK_CACHE_SECTORS 64
#define BLK_CACHE_LINES   16

typedef struct {
    uint64_t base;                /* first LBA held, ~0 when empty */
    uint32_t count;
    uint64_t used;                /* for least-recently-used eviction */
    uint8_t  data[BLK_CACHE_SECTORS * 512];
} blk_cache_line_t;

static blk_cache_line_t *blk_cache = 0;
static uint64_t blk_cache_clock = 0;
static uint64_t blk_cache_hits = 0, blk_cache_misses = 0, blk_readahead = 0;

static void blk_cache_init(void) {
    if (blk_cache) return;
    blk_cache = (blk_cache_line_t *)kmalloc_paged(
        sizeof(blk_cache_line_t) * BLK_CACHE_LINES);
    if (!blk_cache) {
        serial_puts("[blk] no memory for the read-ahead cache\n");
        return;
    }
    for (int i = 0; i < BLK_CACHE_LINES; i++) {
        blk_cache[i].base  = ~(uint64_t)0;
        blk_cache[i].count = 0;
        blk_cache[i].used  = 0;
    }
    serial_puts("[blk] read-ahead cache: ");
    serial_put_dec(BLK_CACHE_LINES);
    serial_puts(" lines of ");
    serial_put_dec(BLK_CACHE_SECTORS / 2);
    serial_puts(" KB\n");
}

static void blk_cache_drop(uint64_t lba, uint32_t count) {
    if (!blk_cache) return;
    for (int i = 0; i < BLK_CACHE_LINES; i++) {
        blk_cache_line_t *l = &blk_cache[i];
        if (l->base == ~(uint64_t)0) continue;
        if (lba + count <= l->base || l->base + l->count <= lba) continue;
        l->base = ~(uint64_t)0;
        l->count = 0;
    }
}

static int blk_read(uint64_t lba, uint32_t count, void *buf) {
    if (blk_cur < 0) return -1;
    const blk_dev_t *d = &blk_devs[blk_cur];
    if (lba + count > d->sectors) return -1;

    /* Anything larger than a cache line goes straight to the device;
     * caching it would evict everything to hold data the caller has
     * already asked for in full. */
    if (!blk_cache || count > BLK_CACHE_SECTORS) {
        uint64_t real = blk_remap_lookup(lba);
        int rc = blk_read_raw(real, count, buf);
        if (rc != 0 && count == 1) {
            if (blk_retire_sector(real) == 0)
                return blk_read_raw(real, 1, buf);
        }
        return rc;
    }

    for (int i = 0; i < BLK_CACHE_LINES; i++) {
        blk_cache_line_t *l = &blk_cache[i];
        if (l->base == ~(uint64_t)0) continue;
        if (lba < l->base || lba + count > l->base + l->count) continue;
        uint64_t off = (lba - l->base) * 512;
        uint8_t *dst = (uint8_t *)buf;
        for (uint64_t k = 0; k < (uint64_t)count * 512; k++)
            dst[k] = l->data[off + k];
        l->used = ++blk_cache_clock;
        blk_cache_hits++;
        return 0;
    }

    /* A miss. Fetch a whole line starting here, so the sectors that
     * follow are already in hand when they are asked for. */
    blk_cache_misses++;
    int victim = 0;
    uint64_t oldest = ~(uint64_t)0;
    for (int i = 0; i < BLK_CACHE_LINES; i++) {
        if (blk_cache[i].base == ~(uint64_t)0) { victim = i; break; }
        if (blk_cache[i].used < oldest) { oldest = blk_cache[i].used; victim = i; }
    }

    blk_cache_line_t *l = &blk_cache[victim];
    uint32_t want = BLK_CACHE_SECTORS;
    if (lba + want > d->sectors) want = (uint32_t)(d->sectors - lba);
    if (want < count) want = count;

    uint64_t real = blk_remap_lookup(lba);
    if (blk_read_raw(real, want, l->data) != 0) {
        /* The bulk read failed. Fall back to exactly what was asked for,
         * one sector at a time, so one bad sector does not lose the
         * sixty-three good ones around it. */
        l->base = ~(uint64_t)0;
        uint8_t *dst = (uint8_t *)buf;
        for (uint32_t k = 0; k < count; k++) {
            uint64_t s = blk_remap_lookup(lba + k);
            if (blk_read_raw(s, 1, dst + (uint64_t)k * 512) != 0) {
                if (blk_retire_sector(s) != 0) return -1;
                if (blk_read_raw(blk_remap_lookup(lba + k), 1,
                                 dst + (uint64_t)k * 512) != 0) return -1;
            }
        }
        return 0;
    }

    if (want > count) blk_readahead += want - count;
    l->base  = lba;
    l->count = want;
    l->used  = ++blk_cache_clock;

    uint8_t *dst = (uint8_t *)buf;
    for (uint64_t k = 0; k < (uint64_t)count * 512; k++) dst[k] = l->data[k];
    return 0;
}

static int blk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (blk_cur < 0) return -1;
    const blk_dev_t *d = &blk_devs[blk_cur];
    if (lba + count > d->sectors) return -1;

    /* Anything the cache is holding for this range is now wrong. Dropping
     * it rather than updating it is the safe direction: a stale line
     * returns data that was never on the disk. */
    blk_cache_drop(lba, count);
    return blk_write_raw(blk_remap_lookup(lba), count, buf);
}

static int blk_flush(void) {
    if (blk_cur < 0) return -1;
    const blk_dev_t *d = &blk_devs[blk_cur];
    switch (d->kind) {
        case BLK_NVME: return nvme_flush(d->unit);
        case BLK_AHCI: return ahci_flush(d->unit);
        case BLK_ATA:  return ata_flush();
    }
    return -1;
}

/*
 * A boot-time self-test, off unless -DSTORAGE_SELFTEST.
 *
 * The problem it solves is that a storage driver has two ways to look
 * like it works.  IDENTIFY succeeding proves the controller answered a
 * command; it proves nothing at all about whether the scatter/gather
 * list describes the right memory, and a PRDT or PRP list built from
 * wrong physical addresses reads plausible-looking garbage or, worse,
 * writes into someone else's pages.  Neither shows up as an error.
 *
 * So the read half compares two paths that must agree: 128 KB fetched as
 * one command (which forces a long scatter list spanning many pages)
 * against the same bytes fetched one sector at a time (which uses a
 * single-entry list and cannot be got wrong the same way).  A mismatch
 * localises the bug to the scatter/gather code immediately.
 *
 * The write half is deliberately reversible: the last sector is saved,
 * overwritten with a pattern, read back, and put straight back the way
 * it was.  That is still a write to a real disk, which is exactly why it
 * is behind a build flag and not in a shipped kernel.
 */
#ifdef STORAGE_SELFTEST

#define BLK_TEST_SECTORS 256          /* 128 KB — many pages, one command */
static uint8_t blk_test_bulk[BLK_TEST_SECTORS * 512] __attribute__((aligned(4096)));
static uint8_t blk_test_one[512];
static uint8_t blk_test_save[512];

static void blk_selftest(void) {
    for (int d = 0; d < blk_count; d++) {
        blk_select(d);
        serial_puts("[blktest] disk ");
        serial_put_dec((uint32_t)d);
        serial_puts(" (");
        serial_puts(blk_bus_name());
        serial_puts(")\n");

        if (blk_read(0, BLK_TEST_SECTORS, blk_test_bulk) != 0) {
            serial_puts("[blktest]   FAIL: bulk read\n");
            continue;
        }
        int bad = -1;
        for (uint32_t s = 0; s < BLK_TEST_SECTORS && bad < 0; s++) {
            if (blk_read(s, 1, blk_test_one) != 0) { bad = (int)s; break; }
            for (int i = 0; i < 512; i++)
                if (blk_test_one[i] != blk_test_bulk[s * 512 + i]) { bad = (int)s; break; }
        }
        if (bad >= 0) {
            serial_puts("[blktest]   FAIL: scatter/gather disagrees at sector ");
            serial_put_dec((uint32_t)bad);
            serial_putc('\n');
            continue;
        }
        serial_puts("[blktest]   read OK (128 KB scattered == 256 single reads)\n");

        uint64_t last = blk_devs[d].sectors - 1;
        if (blk_read(last, 1, blk_test_save) != 0) {
            serial_puts("[blktest]   FAIL: could not read the scratch sector\n");
            continue;
        }
        for (int i = 0; i < 512; i++) blk_test_one[i] = (uint8_t)(i * 7 + d + 1);
        if (blk_write(last, 1, blk_test_one) != 0 || blk_flush() != 0) {
            serial_puts("[blktest]   FAIL: write\n");
            continue;
        }
        for (int i = 0; i < 512; i++) blk_test_bulk[i] = 0;
        if (blk_read(last, 1, blk_test_bulk) != 0) {
            serial_puts("[blktest]   FAIL: read-back\n");
            continue;
        }
        int ok = 1;
        for (int i = 0; i < 512; i++)
            if (blk_test_bulk[i] != blk_test_one[i]) { ok = 0; break; }

        /* Put it back whatever happened — a failed comparison is a bug
         * to report, not a reason to leave the disk modified. */
        blk_write(last, 1, blk_test_save);
        blk_flush();

        serial_puts(ok ? "[blktest]   write OK (pattern survived, sector restored)\n"
                       : "[blktest]   FAIL: read-back did not match what was written\n");
    }
    if (blk_count > 0) blk_select(0);
}

#endif /* STORAGE_SELFTEST */

#endif /* BLK_H */
