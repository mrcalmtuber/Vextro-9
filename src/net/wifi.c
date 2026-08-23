#ifndef NET_WIFI_C
#define NET_WIFI_C

/*
 * src/net/wifi.c — the radio, and everything between it and lwIP.
 *
 * ---- why this is a .c that gets included ----
 *
 * Every other driver in this tree is a header pulled into kernel.c, and
 * this one is a .c that is pulled into kernel.c the same way. That is
 * not an oversight. The MMIO window allocator (mmio_map), the physical
 * address translation (kern_virt_to_phys) and the PCI accessors are all
 * `static` in pci.h, backed by a single pool whose page allocator
 * kernel.c installs at boot. Compiling this as its own translation unit
 * would give it a *second* copy of that pool with a null allocator, and
 * the first BAR mapping would return memory that no page table entry
 * points at. So it shares the kernel's translation unit, like the
 * e1000 beside it.
 *
 * ---- the shape of a wireless driver ----
 *
 * An Ethernet NIC is a pipe: frames in, frames out, and the hardware
 * has no opinion about what they mean. A wireless NIC is a peer on a
 * network that has to be *joined*, and joining is a conversation:
 *
 *     scan        listen on each channel for beacons
 *     authenticate  the vestigial two-frame open exchange
 *     associate   ask to join, get an association ID
 *     4-way       prove both ends know the passphrase, derive keys
 *     encrypt     every data frame from here on
 *
 * Only the last of those looks anything like Ethernet, and it is the
 * only part lwIP ever sees. Everything above the vx_nic_* seam at the
 * bottom of this file is Ethernet frames; everything below is 802.11.
 *
 * ---- what runs and what does not ----
 *
 * This is worth being exact about, because the difference is invisible
 * from the code.
 *
 * The frame layer (net/ieee80211.h) and the WPA2 engine (net/wpa2.h)
 * are complete and are checked against published test vectors by
 * tools/wifi_test.c -- the PMK against IEEE 802.11i's own passphrase
 * vectors, the key wrap against RFC 3394, CCMP against RFC 3610, and
 * the whole 4-way handshake against a synthetic authenticator. Those
 * run on the host, every build, and they are the parts that a wrong
 * byte would silently break.
 *
 * The chipset back-ends below are real register programming -- the PCIe
 * probe, the BAR mapping, the device reset, the APM power sequence, the
 * DMA rings and the firmware-load protocol are what the hardware
 * documents specify. What they cannot do is finish. Both Intel's
 * iwlwifi-class parts and Realtek's PCIe parts are firmware-driven: the
 * silicon has no usable MAC until a signed microcode image is DMA'd
 * into it and booted through an ALIVE handshake, and scanning,
 * association and even reading the MAC address out of OTP are all
 * firmware commands rather than register writes. That image is a
 * proprietary binary -- iwlwifi-*.ucode is a few hundred kilobytes of
 * Intel-signed code -- and it is not in this tree and cannot be written
 * from the specification. wifi_provide_firmware() is where it goes if
 * it is ever supplied; without it the driver probes, resets, maps and
 * then stops at WIFI_STATE_NO_FIRMWARE and says so.
 *
 * The second thing that does not run: QEMU emulates no wireless device
 * at all. There is no `-device iwlwifi`. So on the machine this system
 * is actually developed against, wifi_init() finds nothing on the bus
 * and returns, and the Ethernet path is untouched. This file changes
 * nothing about a build that has no radio in it.
 */

#include <stdint.h>
#include "../pci.h"
#include "ieee80211.h"
#include "wpa2.h"

/* ===== logging ===== */

static void wifi_log(const char *msg) {
    serial_puts("[wifi] ");
    serial_puts(msg);
    serial_putc('\n');
}

static void wifi_log_mac(const char *what, const uint8_t *mac) {
    static const char hex[] = "0123456789abcdef";
    serial_puts("[wifi] ");
    serial_puts(what);
    serial_puts(": ");
    for (int i = 0; i < 6; i++) {
        serial_putc(hex[mac[i] >> 4]);
        serial_putc(hex[mac[i] & 0xF]);
        if (i != 5) serial_putc(':');
    }
    serial_putc('\n');
}

/* A crude spin. The scheduler may not be running when the device is
 * reset, so this cannot sleep; the counts are the ones the hardware
 * documents ask for, expressed as iterations rather than microseconds
 * because there is no calibrated timer this early either. */
static void wifi_udelay(uint32_t usec) {
    for (volatile uint32_t i = 0; i < usec * 40u; i++) { }
}

/* ===== the state a radio can be in ===== */

typedef enum {
    WIFI_STATE_ABSENT = 0,      /* nothing on the bus                  */
    WIFI_STATE_NO_FIRMWARE,     /* found, reset, no microcode to load  */
    WIFI_STATE_IDLE,            /* firmware alive, not associated      */
    WIFI_STATE_SCANNING,
    WIFI_STATE_AUTHENTICATING,
    WIFI_STATE_ASSOCIATING,
    WIFI_STATE_HANDSHAKING,     /* the 4-way is in flight              */
    WIFI_STATE_CONNECTED,
    WIFI_STATE_FAILED
} wifi_state_t;

static const char *wifi_state_name(wifi_state_t s) {
    switch (s) {
    case WIFI_STATE_ABSENT:          return "absent";
    case WIFI_STATE_NO_FIRMWARE:     return "no firmware";
    case WIFI_STATE_IDLE:            return "idle";
    case WIFI_STATE_SCANNING:        return "scanning";
    case WIFI_STATE_AUTHENTICATING:  return "authenticating";
    case WIFI_STATE_ASSOCIATING:     return "associating";
    case WIFI_STATE_HANDSHAKING:     return "4-way handshake";
    case WIFI_STATE_CONNECTED:       return "connected";
    case WIFI_STATE_FAILED:          return "failed";
    }
    return "?";
}

/* ===== the chipset back-end interface =====
 *
 * Two families are implemented below and they share almost nothing --
 * Intel's is a firmware host with a command queue, Realtek's is a much
 * more direct MAC with a power-on sequence table. What they have in
 * common is exactly this: bring the silicon up, hand it a frame, take
 * a frame back, park on a channel, and load a key into the hardware
 * cipher engine. Everything above this interface is chipset-agnostic.
 */

#define WIFI_MAX_BSS        24
#define WIFI_RX_RING        64
#define WIFI_TX_RING        64
#define WIFI_BUF_SIZE       2048

struct wifi_dev;

typedef struct {
    const char *name;
    int  (*probe)(struct wifi_dev *d, const pci_dev_t *pci);
    int  (*start)(struct wifi_dev *d);
    void (*stop)(struct wifi_dev *d);
    int  (*tx)(struct wifi_dev *d, const uint8_t *frame, uint16_t len);
    int  (*rx)(struct wifi_dev *d, uint8_t *out, uint16_t max,
               uint16_t *got, int8_t *rssi);
    int  (*set_channel)(struct wifi_dev *d, uint8_t chan);
    int  (*set_key)(struct wifi_dev *d, int idx, const uint8_t *key,
                    uint32_t len, const uint8_t *addr);
} wifi_ops_t;

typedef struct wifi_dev {
    const wifi_ops_t   *ops;
    pci_dev_t           pci;
    volatile uint8_t   *mmio;
    uint64_t            mmio_len;
    uint8_t             mac[6];
    uint8_t             channel;
    wifi_state_t        state;

    /* Set when the hardware does CCMP itself. Intel parts do; the
     * software path below stays in place for those that do not, and is
     * what the host test exercises. */
    int                 hw_crypto;

    uint16_t            seq;        /* 802.11 sequence number counter  */
    uint16_t            aid;        /* association ID from the AP      */
    void               *priv;
} wifi_dev_t;

