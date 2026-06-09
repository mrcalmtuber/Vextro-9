#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include "idt.h"

/* ===== INTEL AC97 AUDIO CODEC — PCI IDs ===== */

#define AC97_VENDOR_ID    0x8086
#define AC97_DEVICE_ID_1  0x2415  /* 82801AA AC'97 Audio */
#define AC97_DEVICE_ID_2  0x2425  /* 82801AB AC'97 Audio */

/* ===== AC97 NAM (Native Audio Mixer) REGISTERS ===== */

#define AC97_NAM_RESET         0x00
#define AC97_NAM_MASTER_VOL    0x02
#define AC97_NAM_PCM_VOL       0x18
#define AC97_NAM_REC_SELECT    0x1A
#define AC97_NAM_REC_GAIN      0x1C
#define AC97_NAM_POWERDOWN     0x26
#define AC97_NAM_EXT_AUDIO_ID  0x28
#define AC97_NAM_EXT_AUDIO_CSR 0x2A

/* ===== AC97 NABM (Native Audio Bus Master) REGISTERS ===== */

#define AC97_NABM_PCM_OUT_BDBAR  0x10
#define AC97_NABM_PCM_OUT_CIV    0x14
#define AC97_NABM_PCM_OUT_LVI    0x15
#define AC97_NABM_PCM_OUT_SR     0x16
#define AC97_NABM_PCM_OUT_CR     0x1B
#define AC97_NABM_GLOB_CNT       0x2C
#define AC97_NABM_GLOB_STA       0x30

/* ===== DRIVER STATE ===== */

static int      ac97_found   = 0;
static uint16_t ac97_nam_bar = 0;
static uint16_t ac97_nabm_bar = 0;

/* ===== NAM / NABM I/O ACCESSORS ===== */

static inline void ac97_nam_write16(uint16_t reg, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"((uint16_t)(ac97_nam_bar + reg)) : "memory");
}

static inline uint16_t ac97_nam_read16(uint16_t reg) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)(ac97_nam_bar + reg)) : "memory");
    return v;
}

static inline void ac97_nabm_write8(uint16_t reg, uint8_t val) {
    outb((uint16_t)(ac97_nabm_bar + reg), val);
}

static inline void ac97_nabm_write32(uint16_t reg, uint32_t val) {
    outl((uint16_t)(ac97_nabm_bar + reg), val);
}

static inline uint32_t ac97_nabm_read32(uint16_t reg) {
    return inl((uint16_t)(ac97_nabm_bar + reg));
}

static inline uint16_t ac97_nabm_read16(uint16_t reg) {
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((uint16_t)(ac97_nabm_bar + reg)) : "memory");
    return v;
}

/* ===== AC97 LOGGING ===== */

static void ac97_log(const char *msg) {
    serial_puts("[ac97] ");
    serial_puts(msg);
    serial_putc('\n');
}

/* ===== PCI BUS SCAN FOR AC97 ===== */

static int ac97_pci_find(uint8_t *out_bus, uint8_t *out_slot, uint8_t *out_func) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = pci_read32((uint8_t)bus, slot, 0, 0x00);
            if (id == 0xFFFFFFFF) continue;

            uint16_t vendor = (uint16_t)(id & 0xFFFF);
            uint16_t device = (uint16_t)(id >> 16);

            if (vendor == AC97_VENDOR_ID &&
                (device == AC97_DEVICE_ID_1 || device == AC97_DEVICE_ID_2)) {
                *out_bus  = (uint8_t)bus;
                *out_slot = slot;
                *out_func = 0;
                return 1;
            }
        }
    }
    return 0;
}

/* ===== MAIN INITIALIZATION ===== */

static void ac97_init(void) {
    ac97_log("Scanning PCI bus for Intel AC97 (8086:2415/2425)...");

    uint8_t bus, slot, func;
    if (!ac97_pci_find(&bus, &slot, &func)) {
        ac97_log("Device not found on PCI bus");
        return;
    }

    ac97_log("Device found on PCI bus");

    /* Enable PCI I/O space access and bus mastering */
    uint32_t cmd = pci_read32(bus, slot, func, 0x04);
    cmd |= (1u << 0) | (1u << 2);  /* I/O Space + Bus Master */
    pci_write32(bus, slot, func, 0x04, cmd);

    /* Read BAR0 — NAM (Native Audio Mixer), I/O space */
    uint32_t bar0 = pci_read32(bus, slot, func, 0x10);
    ac97_nam_bar = (uint16_t)(bar0 & 0xFFFC);

    /* Read BAR1 — NABM (Native Audio Bus Master), I/O space */
    uint32_t bar1 = pci_read32(bus, slot, func, 0x14);
    ac97_nabm_bar = (uint16_t)(bar1 & 0xFFFC);

    ac97_log("BAR0 (NAM) and BAR1 (NABM) extracted from PCI config space");

    /* Cold reset the codec via NABM Global Control */
    ac97_nabm_write32(AC97_NABM_GLOB_CNT, 0x02);
    for (volatile int i = 0; i < 100000; i++);

    /* Reset the codec via NAM reset register */
    ac97_nam_read16(AC97_NAM_RESET);
    for (volatile int i = 0; i < 100000; i++);

    /* Set master volume: 0 dB attenuation (unmuted) */
    ac97_nam_write16(AC97_NAM_MASTER_VOL, 0x0000);

    /* Set PCM output volume: 0 dB attenuation */
    ac97_nam_write16(AC97_NAM_PCM_VOL, 0x0808);

    /* Enable variable-rate audio if supported */
    uint16_t ext_id = ac97_nam_read16(AC97_NAM_EXT_AUDIO_ID);
    if (ext_id & 0x0001) {
        uint16_t ext_csr = ac97_nam_read16(AC97_NAM_EXT_AUDIO_CSR);
        ext_csr |= 0x0001;  /* VRA bit */
        ac97_nam_write16(AC97_NAM_EXT_AUDIO_CSR, ext_csr);
    }

    ac97_found = 1;
    ac97_log("Driver initialization complete");
}

#endif /* AC97_H */
