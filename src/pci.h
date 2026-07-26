#ifndef PCI_H
#define PCI_H

#include <stdint.h>
#include "idt.h"

/*
 * Generic PCI + MMIO support layer.
 *
 * Portability groundwork: every driver used to hand-roll its own bus
 * scan against one exact device ID on function 0.  This layer provides
 * full bus/device/function enumeration (multifunction aware), lookup by
 * ID table or class code, BAR decoding with sizing, and a shared
 * page-table MMIO mapper — so drivers probe the machine they are
 * actually running on instead of assuming this one.
 */

/* ===== SERIAL DEBUG PORT (COM1) ===== */

static void serial_putc(char c) {
    while (!(inb(0x3FD) & 0x20));
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

static void serial_init(void) {
    outb(0x3F9, 0x00);   /* Disable interrupts */
    outb(0x3FB, 0x80);   /* Enable DLAB */
    outb(0x3F8, 0x01);   /* Divisor low: 115200 baud */
    outb(0x3F9, 0x00);   /* Divisor high */
    outb(0x3FB, 0x03);   /* 8N1 */
    outb(0x3FA, 0xC7);   /* Enable FIFO */
    outb(0x3FC, 0x03);   /* RTS/DSR set */
}

static void serial_put_hex32(uint32_t v) {
    static const char hx[] = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4)
        serial_putc(hx[(v >> i) & 0xF]);
}

/* ===== 32-BIT PORT I/O ===== */

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

/* ===== PCI CONFIGURATION SPACE (mechanism #1) ===== */

#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static uint32_t pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off) {
    uint32_t addr = (uint32_t)((1u << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)(slot & 0x1F) << 11) |
                    ((uint32_t)(func & 0x07) << 8) |
                    (off & 0xFC));
    outl(PCI_CONFIG_ADDR, addr);
    return inl(PCI_CONFIG_DATA);
}

static void pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t val) {
    uint32_t addr = (uint32_t)((1u << 31) |
                    ((uint32_t)bus << 16) |
                    ((uint32_t)(slot & 0x1F) << 11) |
                    ((uint32_t)(func & 0x07) << 8) |
                    (off & 0xFC));
    outl(PCI_CONFIG_ADDR, addr);
    outl(PCI_CONFIG_DATA, val);
}

typedef struct {
    uint8_t  bus, slot, func;
    uint16_t vendor, device;
    uint32_t class_code;     /* class << 16 | subclass << 8 | prog-if */
} pci_dev_t;

/* Enumerate every function on every bus; cb returns nonzero to stop.
 * Returns 1 if the callback stopped the scan. */
typedef int (*pci_scan_cb)(const pci_dev_t *dev, void *ctx);

static int pci_scan(pci_scan_cb cb, void *ctx) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id0 = pci_read32((uint8_t)bus, slot, 0, 0x00);
            if (id0 == 0xFFFFFFFF) continue;

            uint8_t header = (uint8_t)(pci_read32((uint8_t)bus, slot, 0, 0x0C) >> 16);
            uint8_t nfunc = (header & 0x80) ? 8 : 1;

            for (uint8_t func = 0; func < nfunc; func++) {
                uint32_t id = pci_read32((uint8_t)bus, slot, func, 0x00);
                if (id == 0xFFFFFFFF) continue;

                pci_dev_t dev;
                dev.bus = (uint8_t)bus;
                dev.slot = slot;
                dev.func = func;
                dev.vendor = (uint16_t)(id & 0xFFFF);
                dev.device = (uint16_t)(id >> 16);
                dev.class_code = pci_read32((uint8_t)bus, slot, func, 0x08) >> 8;
                if (cb(&dev, ctx))
                    return 1;
            }
        }
    }
    return 0;
}

/* Find first device matching vendor + any ID in a 0-terminated list */
struct pci_find_ctx {
    uint16_t vendor;
    const uint16_t *ids;      /* may be 0: match any device of vendor */
    uint32_t class_mask;      /* 0 = ignore class */
    uint32_t class_val;
    pci_dev_t *out;
};