static wifi_dev_t wifi_dev;

/* ===== firmware ===== */

static const uint8_t *wifi_fw_image = 0;
static uint32_t       wifi_fw_len   = 0;

/*
 * Hand the driver a microcode image.
 *
 * Called from kernel.c if a firmware module was supplied at boot. The
 * pointer is kept rather than copied -- limine modules stay mapped for
 * the life of the system, and the image is large enough that copying it
 * into the heap to achieve nothing would be a waste of a megabyte.
 */
static void wifi_provide_firmware(const uint8_t *image, uint32_t len) {
    wifi_fw_image = image;
    wifi_fw_len   = len;
}

/* ===========================================================
 * Intel iwlwifi-class PCIe
 * ===========================================================
 *
 * Register names and offsets are the ones in Intel's published
 * interface for the 6000/7000/8000 series. The bring-up order is not
 * arbitrary: the device must be told it is being prepared before it
 * will assert NIC_READY, the clocks have to be running before the
 * DMA rings can be written, and the INIT_DONE bit has to be set before
 * the firmware is allowed to touch host memory.
 */

#define CSR_HW_IF_CONFIG_REG        0x000
#define CSR_INT_COALESCING          0x004
#define CSR_INT                     0x008
#define CSR_INT_MASK                0x00c
#define CSR_FH_INT_STATUS           0x010
#define CSR_RESET                   0x020
#define CSR_GP_CNTRL                0x024
#define CSR_HW_REV                  0x028
#define CSR_EEPROM_REG              0x02c
#define CSR_EEPROM_GP               0x030
#define CSR_OTP_GP_REG              0x034
#define CSR_GIO_REG                 0x03c
#define CSR_UCODE_DRV_GP1_CLR       0x05c
#define CSR_LED_REG                 0x094
#define CSR_DRAM_INT_TBL_REG        0x0a0
#define CSR_GIO_CHICKEN_BITS        0x100
#define CSR_HW_REV_WA_REG           0x22c

#define CSR_HW_IF_CONFIG_NIC_READY      0x00400000
#define CSR_HW_IF_CONFIG_PREPARE_DONE   0x02000000
#define CSR_HW_IF_CONFIG_PREPARE        0x08000000

#define CSR_RESET_SW_RESET              0x00000080
#define CSR_RESET_MASTER_DISABLED       0x00000100
#define CSR_RESET_STOP_MASTER           0x00000200

#define CSR_GP_CNTRL_MAC_CLOCK_READY    0x00000001
#define CSR_GP_CNTRL_INIT_DONE          0x00000004
#define CSR_GP_CNTRL_MAC_ACCESS_REQ     0x00000008
#define CSR_GP_CNTRL_HW_RF_KILL_SW      0x08000000

#define CSR_GIO_CHICKEN_L1A_NO_L0S_RX   0x00800000

/* indirect access to the peripheral register space */
#define HBUS_TARG_PRPH_WADDR        0x044
#define HBUS_TARG_PRPH_RADDR        0x048
#define HBUS_TARG_PRPH_WDAT         0x04c
#define HBUS_TARG_PRPH_RDAT         0x050
#define HBUS_TARG_MEM_RADDR         0x40c
#define HBUS_TARG_MEM_WADDR         0x410
#define HBUS_TARG_MEM_WDAT          0x418
#define HBUS_TARG_MEM_RDAT          0x41c

/* the analogue power management gate */
#define APMG_CLK_EN_REG             0x3004
#define APMG_CLK_DIS_REG            0x3008
#define APMG_PS_CTRL_REG            0x300c
#define APMG_PCIDEV_STT_REG         0x3010
#define APMG_DIGITAL_SVR_REG        0x3058

#define APMG_CLK_VAL_DMA_CLK_RQT    0x00000200
#define APMG_PS_CTRL_VAL_PWR_SRC_VMAIN  0x00000000
#define APMG_PS_CTRL_MSK_PWR_SRC        0x03000000
#define APMG_PCIDEV_STT_VAL_L1_ACT_DIS  0x00000800

/* flow handler: the DMA rings */
#define FH_MEM_RSCSR_CHNL0          0x1bc0
#define FH_RSCSR_CHNL0_STTS_WPTR    (FH_MEM_RSCSR_CHNL0 + 0x000)
#define FH_RSCSR_CHNL0_RBDCB_BASE   (FH_MEM_RSCSR_CHNL0 + 0x004)
#define FH_RSCSR_CHNL0_WPTR         (FH_MEM_RSCSR_CHNL0 + 0x008)
#define FH_MEM_RCSR_CHNL0_CONFIG    0x1c00
#define FH_MEM_RSSR_SHARED_CTRL     0x1c40
#define FH_MEM_RSSR_RX_STATUS       0x1c44
#define FH_MEM_CBBC_0_15_LOWER      0x19d0
#define FH_TSSR_TX_STATUS           0x1eb0
#define FH_TSSR_TX_ERROR            0x1eb8

#define FH_RCSR_RX_CONFIG_ENABLE    0x80000000

#define INTEL_WIFI_VENDOR           0x8086

/*
 * The parts this back-end knows how to bring up. All of them present
 * the same CSR block and the same flow handler; they differ in which
 * microcode image they want, which is why the list is long and the
 * code is not.
 */
static const uint16_t intel_wifi_ids[] = {
    0x4232, 0x4235, 0x4236, 0x4237, 0x4239,   /* 5000 / 5300           */
    0x0082, 0x0085, 0x0083, 0x0084, 0x008a,   /* 6000 / 6205           */
    0x0890, 0x0891, 0x0892, 0x0893, 0x0894,   /* 7260 / 3160           */
    0x08b1, 0x08b2, 0x08b3, 0x08b4,           /* 7260 / 7265           */
    0x095a, 0x095b,                           /* 7265D                 */
    0x24f3, 0x24f4, 0x24fd,                   /* 8260 / 8265           */
    0x2526, 0x2723, 0x2725,                   /* 9560 / AX200 / AX210  */
    0
};

/* Descriptors the flow handler walks. The receive side is a ring of
 * bare physical addresses shifted right by eight -- the hardware only
 * has 24 bits of address in each slot and requires 256-byte alignment
 * as a consequence. */
static uint32_t intel_rx_bd[WIFI_RX_RING] __attribute__((aligned(4096)));
static uint8_t  intel_rx_buf[WIFI_RX_RING][WIFI_BUF_SIZE]
                    __attribute__((aligned(4096)));
static uint32_t intel_rx_status[8] __attribute__((aligned(4096)));
static uint8_t  intel_tx_buf[WIFI_TX_RING][WIFI_BUF_SIZE]
                    __attribute__((aligned(4096)));

typedef struct {
    uint32_t rx_read;
    uint32_t tx_write;
    uint32_t hw_rev;
    int      alive;
} intel_priv_t;

static intel_priv_t intel_priv;

static inline void iwl_write32(wifi_dev_t *d, uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(d->mmio + reg) = val;
}

static inline uint32_t iwl_read32(wifi_dev_t *d, uint32_t reg) {
    return *(volatile uint32_t *)(d->mmio + reg);
}

static void iwl_set_bit(wifi_dev_t *d, uint32_t reg, uint32_t mask) {
    iwl_write32(d, reg, iwl_read32(d, reg) | mask);
}

static void iwl_clear_bit(wifi_dev_t *d, uint32_t reg, uint32_t mask) {
    iwl_write32(d, reg, iwl_read32(d, reg) & ~mask);
}

static int iwl_poll_bit(wifi_dev_t *d, uint32_t reg, uint32_t bits,
                        uint32_t mask, uint32_t tries) {
    for (uint32_t i = 0; i < tries; i++) {
        if ((iwl_read32(d, reg) & mask) == bits) return 1;
        wifi_udelay(10);
    }
    return 0;
}

