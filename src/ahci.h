#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include "pci.h"

/*
 * AHCI — SATA the way every machine built after about 2007 presents it.
 *
 * The ATA PIO driver in ata.h talks to ports 0x1F0..0x1F7, an interface
 * that predates the PCI bus.  It still works because chipsets kept a
 * compatibility mode alive for the sake of installers, but that mode is
 * off by default on modern firmware and gone entirely on many laptops.
 * A kernel that only knows PIO boots on QEMU and finds no disk at all on
 * the machine in front of you.
 *
 * AHCI is a different shape of driver, not just different registers.
 * PIO moves data through the CPU a 16-bit word at a time; AHCI hands the
 * controller a command table and a scatter/gather list and the controller
 * moves the data itself.  That is why this file is longer than ata.h and
 * also why it is roughly two orders of magnitude faster: a 4 MB read is
 * one command here and 8192 round trips there.
 *
 * Everything is polled.  There is no interrupt handler, no completion
 * queue, no tagged concurrency — one command in flight at a time, spun
 * on until PxCI clears.  For a single-tasking kernel that is not a
 * compromise worth undoing; the win over PIO is the DMA, not the depth.
 *
 * Tested against QEMU's ich9-ahci.  The port-multiplier case is not
 * handled and neither is staggered spin-up beyond asserting the bit,
 * both of which are noted where they arise rather than pretended away.
 */

/* ---- HBA (host bus adapter) global registers ---- */
#define AHCI_CAP        0x00        /* capabilities                       */
#define AHCI_GHC        0x04        /* global host control                */
#define AHCI_IS         0x08        /* interrupt status                   */
#define AHCI_PI         0x0C        /* ports implemented (bitmap)         */
#define AHCI_VS         0x10        /* version                            */
#define AHCI_CAP2       0x24        /* capabilities extended              */
#define AHCI_BOHC       0x28        /* BIOS/OS handoff control            */

#define AHCI_CAP_S64A   (1u << 31)  /* supports 64-bit addressing         */
#define AHCI_CAP_SSS    (1u << 27)  /* supports staggered spin-up         */
#define AHCI_GHC_AE     (1u << 31)  /* AHCI enable                        */
#define AHCI_GHC_HR     (1u << 0)   /* HBA reset                          */
#define AHCI_CAP2_BOH   (1u << 0)   /* BIOS/OS handoff supported          */
#define AHCI_BOHC_BOS   (1u << 0)   /* BIOS owned                         */
#define AHCI_BOHC_OOS   (1u << 1)   /* OS owned                           */
#define AHCI_BOHC_BB    (1u << 4)   /* BIOS busy                          */

/* ---- per-port registers, at 0x100 + port * 0x80 ---- */
#define AHCI_PORT_BASE(n)  (0x100u + (uint32_t)(n) * 0x80u)
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0C
#define PxIS    0x10
#define PxIE    0x14
#define PxCMD   0x18
#define PxTFD   0x20
#define PxSIG   0x24
#define PxSSTS  0x28
#define PxSCTL  0x2C
#define PxSERR  0x30
#define PxSACT  0x34
#define PxCI    0x38

#define PxCMD_ST    (1u << 0)       /* start command engine               */
#define PxCMD_SUD   (1u << 1)       /* spin-up device                     */
#define PxCMD_POD   (1u << 2)       /* power on device                    */
#define PxCMD_FRE   (1u << 4)       /* FIS receive enable                 */
#define PxCMD_FR    (1u << 14)      /* FIS receive running                */
#define PxCMD_CR    (1u << 15)      /* command list running               */

#define PxTFD_ERR   (1u << 0)
#define PxTFD_DRQ   (1u << 3)
#define PxTFD_BSY   (1u << 7)

#define PxIS_TFES   (1u << 30)      /* task file error                    */

#define AHCI_SIG_ATA    0x00000101u
#define AHCI_SIG_ATAPI  0xEB140101u

/* ATA commands issued through the H2D register FIS */
#define ATA_CMD_IDENTIFY_DEV  0xEC
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_FLUSH_EXT     0xEA

/*
 * A command header.  Thirty-two bytes, thirty-two of them per port, and
 * the controller reads them by physical address — hence the packing and
 * the explicit widths.
 */
