#ifndef E1000_H
#define E1000_H

#include <stdint.h>
#include "idt.h"
#include "pci.h"

/* ===== INTEL e1000 (8254x) NIC DRIVER ===== */

#define E1000_VENDOR_ID   0x8086

/* 8254x family members this driver programs identically */
static const uint16_t e1000_ids[] = {
    0x100E,   /* 82540EM (QEMU default)   */
    0x100F,   /* 82545EM                  */
    0x1004,   /* 82543GC                  */
    0
};

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

#define E1000_CTRL_SLU    (1u << 6)
#define E1000_CTRL_RST    (1u << 26)

#define E1000_RCTL_EN     (1u << 1)
#define E1000_RCTL_BAM    (1u << 15)
#define E1000_RCTL_BSIZE  (0u << 16)  /* 2048-byte buffers */
#define E1000_RCTL_SECRC  (1u << 26)

#define E1000_TCTL_EN     (1u << 1)
#define E1000_TCTL_PSP    (1u << 3)
#define E1000_TCTL_CT     (0x0Fu << 4)
#define E1000_TCTL_COLD   (0x40u << 12)

#define E1000_STATUS_LU   (1u << 1)

#define E1000_NUM_RX_DESC 128
#define E1000_NUM_TX_DESC 128
#define E1000_RX_BUF_SIZE 2048

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} __attribute__((packed));

static volatile uint8_t *e1000_mmio = 0;
static int e1000_found = 0;

static struct e1000_rx_desc e1000_rx_ring[E1000_NUM_RX_DESC] __attribute__((aligned(128)));
static struct e1000_tx_desc e1000_tx_ring[E1000_NUM_TX_DESC] __attribute__((aligned(128)));
static uint8_t e1000_rx_buffers[E1000_NUM_RX_DESC][E1000_RX_BUF_SIZE] __attribute__((aligned(4096)));
static uint8_t e1000_tx_buffers[E1000_NUM_TX_DESC][E1000_RX_BUF_SIZE] __attribute__((aligned(4096)));

static uint8_t  e1000_mac[6];
static uint32_t e1000_rx_cur = 0;
static uint32_t e1000_tx_cur = 0;

static inline void e1000_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(e1000_mmio + reg) = val;
}

static inline uint32_t e1000_read(uint32_t reg) {
    return *(volatile uint32_t *)(e1000_mmio + reg);
}

static void e1000_log(const char *msg) {
    serial_puts("[e1000] ");
    serial_puts(msg);
    serial_putc('\n');
}

/* ===== RX/TX RING INITIALIZATION ===== */

static void e1000_init_rx(void) {
    for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
        e1000_rx_ring[i].addr   = kern_virt_to_phys(&e1000_rx_buffers[i][0]);
        e1000_rx_ring[i].status = 0;
    }

    uint64_t rx_phys = kern_virt_to_phys((void *)e1000_rx_ring);
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

    uint64_t tx_phys = kern_virt_to_phys((void *)e1000_tx_ring);
    e1000_write(E1000_TDBAL, (uint32_t)(tx_phys & 0xFFFFFFFF));
    e1000_write(E1000_TDBAH, (uint32_t)(tx_phys >> 32));
    e1000_write(E1000_TDLEN, (uint32_t)(E1000_NUM_TX_DESC * sizeof(struct e1000_tx_desc)));
    e1000_write(E1000_TDH, 0);
    e1000_write(E1000_TDT, 0);

    e1000_write(E1000_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP |
                             E1000_TCTL_CT | E1000_TCTL_COLD);
}

static void e1000_clear_mta(void) {
    for (uint32_t i = 0; i < 128; i++)
        e1000_write(E1000_MTA + i * 4, 0);
}

/* ===== MAIN INITIALIZATION ===== */

static void e1000_init(uint64_t hhdm_offset) {
    hal_hhdm_offset = hhdm_offset;

    serial_init();
    e1000_log("Scanning PCI bus for Intel 8254x NIC...");

    pci_dev_t dev;
    if (!pci_find_ids(E1000_VENDOR_ID, e1000_ids, &dev)) {
        e1000_log("Device not found on PCI bus");
        return;
    }
    e1000_log("Device found on PCI bus");

    pci_enable(&dev, PCI_CMD_MEM | PCI_CMD_MASTER);

    uint64_t mmio_phys, mmio_size;
    if (pci_bar(&dev, 0, &mmio_phys, &mmio_size) != 0) {
        e1000_log("BAR0 is not a memory BAR - aborting");
        return;
    }
    if (mmio_size < 0x20000) mmio_size = 0x20000;

    e1000_mmio = mmio_map(mmio_phys, mmio_size);
    if (!e1000_mmio) {
        e1000_log("MMIO mapping failed - aborting");
        return;
    }
    e1000_log("BAR0 MMIO region mapped into kernel address space");

    /* Mask all interrupts */
    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    /* Global device reset */
    uint32_t ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_RST;
    e1000_write(E1000_CTRL, ctrl);

    for (volatile int i = 0; i < 100000; i++);

    e1000_write(E1000_IMC, 0xFFFFFFFF);
    e1000_read(E1000_ICR);

    e1000_log("Device reset complete, interrupts masked");

    ctrl = e1000_read(E1000_CTRL);
    ctrl |= E1000_CTRL_SLU;
    e1000_write(E1000_CTRL, ctrl);

    e1000_clear_mta();
    e1000_init_rx();
    e1000_init_tx();

    e1000_log("RX ring: 128 descriptors, 2KB buffers allocated");
    e1000_log("TX ring: 128 descriptors initialized");

    uint32_t status = e1000_read(E1000_STATUS);
    if (status & E1000_STATUS_LU) {
        e1000_log("Link Status: UP");
    } else {
        e1000_log("Link Status: DOWN");
    }

    e1000_found = 1;
    e1000_log("Driver initialization complete");
}

/* ===== MAC ADDRESS READ ===== */

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

/* ===== RX POLL / TX SUBMIT ===== */

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

static int e1000_transmit(const uint8_t *data, uint16_t len) {
    struct e1000_tx_desc *desc = &e1000_tx_ring[e1000_tx_cur];
    if (!(desc->status & 0x01))
        return -1;

    for (uint16_t i = 0; i < len; i++)
        e1000_tx_buffers[e1000_tx_cur][i] = data[i];

    desc->addr   = kern_virt_to_phys(&e1000_tx_buffers[e1000_tx_cur][0]);
    desc->length = len;
    desc->cmd    = 0x0B;   /* EOP | IFCS | RS */
    desc->status = 0;

    e1000_tx_cur = (e1000_tx_cur + 1) % E1000_NUM_TX_DESC;
    e1000_write(E1000_TDT, e1000_tx_cur);

    return 0;
}

#endif /* E1000_H */