/* The peripheral space is behind an address/data pair rather than
 * mapped, and the upper bits of the address register select a
 * three-bit "target" that must be all-ones for ordinary access. */
static uint32_t iwl_read_prph(wifi_dev_t *d, uint32_t addr) {
    iwl_write32(d, HBUS_TARG_PRPH_RADDR, ((addr & 0x000FFFFF) | (3u << 24)));
    return iwl_read32(d, HBUS_TARG_PRPH_RDAT);
}

static void iwl_write_prph(wifi_dev_t *d, uint32_t addr, uint32_t val) {
    iwl_write32(d, HBUS_TARG_PRPH_WADDR, ((addr & 0x000FFFFF) | (3u << 24)));
    iwl_write32(d, HBUS_TARG_PRPH_WDAT, val);
}

/*
 * Ask the device to become accessible.
 *
 * The PREPARE bit starts a hardware sequence that can take milliseconds
 * and that fails permanently if the part is held in reset by the
 * platform -- a laptop with a hardware wireless switch in the off
 * position lands here.
 */
static int iwl_prepare(wifi_dev_t *d) {
    if (iwl_read32(d, CSR_HW_IF_CONFIG_REG) & CSR_HW_IF_CONFIG_NIC_READY)
        return 0;

    iwl_set_bit(d, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_PREPARE);

    if (!iwl_poll_bit(d, CSR_HW_IF_CONFIG_REG, 0,
                      CSR_HW_IF_CONFIG_PREPARE_DONE, 15000)) {
        wifi_log("device did not complete PREPARE");
        return -1;
    }

    iwl_set_bit(d, CSR_HW_IF_CONFIG_REG, CSR_HW_IF_CONFIG_NIC_READY);

    if (!iwl_poll_bit(d, CSR_HW_IF_CONFIG_REG,
                      CSR_HW_IF_CONFIG_NIC_READY,
                      CSR_HW_IF_CONFIG_NIC_READY, 15000)) {
        wifi_log("device never asserted NIC_READY");
        return -1;
    }
    return 0;
}

/*
 * The APM init sequence: clocks, power source, and the L1 workarounds.
 *
 * The two chicken bits are not optional on any part in the list above.
 * With L0s enabled on the receive side the device corrupts DMA under
 * load in a way that looks like random frame loss, and Intel's own
 * driver has disabled it unconditionally for fifteen years.
 */
static int iwl_apm_init(wifi_dev_t *d) {
    iwl_set_bit(d, CSR_GIO_CHICKEN_BITS, CSR_GIO_CHICKEN_L1A_NO_L0S_RX);
    iwl_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);

    iwl_set_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_MAC_ACCESS_REQ);
    if (!iwl_poll_bit(d, CSR_GP_CNTRL,
                      CSR_GP_CNTRL_MAC_CLOCK_READY,
                      CSR_GP_CNTRL_MAC_CLOCK_READY, 25000)) {
        wifi_log("MAC clock never became ready");
        return -1;
    }

    /* Route power from the main supply rather than the PCIe rail, and
     * request the DMA clock the flow handler runs on. */
    iwl_write_prph(d, APMG_PS_CTRL_REG,
                   (iwl_read_prph(d, APMG_PS_CTRL_REG) &
                    ~APMG_PS_CTRL_MSK_PWR_SRC) |
                   APMG_PS_CTRL_VAL_PWR_SRC_VMAIN);
    iwl_write_prph(d, APMG_CLK_EN_REG, APMG_CLK_VAL_DMA_CLK_RQT);
    wifi_udelay(20);

    iwl_write_prph(d, APMG_PCIDEV_STT_REG, APMG_PCIDEV_STT_VAL_L1_ACT_DIS);

    return 0;
}

static void iwl_apm_stop(wifi_dev_t *d) {
    iwl_clear_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_INIT_DONE);
    iwl_clear_bit(d, CSR_GP_CNTRL, CSR_GP_CNTRL_MAC_ACCESS_REQ);
}

/* Stop the bus master and hold the part in software reset. */
static void iwl_sw_reset(wifi_dev_t *d) {
    iwl_set_bit(d, CSR_RESET, CSR_RESET_STOP_MASTER);
    iwl_poll_bit(d, CSR_RESET, CSR_RESET_MASTER_DISABLED,
                 CSR_RESET_MASTER_DISABLED, 10000);
    iwl_set_bit(d, CSR_RESET, CSR_RESET_SW_RESET);
    wifi_udelay(20);
    iwl_clear_bit(d, CSR_RESET, CSR_RESET_SW_RESET);
}

/* Program the receive ring into the flow handler. */
static void iwl_rx_init(wifi_dev_t *d) {
    intel_priv.rx_read = 0;

    for (int i = 0; i < WIFI_RX_RING; i++) {
        uint64_t phys = kern_virt_to_phys(&intel_rx_buf[i][0]);
        intel_rx_bd[i] = (uint32_t)(phys >> 8);
    }

    iwl_write32(d, FH_MEM_RCSR_CHNL0_CONFIG, 0);
    iwl_write32(d, FH_RSCSR_CHNL0_WPTR, 0);
    iwl_write32(d, FH_RSCSR_CHNL0_RBDCB_BASE,
                (uint32_t)(kern_virt_to_phys(intel_rx_bd) >> 8));
    iwl_write32(d, FH_RSCSR_CHNL0_STTS_WPTR,
                (uint32_t)(kern_virt_to_phys(intel_rx_status) >> 4));

    /* enable, 4K buffers, single-frame mode, an interrupt every frame */
    iwl_write32(d, FH_MEM_RCSR_CHNL0_CONFIG,
                FH_RCSR_RX_CONFIG_ENABLE | (1u << 20) | (1u << 12) | 8u);

    iwl_write32(d, FH_RSCSR_CHNL0_WPTR, WIFI_RX_RING - 1);
}

/*
 * Load and start the microcode.
 *
 * This is the point the whole back-end exists to reach and the point it
 * cannot pass without a firmware image. The sequence itself is
 * implemented: the image is a container of typed sections, each with a
 * destination address inside the device's own instruction or data
 * memory, and each is written through the indirect memory window before
 * the part is released from reset and polled for its ALIVE response.
 *
 * With no image, that is reported once and the device is left reset,
 * powered down and harmless rather than half-initialised.
 */
static int iwl_load_firmware(wifi_dev_t *d) {
    uint32_t off = 0;

    if (!wifi_fw_image || wifi_fw_len < 16) {
        wifi_log("no microcode image supplied");
        wifi_log("  the radio was found, reset and mapped, but Intel");
        wifi_log("  parts have no MAC until signed firmware is loaded.");
        wifi_log("  supply one with wifi_provide_firmware() to go further.");
        d->state = WIFI_STATE_NO_FIRMWARE;
        return -1;
    }

    wifi_log("loading microcode into device memory");

    /* Walk the container. Each section is a 32-bit destination address,
     * a 32-bit byte count, then the payload, padded to four bytes. */
    while (off + 8 <= wifi_fw_len) {
        uint32_t dst = ieee80211_get_le32(wifi_fw_image + off);
        uint32_t len = ieee80211_get_le32(wifi_fw_image + off + 4);
        off += 8;

        if (len == 0 || off + len > wifi_fw_len) break;

        iwl_write32(d, HBUS_TARG_MEM_WADDR, dst);
        for (uint32_t i = 0; i + 4 <= len; i += 4)
            iwl_write32(d, HBUS_TARG_MEM_WDAT,
                        ieee80211_get_le32(wifi_fw_image + off + i));

        off += (len + 3) & ~3u;
    }

    /* Release the processor and wait for it to say it is alive. */
    iwl_clear_bit(d, CSR_RESET, CSR_RESET_SW_RESET);

    if (!iwl_poll_bit(d, CSR_GP_CNTRL,
                      CSR_GP_CNTRL_MAC_CLOCK_READY,
                      CSR_GP_CNTRL_MAC_CLOCK_READY, 25000)) {
        wifi_log("microcode did not reach ALIVE");
        return -1;
    }

    intel_priv.alive = 1;
    wifi_log("microcode alive");
    return 0;
}

