#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include "idt.h"

/* ===== PCI CONFIGURATION SPACE ACCESS ===== */

#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

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

/* ===== INTEL e1000 REGISTER MAP ===== */

#define E1000_VENDOR_ID   0x8086
#define E1000_DEVICE_ID   0x100E

#define E1000_CTRL        0x0000
#define E1000_STATUS      0x0008
#define E1000_ICR         0x00C0
#define E1000_IMS         0x00D0
#define E1000_IMC         0x00D8

#define E1000_RCTL        0x0100
#define E1000_RDBAL       0x2800
#define E1000_RDBAH       0x2804
#define E1000_RDLEN       0x2808
#define E1000_RDH         0x2810
#define E1000_RDT         0x2818

#define E1000_TCTL        0x0400
#define E1000_TDBAL       0x3800
#define E1000_TDBAH       0x3804
#define E1000_TDLEN       0x3808
#define E1000_TDH         0x3810
#define E1000_TDT         0x3818

#define E1000_RAL0        0x5400
#define E1000_RAH0        0x5404
#define E1000_MTA         0x5200

/* CTRL register bits */
#define E1000_CTRL_SLU    (1u << 6)
#define E1000_CTRL_RST    (1u << 26)

/* RCTL register bits */
#define E1000_RCTL_EN     (1u << 1)
#define E1000_RCTL_BAM    (1u << 15)
#define E1000_RCTL_BSIZE  (0u << 16)  /* 2048-byte buffers */
#define E1000_RCTL_SECRC  (1u << 26)

/* TCTL register bits */
#define E1000_TCTL_EN     (1u << 1)
#define E1000_TCTL_PSP    (1u << 3)
#define E1000_TCTL_CT     (0x0Fu << 4)
#define E1000_TCTL_COLD   (0x40u << 12)

/* STATUS register bits */
#define E1000_STATUS_LU   (1u << 1)

/* ===== DESCRIPTOR RING GEOMETRY ===== */

#define E1000_NUM_RX_DESC 128
#define E1000_NUM_TX_DESC 128
#define E1000_RX_BUF_SIZE 2048

/* Hardware RX descriptor — 16 bytes, Intel layout */
struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

/* Hardware TX descriptor — 16 bytes, Intel layout */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

/* ===== DRIVER STATE ===== */

static volatile uint8_t *e1000_mmio = 0;
static int e1000_found = 0;

static struct e1000_rx_desc e1000_rx_ring[E1000_NUM_RX_DESC] __attribute__((aligned(128)));
static struct e1000_tx_desc e1000_tx_ring[E1000_NUM_TX_DESC] __attribute__((aligned(128)));
static uint8_t e1000_rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t e1000_tx_buffers[E1000_NUM_TX_DESC][E1000_RX_BUF_SIZE] __attribute__((aligned(4096)));

static uint8_t  e1000_mac[6];
static uint32_t e1000_rx_cur = 0;
static uint32_t e1000_tx_cur = 0;

/* ===== MMIO READ/WRITE ===== */

static inline void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(e1000_mmio + reg) = val;
}

static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(e1000_mmio + reg);
}

/* ===== KERNEL LOG HELPER ===== */

static void serial_putc(char c) {
    while (!(inb(0x3FD) & 0x20));
    outb(0x3F8, (uint8_t)c);
}

static void serial_puts(const char *s) {
    while (*s) serial_putc(*s++);
}

