#ifndef NVME_H
#define NVME_H

#include <stdint.h>
#include "pci.h"

/*
 * NVMe — the interface a solid-state disk actually wants to speak.
 *
 * AHCI is SATA's command set wearing a PCI costume: one command at a
 * time per port, a task file inherited from a 1994 parallel cable, and a
 * register poke for every submission.  NVMe throws that away.  Commands
 * are 64-byte structures in a ring in host memory, completions come back
 * in a second ring, and the only register write is a doorbell saying how
 * far the tail moved.  A machine bought in the last several years almost
 * certainly has no SATA disk in it at all, so without this file the OS
 * would come up on real hardware with no storage.
 *
 * This driver is deliberately narrow: one namespace, one I/O queue pair
 * of 64 entries, one command in flight, polled to completion.  The queue
 * shape is what makes NVMe fast under concurrency, and this kernel has
 * none — so the depth would be dead weight, while the DMA and the
 * absence of per-sector register traffic are the parts that pay.
 *
 * The one piece of genuine subtlety is block size.  NVMe namespaces are
 * not obliged to use 512-byte blocks and modern drives increasingly do
 * not; the filesystems above expect 512 and always will.  Translation
 * lives at the bottom of this file, including the read-modify-write a
 * partial block write needs, because getting that wrong on a 4 KB drive
 * silently destroys the seven neighbouring sectors on every write.
 */

/* ---- controller registers ---- */
#define NVME_CAP     0x00      /* 64-bit                                  */
#define NVME_VS      0x08
#define NVME_INTMS   0x0C
#define NVME_INTMC   0x10
#define NVME_CC      0x14
#define NVME_CSTS    0x1C
#define NVME_AQA     0x24
#define NVME_ASQ     0x28      /* 64-bit                                  */
#define NVME_ACQ     0x30      /* 64-bit                                  */

#define NVME_CC_EN       (1u << 0)
#define NVME_CSTS_RDY    (1u << 0)
#define NVME_CSTS_CFS    (1u << 1)   /* controller fatal status           */

/* ---- admin opcodes ---- */
#define NVME_ADM_CREATE_SQ   0x01
#define NVME_ADM_CREATE_CQ   0x05
#define NVME_ADM_IDENTIFY    0x06

/* ---- I/O opcodes ---- */
#define NVME_IO_FLUSH        0x00
#define NVME_IO_WRITE        0x01
#define NVME_IO_READ         0x02

#define NVME_QDEPTH      64
#define NVME_IOQ_ID      1
#define NVME_MAX_NS      2
#define NVME_MAX_XFER    (512 * 1024)   /* per command                    */

/*
 * Queues are physically contiguous by requirement — the PC bit in the
 * create-queue command asserts it — so each gets its own page-aligned
 * object and a run-length check at set-up rather than a scatter list.
 */
static uint8_t  nvme_asq[NVME_QDEPTH * 64] __attribute__((aligned(4096)));
static uint8_t  nvme_acq[NVME_QDEPTH * 16] __attribute__((aligned(4096)));
static uint8_t  nvme_iosq[NVME_QDEPTH * 64] __attribute__((aligned(4096)));
static uint8_t  nvme_iocq[NVME_QDEPTH * 16] __attribute__((aligned(4096)));
static uint8_t  nvme_idbuf[4096] __attribute__((aligned(4096)));
static uint64_t nvme_prplist[512] __attribute__((aligned(4096)));
static uint8_t  nvme_bounce[4096] __attribute__((aligned(4096)));

struct nvme_queue {
    uint8_t *sq;
    uint8_t *cq;
    uint16_t id;
    uint16_t depth;
    uint16_t sq_tail;
    uint16_t cq_head;
    uint8_t  phase;      /* flips on every wrap; how a stale entry is told
                          * apart from a fresh one without clearing the ring */
};

static volatile uint8_t *nvme_regs = 0;
static struct nvme_queue nvme_adminq, nvme_ioq;
static uint32_t nvme_db_stride = 4;
static uint16_t nvme_cid = 1;
static int nvme_ready = 0;

/* per-namespace geometry */
static uint32_t nvme_ns_id[NVME_MAX_NS];
static uint64_t nvme_ns_blocks[NVME_MAX_NS];
static uint32_t nvme_ns_bsize[NVME_MAX_NS];
static uint32_t nvme_ns_maxblk[NVME_MAX_NS];   /* per command, from MDTS  */
static int      nvme_nns = 0;