static int intel_probe(wifi_dev_t *d, const pci_dev_t *pci) {
    uint64_t bar_phys, bar_size;

    d->pci = *pci;
    d->priv = &intel_priv;

    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);

    if (pci_bar(pci, 0, &bar_phys, &bar_size) != 0) {
        wifi_log("BAR0 is not a memory BAR");
        return -1;
    }
    if (bar_size < 0x2000) bar_size = 0x2000;

    d->mmio = mmio_map(bar_phys, bar_size);
    if (!d->mmio) {
        wifi_log("BAR0 mapping failed");
        return -1;
    }
    d->mmio_len = bar_size;

    intel_priv.hw_rev = iwl_read32(d, CSR_HW_REV);
    serial_puts("[wifi] Intel hardware revision 0x");
    serial_put_hex32(intel_priv.hw_rev);
    serial_putc('\n');

    /* A device that reads back all-ones is not responding: the BAR is
     * mapped somewhere the bridge does not decode. Continuing from here
     * writes into nothing and hangs on the first poll. */
    if (intel_priv.hw_rev == 0xFFFFFFFFu) {
        wifi_log("device not responding on its BAR");
        return -1;
    }

    iwl_write32(d, CSR_INT_MASK, 0);
    iwl_write32(d, CSR_INT, 0xFFFFFFFFu);

    if (iwl_prepare(d) != 0) return -1;
    iwl_sw_reset(d);
    if (iwl_apm_init(d) != 0) return -1;

    if (iwl_read32(d, CSR_GP_CNTRL) & CSR_GP_CNTRL_HW_RF_KILL_SW) {
        wifi_log("radio is disabled by the hardware kill switch");
        return -1;
    }

    iwl_rx_init(d);
    d->hw_crypto = 1;

    return iwl_load_firmware(d);
}

static int intel_start(wifi_dev_t *d) {
    if (!intel_priv.alive) return -1;
    iwl_write32(d, CSR_INT_MASK, 0xFFFFFFFFu);
    return 0;
}

static void intel_stop(wifi_dev_t *d) {
    iwl_write32(d, CSR_INT_MASK, 0);
    iwl_sw_reset(d);
    iwl_apm_stop(d);
    intel_priv.alive = 0;
}

static int intel_tx(wifi_dev_t *d, const uint8_t *frame, uint16_t len) {
    uint32_t slot;
    if (!intel_priv.alive) return -1;
    if (len > WIFI_BUF_SIZE) return -1;

    slot = intel_priv.tx_write % WIFI_TX_RING;
    for (uint16_t i = 0; i < len; i++) intel_tx_buf[slot][i] = frame[i];

    /* Handing the descriptor to the scheduler is a firmware command on
     * these parts rather than a register write, so this is where the
     * path ends until an image is loaded. */
    intel_priv.tx_write++;
    (void)d;
    return 0;
}

static int intel_rx(wifi_dev_t *d, uint8_t *out, uint16_t max,
                    uint16_t *got, int8_t *rssi) {
    uint32_t hw_write;
    uint32_t idx;
    uint8_t *buf;
    uint16_t len;

    if (!intel_priv.alive) return 0;

    /* The hardware write pointer lives in the status block it DMAs
     * into host memory, not in a register: reading a register here
     * would cost a bus round trip per poll. */
    hw_write = intel_rx_status[0] & 0x0FFF;
    if (hw_write == intel_priv.rx_read) return 0;

    idx = intel_priv.rx_read % WIFI_RX_RING;
    buf = intel_rx_buf[idx];

    /* Each buffer holds a small hardware header before the frame: a
     * four-byte length and the per-frame receive statistics. */
    len = (uint16_t)(ieee80211_get_le32(buf) & 0x3FFF);
    if (len > max) len = max;
    if (len > WIFI_BUF_SIZE - 8) len = WIFI_BUF_SIZE - 8;

    for (uint16_t i = 0; i < len; i++) out[i] = buf[8 + i];
    *got = len;
    if (rssi) *rssi = (int8_t)(buf[4]);

    intel_priv.rx_read++;
    iwl_write32(d, FH_RSCSR_CHNL0_WPTR,
                (intel_priv.rx_read + WIFI_RX_RING - 1) & 0x0FFF);
    return 1;
}

static int intel_set_channel(wifi_dev_t *d, uint8_t chan) {
    /* Tuning is a firmware command (PHY_CONTEXT_CMD); there is no
     * register that sets the channel directly on these parts. */
    if (!intel_priv.alive) return -1;
    d->channel = chan;
    return 0;
}

static int intel_set_key(wifi_dev_t *d, int idx, const uint8_t *key,
                         uint32_t len, const uint8_t *addr) {
    (void)d; (void)idx; (void)key; (void)len; (void)addr;
    if (!intel_priv.alive) return -1;
    return 0;   /* ADD_STA firmware command */
}

static const wifi_ops_t intel_ops = {
    "Intel Wireless", intel_probe, intel_start, intel_stop,
    intel_tx, intel_rx, intel_set_channel, intel_set_key
};

/* ===========================================================
 * Realtek PCIe (rtl8192/8188/8821 class)
 * ===========================================================
 *
 * A more direct part than the Intel: the MAC registers are real
 * registers and the power-on sequence is a table of writes rather than
 * a conversation with a processor. It still wants firmware -- the rate
 * adaptation and power save live there -- but far more of the bring-up
 * is visible from the host.
 */

#define REG_SYS_ISO_CTRL            0x0000
#define REG_SYS_FUNC_EN             0x0002
#define REG_APS_FSMCO               0x0004
#define REG_SYS_CLKR                0x0008
#define REG_9346CR                  0x000a
#define REG_AFE_MISC                0x0010
#define REG_SPS0_CTRL               0x0011
#define REG_RSV_CTRL                0x001c
#define REG_RF_CTRL                 0x001f
#define REG_LDOA15_CTRL             0x0020
#define REG_EFUSE_CTRL              0x0030
#define REG_PWR_DATA                0x0038
#define REG_CR                      0x0100
#define REG_PBP                     0x0104
#define REG_TRXDMA_CTRL             0x010c
#define REG_TRXFF_BNDY              0x0114
#define REG_LLT_INIT                0x01e0
#define REG_RQPN                    0x0200
#define REG_RX_DESA                 0x0340
#define REG_RCR                     0x0608
#define REG_MACID                   0x0610
#define REG_BSSID                   0x0618
#define REG_CAMCMD                  0x0670
#define REG_CAMWRITE                0x0674
#define REG_CAMREAD                 0x0678
#define REG_RXFLTMAP0               0x06a0

#define RCR_AAP                     0x00000001  /* accept all           */
#define RCR_APM                     0x00000002  /* accept ours          */
#define RCR_AM                      0x00000004  /* accept multicast     */
#define RCR_AB                      0x00000008  /* accept broadcast     */
#define RCR_APWRMGT                 0x00000020
#define RCR_CBSSID_DATA             0x00000040
#define RCR_CBSSID_BCN              0x00000080
#define RCR_APP_PHYST_RXFF          0x02000000

#define REALTEK_VENDOR              0x10ec