static int pci_find_cb(const pci_dev_t *dev, void *vctx) {
    struct pci_find_ctx *c = (struct pci_find_ctx *)vctx;
    if (c->vendor && dev->vendor != c->vendor) return 0;
    if (c->class_mask &&
        (dev->class_code & c->class_mask) != c->class_val) return 0;
    if (c->ids) {
        int hit = 0;
        for (int i = 0; c->ids[i]; i++)
            if (dev->device == c->ids[i]) { hit = 1; break; }
        if (!hit) return 0;
    }
    *c->out = *dev;
    return 1;
}

static int pci_find_ids(uint16_t vendor, const uint16_t *ids, pci_dev_t *out) {
    struct pci_find_ctx c = { vendor, ids, 0, 0, out };
    return pci_scan(pci_find_cb, &c);
}

static int pci_find_class(uint32_t class_mask, uint32_t class_val,
                          uint16_t vendor, pci_dev_t *out) {
    struct pci_find_ctx c = { vendor, 0, class_mask, class_val, out };
    return pci_scan(pci_find_cb, &c);
}

/* Enable memory / io / bus-master bits in the PCI command register */
static void pci_enable(const pci_dev_t *d, uint32_t bits) {
    uint32_t cmd = pci_read32(d->bus, d->slot, d->func, 0x04);
    cmd |= bits;
    pci_write32(d->bus, d->slot, d->func, 0x04, cmd);
}

#define PCI_CMD_IO      (1u << 0)
#define PCI_CMD_MEM     (1u << 1)
#define PCI_CMD_MASTER  (1u << 2)

/* Decode a memory BAR: base physical address + size (via sizing probe).
 * Handles 64-bit BARs. Returns 0 on success. */
static int pci_bar(const pci_dev_t *d, int bar_idx,
                   uint64_t *out_base, uint64_t *out_size) {
    uint8_t off = (uint8_t)(0x10 + bar_idx * 4);
    uint32_t lo = pci_read32(d->bus, d->slot, d->func, off);
    if (lo & 1) return -1;                       /* I/O BAR */

    int is64 = ((lo >> 1) & 3) == 2;
    uint32_t hi = is64 ? pci_read32(d->bus, d->slot, d->func,
                                    (uint8_t)(off + 4)) : 0;
    uint64_t base = (((uint64_t)hi << 32) | lo) & ~0xFULL;

    /* sizing: write all-ones, read mask, restore */
    pci_write32(d->bus, d->slot, d->func, off, 0xFFFFFFFF);
    uint32_t mask_lo = pci_read32(d->bus, d->slot, d->func, off);
    pci_write32(d->bus, d->slot, d->func, off, lo);
    uint32_t mask_hi = 0xFFFFFFFF;
    if (is64) {
        pci_write32(d->bus, d->slot, d->func, (uint8_t)(off + 4), 0xFFFFFFFF);
        mask_hi = pci_read32(d->bus, d->slot, d->func, (uint8_t)(off + 4));
        pci_write32(d->bus, d->slot, d->func, (uint8_t)(off + 4), hi);
    }
    uint64_t mask = (((uint64_t)mask_hi << 32) | mask_lo) & ~0xFULL;
    if (mask == 0) return -1;

    *out_base = base;
    *out_size = (~mask) + 1;
    return 0;
}

/* ===== PHYSICAL <-> VIRTUAL (Limine HHDM) ===== */

static uint64_t hal_hhdm_offset = 0;

static inline volatile uint8_t *phys_to_virt(uint64_t phys) {
    return (volatile uint8_t *)(uintptr_t)(phys + hal_hhdm_offset);
}