static void nvme_log(const char *s) {
    serial_puts("[nvme] ");
    serial_puts(s);
    serial_putc('\n');
}

static inline void nvme_relax(void) { __asm__ volatile("pause" ::: "memory"); }

#define NVME_SPIN 4000000

static inline uint32_t nvme_rd32(uint32_t off) {
    return *(volatile uint32_t *)(nvme_regs + off);
}
static inline void nvme_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(nvme_regs + off) = v;
}
static inline uint64_t nvme_rd64(uint32_t off) {
    /* CAP is the only 64-bit register read here and it is defined to be
     * readable as two dwords, which keeps this honest on a controller
     * that dislikes a single 8-byte access. */
    return (uint64_t)nvme_rd32(off) | ((uint64_t)nvme_rd32(off + 4) << 32);
}
static inline void nvme_wr64(uint32_t off, uint64_t v) {
    nvme_wr32(off, (uint32_t)v);
    nvme_wr32(off + 4, (uint32_t)(v >> 32));
}

/* Doorbell for queue `qid`; `cq` selects the completion half.  The
 * spacing between doorbells is the controller's, not a constant. */
static inline uint32_t nvme_doorbell(uint16_t qid, int cq) {
    return 0x1000u + ((uint32_t)(2 * qid + (cq ? 1 : 0))) * nvme_db_stride;
}

/*
 * Submit one command and wait for its completion.
 *
 * `cmd` is sixteen dwords laid out by the caller.  The command id is
 * filled in here so no caller can forget it, and the status is returned
 * as-is: a nonzero NVMe status code is far more useful in a log than a
 * bare -1, since it names the reason.
 */
static int nvme_exec(struct nvme_queue *q, uint32_t *cmd, uint32_t *result) {
    cmd[0] = (cmd[0] & 0x0000FFFFu) | ((uint32_t)nvme_cid << 16);
    uint16_t want_cid = nvme_cid++;
    if (nvme_cid == 0) nvme_cid = 1;

    uint32_t *slot = (uint32_t *)(q->sq + (uint32_t)q->sq_tail * 64u);
    for (int i = 0; i < 16; i++) slot[i] = cmd[i];

    q->sq_tail = (uint16_t)((q->sq_tail + 1) % q->depth);
    __asm__ volatile("" ::: "memory");
    nvme_wr32(nvme_doorbell(q->id, 0), q->sq_tail);

    for (int spin = 0; spin < NVME_SPIN; spin++) {
        volatile uint32_t *ce =
            (volatile uint32_t *)(q->cq + (uint32_t)q->cq_head * 16u);
        uint32_t dw3 = ce[3];
        if (((dw3 >> 16) & 1u) != q->phase) { nvme_relax(); continue; }

        uint16_t cid    = (uint16_t)(dw3 & 0xFFFF);
        uint16_t status = (uint16_t)(dw3 >> 17);
        if (result) *result = ce[0];

        q->cq_head = (uint16_t)(q->cq_head + 1);
        if (q->cq_head == q->depth) { q->cq_head = 0; q->phase ^= 1; }
        nvme_wr32(nvme_doorbell(q->id, 1), q->cq_head);

        if (cid != want_cid) {
            nvme_log("completion for a command nobody issued");
            return -1;
        }
        if (status) {
            serial_puts("[nvme] command failed, status 0x");
            serial_put_hex32(status);
            serial_putc('\n');
            return -1;
        }
        return 0;
    }
    nvme_log("command timed out");
    return -1;
}

/*
 * Describe a buffer with PRPs.
 *
 * NVMe's scatter/gather is unusual: the first pointer may sit at any
 * offset inside a page, every subsequent one must be page-aligned, and
 * the "list" only exists once the transfer needs more than two pages.
 * The two-page case using prp2 directly rather than a one-entry list is
 * not an optimisation — a list of one is not legal there.
 */