static const uint16_t realtek_wifi_ids[] = {
    0x8176, 0x8178, 0x8179,     /* RTL8188/8192 CU / EE   */
    0x8171, 0x8172, 0x8173,
    0x8191, 0x8192, 0x8193,
    0x8812, 0x8813, 0x8821,     /* RTL8812AE / 8821AE     */
    0xb723, 0xc821, 0xc82f,     /* RTL8723BE / 8821CE     */
    0
};

static uint8_t rtl_rx_buf[WIFI_RX_RING][WIFI_BUF_SIZE]
                   __attribute__((aligned(4096)));

typedef struct {
    uint32_t rx_read;
    int      up;
} rtl_priv_t;

static rtl_priv_t rtl_priv;

static inline void rtl_write8(wifi_dev_t *d, uint32_t r, uint8_t v) {
    *(volatile uint8_t *)(d->mmio + r) = v;
}
static inline void rtl_write16(wifi_dev_t *d, uint32_t r, uint16_t v) {
    *(volatile uint16_t *)(d->mmio + r) = v;
}
static inline void rtl_write32(wifi_dev_t *d, uint32_t r, uint32_t v) {
    *(volatile uint32_t *)(d->mmio + r) = v;
}
static inline uint8_t rtl_read8(wifi_dev_t *d, uint32_t r) {
    return *(volatile uint8_t *)(d->mmio + r);
}
static inline uint16_t rtl_read16(wifi_dev_t *d, uint32_t r) {
    return *(volatile uint16_t *)(d->mmio + r);
}
static inline uint32_t rtl_read32(wifi_dev_t *d, uint32_t r) {
    return *(volatile uint32_t *)(d->mmio + r);
}

/*
 * The card power-on sequence.
 *
 * Realtek parts come out of reset with the analogue front end and the
 * digital core independently gated, and the order matters: enabling the
 * MAC before the regulator has settled leaves the part in a state where
 * registers read back plausible values and nothing transmits.
 */
static int rtl_power_on(wifi_dev_t *d) {
    uint16_t val16;
    uint8_t  val8;

    /* release the analogue power-down */
    val8 = rtl_read8(d, REG_APS_FSMCO + 1);
    rtl_write8(d, REG_APS_FSMCO + 1, (uint8_t)(val8 & ~0x04));
    wifi_udelay(1000);

    /* wait for the power-on ready bit */
    for (int i = 0; i < 5000; i++) {
        if (rtl_read8(d, REG_APS_FSMCO + 1) & 0x02) break;
        wifi_udelay(10);
    }

    val8 = rtl_read8(d, REG_SPS0_CTRL);
    rtl_write8(d, REG_SPS0_CTRL, (uint8_t)(val8 | 0x01));

    /* enable the digital core and the MAC clock */
    val16 = rtl_read16(d, REG_SYS_FUNC_EN);
    rtl_write16(d, REG_SYS_FUNC_EN, (uint16_t)(val16 | 0x000F));
    wifi_udelay(200);

    val16 = rtl_read16(d, REG_SYS_CLKR);
    rtl_write16(d, REG_SYS_CLKR, (uint16_t)(val16 | 0x0C00));

    /* take the MAC out of reset: enable the transmit and receive DMA
     * engines and the protocol offload block */
    rtl_write8(d, REG_CR, 0x00);
    wifi_udelay(200);
    rtl_write16(d, REG_CR, 0x2F3F);

    return 0;
}

static int realtek_probe(wifi_dev_t *d, const pci_dev_t *pci) {
    uint64_t bar_phys, bar_size;
    uint32_t probe;
    int bar_idx = 2;

    d->pci = *pci;
    d->priv = &rtl_priv;

    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);

    /* Realtek wireless parts put their register block in BAR2; BAR0 is
     * the I/O alias and is not usable for the descriptor rings. */
    if (pci_bar(pci, bar_idx, &bar_phys, &bar_size) != 0) {
        bar_idx = 0;
        if (pci_bar(pci, bar_idx, &bar_phys, &bar_size) != 0) {
            wifi_log("no usable memory BAR");
            return -1;
        }
    }
    if (bar_size < 0x1000) bar_size = 0x1000;

    d->mmio = mmio_map(bar_phys, bar_size);
    if (!d->mmio) {
        wifi_log("BAR mapping failed");
        return -1;
    }
    d->mmio_len = bar_size;

    probe = rtl_read32(d, REG_SYS_ISO_CTRL);
    if (probe == 0xFFFFFFFFu) {
        wifi_log("device not responding on its BAR");
        return -1;
    }

    if (rtl_power_on(d) != 0) return -1;

    /* The MAC address lives in the efuse and is shadowed into the MACID
     * registers once the part is powered; six bytes, low first. */
    for (int i = 0; i < 6; i++) d->mac[i] = rtl_read8(d, REG_MACID + i);

    /* Receive filter: our own address, broadcast and multicast, and the
     * beacons of the BSS we are on. Promiscuous is deliberately off --
     * it multiplies the interrupt load by every station in range. */
    rtl_write32(d, REG_RCR, RCR_APM | RCR_AM | RCR_AB |
                            RCR_CBSSID_DATA | RCR_CBSSID_BCN |
                            RCR_APP_PHYST_RXFF);
    rtl_write16(d, REG_RXFLTMAP0, 0xFFFF);

    rtl_write32(d, REG_RX_DESA, (uint32_t)kern_virt_to_phys(rtl_rx_buf));

    d->hw_crypto = 0;   /* the software CCMP path below carries these */
    rtl_priv.up = 1;

    if (!wifi_fw_image) {
        wifi_log("Realtek MAC is up; no firmware image for rate control");
        d->state = WIFI_STATE_NO_FIRMWARE;
        return -1;
    }
    return 0;
}

static int realtek_start(wifi_dev_t *d) { (void)d; return rtl_priv.up ? 0 : -1; }

static void realtek_stop(wifi_dev_t *d) {
    if (!rtl_priv.up) return;
    rtl_write16(d, REG_CR, 0);
    rtl_write8(d, REG_RF_CTRL, 0);
    rtl_priv.up = 0;
}

static int realtek_tx(wifi_dev_t *d, const uint8_t *frame, uint16_t len) {
    (void)d; (void)frame; (void)len;
    return rtl_priv.up ? 0 : -1;
}

static int realtek_rx(wifi_dev_t *d, uint8_t *out, uint16_t max,
                      uint16_t *got, int8_t *rssi) {
    (void)d; (void)out; (void)max; (void)got; (void)rssi;
    return 0;
}

static int realtek_set_channel(wifi_dev_t *d, uint8_t chan) {
    if (!rtl_priv.up) return -1;
    d->channel = chan;
    return 0;
}

static int realtek_set_key(wifi_dev_t *d, int idx, const uint8_t *key,
                           uint32_t len, const uint8_t *addr) {
    /* The CAM is a 32-entry content-addressable key store written
     * through a command/data register pair, four bytes at a time. */
    if (!rtl_priv.up || len > 16) return -1;

    for (int i = 0; i < 6; i++) {
        uint32_t word = 0;
        if (i == 0) {
            word = (uint32_t)addr[0] | ((uint32_t)addr[1] << 8) |
                   ((uint32_t)addr[2] << 16) | ((uint32_t)addr[3] << 24);
        } else if (i == 1) {
            word = (uint32_t)addr[4] | ((uint32_t)addr[5] << 8);
        } else {
            uint32_t off = (uint32_t)(i - 2) * 4;
            if (off < len)
                word = (uint32_t)key[off] | ((uint32_t)key[off + 1] << 8) |
                       ((uint32_t)key[off + 2] << 16) |
                       ((uint32_t)key[off + 3] << 24);
        }
        rtl_write32(d, REG_CAMWRITE, word);
        rtl_write32(d, REG_CAMCMD,
                    0x80010000u | (uint32_t)((idx << 3) + i));
        wifi_udelay(100);
    }
    return 0;
}

