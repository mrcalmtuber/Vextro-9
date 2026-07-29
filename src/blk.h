#ifndef BLK_H
#define BLK_H

#include <stdint.h>
#include "pci.h"
#include "ata.h"     /* legacy PIO, ports 0x1F0                          */
#include "ahci.h"    /* SATA                                             */
#include "nvme.h"    /* PCIe SSD                                         */

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
static int blk_read(uint64_t lba, uint32_t count, void *buf) {
    if (blk_cur < 0) return -1;
    const blk_dev_t *d = &blk_devs[blk_cur];
    if (lba + count > d->sectors) return -1;
    switch (d->kind) {
        case BLK_NVME: return nvme_read(d->unit, lba, count, buf);
        case BLK_AHCI: return ahci_read(d->unit, lba, count, buf);
        case BLK_ATA:  return ata_read(lba, count, buf);
    }
    return -1;
}

static int blk_write(uint64_t lba, uint32_t count, const void *buf) {
    if (blk_cur < 0) return -1;
    const blk_dev_t *d = &blk_devs[blk_cur];
    if (lba + count > d->sectors) return -1;
    switch (d->kind) {
        case BLK_NVME: return nvme_write(d->unit, lba, count, buf);
        case BLK_AHCI: return ahci_write(d->unit, lba, count, buf);
        case BLK_ATA:  return ata_write(lba, count, buf);
    }
    return -1;
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