struct ahci_cmd_hdr {
    uint16_t flags;             /* CFL[4:0] A[5] W[6] P[7] R[8] B[9] C[10] */
    uint16_t prdtl;             /* PRDT entry count                        */
    volatile uint32_t prdbc;    /* bytes transferred — written by the HBA  */
    uint64_t ctba;              /* command table, 128-byte aligned         */
    uint32_t rsv[4];
} __attribute__((packed));

/* One scatter/gather entry.  dbc holds byte-count-1, so 0 means one byte
 * and the 22-bit field tops out at 4 MB. */
struct ahci_prd {
    uint64_t dba;
    uint32_t rsv;
    uint32_t dbc;
} __attribute__((packed));

/*
 * 168 entries is chosen against the transfer cap below, not picked for
 * roundness: 512 KB spanning worst-case page boundaries needs 129 runs,
 * and the slack absorbs a buffer that starts mid-page.
 */
#define AHCI_PRDT_MAX     168
#define AHCI_MAX_SECTORS  1024      /* 512 KB per command                 */

struct ahci_cmd_tbl {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    struct ahci_prd prdt[AHCI_PRDT_MAX];
} __attribute__((packed));

#define AHCI_MAX_DISKS 2

/*
 * DMA-visible structures live in .bss and are handed to the controller by
 * physical address.  The alignment requirements are the specification's:
 * 1 KB for a command list, 256 bytes for a received-FIS area, 128 for a
 * command table.  Getting one of these wrong does not fail loudly — the
 * HBA masks the low bits and reads someone else's memory.
 */
static struct ahci_cmd_hdr ahci_clist[AHCI_MAX_DISKS][32] __attribute__((aligned(1024)));
static uint8_t             ahci_rxfis[AHCI_MAX_DISKS][256] __attribute__((aligned(256)));
static struct ahci_cmd_tbl ahci_ctbl[AHCI_MAX_DISKS] __attribute__((aligned(128)));
static uint16_t            ahci_ident[256] __attribute__((aligned(4096)));

static volatile uint8_t *ahci_abar = 0;
static uint8_t  ahci_port_of[AHCI_MAX_DISKS];
static uint64_t ahci_disk_sectors[AHCI_MAX_DISKS];
static int      ahci_ndisks = 0;

static inline uint32_t ahci_rd(uint32_t off) {
    return *(volatile uint32_t *)(ahci_abar + off);
}
static inline void ahci_wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(ahci_abar + off) = v;
}
static inline uint32_t ahci_prd_rd(int port, uint32_t reg) {
    return ahci_rd(AHCI_PORT_BASE(port) + reg);
}
static inline void ahci_prd_wr(int port, uint32_t reg, uint32_t v) {
    ahci_wr(AHCI_PORT_BASE(port) + reg, v);
}

static void ahci_log(const char *s) {
    serial_puts("[ahci] ");
    serial_puts(s);
    serial_putc('\n');
}

/* Every wait in this file is bounded.  A disk controller that never
 * answers must not be able to hang the boot — the machine still has a
 * ramdisk and a screen, and a kernel that spins here dies with neither. */
static inline void ahci_relax(void) { __asm__ volatile("pause" ::: "memory"); }

#define AHCI_SPIN 2000000

/* ---- port command engine ---- */

static int ahci_port_stop(int port) {
    uint32_t cmd = ahci_prd_rd(port, PxCMD);
    ahci_prd_wr(port, PxCMD, cmd & ~PxCMD_ST);
    for (int i = 0; i < AHCI_SPIN; i++) {
        if (!(ahci_prd_rd(port, PxCMD) & PxCMD_CR)) break;
        ahci_relax();
    }
    if (ahci_prd_rd(port, PxCMD) & PxCMD_CR) return -1;

    cmd = ahci_prd_rd(port, PxCMD);
    ahci_prd_wr(port, PxCMD, cmd & ~PxCMD_FRE);
    for (int i = 0; i < AHCI_SPIN; i++) {
        if (!(ahci_prd_rd(port, PxCMD) & PxCMD_FR)) break;
        ahci_relax();
    }
    return (ahci_prd_rd(port, PxCMD) & PxCMD_FR) ? -1 : 0;
}

static void ahci_port_start(int port) {
    for (int i = 0; i < AHCI_SPIN; i++) {
        if (!(ahci_prd_rd(port, PxCMD) & PxCMD_CR)) break;
        ahci_relax();
    }
    uint32_t cmd = ahci_prd_rd(port, PxCMD);
    ahci_prd_wr(port, PxCMD, cmd | PxCMD_FRE);
    cmd = ahci_prd_rd(port, PxCMD);
    ahci_prd_wr(port, PxCMD, cmd | PxCMD_ST);
}