static const wifi_ops_t realtek_ops = {
    "Realtek Wireless", realtek_probe, realtek_start, realtek_stop,
    realtek_tx, realtek_rx, realtek_set_channel, realtek_set_key
};

/* ===========================================================
 * the station: scan, join, and stay joined
 * =========================================================== */

static wifi_bss_t wifi_bss_list[WIFI_MAX_BSS];
static int        wifi_bss_count = 0;

static wpa_sm_t   wifi_sm;
static wifi_bss_t wifi_target;
static char       wifi_pass[64];
static int        wifi_have_target = 0;

static uint8_t    wifi_assoc_rsn_ie[64];
static uint8_t    wifi_assoc_rsn_len = 0;

static uint32_t   wifi_rx_frames = 0;
static uint32_t   wifi_tx_frames = 0;
static uint32_t   wifi_rx_drop   = 0;
static uint32_t   wifi_mic_fail  = 0;

/*
 * The frame buffers.
 *
 * Static rather than automatic, which is the same choice vx_linkoutput()
 * makes in lwipglue.c and for the same reason: a kernel thread gets a
 * 32 KB stack, and a receive path that puts a 2 KB frame, a 2 KB
 * decrypted copy and a 2 KB Ethernet copy on it -- then calls something
 * that does the same again to answer a rekey -- is most of that stack
 * gone on one packet.
 *
 * Static buffers are not reentrant, and these are safe only because
 * exactly one thread is ever in this path at a time. The scan and join
 * run from the terminal while the link is down; the receive path runs
 * on lwIP's thread and vx_use_wifi() routes to it only once
 * wifi_connected() is true, which cannot be the case while a join is
 * still in progress. Anything that made the join concurrent with
 * carrying traffic would have to revisit this.
 */
static uint8_t wifi_rx_frame[WIFI_BUF_SIZE];
static uint8_t wifi_plain[WIFI_BUF_SIZE];
static uint8_t wifi_cipher[WIFI_BUF_SIZE];
static uint8_t wifi_eth_buf[WIFI_BUF_SIZE];
static uint8_t wifi_mgmt_buf[WIFI_BUF_SIZE];
static uint8_t wifi_eapol_buf[512];

/* The 2.4 GHz channels that are legal essentially everywhere, plus the
 * lower 5 GHz band. Scanning parks on each in turn. */
static const uint8_t wifi_scan_chans[] = {
    1, 6, 11, 2, 7, 3, 8, 4, 9, 5, 10, 12, 13,
    36, 40, 44, 48, 149, 153, 157, 161
};

/* Defined below; named here because the join sequence and the data
 * path each reach into the other -- a handshake starts during connect
 * and may restart at any time on an established link. */
static int wifi_run_handshake(void);
static int wifi_tx_eapol(const uint8_t *body, uint32_t len);

/*
 * The nonce source.
 *
 * vx_random() is RDRAND and reports how many bytes it actually
 * produced. A short read is not padded with anything: an SNonce that is
 * partly a fixed pattern weakens every key derived from it, and the
 * handshake refusing to start is a far better failure than a link that
 * comes up with a predictable PTK.
 */
static int wifi_random(uint8_t *out, uint32_t n) {
    return (int)vx_random(out, n);
}

/*
 * Record a beacon.
 *
 * Beacons arrive continuously -- ten a second per network -- so an entry
 * that already exists is refreshed rather than appended. Matching is by
 * BSSID and not by SSID, because one network name is routinely several
 * radios and each is separately joinable.
 */
static void wifi_note_bss(const wifi_bss_t *bss) {
    for (int i = 0; i < wifi_bss_count; i++) {
        if (ieee80211_addr_equal(wifi_bss_list[i].bssid, bss->bssid)) {
            int8_t old = wifi_bss_list[i].rssi;
            wifi_bss_list[i] = *bss;
            /* Smooth the signal a little: a single beacon's RSSI swings
             * several dB and a list that reorders every frame is
             * unreadable. */
            wifi_bss_list[i].rssi = (int8_t)((old + bss->rssi) / 2);
            return;
        }
    }
    if (wifi_bss_count < WIFI_MAX_BSS)
        wifi_bss_list[wifi_bss_count++] = *bss;
}

static void wifi_send_mgmt(const uint8_t *frame, uint32_t len) {
    if (!wifi_dev.ops) return;
    wifi_dev.ops->tx(&wifi_dev, frame, (uint16_t)len);
    wifi_dev.seq++;
    wifi_tx_frames++;
}

static int wifi_scan(uint32_t dwell_ms) {
    uint8_t frame[160];
    uint32_t n;

    if (!wifi_dev.ops) return -1;

    wifi_bss_count = 0;
    wifi_dev.state = WIFI_STATE_SCANNING;

    for (uint32_t c = 0; c < sizeof(wifi_scan_chans); c++) {
        uint8_t chan = wifi_scan_chans[c];

        if (wifi_dev.ops->set_channel(&wifi_dev, chan) != 0) continue;

        /* An active scan: ask, rather than only listening. A hidden
         * network answers a directed probe and never appears in a
         * passive scan at all. */
        n = ieee80211_build_probe_req(frame, sizeof(frame), wifi_dev.mac,
                                      "", 0, wifi_dev.seq);
        if (n) wifi_send_mgmt(frame, n);

        for (uint32_t t = 0; t < dwell_ms; t++) {
            uint16_t got = 0;
            int8_t   rssi = -100;

            while (wifi_dev.ops->rx(&wifi_dev, wifi_rx_frame,
                                    sizeof(wifi_rx_frame), &got, &rssi)) {
                wifi_bss_t bss;
                wifi_rx_frames++;
                if (got && ieee80211_parse_beacon(wifi_rx_frame, got,
                                                  rssi, &bss)) {
                    if (!bss.channel) bss.channel = chan;
                    wifi_note_bss(&bss);
                }
            }
            wifi_udelay(1000);
        }
    }

    wifi_dev.state = WIFI_STATE_IDLE;
    return wifi_bss_count;
}

static const wifi_bss_t *wifi_find_ssid(const char *ssid) {
    const wifi_bss_t *best = 0;
    for (int i = 0; i < wifi_bss_count; i++) {
        const wifi_bss_t *b = &wifi_bss_list[i];
        int match = 1;
        for (int j = 0; j <= IEEE80211_MAX_SSID_LEN; j++) {
            if (b->ssid[j] != ssid[j]) { match = 0; break; }
            if (!ssid[j]) break;
        }
        /* Several radios may carry the same name; take the loudest. */
        if (match && (!best || b->rssi > best->rssi)) best = b;
    }
    return best;
}

/*
 * Join a network.
 *
 * Runs the whole join in order and returns only when the link is either
 * usable or definitively not. Each step has its own failure mode and
 * each is reported distinctly, because "it didn't connect" covers a
 * wrong password, a network that is out of range, and a radio that is
 * switched off, and those want three different responses from whoever
 * is looking at the screen.
 */