static int nvme_prp(const void *buf, uint32_t bytes,
                    uint64_t *prp1, uint64_t *prp2) {
    uint64_t va  = (uint64_t)(uintptr_t)buf;
    uint32_t off = (uint32_t)(va & 0xFFF);

    *prp1 = kern_virt_to_phys((void *)(uintptr_t)va);
    if (!*prp1) return -1;
    *prp2 = 0;

    uint32_t first = 4096u - off;
    if (bytes <= first) return 0;

    uint32_t rest = bytes - first;
    uint64_t next = (va + first) & ~0xFFFULL;

    if (rest <= 4096u) {
        *prp2 = kern_virt_to_phys((void *)(uintptr_t)next);
        return *prp2 ? 0 : -1;
    }

    uint32_t npages = (rest + 4095u) / 4096u;
    if (npages > 512) return -1;
    for (uint32_t i = 0; i < npages; i++) {
        uint64_t pa = kern_virt_to_phys((void *)(uintptr_t)(next + (uint64_t)i * 4096u));
        if (!pa) return -1;
        nvme_prplist[i] = pa;
    }
    *prp2 = kern_virt_to_phys(nvme_prplist);
    return *prp2 ? 0 : -1;
}

/* ---- raw namespace I/O, in the namespace's own block size ---- */

static int nvme_blk_rw(int ns, uint64_t block, uint32_t nblocks,
                       void *buf, int write) {
    if (!nvme_ready || ns < 0 || ns >= nvme_nns) return -1;
    uint32_t bs = nvme_ns_bsize[ns];
    uint8_t *p  = (uint8_t *)buf;

    while (nblocks > 0) {
        uint32_t n = nblocks;
        if (n > nvme_ns_maxblk[ns]) n = nvme_ns_maxblk[ns];

        uint32_t bytes = n * bs;
        uint64_t prp1, prp2;
        if (nvme_prp(p, bytes, &prp1, &prp2) != 0) {
            nvme_log("could not describe the buffer with PRPs");
            return -1;
        }

        uint32_t cmd[16];
        for (int i = 0; i < 16; i++) cmd[i] = 0;
        cmd[0]  = write ? NVME_IO_WRITE : NVME_IO_READ;
        cmd[1]  = nvme_ns_id[ns];
        cmd[6]  = (uint32_t)prp1;
        cmd[7]  = (uint32_t)(prp1 >> 32);
        cmd[8]  = (uint32_t)prp2;
        cmd[9]  = (uint32_t)(prp2 >> 32);
        cmd[10] = (uint32_t)block;
        cmd[11] = (uint32_t)(block >> 32);
        cmd[12] = n - 1;                 /* NLB is 0-based                 */

        if (nvme_exec(&nvme_ioq, cmd, 0) != 0) return -1;

        p       += bytes;
        block   += n;
        nblocks -= n;
    }
    return 0;
}

/* ---- 512-byte view, which is what the filesystems speak ---- */

static uint64_t nvme_capacity(int ns) {
    if (ns < 0 || ns >= nvme_nns) return 0;
    return nvme_ns_blocks[ns] * (nvme_ns_bsize[ns] / 512u);
}

static void nvme_copy(uint8_t *dst, const uint8_t *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
}

/*
 * Translate a 512-byte request onto whatever the namespace uses.
 *
 * When the block size is 512 this is a straight pass-through and costs
 * one comparison.  Otherwise the request is cut into three pieces: a
 * leading partial block, a whole-block middle, and a trailing partial
 * one.  The partials go through a bounce buffer, and on a write that
 * means reading the block first so the sectors either side survive —
 * the read-modify-write that a driver assuming 512 everywhere silently
 * skips.
 */
static int nvme_rw512(int ns, uint64_t lba, uint32_t count, void *buf, int write) {
    if (ns < 0 || ns >= nvme_nns) return -1;
    uint32_t bs = nvme_ns_bsize[ns];
    if (bs == 512) return nvme_blk_rw(ns, lba, count, buf, write);

    uint32_t ratio = bs / 512u;          /* 512-byte sectors per block     */
    uint8_t *p = (uint8_t *)buf;

    while (count > 0) {
        uint64_t block = lba / ratio;
        uint32_t within = (uint32_t)(lba % ratio);

        if (within == 0 && count >= ratio) {
            /* aligned run: hand the whole thing over untouched */
            uint32_t nblk = count / ratio;
            if (nvme_blk_rw(ns, block, nblk, p, write) != 0) return -1;
            uint32_t done = nblk * ratio;
            p     += (uint64_t)done * 512u;
            lba   += done;
            count -= done;
            continue;
        }

        uint32_t nsec = ratio - within;
        if (nsec > count) nsec = count;

        if (nvme_blk_rw(ns, block, 1, nvme_bounce, 0) != 0) return -1;
        if (write) {
            nvme_copy(nvme_bounce + within * 512u, p, nsec * 512u);
            if (nvme_blk_rw(ns, block, 1, nvme_bounce, 1) != 0) return -1;
        } else {
            nvme_copy(p, nvme_bounce + within * 512u, nsec * 512u);
        }

        p     += (uint64_t)nsec * 512u;
        lba   += nsec;
        count -= nsec;
    }
    return 0;
}