static void e1000_log(const char *msg) {
    serial_puts("[e1000] ");
    serial_puts(msg);
    serial_putc('\n');
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

/* ===== PCI BUS SCAN ===== */

static int e1000_pci_find(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, slot, 0, 0x00);
            if (id == 0xFFFFFFFF) continue;

            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);

            if (vendor == E1000_VENDOR_ID && device == E1000_DEVICE_ID) {
                *out_bus  = (uint8_t)bus;
                *out_slot = slot;
                *out_func = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* ===== PHYSICAL ↔ VIRTUAL ADDRESS TRANSLATION ===== */

static uint64_t e1000_hhdm_offset = 0;

static inline uint64_t e1000_virt_to_phys(void *virt) {
    return (uint64_t)(uintptr_t)virt - e1000_hhdm_offset;
}

static inline volatile uint8_t *e1000_phys_to_virt(uint64_t phys) {
    return (volatile uint8_t *)(uintptr_t)(phys + e1000_hhdm_offset);
}

/* ===== MMIO PAGE TABLE MAPPING =====
 * Limine's HHDM maps RAM but not device MMIO regions.
 * We walk the existing PML4 to map the MMIO BAR into the HHDM range.
 */

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
#define PTE_PCD      (1ULL << 4)  /* Page Cache Disable (uncacheable for MMIO) */
#define PTE_PWT      (1ULL << 3)  /* Page Write-Through */
#define PTE_HUGE     (1ULL << 7)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

static uint8_t e1000_extra_pages[4][4096] __attribute__((aligned(4096)));
static int e1000_extra_page_idx = 0;

static uint64_t e1000_kern_virt_to_phys(void *virt) {
    /* Kernel is linked at 0xFFFFFFFF80000000; Limine loads the kernel
     * with its own mapping. We need the physical address. Walk the
     * page tables from CR3 to resolve it. However, for simplicity,
     * since Limine maps the kernel at the higher-half base, the physical
     * address is typically: virt - kernel_virtual_base.
     * But we can also derive it from the HHDM: the kernel's physical
     * load address is typically low in RAM. We'll use CR3 walk instead. */
    uint64_t vaddr = (uint64_t)(uintptr_t)virt;
    uint64_t cr3_phys = read_cr3() & PTE_ADDR_MASK;

    /* We need HHDM to read page tables — it covers RAM */
    volatile uint64_t *pml4 = (volatile uint64_t *)(uintptr_t)(cr3_phys + e1000_hhdm_offset);

    uint64_t pml4i = (vaddr >> 39) & 0x1FF;
    if (!(pml4[pml4i] & PTE_PRESENT)) return 0;

    volatile uint64_t *pdpt = (volatile uint64_t *)(uintptr_t)((pml4[pml4i] & PTE_ADDR_MASK) + e1000_hhdm_offset);
    uint64_t pdpti = (vaddr >> 30) & 0x1FF;
    if (!(pdpt[pdpti] & PTE_PRESENT)) return 0;
    if (pdpt[pdpti] & PTE_HUGE)
        return (pdpt[pdpti] & 0x000FFFFFC0000000ULL) | (vaddr & 0x3FFFFFFF);

    volatile uint64_t *pd = (volatile uint64_t *)(uintptr_t)((pdpt[pdpti] & PTE_ADDR_MASK) + e1000_hhdm_offset);
    uint64_t pdi = (vaddr >> 21) & 0x1FF;
    if (!(pd[pdi] & PTE_PRESENT)) return 0;
    if (pd[pdi] & PTE_HUGE)
        return (pd[pdi] & 0x000FFFFFFFE00000ULL) | (vaddr & 0x1FFFFF);

    volatile uint64_t *pt = (volatile uint64_t *)(uintptr_t)((pd[pdi] & PTE_ADDR_MASK) + e1000_hhdm_offset);
    uint64_t pti = (vaddr >> 12) & 0x1FF;
    if (!(pt[pti] & PTE_PRESENT)) return 0;
    return (pt[pti] & PTE_ADDR_MASK) | (vaddr & 0xFFF);
}

static uint64_t e1000_alloc_page_phys(void) {
    if (e1000_extra_page_idx >= 4) return 0;
    void *p = &e1000_extra_pages[e1000_extra_page_idx++][0];
    for (int i = 0; i < 4096; i++) ((uint8_t *)p)[i] = 0;
    return e1000_kern_virt_to_phys(p);
}

static void e1000_map_mmio(uint64_t phys_addr, uint64_t size) {
    uint64_t cr3_phys = read_cr3() & PTE_ADDR_MASK;

    volatile uint64_t *pml4 = (volatile uint64_t *)e1000_phys_to_virt(cr3_phys);

    uint64_t virt_base = phys_addr + e1000_hhdm_offset;

    for (uint64_t offset = 0; offset < size; offset += 4096) {
        uint64_t phys = phys_addr + offset;
        uint64_t virt = virt_base + offset;

        uint64_t pml4i = (virt >> 39) & 0x1FF;
        uint64_t pdpti = (virt >> 30) & 0x1FF;
        uint64_t pdi   = (virt >> 21) & 0x1FF;
        uint64_t pti   = (virt >> 12) & 0x1FF;

        if (!(pml4[pml4i] & PTE_PRESENT)) {
            uint64_t pg = e1000_alloc_page_phys();
            if (!pg) return;
            pml4[pml4i] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pdpt = (volatile uint64_t *)
            e1000_phys_to_virt(pml4[pml4i] & PTE_ADDR_MASK);

        if (pdpt[pdpti] & PTE_PRESENT) {
            if (pdpt[pdpti] & PTE_HUGE) continue;
        } else {
            uint64_t pg = e1000_alloc_page_phys();
            if (!pg) return;
            pdpt[pdpti] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pd = (volatile uint64_t *)
            e1000_phys_to_virt(pdpt[pdpti] & PTE_ADDR_MASK);

        if (pd[pdi] & PTE_PRESENT) {
            if (pd[pdi] & PTE_HUGE) continue;
        } else {
            uint64_t pg = e1000_alloc_page_phys();
            if (!pg) return;
            pd[pdi] = pg | PTE_PRESENT | PTE_WRITE;
        }
        volatile uint64_t *pt = (volatile uint64_t *)
            e1000_phys_to_virt(pd[pdi] & PTE_ADDR_MASK);

        pt[pti] = phys | PTE_PRESENT | PTE_WRITE | PTE_PCD | PTE_PWT;
        flush_tlb_page(virt);
    }
}

/* ===== RX/TX RING INITIALIZATION ===== */

static void e1000_init_rx(void) {
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_rx_ring[i].addr   = e1000_kern_virt_to_phys(&e1000_rx_buffers[i][0]);
        e1000_rx_ring[i].status = 0;
    }

    uint64_t rx_phys = e1000_kern_virt_to_phys((void *)e1000_rx_ring);
    e1000_write(E1000_RDBAL, (uint32_t)(rx_phys & 0xFFFFFFFF));
    e1000_write(E1000_RDBAH, (uint32_t)(rx_phys >> 32));
    e1000_write(E1000_RDLEN, (uint32_t)(E1000_NUM_RX_DESC * sizeof(struct e1000_rx_desc)));
    e1000_write(E1000_RDH, 0);
    e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

    e1000_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_BAM |
                             E1000_RCTL_BSIZE | E1000_RCTL_SECRC);
}

static void e1000_init_tx(void) {
    for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
        e1000_tx_ring[i].addr   = 0;
        e1000_tx_ring[i].status = 1;  /* DD bit set = descriptor done */
        e1000_tx_ring[i].cmd    = 0;
    }

    uint64_t tx_phys = e1000_kern_virt_to_phys((void *)e1000_tx_ring);
    e1000_write(E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000_write(E1000_TDLEN, (uint32_t)(E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc)));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                             E1000_TCTL_CT | E1000_TCTL_COLD);
}