static inline uint64_t read_cr3(void) {
    uint64_t v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void flush_tlb_page(uint64_t addr) {
    __asm__ volatile("invlpg (%0)" :: "r"(addr) : "memory");
}

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_PWT      (1ULL << 3)  /* Page Write-Through */
#define PTE_PCD      (1ULL << 4)  /* Page Cache Disable (for MMIO) */
#define PTE_HUGE     (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/* Resolve a kernel virtual address to physical by walking CR3 */
static uint64_t kern_virt_to_phys(void *virt) {
    uint64_t vaddr = (uint64_t)(uintptr_t)virt;
    uint64_t cr3_phys = read_cr3() & PTE_ADDR_MASK;

    volatile uint64_t *pml4 =
        (volatile uint64_t *)(uintptr_t)(cr3_phys + hal_hhdm_offset);

    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    if (!(pml4[pml4i] & PTE_PRESENT)) return 0;

    volatile uint64_t *pdpt = (volatile uint64_t *)
        (uintptr_t)((pml4[pml4i] & PTE_ADDR_MASK) + hal_hhdm_offset);
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    if (pdpt[pdpti] & PTE_HUGE)
        return (pdpt[pdpti] & 0x000FFFFFC0000000ULL) | (vaddr & 0x3FFFFFFF);

    volatile uint64_t *pd = (volatile uint64_t *)
        (uintptr_t)((pdpt[pdpti] & PTE_ADDR_MASK) + hal_hhdm_offset);
    uint64_t pdi = (vaddr >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    if (pd[pdi] & PTE_HUGE)
        return (pd[pdi] & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFF);

    volatile uint64_t *pt = (volatile uint64_t *)
        (uintptr_t)((pd[pdi] & PTE_ADDR_MASK) + hal_hhdm_offset);
    uint64_t pti = (vaddr >> 12) & 0x1FF;
    if (!(pt[pti] & PTE_PRESENT)) return 0;
    return (pt[pti] & PTE_ADDR_MASK) | (vaddr & 0xFFF);
}

/* ===== MMIO MAPPER =====
 * Limine's HHDM covers RAM but not necessarily device BARs.  Map a BAR
 * region into the HHDM range as uncacheable pages.  Page-table pages
 * come from a static pool (enough for tens of MB of MMIO). */

#define MMIO_POOL_PAGES 32
static uint8_t mmio_pool[MMIO_POOL_PAGES][4096] __attribute__((aligned(4096)));
static int mmio_pool_idx = 0;

static uint64_t mmio_alloc_page_phys(void) {
    if (mmio_pool_idx >= MMIO_POOL_PAGES) return 0;
    void *p = &mmio_pool[mmio_pool_idx++][0];
    for (int i = 0; i < 4096; i++) ((uint8_t *)p)[i] = 0;
    return kern_virt_to_phys(p);
}

/* Returns the virtual address of the mapped region, or 0 on failure */
static volatile uint8_t *mmio_map(uint64_t phys_addr, uint64_t size) {
    uint64_t cr3_phys = read_cr3() & PTE_ADDR_MASK;
    volatile uint64_t *pml4 = (volatile uint64_t *)phys_to_virt(cr3_phys);
    uint64_t virt_base = phys_addr + hal_hhdm_offset;

    for (uint64_t offset = 0; offset < size; offset += 4096) {
        uint64_t phys = phys_addr + offset;
        uint64_t virt = virt_base + offset;

        uint64_t pml4i = (virt >> 39) & 0x1FF;
        uint64_t pdpti = (virt >> 30) & 0x1FF;
        uint64_t pdi   = (virt >> 21) & 0x1FF;
        uint64_t pti   = (virt >> 12) & 0x1FF;

        if (!(pml4[pml4i] & PTE_PRESENT)) {
            uint64_t pg = mmio_alloc_page_phys();
            if (!pg) return 0;
            pml4[pml4i] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pdpt = (volatile uint64_t *)
            phys_to_virt(pml4[pml4i] & PTE_ADDR_MASK);

        if (pdpt[pdpti] & PTE_PRESENT) {
            if (pdpt[pdpti] & PTE_HUGE) continue;   /* already covered */
        } else {
            uint64_t pg = mmio_alloc_page_phys();
            if (!pg) return 0;
            pdpt[pdpti] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pd = (volatile uint64_t *)
            phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);

        if (pd[pdi] & PTE_PRESENT) {
            if (pd[pdi] & PTE_HUGE) continue;
        } else {
            uint64_t pg = mmio_alloc_page_phys();
            if (!pg) return 0;
            pd[pdi] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pt = (volatile uint64_t *)
            phys_to_virt(pd[pdi] & PTE_ADDR_MASK);

        pt[pti] = phys | PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT;
        flush_tlb_page(virt);
    }
    return (volatile uint8_t *)(uintptr_t)virt_base;
}

#endif /* PCI_H */