static int nvme_read(int ns, uint64_t lba, uint32_t count, void *buf) {
    return nvme_rw512(ns, lba, count, buf, 0);
}

static int nvme_write(int ns, uint64_t lba, uint32_t count, const void *buf) {
    return nvme_rw512(ns, lba, count, (void *)(uintptr_t)buf, 1);
}

static int nvme_flush(int ns) {
    if (!nvme_ready || ns < 0 || ns >= nvme_nns) return -1;
    uint32_t cmd[16];
    for (int i = 0; i < 16; i++) cmd[i] = 0;
    cmd[0] = NVME_IO_FLUSH;
    cmd[1] = nvme_ns_id[ns];
    return nvme_exec(&nvme_ioq, cmd, 0);
}

/* ---- bring-up ---- */

static int nvme_identify(uint32_t nsid, uint32_t cns) {
    uint64_t prp1, prp2;
    if (nvme_prp(nvme_idbuf, 4096, &prp1, &prp2) != 0) return -1;

    uint32_t cmd[16];
    for (int i = 0; i < 16; i++) cmd[i] = 0;
    cmd[0]  = NVME_ADM_IDENTIFY;
    cmd[1]  = nsid;
    cmd[6]  = (uint32_t)prp1;
    cmd[7]  = (uint32_t)(prp1 >> 32);
    cmd[8]  = (uint32_t)prp2;
    cmd[9]  = (uint32_t)(prp2 >> 32);
    cmd[10] = cns;
    return nvme_exec(&nvme_adminq, cmd, 0);
}

static uint32_t nvme_id_u32(int off) {
    return (uint32_t)nvme_idbuf[off]            |
           ((uint32_t)nvme_idbuf[off + 1] << 8)  |
           ((uint32_t)nvme_idbuf[off + 2] << 16) |
           ((uint32_t)nvme_idbuf[off + 3] << 24);
}

static uint64_t nvme_id_u64(int off) {
    return (uint64_t)nvme_id_u32(off) | ((uint64_t)nvme_id_u32(off + 4) << 32);
}