/*
 * Issue one command on slot 0 and wait for it.
 *
 * `fis` is a filled 20-byte host-to-device register FIS; `buf` and
 * `bytes` describe the data payload, which may be null for a command
 * that moves none.  Only slot 0 is ever used: the driver is synchronous,
 * so a second slot would sit idle.
 */
static int ahci_exec(int disk, const uint8_t *fis, void *buf,
                     uint32_t bytes, int write) {
    int port = ahci_port_of[disk];
    struct ahci_cmd_tbl *tbl = &ahci_ctbl[disk];
    struct ahci_cmd_hdr *hdr = &ahci_clist[disk][0];

    /* Wait for the device to be idle before touching the task file. */
    int i;
    for (i = 0; i < AHCI_SPIN; i++) {
        if (!(ahci_prd_rd(port, PxTFD) & (PxTFD_BSY | PxTFD_DRQ))) break;
        ahci_relax();
    }
    if (i == AHCI_SPIN) { ahci_log("device stuck busy"); return -1; }

    for (int b = 0; b < 64; b++) tbl->cfis[b] = 0;
    for (int b = 0; b < 20; b++) tbl->cfis[b] = fis[b];

    int nprd = 0;
    if (buf && bytes) {
        /* Static, not automatic: 168 runs is 2.7 KB, and this is reached
         * from deep inside the filesystem code on a kernel stack that
         * Limine sized, not this driver. One command is ever in flight,
         * so there is nothing to share it with. */
        static dma_run_t runs[AHCI_PRDT_MAX];
        nprd = dma_split(buf, bytes, runs, AHCI_PRDT_MAX);
        if (nprd <= 0) { ahci_log("buffer too fragmented for one command"); return -1; }
        for (int r = 0; r < nprd; r++) {
            tbl->prdt[r].dba = runs[r].phys;
            tbl->prdt[r].rsv = 0;
            tbl->prdt[r].dbc = runs[r].len - 1;   /* byte count is 0-based */
        }
    }

    hdr->flags = (uint16_t)(5 /* CFL: 20 bytes = 5 dwords */ |
                            (write ? (1u << 6) : 0));
    hdr->prdtl = (uint16_t)nprd;
    hdr->prdbc = 0;
    hdr->ctba  = kern_virt_to_phys(tbl);
    for (int r = 0; r < 4; r++) hdr->rsv[r] = 0;
    if (!hdr->ctba) { ahci_log("command table has no physical address"); return -1; }

    ahci_prd_wr(port, PxIS, 0xFFFFFFFFu);
    ahci_prd_wr(port, PxSERR, 0xFFFFFFFFu);
    ahci_prd_wr(port, PxCI, 1u);              /* slot 0 */

    for (i = 0; i < AHCI_SPIN; i++) {
        if (!(ahci_prd_rd(port, PxCI) & 1u)) break;
        if (ahci_prd_rd(port, PxIS) & PxIS_TFES) break;
        ahci_relax();
    }
    if (i == AHCI_SPIN) { ahci_log("command timed out"); return -1; }

    if ((ahci_prd_rd(port, PxIS) & PxIS_TFES) ||
        (ahci_prd_rd(port, PxTFD) & PxTFD_ERR)) {
        ahci_log("task file error");
        return -1;
    }
    return 0;
}

/* Fill a host-to-device register FIS.  `count` is in sectors and a value
 * of 65536 is encoded as zero, which is why it is taken as a uint32_t. */
static void ahci_make_fis(uint8_t *fis, uint8_t cmd, uint64_t lba, uint32_t count) {
    for (int i = 0; i < 20; i++) fis[i] = 0;
    fis[0]  = 0x27;                       /* register FIS, host to device */
    fis[1]  = 0x80;                       /* C: this is a command         */
    fis[2]  = cmd;
    fis[4]  = (uint8_t)(lba);
    fis[5]  = (uint8_t)(lba >> 8);
    fis[6]  = (uint8_t)(lba >> 16);
    fis[7]  = 0x40;                       /* LBA mode                     */
    fis[8]  = (uint8_t)(lba >> 24);
    fis[9]  = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)(count);
    fis[13] = (uint8_t)(count >> 8);
}

/* ---- public block operations ---- */

