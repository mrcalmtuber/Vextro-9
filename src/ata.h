#ifndef ATA_H
#define ATA_H

#include <stdint.h>
#include "idt.h"

/*
 * ATA PIO driver — primary bus master drive, LBA28, polling only.
 * No IRQs: every transfer spins on the status register, which is fine
 * for a single-tasking kernel and QEMU's instant virtual disk.
 */

#define ATA_IO      0x1F0
#define ATA_CTRL    0x3F6

#define ATA_REG_DATA     (ATA_IO + 0)
#define ATA_REG_ERROR    (ATA_IO + 1)
#define ATA_REG_SECCNT   (ATA_IO + 2)
#define ATA_REG_LBA0     (ATA_IO + 3)
#define ATA_REG_LBA1     (ATA_IO + 4)
#define ATA_REG_LBA2     (ATA_IO + 5)
#define ATA_REG_DRIVE    (ATA_IO + 6)
#define ATA_REG_STATUS   (ATA_IO + 7)
#define ATA_REG_CMD      (ATA_IO + 7)

#define ATA_SR_BSY  0x80
#define ATA_SR_DRDY 0x40
#define ATA_SR_DF   0x20
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define ATA_CMD_READ     0x20
#define ATA_CMD_WRITE    0x30
#define ATA_CMD_FLUSH    0xE7
#define ATA_CMD_IDENTIFY 0xEC

static inline uint16_t inw(uint16_t port) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port) : "memory");
}

static int ata_present = 0;
static uint32_t ata_sectors = 0;      /* LBA28 capacity */

/* ~400 ns settle: four reads of the alternate status register */
static void ata_io_delay(void) {
    for (int i = 0; i < 4; i++)
        (void)inb(ATA_CTRL);
}

/* Wait for BSY to clear; returns -1 on timeout */
static int ata_wait_busy(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(inb(ATA_REG_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;
}

static void ata_put_hex8(uint8_t v) {
    static const char hx[] = "0123456789ABCDEF";
    serial_putc(hx[v >> 4]);
    serial_putc(hx[v & 0xF]);
}

/* Wait for DRQ (data ready); returns -1 on timeout/error */
static int ata_wait_drq(void) {
    uint8_t s = 0;
    for (int i = 0; i < 1000000; i++) {
        s = inb(ATA_REG_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) {
            serial_puts("[ata] DRQ error, status=");
            ata_put_hex8(s);
            serial_puts(" err=");
            ata_put_hex8(inb(ATA_REG_ERROR));
            serial_putc('\n');
            return -1;
        }
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ))
            return 0;
    }
    serial_puts("[ata] DRQ timeout, status=");
    ata_put_hex8(s);
    serial_putc('\n');
    return -1;
}

/* Wait until the device will accept a new command (BSY and DRQ clear) */
static int ata_cmd_ready(void) {
    for (int i = 0; i < 1000000; i++) {
        uint8_t s = inb(ATA_REG_STATUS);
        if (!(s & (ATA_SR_BSY | ATA_SR_DRQ)))
            return 0;
    }
    serial_puts("[ata] device never became command-ready\n");
    return -1;
}

static void ata_select(uint32_t lba) {
    outb(ATA_REG_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_delay();
}

static int ata_read(uint32_t lba, uint32_t count, void *buf) {
    if (!ata_present) return -1;
    uint16_t *p = (uint16_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_cmd_ready() != 0) return -1;
        ata_select(lba);
        outb(ATA_REG_SECCNT, (uint8_t)n);
        outb(ATA_REG_LBA0, (uint8_t)lba);
        outb(ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(ATA_REG_CMD, ATA_CMD_READ);
        for (uint32_t s = 0; s < n; s++) {
            if (ata_wait_drq() != 0) return -1;
            for (int i = 0; i < 256; i++)
                *p++ = inw(ATA_REG_DATA);
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_write(uint32_t lba, uint32_t count, const void *buf) {
    if (!ata_present) return -1;
    const uint16_t *p = (const uint16_t *)buf;
    while (count > 0) {
        uint32_t n = count > 255 ? 255 : count;
        if (ata_cmd_ready() != 0) return -1;
        ata_select(lba);
        outb(ATA_REG_SECCNT, (uint8_t)n);
        outb(ATA_REG_LBA0, (uint8_t)lba);
        outb(ATA_REG_LBA1, (uint8_t)(lba >> 8));
        outb(ATA_REG_LBA2, (uint8_t)(lba >> 16));
        outb(ATA_REG_CMD, ATA_CMD_WRITE);
        for (uint32_t s = 0; s < n; s++) {
            if (ata_wait_drq() != 0) return -1;
            for (int i = 0; i < 256; i++)
                outw(ATA_REG_DATA, *p++);
            /* the device goes busy while it commits the sector — wait it
             * out, or the next command gets silently swallowed */
            ata_io_delay();
            if (ata_wait_busy() != 0) return -1;
            uint8_t st = inb(ATA_REG_STATUS);
            if (st & (ATA_SR_ERR | ATA_SR_DF)) {
                serial_puts("[ata] write error, status=");
                ata_put_hex8(st);
                serial_putc('\n');
                return -1;
            }
        }
        lba += n;
        count -= n;
    }
    return 0;
}

static int ata_flush(void) {
    if (!ata_present) return -1;
    outb(ATA_REG_DRIVE, 0xE0);
    ata_io_delay();
    outb(ATA_REG_CMD, ATA_CMD_FLUSH);
    return ata_wait_busy();
}

static void ata_init(void) {
    /* floating bus = no controller at all */
    if (inb(ATA_REG_STATUS) == 0xFF) {
        serial_puts("[ata] no controller (bus floats)\n");
        return;
    }

    outb(ATA_REG_DRIVE, 0xA0);            /* select master, CHS mode bit */
    ata_io_delay();
    outb(ATA_REG_SECCNT, 0);
    outb(ATA_REG_LBA0, 0);
    outb(ATA_REG_LBA1, 0);
    outb(ATA_REG_LBA2, 0);
    outb(ATA_REG_CMD, ATA_CMD_IDENTIFY);
    ata_io_delay();

    uint8_t status = inb(ATA_REG_STATUS);
    if (status == 0) {
        serial_puts("[ata] no drive on primary master\n");
        return;
    }
    if (ata_wait_busy() != 0) {
        serial_puts("[ata] drive stuck busy\n");
        return;
    }
    /* ATAPI devices set the signature registers non-zero */
    if (inb(ATA_REG_LBA1) != 0 || inb(ATA_REG_LBA2) != 0) {
        serial_puts("[ata] primary master is ATAPI - skipping\n");
        return;
    }
    if (ata_wait_drq() != 0) {
        serial_puts("[ata] IDENTIFY failed\n");
        return;
    }

    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_REG_DATA);

    ata_sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    ata_present = 1;

    serial_puts("[ata] primary master: ");
    serial_put_dec((uint16_t)(ata_sectors / 2048));
    serial_puts(" MB (LBA28)\n");
}

#endif /* ATA_H */