/* Returns the number of usable namespaces. */
static int nvme_init(void) {
    nvme_ready = 0;
    nvme_nns   = 0;

    pci_dev_t dev;
    /* class 0x01 mass storage, subclass 0x08 NVM, prog-if 0x02 NVMe */
    if (!pci_find_class(0xFFFFFFu, 0x010802u, 0, &dev)) return 0;

    uint64_t bar, size;
    if (pci_bar(&dev, 0, &bar, &size) != 0 || !bar) {
        nvme_log("BAR0 is not a memory BAR");
        return 0;
    }
    pci_enable(&dev, PCI_CMD_MEM | PCI_CMD_MASTER);

    /* Registers end at 0x1000 and the doorbells follow.  Mapping the
     * whole BAR would burn page-table pool on space nothing here reads,
     * so this takes 8 KB and checks below that the doorbells fit. */
    uint64_t mapped = size > 0x2000 ? 0x2000 : size;
    nvme_regs = mmio_map(bar, mapped);
    if (!nvme_regs) { nvme_log("could not map BAR0"); return 0; }

    uint64_t cap = nvme_rd64(NVME_CAP);
    uint32_t mqes   = (uint32_t)(cap & 0xFFFF) + 1;
    uint32_t dstrd  = (uint32_t)((cap >> 32) & 0xF);
    uint32_t mpsmin = (uint32_t)((cap >> 48) & 0xF);
    nvme_db_stride  = 4u << dstrd;

    /*
     * The doorbell spacing is the controller's to choose, and the field
     * allows up to 4 MB between them. Every real controller uses the
     * minimum, but "every real one" is not a bound — and the write that
     * would go past the mapping is a store to an unmapped page in the
     * middle of a disk transfer, which is a fault with no useful
     * explanation attached. Checking costs one comparison at boot.
     */
    if (nvme_doorbell(NVME_IOQ_ID, 1) + 4 > mapped) {
        nvme_log("doorbells are spaced further apart than the mapped window");
        return 0;
    }

    /*
     * The host page size the controller is told to use is fixed at 4 KB
     * because every PRP in this file is built from 4 KB pages.  A
     * controller whose minimum is larger cannot be driven by this code,
     * and saying so is better than programming CC.MPS with a lie.
     */
    if (mpsmin != 0) {
        nvme_log("controller needs a page size larger than 4 KB");
        return 0;
    }
    if (!((cap >> 37) & 1u)) {
        nvme_log("controller does not support the NVM command set");
        return 0;
    }

    uint16_t depth = NVME_QDEPTH;
    if (mqes < depth) depth = (uint16_t)mqes;

    /* Disable, then wait for the controller to admit it is down.  Coming
     * up on top of a half-configured controller left by the firmware is
     * the classic way to get a queue that never completes anything. */
    nvme_wr32(NVME_CC, 0);
    int i;
    for (i = 0; i < NVME_SPIN; i++) {
        if (!(nvme_rd32(NVME_CSTS) & NVME_CSTS_RDY)) break;
        nvme_relax();
    }
    if (i == NVME_SPIN) { nvme_log("controller would not go idle"); return 0; }

    for (uint32_t b = 0; b < sizeof(nvme_asq); b++)  nvme_asq[b]  = 0;
    for (uint32_t b = 0; b < sizeof(nvme_acq); b++)  nvme_acq[b]  = 0;
    for (uint32_t b = 0; b < sizeof(nvme_iosq); b++) nvme_iosq[b] = 0;
    for (uint32_t b = 0; b < sizeof(nvme_iocq); b++) nvme_iocq[b] = 0;

    uint64_t asq_pa = kern_virt_to_phys(nvme_asq);
    uint64_t acq_pa = kern_virt_to_phys(nvme_acq);
    if (!asq_pa || !acq_pa) { nvme_log("admin queues have no physical address"); return 0; }

    nvme_wr32(NVME_AQA, ((uint32_t)(depth - 1) << 16) | (uint32_t)(depth - 1));
    nvme_wr64(NVME_ASQ, asq_pa);
    nvme_wr64(NVME_ACQ, acq_pa);

    nvme_adminq.sq = nvme_asq;
    nvme_adminq.cq = nvme_acq;
    nvme_adminq.id = 0;
    nvme_adminq.depth = depth;
    nvme_adminq.sq_tail = 0;
    nvme_adminq.cq_head = 0;
    nvme_adminq.phase = 1;   /* entries start zeroed, so the first valid
                              * completion has its phase bit set */

    /* IOSQES/IOCQES are log2 of the entry sizes: 64 and 16 bytes. */
    uint32_t cc = NVME_CC_EN | (0u << 4) | (0u << 7) | (0u << 11) |
                  (6u << 16) | (4u << 20);
    nvme_wr32(NVME_CC, cc);

    for (i = 0; i < NVME_SPIN; i++) {
        uint32_t csts = nvme_rd32(NVME_CSTS);
        if (csts & NVME_CSTS_CFS) { nvme_log("controller reported a fatal error"); return 0; }
        if (csts & NVME_CSTS_RDY) break;
        nvme_relax();
    }
    if (i == NVME_SPIN) { nvme_log("controller never became ready"); return 0; }
    nvme_log("controller ready");

    /* Identify the controller: MDTS caps how much one command may move. */
    if (nvme_identify(0, 1) != 0) { nvme_log("IDENTIFY controller failed"); return 0; }
    uint8_t mdts = nvme_idbuf[77];
    uint32_t max_bytes = NVME_MAX_XFER;
    if (mdts && mdts < 20) {
        uint32_t lim = 4096u << mdts;     /* MDTS is in host pages         */
        if (lim < max_bytes) max_bytes = lim;
    }

    /*
     * Create the I/O queues.  Order matters: the completion queue must
     * exist before a submission queue can name it, and getting that
     * backwards returns "invalid queue identifier" rather than anything
     * that hints at ordering.
     */
    uint64_t iocq_pa = kern_virt_to_phys(nvme_iocq);
    uint64_t iosq_pa = kern_virt_to_phys(nvme_iosq);
    if (!iocq_pa || !iosq_pa) { nvme_log("I/O queues have no physical address"); return 0; }

    uint32_t cmd[16];
    for (i = 0; i < 16; i++) cmd[i] = 0;
    cmd[0]  = NVME_ADM_CREATE_CQ;
    cmd[6]  = (uint32_t)iocq_pa;
    cmd[7]  = (uint32_t)(iocq_pa >> 32);
    cmd[10] = ((uint32_t)(depth - 1) << 16) | NVME_IOQ_ID;
    cmd[11] = 1u;                        /* physically contiguous, no IRQ  */
    if (nvme_exec(&nvme_adminq, cmd, 0) != 0) { nvme_log("create I/O CQ failed"); return 0; }

    for (i = 0; i < 16; i++) cmd[i] = 0;
    cmd[0]  = NVME_ADM_CREATE_SQ;
    cmd[6]  = (uint32_t)iosq_pa;
    cmd[7]  = (uint32_t)(iosq_pa >> 32);
    cmd[10] = ((uint32_t)(depth - 1) << 16) | NVME_IOQ_ID;
    cmd[11] = 1u | ((uint32_t)NVME_IOQ_ID << 16);
    if (nvme_exec(&nvme_adminq, cmd, 0) != 0) { nvme_log("create I/O SQ failed"); return 0; }

    nvme_ioq.sq = nvme_iosq;
    nvme_ioq.cq = nvme_iocq;
    nvme_ioq.id = NVME_IOQ_ID;
    nvme_ioq.depth = depth;
    nvme_ioq.sq_tail = 0;
    nvme_ioq.cq_head = 0;
    nvme_ioq.phase = 1;
    nvme_ready = 1;

    /*
     * Enumerate namespaces.
     *
     * Namespace 1 is what a consumer drive always presents and assuming
     * it works everywhere it matters, but the active-namespace list is
     * one command and covers the drive that starts numbering elsewhere.
     */
    uint32_t nsids[NVME_MAX_NS];
    int nfound = 0;
    if (nvme_identify(0, 2) == 0) {
        for (int k = 0; k < NVME_MAX_NS; k++) {
            uint32_t id = nvme_id_u32(k * 4);
            if (!id) break;
            nsids[nfound++] = id;
        }
    }
    if (nfound == 0) { nsids[0] = 1; nfound = 1; }

    for (int k = 0; k < nfound; k++) {
        if (nvme_identify(nsids[k], 0) != 0) continue;

        uint64_t nsze  = nvme_id_u64(0);
        uint8_t  flbas = nvme_idbuf[26];
        int      fmt   = flbas & 0x0F;
        uint32_t lbaf  = nvme_id_u32(128 + fmt * 4);
        uint32_t lbads = (lbaf >> 16) & 0xFF;

        if (nsze == 0) continue;
        if (lbads < 9 || lbads > 12) {      /* 512 B .. 4 KB              */
            nvme_log("namespace uses an unsupported block size");
            continue;
        }
        uint32_t bsize = 1u << lbads;

        nvme_ns_id[nvme_nns]     = nsids[k];
        nvme_ns_blocks[nvme_nns] = nsze;
        nvme_ns_bsize[nvme_nns]  = bsize;
        nvme_ns_maxblk[nvme_nns] = max_bytes / bsize;
        if (nvme_ns_maxblk[nvme_nns] == 0) nvme_ns_maxblk[nvme_nns] = 1;

        serial_puts("[nvme] namespace ");
        serial_put_dec(nsids[k]);
        serial_puts(": ");
        serial_put_dec((uint32_t)(nsze * bsize / (1024 * 1024)));
        serial_puts(" MB, ");
        serial_put_dec(bsize);
        serial_puts("-byte blocks\n");
        nvme_nns++;
    }

    if (nvme_nns == 0) nvme_log("controller ready, no usable namespaces");
    return nvme_nns;
}

#endif /* NVME_H */