static int ahci_rw(int disk, uint64_t lba, uint32_t count, void *buf, int write) {
    if (disk < 0 || disk >= ahci_ndisks) return -1;
    uint8_t *p = (uint8_t *)buf;

    while (count > 0) {
        uint32_t n = count > AHCI_MAX_SECTORS ? AHCI_MAX_SECTORS : count;
        uint8_t fis[20];
        ahci_make_fis(fis, write ? ATA_CMD_WRITE_DMA_EXT : ATA_CMD_READ_DMA_EXT,
                      lba, n);
        if (ahci_exec(disk, fis, p, n * 512u, write) != 0) return -1;
        p     += (uint64_t)n * 512u;
        lba   += n;
        count -= n;
    }
    return 0;
}

static int ahci_read(int disk, uint64_t lba, uint32_t count, void *buf) {
    return ahci_rw(disk, lba, count, buf, 0);
}

static int ahci_write(int disk, uint64_t lba, uint32_t count, const void *buf) {
    return ahci_rw(disk, lba, count, (void *)(uintptr_t)buf, 1);
}

static int ahci_flush(int disk) {
    if (disk < 0 || disk >= ahci_ndisks) return -1;
    uint8_t fis[20];
    ahci_make_fis(fis, ATA_CMD_FLUSH_EXT, 0, 0);
    return ahci_exec(disk, fis, 0, 0, 0);
}

static uint64_t ahci_capacity(int disk) {
    if (disk < 0 || disk >= ahci_ndisks) return 0;
    return ahci_disk_sectors[disk];
}

/* ---- bring-up ---- */

/*
 * Claim a port and learn how big the disk is.
 *
 * The received-FIS area and command list are pointed at .bss and the
 * engine restarted, then IDENTIFY DEVICE runs through the ordinary
 * command path — the same path every later read uses, which means a
 * mistake in ahci_exec shows up here at boot rather than the first time
 * something writes a file.
 */
static int ahci_setup_port(int port) {
    if (ahci_ndisks >= AHCI_MAX_DISKS) return 0;
    int disk = ahci_ndisks;

    uint32_t ssts = ahci_prd_rd(port, PxSSTS);
    if ((ssts & 0x0F) != 3) return 0;             /* no device, or no link */
    if (((ssts >> 8) & 0x0F) != 1) return 0;      /* not in active power    */

    uint32_t sig = ahci_prd_rd(port, PxSIG);
    if (sig == AHCI_SIG_ATAPI) {
        ahci_log("port holds an ATAPI device - skipping");
        return 0;
    }
    if (sig != AHCI_SIG_ATA) return 0;

    if (ahci_port_stop(port) != 0) {
        ahci_log("port would not stop");
        return 0;
    }

    for (int i = 0; i < 32; i++) {
        ahci_clist[disk][i].flags = 0;
        ahci_clist[disk][i].prdtl = 0;
        ahci_clist[disk][i].prdbc = 0;
        ahci_clist[disk][i].ctba  = 0;
    }
    for (int i = 0; i < 256; i++) ahci_rxfis[disk][i] = 0;

    uint64_t clb = kern_virt_to_phys(&ahci_clist[disk][0]);
    uint64_t fb  = kern_virt_to_phys(&ahci_rxfis[disk][0]);
    if (!clb || !fb) {
        ahci_log("DMA structures have no physical address");
        return 0;
    }

    ahci_prd_wr(port, PxCLB,  (uint32_t)clb);
    ahci_prd_wr(port, PxCLBU, (uint32_t)(clb >> 32));
    ahci_prd_wr(port, PxFB,   (uint32_t)fb);
    ahci_prd_wr(port, PxFBU,  (uint32_t)(fb >> 32));
    ahci_prd_wr(port, PxSERR, 0xFFFFFFFFu);
    ahci_prd_wr(port, PxIS,   0xFFFFFFFFu);
    ahci_prd_wr(port, PxIE,   0);                 /* polled, never signalled */

    ahci_port_of[disk] = (uint8_t)port;
    ahci_ndisks = disk + 1;                       /* ahci_exec checks this   */

    ahci_port_start(port);

    uint8_t fis[20];
    ahci_make_fis(fis, ATA_CMD_IDENTIFY_DEV, 0, 0);
    if (ahci_exec(disk, fis, ahci_ident, 512, 0) != 0) {
        ahci_log("IDENTIFY failed");
        ahci_ndisks = disk;                       /* give the slot back      */
        return 0;
    }

    /* Word 83 bit 10 announces 48-bit addressing and words 100..103 then
     * carry the real capacity.  Anything without it is old enough that
     * READ DMA EXT would be refused, so it is left to the PIO driver. */
    if (!(ahci_ident[83] & (1u << 10))) {
        ahci_log("device has no 48-bit addressing - leaving it to ATA PIO");
        ahci_ndisks = disk;
        return 0;
    }
    uint64_t sectors = (uint64_t)ahci_ident[100]        |
                       ((uint64_t)ahci_ident[101] << 16) |
                       ((uint64_t)ahci_ident[102] << 32) |
                       ((uint64_t)ahci_ident[103] << 48);
    if (sectors == 0) {
        ahci_ndisks = disk;
        return 0;
    }
    ahci_disk_sectors[disk] = sectors;

    serial_puts("[ahci] port ");
    serial_put_dec((uint16_t)port);
    serial_puts(": ");
    serial_put_dec((uint16_t)(sectors / 2048));
    serial_puts(" MB\n");
    return 1;
}