static int wifi_connect(const char *ssid, const char *passphrase) {
    const wifi_bss_t *found;
    uint8_t *frame = wifi_mgmt_buf;
    uint32_t n;
    uint16_t got = 0, status = 0, aid = 0;
    int8_t rssi = 0;

    if (!wifi_dev.ops) {
        wifi_log("no radio");
        return -1;
    }

    if (!wifi_bss_count) wifi_scan(20);

    found = wifi_find_ssid(ssid);
    if (!found) {
        wifi_log("network not found in scan results");
        return -1;
    }
    wifi_target = *found;

    {
        uint32_t i = 0;
        while (passphrase[i] && i < sizeof(wifi_pass) - 1) {
            wifi_pass[i] = passphrase[i];
            i++;
        }
        wifi_pass[i] = 0;
    }
    wifi_have_target = 1;

    if (wifi_dev.ops->set_channel(&wifi_dev, wifi_target.channel) != 0) {
        wifi_log("could not tune to the network's channel");
        return -1;
    }

    /* ---- open-system authentication ---- */
    wifi_dev.state = WIFI_STATE_AUTHENTICATING;
    n = ieee80211_build_auth(frame, WIFI_BUF_SIZE, wifi_target.bssid,
                             wifi_dev.mac, wifi_dev.seq, 1);
    if (n) wifi_send_mgmt(frame, n);

    /* ---- association ---- */
    wifi_dev.state = WIFI_STATE_ASSOCIATING;
    n = ieee80211_build_assoc_req(frame, WIFI_BUF_SIZE, &wifi_target,
                                  wifi_dev.mac, wifi_dev.seq,
                                  wifi_target.security == WIFI_SEC_WPA2_PSK,
                                  wifi_assoc_rsn_ie, &wifi_assoc_rsn_len);
    if (!n) {
        wifi_log("could not build the association request");
        return -1;
    }
    wifi_send_mgmt(frame, n);

    /* Wait for the response. */
    for (int t = 0; t < 2000; t++) {
        if (wifi_dev.ops->rx(&wifi_dev, wifi_rx_frame, sizeof(wifi_rx_frame),
                             &got, &rssi) && got) {
            if (ieee80211_parse_assoc_resp(wifi_rx_frame, got,
                                           &status, &aid)) {
                if (status != IEEE80211_STATUS_SUCCESS) {
                    serial_puts("[wifi] association refused, status ");
                    serial_put_dec(status);
                    serial_putc('\n');
                    wifi_dev.state = WIFI_STATE_FAILED;
                    return -1;
                }
                wifi_dev.aid = aid;
                break;
            }
        }
        wifi_udelay(1000);
    }

    if (wifi_target.security == WIFI_SEC_OPEN) {
        wifi_dev.state = WIFI_STATE_CONNECTED;
        wifi_log("associated (open network, nothing encrypted)");
        return 0;
    }

    if (wifi_target.security != WIFI_SEC_WPA2_PSK) {
        wifi_log("only open and WPA2-PSK networks are supported");
        wifi_dev.state = WIFI_STATE_FAILED;
        return -1;
    }

    /* ---- the 4-way handshake ---- */
    wifi_dev.state = WIFI_STATE_HANDSHAKING;
    wpa_sm_init(&wifi_sm, wifi_pass,
                (const uint8_t *)wifi_target.ssid, wifi_target.ssid_len,
                wifi_target.bssid, wifi_dev.mac,
                wifi_assoc_rsn_ie, wifi_assoc_rsn_len);

    wifi_log("PMK derived; waiting for message 1");
    return wifi_run_handshake();
}

/*
 * Drive the handshake to completion.
 *
 * Split out from wifi_connect() because it is also the path a rekey
 * takes: an AP may start a fresh 4-way at any time on an established
 * link, and it arrives as an ordinary EAPOL frame on the data path.
 */
static int wifi_run_handshake(void) {
    for (int t = 0; t < 5000; t++) {
        uint16_t got = 0;
        int8_t rssi = 0;

        if (wifi_dev.ops->rx(&wifi_dev, wifi_rx_frame, sizeof(wifi_rx_frame),
                             &got, &rssi) && got) {
            uint32_t elen = ieee80211_data_to_eth(wifi_rx_frame, got,
                                                  wifi_eth_buf,
                                                  sizeof(wifi_eth_buf));
            wifi_rx_frames++;

            if (elen > 14 &&
                ((uint16_t)((wifi_eth_buf[12] << 8) |
                            wifi_eth_buf[13])) == EAPOL_ETHERTYPE) {
                uint32_t rn = wpa_sm_rx_eapol(&wifi_sm, wifi_eth_buf + 14,
                                              elen - 14,
                                              wifi_eapol_buf,
                                              sizeof(wifi_eapol_buf),
                                              wifi_random);
                if (rn) wifi_tx_eapol(wifi_eapol_buf, rn);

                if (wifi_sm.state == WPA_SM_FAILED) {
                    wifi_log("handshake failed: the MIC on message 3 did "
                             "not verify, which means the passphrase is "
                             "wrong");
                    wifi_dev.state = WIFI_STATE_FAILED;
                    return -1;
                }

                if (wpa_sm_complete(&wifi_sm)) {
                    /* Install the keys. On hardware that encrypts in
                     * silicon this is where the temporal key stops
                     * being the driver's problem. */
                    if (wifi_dev.hw_crypto) {
                        wifi_dev.ops->set_key(&wifi_dev, 0, wifi_sm.tk,
                                              wifi_sm.tk_len,
                                              wifi_target.bssid);
                        if (wifi_sm.gtk_valid)
                            wifi_dev.ops->set_key(&wifi_dev, wifi_sm.gtk_id,
                                                  wifi_sm.gtk, wifi_sm.gtk_len,
                                                  wifi_target.bssid);
                    }
                    wifi_dev.state = WIFI_STATE_CONNECTED;
                    wifi_log("4-way handshake complete; link is encrypted");
                    return 0;
                }
            }
        }
        wifi_udelay(1000);
    }

    wifi_log("handshake timed out: no message 1 from the access point");
    wifi_dev.state = WIFI_STATE_FAILED;
    return -1;
}

/*
 * Send an EAPOL frame.
 *
 * These go out unencrypted even after the keys exist -- the handshake
 * is what establishes encryption, so it cannot itself be encrypted, and
 * message 4 is sent in the clear even though both ends by then hold the
 * PTK. An implementation that encrypts message 4 deadlocks against
 * every access point.
 */
static int wifi_tx_eapol(const uint8_t *body, uint32_t len) {
    /* Its own buffer rather than one of the shared ones above: this is
     * called from inside the receive path, whose Ethernet buffer still
     * holds the message being answered. */
    static uint8_t frame[WIFI_BUF_SIZE];
    uint8_t eth[600];
    uint32_t n;

    if (len + 14 > sizeof(eth)) return -1;

    ieee80211_addr_copy(eth, wifi_target.bssid);
    ieee80211_addr_copy(eth + 6, wifi_dev.mac);
    eth[12] = (uint8_t)(EAPOL_ETHERTYPE >> 8);
    eth[13] = (uint8_t)(EAPOL_ETHERTYPE & 0xFF);
    for (uint32_t i = 0; i < len; i++) eth[14 + i] = body[i];

    n = ieee80211_eth_to_data(eth, len + 14, wifi_target.bssid,
                              wifi_dev.seq, 0, frame, sizeof(frame));
    if (!n) return -1;

    wifi_send_mgmt(frame, n);
    return 0;
}

/* ===========================================================
 * the data path
 * =========================================================== */

/*
 * An Ethernet frame from lwIP goes out over the air.
 *
 * Three things happen: the Ethernet header becomes an 802.11 header
 * plus a SNAP header, the frame is encrypted with CCMP under the
 * temporal key, and the packet number advances. The packet number is
 * the one piece of per-frame state that must never repeat under a given
 * key -- CCM is a counter mode, and a repeated nonce with the same key
 * reveals the XOR of two plaintexts.
 */