/* ===== MULTICAST TABLE ARRAY CLEAR ===== */

static void e1000_clear_mta(void) {
    for (uint32_t i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);
}

/* ===== MAIN INITIALIZATION ===== */

static void e1000_init(uint64_t hhdm_offset) {
    e1000_hhdm_offset = hhdm_offset;

    serial_init();
    e1000_log("Scanning PCI bus for Intel 82540EM (8086:100E)...");

    uint8_t bus, slot, func;
    if (!e1000_pci_find(&bus, &slot, &func)) {
        e1000_log("Device not found on PCI bus");
        return;
    }

    e1000_log("Device found on PCI bus");

    /* Enable PCI bus mastering and memory space access */
    uint32_t cmd = pci_read32(bus, slot, func, 0x04);
    cmd |= (1u << 1) | (1u << 2);  /* Memory Space + Bus Master */
    pci_write32(bus, slot, func, 0x04, cmd);

    /* Read BAR0 (MMIO base address) */
    uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
    uint64_t mmio_phys = (uint64_t)(bar0 & 0xFFFFFFF0u);

    /* Map BAR0 MMIO pages into kernel address space via page tables */
    e1000_map_mmio(mmio_phys, 0x20000);

    /* Map BAR0 through Limine HHDM */
    e1000_mmio = e1000_phys_to_virt(mmio_phys);

    e1000_log("BAR0 MMIO region mapped into kernel address space");

    /* Mask all interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    /* Global device reset */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_RST;
    e1000_write(E1000_CTRL, ctrl);

    for (volatile int i = 0; i < 100000; i++);

    /* Mask interrupts again after reset */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    e1000_log("Device reset complete, interrupts masked");

    /* Step 4: Assert Link Up (SLU) in Device Control Register */
    ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    e1000_write(E1000_CTRL, ctrl);

    /* Step 5: Clear the Multicast Table Array */
    e1000_clear_mta();

    /* Step 6: Initialize RX and TX descriptor rings */
    e1000_init_rx();
    e1000_init_tx();

    e1000_log("RX ring: 128 descriptors, 2KB buffers allocated");
    e1000_log("TX ring: 128 descriptors initialized");

    /* Step 7: Check link status */
    uint32_t status = e1000_read(E1000_STATUS);
    if (status & E1000_STATUS_LU) {
        e1000_log("Link Status: UP");
    } else {
        e1000_log("Link Status: DOWN");
    }

    e1000_found = 1;
    e1000_log("Driver initialization complete");
}