/* Returns the number of usable SATA disks found. */
static int ahci_init(void) {
    ahci_ndisks = 0;

    pci_dev_t dev;
    /* class 0x01 mass storage, subclass 0x06 SATA, prog-if 0x01 AHCI 1.0 */
    if (!pci_find_class(0xFFFFFFu, 0x010601u, 0, &dev)) return 0;

    uint64_t bar, size;
    if (pci_bar(&dev, 5, &bar, &size) != 0 || !bar) {
        ahci_log("ABAR is not a memory BAR");
        return 0;
    }
    pci_enable(&dev, PCI_CMD_MEM | PCI_CMD_MASTER);

    /* Registers stop at 0x100 + 32 * 0x80 = 0x1100; mapping the whole BAR
     * would burn page-table pool on vendor space nothing here reads. */
    if (size > 0x2000) size = 0x2000;
    ahci_abar = mmio_map(bar, size);
    if (!ahci_abar) {
        ahci_log("could not map ABAR");
        return 0;
    }

    /*
     * Ask the firmware to hand the controller over.
     *
     * On a real machine the BIOS has been using this HBA to read the boot
     * loader and is still holding it.  Taking the registers without the
     * handshake works right up until SMM fires and both drivers issue a
     * command into the same port at once.
     */
    if (ahci_rd(AHCI_CAP2) & AHCI_CAP2_BOH) {
        ahci_wr(AHCI_BOHC, ahci_rd(AHCI_BOHC) | AHCI_BOHC_OOS);
        for (int i = 0; i < AHCI_SPIN; i++) {
            if (!(ahci_rd(AHCI_BOHC) & AHCI_BOHC_BOS)) break;
            ahci_relax();
        }
        for (int i = 0; i < AHCI_SPIN; i++) {
            if (!(ahci_rd(AHCI_BOHC) & AHCI_BOHC_BB)) break;
            ahci_relax();
        }
        ahci_log("took ownership from firmware");
    }

    /* AE must be set before anything else in the register file means what
     * this driver thinks it means. */
    ahci_wr(AHCI_GHC, ahci_rd(AHCI_GHC) | AHCI_GHC_AE);

    uint32_t cap = ahci_rd(AHCI_CAP);
    uint32_t pi  = ahci_rd(AHCI_PI);

    /* The static DMA buffers are 32-bit reachable in every layout Limine
     * produces, so a controller without 64-bit support is still fine —
     * but say so, because the alternative is a truncated address and a
     * transfer into the wrong page. */
    if (!(cap & AHCI_CAP_S64A))
        ahci_log("controller is 32-bit only");

    for (int port = 0; port < 32 && ahci_ndisks < AHCI_MAX_DISKS; port++) {
        if (!(pi & (1u << port))) continue;

        /* Staggered spin-up: without this a drive on a board that uses it
         * never leaves DET=0 and looks like an empty port. */
        if (cap & AHCI_CAP_SSS) {
            uint32_t c = ahci_prd_rd(port, PxCMD);
            if (!(c & PxCMD_SUD)) {
                ahci_prd_wr(port, PxCMD, c | PxCMD_SUD | PxCMD_POD);
                for (int i = 0; i < AHCI_SPIN; i++) {
                    if ((ahci_prd_rd(port, PxSSTS) & 0x0F) == 3) break;
                    ahci_relax();
                }
            }
        }
        ahci_setup_port(port);
    }

    if (ahci_ndisks == 0) ahci_log("controller present, no SATA disks");
    return ahci_ndisks;
}

#endif /* AHCI_H */
