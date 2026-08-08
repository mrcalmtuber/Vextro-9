#ifndef AC97_H
#define AC97_H

#include <stdint.h>
#include "idt.h"
#include "pci.h"   /* kern_virt_to_phys: the controller DMAs from physical
                    * addresses, and a static buffer is a virtual one */

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


/* This port has an audio device; media.h checks for the definition
 * rather than assuming one exists on every architecture. */
#define VEXTRO_HAVE_AUDIO 1

/* ===== PCM PLAYBACK =====
 *
 * Everything above brings the codec up. None of it makes a sound: the
 * chip plays audio by bus-mastering out of a Buffer Descriptor List, and
 * until this was written there was no list, no buffer, and nothing ever
 * set the run bit. "AC97 audio" meant an initialised mixer.
 *
 * The BDL is 32 entries of {physical address, sample count, flags}. The
 * controller walks it, and LVI says which entry is the last valid one.
 * Two things about it are easy to get wrong and both are silent:
 *
 *   The count is in SAMPLES, not bytes, and a "sample" here is one
 *   16-bit value -- so a stereo frame is two of them. Halving it plays
 *   the buffer at double speed with a click at the end.
 *
 *   The address is PHYSICAL. The kernel runs on the higher-half direct
 *   map, so a pointer to a static buffer is a virtual address the
 *   controller cannot follow; handing one over makes the device DMA from
 *   whatever happens to live at that physical address instead.
 */

#define AC97_BDL_ENTRIES  32
#define AC97_MAX_SAMPLES  0xFFFE        /* the count field is 16 bits */

/* Buffer descriptor: address, then samples, then flags. Packed and
 * 8-byte aligned because the controller reads it as an array. */
typedef struct {
    uint32_t addr;
    uint16_t samples;
    uint16_t flags;
} __attribute__((packed)) ac97_bd_t;

#define AC97_BD_IOC  0x8000u            /* interrupt when this one ends */
#define AC97_BD_BUP  0x4000u            /* play silence rather than stop */

static ac97_bd_t ac97_bdl[AC97_BDL_ENTRIES] __attribute__((aligned(8)));

/* The mixer's sample-rate register, present only with variable rate. */
#define AC97_NAM_PCM_FRONT_RATE 0x2C

/* Control register bits. */
#define AC97_CR_RPBM  0x01              /* run */
#define AC97_CR_RR    0x02              /* reset registers */

static int ac97_playing = 0;

static void ac97_stop(void) {
    if (!ac97_found) return;
    ac97_nabm_write8(AC97_NABM_PCM_OUT_CR, 0);
    /* The reset bit clears itself when the controller is done with it,
     * so this waits rather than assuming. */
    ac97_nabm_write8(AC97_NABM_PCM_OUT_CR, AC97_CR_RR);
    for (int i = 0; i < 100000; i++) {
        if (!(ac97_nabm_read16(AC97_NABM_PCM_OUT_CR) & AC97_CR_RR)) break;
    }
    ac97_playing = 0;
}

/*
 * Play one buffer of signed 16-bit stereo samples.
 *
 * `nsamples` counts individual 16-bit values, so a second of 48 kHz
 * stereo is 96000. Returns 0 on success.
 */
static int ac97_play(const int16_t *pcm, uint32_t nsamples, uint32_t rate) {
    if (!ac97_found || !pcm || nsamples == 0) return -1;

    ac97_stop();

    if (rate) ac97_nam_write16(AC97_NAM_PCM_FRONT_RATE, (uint16_t)rate);

    const uint64_t phys = kern_virt_to_phys((void *)(uintptr_t)pcm);
    if (!phys || phys > 0xFFFFFFFFull) return -1;   /* the field is 32 bits */

    /*
     * One descriptor per chunk, because the sample count is 16 bits and
     * anything longer than 65534 samples has to be split. The chunks are
     * contiguous in memory, so their physical addresses are contiguous
     * too -- which is only true because this buffer is a single static
     * array and not something assembled from pages.
     */
    uint32_t left = nsamples, off = 0;
    int n = 0;
    while (left && n < AC97_BDL_ENTRIES) {
        uint32_t take = left > AC97_MAX_SAMPLES ? AC97_MAX_SAMPLES : left;
        ac97_bdl[n].addr    = (uint32_t)(phys + (uint64_t)off * 2u);
        ac97_bdl[n].samples = (uint16_t)take;
        ac97_bdl[n].flags   = 0;
        off  += take;
        left -= take;
        n++;
    }
    if (n == 0) return -1;
    ac97_bdl[n - 1].flags = AC97_BD_IOC;

    const uint64_t bdl_phys = kern_virt_to_phys((void *)ac97_bdl);
    if (!bdl_phys || bdl_phys > 0xFFFFFFFFull) return -1;

    ac97_nabm_write32(AC97_NABM_PCM_OUT_BDBAR, (uint32_t)bdl_phys);
    ac97_nabm_write8(AC97_NABM_PCM_OUT_LVI, (uint8_t)(n - 1));
    ac97_nabm_write8(AC97_NABM_PCM_OUT_CR, AC97_CR_RPBM);

    ac97_playing = 1;
    return 0;
}

/* Has the controller reached the last descriptor? */
static int ac97_busy(void) {
    if (!ac97_found || !ac97_playing) return 0;
    const uint8_t civ = (uint8_t)(ac97_nabm_read16(AC97_NABM_PCM_OUT_CIV) & 0xFF);
    const uint8_t lvi = (uint8_t)(ac97_nabm_read16(AC97_NABM_PCM_OUT_LVI) & 0xFF);
    if (civ == lvi) { ac97_playing = 0; return 0; }
    return 1;
}

#endif /* AC97_H */