/* ===== MAC ADDRESS READ (from RAL0/RAH0 loaded by EEPROM) ===== */

static void e1000_read_mac(void) {
    uint32_t ral = e1000_read(E1000_RAL0);
    uint32_t rah = e1000_read(E1000_RAH0);
    e1000_mac[0] = (uint8_t)(ral & 0xFF);
    e1000_mac[1] = (uint8_t)((ral >> 8)  & 0xFF);
    e1000_mac[2] = (uint8_t)((ral >> 16) & 0xFF);
    e1000_mac[3] = (uint8_t)((ral >> 24) & 0xFF);
    e1000_mac[4] = (uint8_t)(rah & 0xFF);
    e1000_mac[5] = (uint8_t)((rah >> 8)  & 0xFF);
}

/* ===== RX RING POLL — returns 1 if a packet was available ===== */

static int e1000_rx_poll(uint8_t **out_buf, uint16_t *out_len) {
    struct e1000_rx_desc *desc = &e1000_rx_ring[e1000_rx_cur];
    if (!(desc->status & 0x01))
        return 0;

    *out_buf = e1000_rx_buffers[e1000_rx_cur];
    *out_len = desc->length;

    desc->status = 0;
    uint32_t old = e1000_rx_cur;
    e1000_rx_cur = (e1000_rx_cur + 1) % E1000_NUM_RX_DESC;
    e1000_write(E1000_RDT, old);

    return 1;
}

/* ===== TX RING SUBMIT — copies frame into next TX descriptor ===== */

static int e1000_transmit(const uint8_t *data, uint16_t len) {
    struct e1000_tx_desc *desc = &e1000_tx_ring[e1000_tx_cur];
    if (!(desc->status & 0x01))
        return -1;

    for (uint16_t i = 0; i < len; i++)
        e1000_tx_buffers[e1000_tx_cur][i] = data[i];

    desc->addr   = e1000_kern_virt_to_phys(&e1000_tx_buffers[e1000_tx_cur][0]);
    desc->length = len;
    desc->cmd    = 0x0B;   /* EOP | IFCS | RS */
    desc->status = 0;

    e1000_tx_cur = (e1000_tx_cur + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, e1000_tx_cur);

    return 0;
}

#endif /* E1000_H */