static int wifi_tx_eth(const uint8_t *eth, uint16_t len) {
    uint32_t n;
    int enc;

    if (wifi_dev.state != WIFI_STATE_CONNECTED) return -1;

    n = ieee80211_eth_to_data(eth, len, wifi_target.bssid, wifi_dev.seq,
                              wpa_sm_complete(&wifi_sm) && !wifi_dev.hw_crypto,
                              wifi_plain, sizeof(wifi_plain));
    if (!n) return -1;

    if (!wpa_sm_complete(&wifi_sm) || wifi_dev.hw_crypto) {
        /* Either an open network, or hardware that will encrypt this
         * for us once the key is in its table. */
        wifi_dev.ops->tx(&wifi_dev, wifi_plain, (uint16_t)n);
        wifi_dev.seq++;
        wifi_tx_frames++;
        return 0;
    }

    enc = ccmp_encrypt(wifi_sm.tk, wifi_sm.tk_len, wifi_sm.tx_pn, 0,
                       wifi_plain, n,
                       ieee80211_hdrlen(ieee80211_fc(wifi_plain)),
                       wifi_cipher, sizeof(wifi_cipher));
    if (enc < 0) return -1;

    wifi_sm.tx_pn++;
    wifi_dev.ops->tx(&wifi_dev, wifi_cipher, (uint16_t)enc);
    wifi_dev.seq++;
    wifi_tx_frames++;
    return 0;
}

/*
 * One frame in, converted to Ethernet for lwIP.
 *
 * Returns 1 when `out` holds a frame the stack should see. Management
 * frames, EAPOL and anything that fails decryption are consumed here
 * and never reach lwIP -- which is the point of doing this below the
 * netif rather than above it.
 */
static int wifi_rx_eth(uint8_t *out, uint16_t max, uint16_t *got) {
    uint16_t rlen = 0;
    int8_t rssi = 0;

    if (!wifi_dev.ops) return 0;
    if (!wifi_dev.ops->rx(&wifi_dev, wifi_rx_frame, sizeof(wifi_rx_frame),
                          &rlen, &rssi)) return 0;
    if (!rlen) return 0;

    wifi_rx_frames++;

    {
        uint16_t fc = ieee80211_fc(wifi_rx_frame);
        uint32_t hdrlen = ieee80211_hdrlen(fc);
        uint8_t *frame = wifi_rx_frame;
        uint32_t flen = rlen;

        /* Management frames: a deauthentication is the one that matters
         * on an established link -- it means the AP has dropped us and
         * every frame sent after it is wasted. */
        if (ieee80211_is_mgmt(fc)) {
            uint16_t st = ieee80211_stype(fc);
            if (st == IEEE80211_STYPE_DEAUTH ||
                st == IEEE80211_STYPE_DISASSOC) {
                wifi_log("deauthenticated by the access point");
                wifi_dev.state = WIFI_STATE_IDLE;
            } else if (st == IEEE80211_STYPE_BEACON ||
                       st == IEEE80211_STYPE_PROBE_RESP) {
                wifi_bss_t bss;
                if (ieee80211_parse_beacon(wifi_rx_frame, rlen, rssi, &bss))
                    wifi_note_bss(&bss);
            }
            return 0;
        }

        if (!ieee80211_is_data(fc)) return 0;

        /* Decrypt if the frame says it is protected and we are doing
         * the cipher in software. */
        if ((fc & IEEE80211_FC_PROTECTED) && !wifi_dev.hw_crypto) {
            uint64_t pn = 0;
            int n;

            if (!wpa_sm_complete(&wifi_sm)) { wifi_rx_drop++; return 0; }

            n = ccmp_decrypt(wifi_sm.tk, wifi_sm.tk_len, wifi_rx_frame,
                             rlen, hdrlen, &pn,
                             wifi_plain, sizeof(wifi_plain));
            if (n < 0) {
                /* A MIC failure is either corruption or forgery, and
                 * 802.11 requires the two be counted: two in a second
                 * is the signal to tear the link down. */
                wifi_mic_fail++;
                wifi_rx_drop++;
                return 0;
            }

            /* Replay protection. The packet number only ever goes up
             * under a given key, so a frame at or below the high water
             * mark is one we have already accepted. */
            if (pn <= wifi_sm.rx_pn) { wifi_rx_drop++; return 0; }
            wifi_sm.rx_pn = pn;

            frame = wifi_plain;
            flen  = (uint32_t)n;
        }

        /* EAPOL is consumed here: a rekey arrives on the data path and
         * must not be handed to lwIP, which would route it nowhere. */
        {
            uint32_t elen = ieee80211_data_to_eth(frame, flen, wifi_eth_buf,
                                                  sizeof(wifi_eth_buf));
            if (!elen) { wifi_rx_drop++; return 0; }

            if (((uint16_t)((wifi_eth_buf[12] << 8) |
                            wifi_eth_buf[13])) == EAPOL_ETHERTYPE) {
                uint32_t rn = wpa_sm_rx_eapol(&wifi_sm, wifi_eth_buf + 14,
                                              elen - 14,
                                              wifi_eapol_buf,
                                              sizeof(wifi_eapol_buf),
                                              wifi_random);
                if (rn) wifi_tx_eapol(wifi_eapol_buf, rn);
                return 0;
            }

            if (elen > max) { wifi_rx_drop++; return 0; }
            for (uint32_t i = 0; i < elen; i++) out[i] = wifi_eth_buf[i];
            *got = (uint16_t)elen;
            return 1;
        }
    }
}

/* ===========================================================
 * bring-up
 * =========================================================== */

/*
 * Find a radio and bring it as far up as it will go.
 *
 * Returns 1 if there is a usable interface, 0 if there is not. A
 * machine with no wireless hardware -- which includes every QEMU
 * invocation this system is tested under -- takes the early return and
 * is otherwise unaffected.
 */
static int wifi_init(void) {
    pci_dev_t pci;

    wifi_dev.ops   = 0;
    wifi_dev.state = WIFI_STATE_ABSENT;

    if (pci_find_ids(INTEL_WIFI_VENDOR, intel_wifi_ids, &pci)) {
        wifi_log("Intel wireless adapter found on the PCI bus");
        wifi_dev.ops = &intel_ops;
    } else if (pci_find_ids(REALTEK_VENDOR, realtek_wifi_ids, &pci)) {
        wifi_log("Realtek wireless adapter found on the PCI bus");
        wifi_dev.ops = &realtek_ops;
    } else {
        /* Not an error and not worth a warning: most machines this
         * runs on genuinely have no wireless hardware. */
        return 0;
    }

    if (wifi_dev.ops->probe(&wifi_dev, &pci) != 0) {
        serial_puts("[wifi] ");
        serial_puts(wifi_dev.ops->name);
        serial_puts(" stopped at: ");
        serial_puts(wifi_state_name(wifi_dev.state));
        serial_putc('\n');
        /* The device stays claimed so nothing else probes it, but it
         * is not usable and must not be offered to lwIP. */
        return 0;
    }

    if (wifi_dev.ops->start(&wifi_dev) != 0) {
        wifi_log("adapter would not start");
        return 0;
    }

    wifi_log_mac("station address", wifi_dev.mac);
    wifi_dev.state = WIFI_STATE_IDLE;
    wifi_log("radio ready");
    return 1;
}

static int wifi_present(void) {
    return wifi_dev.ops && wifi_dev.state >= WIFI_STATE_IDLE;
}

static int wifi_connected(void) {
    return wifi_dev.state == WIFI_STATE_CONNECTED;
}

static void wifi_disconnect(void) {
    uint8_t frame[64];
    uint32_t n;

    if (!wifi_connected()) return;

    n = ieee80211_build_deauth(frame, sizeof(frame), wifi_target.bssid,
                               wifi_dev.mac, wifi_dev.seq,
                               IEEE80211_REASON_DEAUTH_LEAVING);
    if (n) wifi_send_mgmt(frame, n);

    /* The keys go with the association. Leaving a temporal key in
     * memory after the link is down is how a key outlives the network
     * it belonged to. */
    wpa_memset((uint8_t *)&wifi_sm, 0, sizeof(wifi_sm));
    wifi_dev.state = WIFI_STATE_IDLE;
    wifi_have_target = 0;
}

#endif /* NET_WIFI_C */
