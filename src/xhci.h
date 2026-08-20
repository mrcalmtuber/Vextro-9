#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include "pci.h"

/*
 * USB keyboard and mouse, over xHCI.
 *
 * This is what a real machine needs. Everything else in this system talks
 * to hardware that a modern PC either emulates grudgingly or does not
 * have at all: the PS/2 controller is a firmware compatibility feature
 * that many UEFI-only machines no longer provide, and the VMware backdoor
 * needs a hypervisor to answer it. On real hardware the keyboard and
 * mouse are USB, and reaching them means speaking xHCI.
 *
 * Scope is deliberately small. This drives the HID *boot protocol* — the
 * fixed 8-byte keyboard report and 3-or-4-byte mouse report that every
 * keyboard and mouse implements so that a BIOS can use it before any
 * class driver exists. No report descriptor parsing, no hubs, no
 * hot-plug beyond what start-up enumeration finds. That covers the case
 * that matters (a keyboard and a mouse plugged into a machine at boot)
 * and leaves out several thousand lines that would need hardware
 * variety to test properly.
 *
 * The controller is the same on both architectures — xHCI is a register
 * spec, not an x86 one — so the only architecture-specific part is how
 * the PCI device is found and its BAR mapped.
 *
 * ============================ STATUS ============================
 * PARTIAL, and behind -DENABLE_XHCI (off by default).
 *
 * WORKING: the controller comes up. It is found on the PCI bus, its BAR
 * is mapped, it halts and resets cleanly, the device context array,
 * scratchpad buffers, command ring and event ring are all accepted, and
 * it reaches the run state — reporting 8 ports and 64 slots under qemu.
 * The kernel boots normally with this enabled.
 *
 * NOT WORKING: enumeration finds no devices. xhci_enumerate() walks the
 * ports looking for PORTSC.CCS and reports "no HID devices found" even
 * with usb-kbd and usb-mouse attached to the controller.
 *
 * The likely reason, for whoever picks this up: on a USB 3 capable
 * controller the port set is split. Ports appear twice — once as USB 2
 * and once as USB 3 — and which half a device shows up on depends on its
 * speed. Which register index corresponds to which protocol is not
 * implied by the port number; it comes from the Supported Protocol
 * extended capabilities, walked from HCCPARAMS1's xECP field, and this
 * driver does not read them yet. A full-speed HID device attached to
 * qemu-xhci will sit on a port this code is not looking at.
 *
 * So the next step is the xECP walk: read HCCPARAMS1 >> 16 for the
 * capability list offset, follow it to the Supported Protocol entries
 * (capability ID 2), and use their port offset/count fields to know
 * which PORTSC indices are USB 2 and which are USB 3.
 *
 * Everything past enumeration — slot setup, control transfers, report
 * decoding — is written but has still never executed.
 * ================================================================
 *
 * Structure of the thing, since the register names alone do not convey
 * it: the driver owns a *command ring* it writes requests into, an
 * *event ring* the controller writes completions into, and one *transfer
 * ring* per endpoint it wants to talk to. Each device gets a slot, and
 * each slot has a context describing its endpoints. Enumerating a device
 * means: enable a slot, tell the controller where that device's context
 * lives, address it, read its descriptors, then set up an interrupt
 * endpoint and poll the ring for reports.
 */

/* ---- capability registers (at BAR0 + 0) ---- */
#define XHCI_CAPLENGTH   0x00      /* 8-bit: offset to operational regs  */
#define XHCI_HCSPARAMS1  0x04
#define XHCI_HCCPARAMS1  0x10
#define XHCI_DBOFF       0x14      /* doorbell array offset              */
#define XHCI_RTSOFF      0x18      /* runtime register offset            */

/* ---- operational registers (at BAR0 + CAPLENGTH) ---- */
#define XHCI_USBCMD      0x00
#define XHCI_USBSTS      0x04
#define XHCI_PAGESIZE    0x08
#define XHCI_DNCTRL      0x14
#define XHCI_CRCR        0x18      /* command ring control (64-bit)      */
#define XHCI_DCBAAP      0x30      /* device context base array (64-bit) */
#define XHCI_CONFIG      0x38
#define XHCI_PORTSC(n)   (0x400 + (n) * 0x10)

#define XHCI_CMD_RUN     (1u << 0)
#define XHCI_CMD_HCRST   (1u << 1)
#define XHCI_CMD_INTE    (1u << 2)

#define XHCI_STS_HCH     (1u << 0)  /* halted            */
#define XHCI_STS_CNR     (1u << 11) /* controller not ready */

#define XHCI_PORTSC_CCS  (1u << 0)  /* current connect status */
#define XHCI_PORTSC_PED  (1u << 1)  /* port enabled           */
#define XHCI_PORTSC_PR   (1u << 4)  /* port reset             */
#define XHCI_PORTSC_PP   (1u << 9)  /* port power             */
#define XHCI_PORTSC_CSC  (1u << 17) /* connect status change  */
#define XHCI_PORTSC_PRC  (1u << 21) /* port reset change      */

/* ---- runtime / interrupter 0 ---- */
#define XHCI_IMAN        0x20
#define XHCI_IMOD        0x24
#define XHCI_ERSTSZ      0x28
#define XHCI_ERSTBA      0x30
#define XHCI_ERDP        0x38

/* ---- TRB types ---- */
#define TRB_NORMAL             1
#define TRB_SETUP_STAGE        2
#define TRB_DATA_STAGE         3
#define TRB_STATUS_STAGE       4
#define TRB_LINK               6
#define TRB_ENABLE_SLOT        9
#define TRB_DISABLE_SLOT       10
#define TRB_ADDRESS_DEVICE     11
#define TRB_CONFIGURE_ENDPOINT 12
#define TRB_EVAL_CONTEXT       13
#define TRB_NOOP_CMD           23
#define TRB_TRANSFER_EVENT     32
#define TRB_COMMAND_COMPLETE   33
#define TRB_PORT_STATUS_CHANGE 34

#define TRB_CYCLE     (1u << 0)
#define TRB_ENT       (1u << 1)
#define TRB_ISP       (1u << 2)
#define TRB_IOC       (1u << 5)
#define TRB_IDT       (1u << 6)
#define TRB_TYPE(t)   ((uint32_t)(t) << 10)
#define TRB_GET_TYPE(c) (((c) >> 10) & 0x3F)
#define TRB_GET_CC(s)   (((s) >> 24) & 0xFF)
#define TRB_CC_SUCCESS  1
/* A transfer that moved less than was asked for. Success with a residue,
 * not an error: a device answering INQUIRY with fewer bytes than the
 * buffer offered is doing exactly what it should. */
#define TRB_CC_SHORT_PACKET 13

#define XHCI_RING_TRBS 64

typedef struct {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} __attribute__((packed, aligned(16))) xhci_trb_t;

typedef struct {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} __attribute__((packed, aligned(64))) xhci_erst_t;

/* One connected HID device we actually poll. */
#define XHCI_MAX_HID 4

typedef struct {
    int      used;
    int      is_keyboard;       /* else pointer */
    uint8_t  slot;
    uint8_t  ep_id;             /* endpoint index in the slot context */
    uint32_t port;
    xhci_trb_t *ring;           /* transfer ring for the interrupt IN ep */
    uint32_t   ring_idx;
    uint8_t    cycle;
    uint8_t   *buf;             /* report buffer the controller fills */
    uint16_t   report_len;
} xhci_hid_t;

static volatile uint8_t *xhci_mmio = 0;
static volatile uint8_t *xhci_op   = 0;
static volatile uint8_t *xhci_rt   = 0;
static volatile uint32_t *xhci_db  = 0;
static int xhci_present = 0;
static uint32_t xhci_max_ports = 0;
static uint32_t xhci_max_slots = 0;
static int xhci_ctx64 = 0;      /* HCCPARAMS1.CSZ: 64-byte contexts */

/* Rings and tables. All must be physically contiguous and aligned, which
 * static arrays in .bss are — the kernel image is loaded as one block. */
static xhci_trb_t xhci_cmd_ring[XHCI_RING_TRBS] __attribute__((aligned(64)));
static xhci_trb_t xhci_evt_ring[XHCI_RING_TRBS] __attribute__((aligned(64)));
static xhci_erst_t xhci_erst[1]                 __attribute__((aligned(64)));
static uint64_t   xhci_dcbaa[64]                __attribute__((aligned(64)));
static uint8_t    xhci_scratch[64][4096]        __attribute__((aligned(4096)));
static uint64_t   xhci_scratch_arr[64]          __attribute__((aligned(64)));

/* Per-slot device and input contexts. 2048 bytes covers a 64-byte-context
 * controller's 32 entries with room to spare. */
static uint8_t xhci_dev_ctx[XHCI_MAX_HID + 1][2048] __attribute__((aligned(64)));
static uint8_t xhci_in_ctx[XHCI_MAX_HID + 1][2048]  __attribute__((aligned(64)));
static xhci_trb_t xhci_ep_ring[XHCI_MAX_HID][XHCI_RING_TRBS] __attribute__((aligned(64)));
static uint8_t xhci_report[XHCI_MAX_HID][64] __attribute__((aligned(64)));
static uint8_t xhci_setup_buf[XHCI_MAX_HID][256] __attribute__((aligned(64)));

static xhci_hid_t xhci_hid[XHCI_MAX_HID];
static int xhci_hid_count = 0;

static uint32_t xhci_cmd_idx = 0;
static uint8_t  xhci_cmd_cycle = 1;
static uint32_t xhci_evt_idx = 0;
static uint8_t  xhci_evt_cycle = 1;

static void xhci_dec(uint32_t v) {
    char b[12]; int i = 11; b[11] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    serial_puts(b + i);
}

static void xhci_log(const char *m) {
    serial_puts("[xhci] "); serial_puts(m); serial_putc('\n');
}

static inline uint32_t xhci_rd32(volatile uint8_t *base, uint32_t off) {
    return *(volatile uint32_t *)(base + off);
}
static inline void xhci_wr32(volatile uint8_t *base, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(base + off) = v;
}
static inline void xhci_wr64(volatile uint8_t *base, uint32_t off, uint64_t v) {
    *(volatile uint32_t *)(base + off)     = (uint32_t)v;
    *(volatile uint32_t *)(base + off + 4) = (uint32_t)(v >> 32);
}

static void xhci_delay(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops * 1000u; i++) { }
}

/* ---- rings ---- */

static void xhci_ring_init(xhci_trb_t *ring, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        ring[i].param = 0; ring[i].status = 0; ring[i].control = 0;
    }
    /* Last TRB links back to the start, so the ring is a ring. Toggle
     * Cycle tells the controller the producer cycle bit flips here. */
    ring[n - 1].param   = kern_virt_to_phys(ring);
    ring[n - 1].status  = 0;
    ring[n - 1].control = TRB_TYPE(TRB_LINK) | (1u << 1) /* toggle */;
}

static void xhci_cmd_push(uint64_t param, uint32_t status, uint32_t control) {
    xhci_trb_t *t = &xhci_cmd_ring[xhci_cmd_idx];
    t->param  = param;
    t->status = status;
    t->control = control | (xhci_cmd_cycle ? TRB_CYCLE : 0);

    xhci_cmd_idx++;
    if (xhci_cmd_idx == XHCI_RING_TRBS - 1) {
        xhci_cmd_ring[XHCI_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | (1u << 1) | (xhci_cmd_cycle ? TRB_CYCLE : 0);
        xhci_cmd_idx = 0;
        xhci_cmd_cycle ^= 1;
    }
    xhci_db[0] = 0;                 /* ring the command doorbell */
}

/*
 * Wait for the next event, returning its type, and hand back the whole
 * TRB so callers can read a slot id or a completion code out of it.
 */
static int xhci_wait_event(xhci_trb_t *out, uint32_t timeout) {
    for (uint32_t spin = 0; spin < timeout; spin++) {
        xhci_trb_t *e = &xhci_evt_ring[xhci_evt_idx];
        uint32_t ctrl = e->control;
        if ((ctrl & TRB_CYCLE) == (xhci_evt_cycle ? TRB_CYCLE : 0)) {
            if (out) *out = *e;

            xhci_evt_idx++;
            if (xhci_evt_idx == XHCI_RING_TRBS) {
                xhci_evt_idx = 0;
                xhci_evt_cycle ^= 1;
            }
            /* Tell the controller how far we have consumed. Bit 3 is the
             * Event Handler Busy flag, written back to clear it. */
            xhci_wr64(xhci_rt, XHCI_ERDP,
                      kern_virt_to_phys(&xhci_evt_ring[xhci_evt_idx]) | (1u << 3));
            return (int)TRB_GET_TYPE(ctrl);
        }
        xhci_delay(1);
    }
    return -1;
}

/* Drain events until one of `want` arrives, so a stray port-status event
 * does not make a command look like it failed. */
static int xhci_wait_for(int want, xhci_trb_t *out, uint32_t timeout) {
    for (int i = 0; i < 16; i++) {
        int t = xhci_wait_event(out, timeout);
        if (t < 0) return -1;
        if (t == want) return t;
    }
    return -1;
}

/* ---- contexts ---- */

static uint32_t xhci_ctx_stride(void) { return xhci_ctx64 ? 64u : 32u; }

static uint32_t *xhci_ctx_at(uint8_t *base, uint32_t index) {
    return (uint32_t *)(base + index * xhci_ctx_stride());
}

static void xhci_zero(uint8_t *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) p[i] = 0;
}

/*
 * A control transfer on endpoint 0, used to read descriptors and to set
 * the HID boot protocol. Three TRBs: setup, data, status — which is the
 * shape of every USB control transfer, made explicit by the ring.
 */
static void xhci_release_slot(uint8_t slot);

static int xhci_control(xhci_hid_t *h, uint8_t rtype, uint8_t req,
                        uint16_t value, uint16_t index,
                        void *data, uint16_t len) {
    xhci_trb_t *ring = h->ring;      /* ep0 ring, reused for setup */
    uint32_t i = h->ring_idx;
    uint8_t  cyc = h->cycle;

    uint64_t setup = (uint64_t)rtype | ((uint64_t)req << 8) |
                     ((uint64_t)value << 16) | ((uint64_t)index << 32) |
                     ((uint64_t)len << 48);

    ring[i].param  = setup;
    ring[i].status = 8;
    ring[i].control = TRB_TYPE(TRB_SETUP_STAGE) | TRB_IDT |
                      (len ? (rtype & 0x80 ? (3u << 16) : (2u << 16)) : 0) |
                      (cyc ? TRB_CYCLE : 0);
    i = (i + 1) % (XHCI_RING_TRBS - 1);

    if (len) {
        ring[i].param  = kern_virt_to_phys(data);
        ring[i].status = len;
        ring[i].control = TRB_TYPE(TRB_DATA_STAGE) |
                          ((rtype & 0x80) ? (1u << 16) : 0) |
                          (cyc ? TRB_CYCLE : 0);
        i = (i + 1) % (XHCI_RING_TRBS - 1);
    }

    ring[i].param  = 0;
    ring[i].status = 0;
    ring[i].control = TRB_TYPE(TRB_STATUS_STAGE) | TRB_IOC |
                      ((len && (rtype & 0x80)) ? 0 : (1u << 16)) |
                      (cyc ? TRB_CYCLE : 0);
    i = (i + 1) % (XHCI_RING_TRBS - 1);

    h->ring_idx = i;

    xhci_db[h->slot] = 1;            /* doorbell target 1 = ep0 */

    xhci_trb_t ev;
    if (xhci_wait_for(TRB_TRANSFER_EVENT, &ev, 200000) < 0) return -1;
    return TRB_GET_CC(ev.status) == TRB_CC_SUCCESS ? 0 : -1;
}


/* ---- controller bring-up ---- */

/*
 * Bring the controller up.
 *
 * INCOMPLETE — see the note at the top of the file. This currently hangs
 * somewhere between finding the controller and starting it, so it is
 * behind a build flag and off by default. Every step logs before it runs,
 * so the next attempt starts by reading the serial line rather than
 * guessing where it stopped.
 */
static int xhci_init(void) {
    pci_dev_t dev;
    /* class_code is already class<<16 | subclass<<8 | prog-if:
     * 0x0C serial bus, 0x03 USB, 0x30 xHCI. */
    if (!pci_find_class(0xFFFFFFu, 0x0C0330u, 0, &dev)) {
        xhci_log("no controller");
        return 0;
    }

    uint64_t bar_phys = 0, bar_len = 0;
    /* pci_bar returns 0 for success, unlike pci_find_class beside it
     * which returns 1 for found. Reading either as a boolean inverts it. */
    if (pci_bar(&dev, 0, &bar_phys, &bar_len) != 0 || !bar_phys) {
        xhci_log("BAR0 unusable");
        return 0;
    }
    xhci_log("found controller");
    pci_enable(&dev, 0x0006);        /* memory space + bus master */
    xhci_log("decode enabled");

    /*
     * Cap what gets mapped. Only the capability, operational, runtime and
     * doorbell register blocks are touched here, which fit in 64 KB on
     * every controller — while a BAR may legitimately be far larger, and
     * mmio_map walks it a page at a time out of a 32-page table pool.
     * Asking for the whole BAR is how a driver that only needs the first
     * few pages exhausts the pool and returns nothing, or spends a very
     * long time not saying so.
     */
    uint64_t map_len = bar_len ? bar_len : 0x10000;
    if (map_len > 0x10000) map_len = 0x10000;
    xhci_mmio = mmio_map(bar_phys, map_len);
    xhci_log("mapped BAR0");
    if (!xhci_mmio) { xhci_log("cannot map BAR0"); return 0; }

    uint8_t caplen = *(volatile uint8_t *)(xhci_mmio + XHCI_CAPLENGTH);
    uint32_t hcs1  = xhci_rd32(xhci_mmio, XHCI_HCSPARAMS1);
    uint32_t hcc1  = xhci_rd32(xhci_mmio, XHCI_HCCPARAMS1);
    xhci_op = xhci_mmio + caplen;
    xhci_rt = xhci_mmio + (xhci_rd32(xhci_mmio, XHCI_RTSOFF) & ~0x1Fu);
    xhci_db = (volatile uint32_t *)(xhci_mmio +
                                    (xhci_rd32(xhci_mmio, XHCI_DBOFF) & ~0x3u));

    xhci_max_slots = hcs1 & 0xFF;
    xhci_max_ports = (hcs1 >> 24) & 0xFF;
    xhci_ctx64     = (hcc1 & 0x04) ? 1 : 0;

    /* Halt, then reset. A controller the firmware left running will
     * otherwise keep servicing rings we are about to reuse. */
    uint32_t cmd = xhci_rd32(xhci_op, XHCI_USBCMD);
    xhci_wr32(xhci_op, XHCI_USBCMD, cmd & ~XHCI_CMD_RUN);
    for (int i = 0; i < 1000; i++) {
        if (xhci_rd32(xhci_op, XHCI_USBSTS) & XHCI_STS_HCH) break;
        xhci_delay(1);
    }
    xhci_log("halted");
    xhci_wr32(xhci_op, XHCI_USBCMD, XHCI_CMD_HCRST);
    for (int i = 0; i < 1000; i++) {
        if (!(xhci_rd32(xhci_op, XHCI_USBCMD) & XHCI_CMD_HCRST) &&
            !(xhci_rd32(xhci_op, XHCI_USBSTS) & XHCI_STS_CNR)) break;
        xhci_delay(2);
    }

    /* Device context base address array, plus scratchpad buffers if the
     * controller asked for any. */
    xhci_log("reset done");
    for (uint32_t i = 0; i < 64; i++) xhci_dcbaa[i] = 0;
    uint32_t hcs2 = xhci_rd32(xhci_mmio, 0x08);
    uint32_t spb  = ((hcs2 >> 21) & 0x1F) | (((hcs2 >> 27) & 0x1F) << 5);
    if (spb) {
        if (spb > 64) spb = 64;
        for (uint32_t i = 0; i < spb; i++) {
            xhci_zero(xhci_scratch[i], 4096);
            xhci_scratch_arr[i] = kern_virt_to_phys(xhci_scratch[i]);
        }
        xhci_dcbaa[0] = kern_virt_to_phys(xhci_scratch_arr);
    }

    xhci_wr32(xhci_op, XHCI_CONFIG, xhci_max_slots);
    xhci_wr64(xhci_op, XHCI_DCBAAP, kern_virt_to_phys(xhci_dcbaa));

    xhci_ring_init(xhci_cmd_ring, XHCI_RING_TRBS);
    xhci_cmd_idx = 0; xhci_cmd_cycle = 1;
    xhci_wr64(xhci_op, XHCI_CRCR, kern_virt_to_phys(xhci_cmd_ring) | 1);

    /* Event ring, described to the controller by a one-entry table. */
    for (uint32_t i = 0; i < XHCI_RING_TRBS; i++) {
        xhci_evt_ring[i].param = 0;
        xhci_evt_ring[i].status = 0;
        xhci_evt_ring[i].control = 0;
    }
    xhci_evt_idx = 0; xhci_evt_cycle = 1;
    xhci_erst[0].base = kern_virt_to_phys(xhci_evt_ring);
    xhci_erst[0].size = XHCI_RING_TRBS;
    xhci_erst[0].rsvd = 0;
    xhci_wr32(xhci_rt, XHCI_ERSTSZ, 1);
    xhci_wr64(xhci_rt, XHCI_ERDP, kern_virt_to_phys(xhci_evt_ring));
    xhci_wr64(xhci_rt, XHCI_ERSTBA, kern_virt_to_phys(xhci_erst));

    /* Run. Interrupts stay masked: this driver is polled, like every
     * other one here, and the render loop visits it once a frame. */
    xhci_log("rings built");
    xhci_wr32(xhci_op, XHCI_USBCMD, XHCI_CMD_RUN);
    for (int i = 0; i < 1000; i++) {
        if (!(xhci_rd32(xhci_op, XHCI_USBSTS) & XHCI_STS_HCH)) break;
        xhci_delay(1);
    }

    xhci_present = 1;
    serial_puts("[xhci] up: ");
    xhci_dec(xhci_max_ports); serial_puts(" ports, ");
    xhci_dec(xhci_max_slots); serial_puts(" slots, ");
    serial_puts(xhci_ctx64 ? "64-byte contexts\n" : "32-byte contexts\n");
    return 1;
}

/* ---- device enumeration ---- */

/*
 * Bring one connected port up to a device we can read reports from.
 *
 * The sequence is fixed by the spec and every step depends on the last:
 * reset the port, ask for a slot, describe the device's endpoint 0, let
 * the controller address it, read enough descriptors to find an
 * interrupt IN endpoint, configure that endpoint, then put a transfer on
 * its ring and leave it there.
 */
static int xhci_setup_routed(uint32_t port, uint32_t route,
                             uint8_t parent_slot, uint8_t parent_port,
                             uint32_t routed_speed) {
    if (xhci_hid_count >= XHCI_MAX_HID) return 0;

    uint32_t speed;

    if (route == 0) {
        uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
        if (!(psc & XHCI_PORTSC_CCS)) return 0;

        /* Reset, and wait for the controller to say it finished. */
        xhci_wr32(xhci_op, XHCI_PORTSC(port),
                  (psc & ~0x80FFu) | XHCI_PORTSC_PR | XHCI_PORTSC_PP);
        int ok = 0;
        for (int i = 0; i < 1000; i++) {
            psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
            if (psc & XHCI_PORTSC_PRC) { ok = 1; break; }
            xhci_delay(2);
        }
        if (!ok || !(psc & XHCI_PORTSC_PED)) return 0;
        xhci_wr32(xhci_op, XHCI_PORTSC(port), psc);   /* ack the change bits */
        speed = (psc >> 10) & 0xF;
    } else {
        /* Behind a hub. The hub has already reset the port and reported
         * the speed; resetting the *root* port here would knock the hub
         * itself off the bus along with everything else on it. */
        speed = routed_speed;
    }

    xhci_cmd_push(0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    xhci_trb_t ev;
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return 0;
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return 0;
    uint8_t slot = (uint8_t)(ev.control >> 24);
    if (slot == 0 || slot > XHCI_MAX_HID) return 0;

    int h_idx = xhci_hid_count;
    xhci_hid_t *h = &xhci_hid[h_idx];
    h->slot = slot;
    h->port = port;
    h->ring = xhci_ep_ring[h_idx];
    h->ring_idx = 0;
    h->cycle = 1;
    h->buf = xhci_report[h_idx];

    xhci_zero(xhci_dev_ctx[h_idx], 2048);
    xhci_zero(xhci_in_ctx[h_idx], 2048);
    xhci_ring_init(h->ring, XHCI_RING_TRBS);
    xhci_dcbaa[slot] = kern_virt_to_phys(xhci_dev_ctx[h_idx]);

    /* Input context: control (add ep0 and the slot), slot, then ep0. */
    uint32_t *icc = xhci_ctx_at(xhci_in_ctx[h_idx], 0);
    icc[1] = 0x3;                                  /* add slot + ep0 */
    uint32_t *sc = xhci_ctx_at(xhci_in_ctx[h_idx], 1);
    sc[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);
    sc[1] = (port + 1) << 16;                      /* root hub port number */
    /* Which hub translates for a slow device on a fast bus. Wrong here
     * does not fail loudly: the device is addressed and then silent. */
    if (route) sc[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8);

    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
    uint32_t *ep0 = xhci_ctx_at(xhci_in_ctx[h_idx], 2);
    ep0[1] = (4u << 3) | (mps << 16) | (3u << 1);  /* control, CErr=3 */
    ((uint64_t *)ep0)[1] = kern_virt_to_phys(h->ring) | 1;  /* DCS */

    xhci_cmd_push(kern_virt_to_phys(xhci_in_ctx[h_idx]), 0,
                  TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot << 24));
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return 0;
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return 0;

    /* Read the configuration descriptor and find an interrupt IN endpoint
     * on a HID interface. The boot protocol means the report layout is
     * known from the interface's subclass and protocol alone. */
    uint8_t *cfg = xhci_setup_buf[h_idx];
    if (xhci_control(h, 0x80, 6, 0x0200, 0, cfg, 64) != 0) return 0;
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total > 255) total = 255;
    if (total > 64 && xhci_control(h, 0x80, 6, 0x0200, 0, cfg, total) != 0)
        return 0;

    int is_kbd = -1, ep_addr = -1, ep_mps = 8, ep_interval = 8;
    uint8_t iface_num = 0;
    for (uint32_t o = 0; o + 1 < total; ) {
        uint8_t dl = cfg[o], dt = cfg[o + 1];
        if (dl == 0) break;
        if (dt == 4 && o + 8 < total) {            /* interface */
            if (cfg[o + 5] == 3 && cfg[o + 6] == 1) {  /* HID, boot */
                iface_num = cfg[o + 2];
                is_kbd = (cfg[o + 7] == 1) ? 1 : (cfg[o + 7] == 2) ? 0 : -1;
            } else {
                is_kbd = -1;
            }
        } else if (dt == 5 && is_kbd >= 0 && o + 6 < total) {  /* endpoint */
            if ((cfg[o + 2] & 0x80) && (cfg[o + 3] & 0x3) == 3) {
                ep_addr = cfg[o + 2] & 0xF;
                ep_mps  = cfg[o + 4] | (cfg[o + 5] << 8);
                ep_interval = cfg[o + 6];
                break;
            }
        }
        o += dl;
    }
    if (ep_addr < 0 || is_kbd < 0) {
        /*
         * Addressed, and not a keyboard or a mouse.
         *
         * Returning here without giving the slot back was a real fault
         * and not a leak of a scarce resource: the device stays
         * addressed on this slot, so when the storage driver is offered
         * the same port and tries to address it again, the controller
         * refuses -- correctly -- and a perfectly good memory stick
         * reports "gave up at address device". Handing the slot back
         * leaves the device exactly as it was found.
         */
        xhci_release_slot(slot);
        return 0;
    }

    /* Boot protocol, so reports arrive in the fixed layout. */
    xhci_control(h, 0x21, 0x0B, 0, iface_num, 0, 0);   /* SET_PROTOCOL(boot) */

    /* Configure the interrupt endpoint. Endpoint id for IN ep N is 2N+1. */
    uint32_t ep_id = (uint32_t)(ep_addr * 2 + 1);
    xhci_zero(xhci_in_ctx[h_idx], 2048);
    icc = xhci_ctx_at(xhci_in_ctx[h_idx], 0);
    icc[1] = 1u | (1u << ep_id);
    sc = xhci_ctx_at(xhci_in_ctx[h_idx], 1);
    sc[0] = (ep_id << 27) | (speed << 20) | (route & 0xFFFFF);
    sc[1] = (port + 1) << 16;
    if (route) sc[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8);

    uint32_t *epc = xhci_ctx_at(xhci_in_ctx[h_idx], ep_id + 1);
    uint32_t ival = ep_interval ? ep_interval - 1 : 3;
    if (ival > 15) ival = 15;
    epc[0] = (ival << 16);
    epc[1] = (7u << 3) | ((uint32_t)ep_mps << 16) | (3u << 1);  /* int IN */
    ((uint64_t *)epc)[1] = kern_virt_to_phys(h->ring) | 1;
    epc[4] = ep_mps;

    xhci_cmd_push(kern_virt_to_phys(xhci_in_ctx[h_idx]), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot << 24));
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return 0;
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return 0;

    h->is_keyboard = is_kbd;
    h->ep_id = (uint8_t)ep_id;
    h->report_len = (uint16_t)(ep_mps > 64 ? 64 : ep_mps);
    h->used = 1;
    h->ring_idx = 0;
    h->cycle = 1;
    xhci_ring_init(h->ring, XHCI_RING_TRBS);
    xhci_hid_count++;

    serial_puts("[xhci] port ");
    xhci_dec(port);
    if (route) { serial_puts(" route "); xhci_dec(route); }
    serial_puts(is_kbd ? ": keyboard" : ": pointer");
    serial_puts(" on slot ");
    xhci_dec(slot);
    serial_putc('\n');
    return 1;
}

/* The root-port case, which is what every caller outside the hub driver
 * wants. */
static int xhci_setup_port(uint32_t port) {
    return xhci_setup_routed(port, 0, 0, 0, 0);
}

/* Queue one interrupt-IN transfer, so the controller has somewhere to
 * put the next report. */
static void xhci_queue_report(xhci_hid_t *h) {
    xhci_trb_t *t = &h->ring[h->ring_idx];
    t->param  = kern_virt_to_phys(h->buf);
    t->status = h->report_len;
    t->control = TRB_TYPE(TRB_NORMAL) | TRB_IOC | TRB_ISP |
                 (h->cycle ? TRB_CYCLE : 0);

    h->ring_idx++;
    if (h->ring_idx == XHCI_RING_TRBS - 1) {
        h->ring[XHCI_RING_TRBS - 1].control =
            TRB_TYPE(TRB_LINK) | (1u << 1) | (h->cycle ? TRB_CYCLE : 0);
        h->ring_idx = 0;
        h->cycle ^= 1;
    }
    xhci_db[h->slot] = h->ep_id;
}

/*
 * ---- hot plug ----
 *
 * Enumeration used to happen once, at boot, and that is the difference
 * between "USB works" and "USB worked when the machine started". A
 * keyboard plugged in afterwards was invisible; one unplugged left a
 * slot pointing at a device that would never answer again, and the
 * report queued to it sat in the ring forever.
 *
 * A port carries its own change bits, and they are write-one-to-clear:
 * reading them and writing them back is what acknowledges the event.
 * Getting that wrong in the other direction is worse than missing the
 * event -- PORTSC has several bits that are also write-one-to-clear, so
 * a careless read-modify-write acknowledges changes nobody has looked
 * at yet.
 */
#define XHCI_PORTSC_OCC  (1u << 20)  /* over-current change */
#define XHCI_PORTSC_PLC  (1u << 22)  /* port link state change */
#define XHCI_PORTSC_CEC  (1u << 23)  /* config error change */
#define XHCI_PORTSC_ACK  (XHCI_PORTSC_CSC | XHCI_PORTSC_PRC | \
                          XHCI_PORTSC_OCC | XHCI_PORTSC_PLC | \
                          XHCI_PORTSC_CEC)

/* The bits that must never be written back as ones by accident. */
#define XHCI_PORTSC_RW   (XHCI_PORTSC_PP)

static void xhci_ack_port(uint32_t port, uint32_t bits) {
    uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
    /* Keep only the read-write bits, then set exactly the change bits
     * being acknowledged. */
    uint32_t out = (psc & XHCI_PORTSC_RW) | (bits & XHCI_PORTSC_ACK);
    xhci_wr32(xhci_op, XHCI_PORTSC(port), out);
}

static int xhci_hid_on_port(uint32_t port) {
    for (int i = 0; i < XHCI_MAX_HID; i++)
        if (xhci_hid[i].used && xhci_hid[i].port == port) return i;
    return -1;
}

/*
 * Let a device go.
 *
 * The slot is not disabled through the command ring, deliberately: the
 * device is already gone, the command would time out waiting for a
 * controller response about hardware that is not there, and a timeout
 * inside the frame loop is exactly what a hot-unplug must not cost. The
 * slot leaks until the next reset, which on a machine with four slots
 * and a lifetime of a few hours is a trade worth making.
 */
/*
 * Give a slot back.
 *
 * Every path that enables a slot and then decides the device is not
 * its business must call this, and the reason is sharper than tidiness:
 * a slot that is not disabled leaves the device *addressed*, and the
 * next driver offered the same port cannot address it again. The
 * controller refuses, correctly, and a working device reports a failure
 * that names the wrong step. It cost this twice -- once for a memory
 * stick behind the keyboard path, once for a hub behind the storage
 * path -- before it was worth a function.
 */
static void xhci_release_slot(uint8_t slot) {
    if (!slot) return;
    xhci_cmd_push(0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | ((uint32_t)slot << 24));
    xhci_trb_t ev;
    xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000);
    xhci_dcbaa[slot] = 0;
}

static void xhci_drop_device(int idx) {
    if (idx < 0 || idx >= XHCI_MAX_HID || !xhci_hid[idx].used) return;
    xhci_log(xhci_hid[idx].is_keyboard ? "keyboard unplugged"
                                       : "pointer unplugged");
    xhci_hid[idx].used = 0;
    xhci_hid[idx].slot = 0;

    int n = 0;
    for (int i = 0; i < XHCI_MAX_HID; i++) if (xhci_hid[i].used) n++;
    xhci_hid_count = n;
}

/*
 * Filled in by src/usbmsc.h. A hook rather than a direct call because
 * storage is defined after this file -- and because the dependency runs
 * the right way round this way: xhci knows nothing about SCSI, and the
 * storage driver knows about xhci.
 *
 * Called for a port that has something on it which is not a HID device.
 */
static int (*xhci_storage_hook)(uint32_t port, uint32_t route,
                                uint8_t parent_slot, uint8_t parent_port) = 0;

/* And src/usbhub.h, for a port that turned out to hold neither a
 * keyboard, a mouse nor a disk. A hub is what is left. */
static int (*xhci_hub_hook)(uint32_t port, uint32_t route,
                            uint8_t parent_slot, uint8_t parent_port) = 0;

static void xhci_enumerate(void) {
    if (!xhci_present) return;
    for (uint32_t p = 0; p < xhci_max_ports; p++) {
        uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(p));
        if (!(psc & XHCI_PORTSC_CCS)) continue;
        if (xhci_hid_on_port(p) >= 0) continue;
        if (xhci_hid_count < XHCI_MAX_HID && xhci_setup_port(p)) {
            xhci_queue_report(&xhci_hid[xhci_hid_count - 1]);
            continue;
        }
        /* Not a keyboard or a mouse. Storage next, then a hub -- and a
         * hub enumerates everything behind it before returning. */
        if (xhci_storage_hook && xhci_storage_hook(p, 0, 0, 0)) continue;
        if (xhci_hub_hook) xhci_hub_hook(p, 0, 0, 0);
    }
    if (xhci_hid_count == 0) xhci_log("no HID devices found");
}

/*
 * Called every frame. Walks the ports looking for anything that changed
 * since last time, and acts on it: a device that has appeared is
 * enumerated, one that has gone is released.
 *
 * Sixty times a second over eight ports is eight register reads a
 * frame, which is nothing, and it means a keyboard is usable about a
 * sixtieth of a second after it is plugged in.
 */
static void xhci_hotplug_poll(void) {
    if (!xhci_present) return;

    for (uint32_t p = 0; p < xhci_max_ports; p++) {
        uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(p));
        if (!(psc & XHCI_PORTSC_ACK)) continue;

        int connected = (psc & XHCI_PORTSC_CCS) != 0;
        xhci_ack_port(p, psc);

        int idx = xhci_hid_on_port(p);
        if (!connected) {
            if (idx >= 0) xhci_drop_device(idx);
            continue;
        }
        if (idx >= 0) continue;               /* already ours */

        xhci_log("device connected - enumerating");
        if (xhci_hid_count < XHCI_MAX_HID && xhci_setup_port(p)) {
            xhci_queue_report(&xhci_hid[xhci_hid_count - 1]);
            continue;
        }
        if (xhci_storage_hook && xhci_storage_hook(p, 0, 0, 0)) continue;
        if (xhci_hub_hook) xhci_hub_hook(p, 0, 0, 0);
    }
}


/* ---- HID reports -> the interfaces the rest of the system already uses ---- */

/*
 * USB HID usage codes are not PS/2 scancodes, so unlike the aarch64 port
 * (where Linux evdev codes happened to match set-1) this needs a real
 * table. Usages 0x04..0x1D are 'a'..'z', 0x1E..0x27 are '1'..'0'.
 */
static const char xhci_hid_lower[128] = {
    0,0,0,0, 'a','b','c','d','e','f','g','h','i','j','k','l','m',
    'n','o','p','q','r','s','t','u','v','w','x','y','z',
    '1','2','3','4','5','6','7','8','9','0',
    '\n','\033','\b','\t',' ','-','=','[',']','\\','\\',';','\'','`',',','.','/',
    0, /* caps lock */
    0,0,0,0,0,0,0,0,0,0,0,0,  /* F1-F12 */
    0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};
static const char xhci_hid_upper[128] = {
    0,0,0,0, 'A','B','C','D','E','F','G','H','I','J','K','L','M',
    'N','O','P','Q','R','S','T','U','V','W','X','Y','Z',
    '!','@','#','$','%','^','&','*','(',')',
    '\n','\033','\b','\t',' ','_','+','{','}','|','|',':','"','~','<','>','?',
    0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

/* HID usages for the keys delivered as control codes. */
#define HID_RIGHT 0x4F
#define HID_LEFT  0x50
#define HID_DOWN  0x51
#define HID_UP    0x52
#define HID_HOME  0x4A
#define HID_PGUP  0x4B
#define HID_DEL   0x4C
#define HID_END   0x4D
#define HID_PGDN  0x4E

/* Previous keyboard report, so only newly-pressed keys produce output —
 * a HID keyboard reports the full set of held keys every time, not
 * transitions. Without this, holding a key floods the buffer. */
static uint8_t xhci_prev_keys[6];

static void xhci_handle_keyboard(const uint8_t *r) {
    uint8_t mods = r[0];
    kb_shift = (mods & 0x22) ? 1 : 0;          /* either shift */

    for (int i = 2; i < 8; i++) {
        uint8_t u = r[i];
        if (u == 0) continue;

        int already = 0;
        for (int j = 0; j < 6; j++)
            if (xhci_prev_keys[j] == u) { already = 1; break; }
        if (already) continue;                  /* still held, not new */

        switch (u) {
        case HID_UP:    kb_push(KEY_UP);    continue;
        case HID_DOWN:  kb_push(KEY_DOWN);  continue;
        case HID_LEFT:  kb_push(KEY_LEFT);  continue;
        case HID_RIGHT: kb_push(KEY_RIGHT); continue;
        case HID_PGUP:  kb_push(KEY_PGUP);  continue;
        case HID_PGDN:  kb_push(KEY_PGDN);  continue;
        case HID_HOME:  kb_push(KEY_HOME);  continue;
        case HID_END:   kb_push(KEY_END);   continue;
        case HID_DEL:   kb_push(KEY_DEL);   continue;
        default: break;
        }
        if (u < 128) {
            char c = kb_shift ? xhci_hid_upper[u] : xhci_hid_lower[u];
            if (c) kb_push(c);
        }
    }
    for (int i = 0; i < 6; i++) xhci_prev_keys[i] = r[i + 2];
}

/*
 * The boot-protocol mouse is relative, so this moves the pointer by the
 * signed deltas rather than setting it. That is the same arrangement the
 * PS/2 path uses and the reason the VMware backdoor exists on the x86
 * side: USB HID boot mice cannot report absolute position, so on real
 * hardware the cursor is driven, not tracked. Under a hypervisor the
 * absolute backdoor still wins and this never runs.
 */
static void xhci_handle_mouse(const uint8_t *r) {
    mouse_buttons = (uint8_t)(r[0] & 0x07);
    int8_t dx = (int8_t)r[1];
    int8_t dy = (int8_t)r[2];
    mouse_x += dx;
    mouse_y += dy;
    if (r[3]) mouse_wheel += (int8_t)r[3];

    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_max_x && mouse_x > (int32_t)mouse_max_x) mouse_x = (int32_t)mouse_max_x;
    if (mouse_max_y && mouse_y > (int32_t)mouse_max_y) mouse_y = (int32_t)mouse_max_y;
}

/*
 * Called once a frame. Drains completed transfers and re-queues each
 * device's buffer so the controller always has somewhere to put the next
 * report — a device with nothing queued simply stops reporting, which
 * looks like a keyboard that works once.
 */
static void xhci_poll(void) {
    if (!xhci_present) return;

    /* Ports first: a device that arrived this frame gets its transfer
     * queued before the event ring is drained, so its first report is
     * not one frame late. */
    xhci_hotplug_poll();
    if (xhci_hid_count == 0) return;

    for (int guard = 0; guard < 32; guard++) {
        xhci_trb_t *e = &xhci_evt_ring[xhci_evt_idx];
        uint32_t ctrl = e->control;
        if ((ctrl & TRB_CYCLE) != (xhci_evt_cycle ? TRB_CYCLE : 0)) return;

        uint32_t type = TRB_GET_TYPE(ctrl);
        uint8_t  slot = (uint8_t)(ctrl >> 24);

        xhci_evt_idx++;
        if (xhci_evt_idx == XHCI_RING_TRBS) {
            xhci_evt_idx = 0;
            xhci_evt_cycle ^= 1;
        }
        xhci_wr64(xhci_rt, XHCI_ERDP,
                  kern_virt_to_phys(&xhci_evt_ring[xhci_evt_idx]) | (1u << 3));

        if (type != TRB_TRANSFER_EVENT) continue;

        for (int i = 0; i < xhci_hid_count; i++) {
            xhci_hid_t *h = &xhci_hid[i];
            if (!h->used || h->slot != slot) continue;

            uint32_t cc = TRB_GET_CC(e->status);
            /* Short packets are normal for HID: the device sends fewer
             * bytes than the endpoint's maximum and that is not an error. */
            if (cc == TRB_CC_SUCCESS || cc == 13 /* short packet */) {
                if (h->is_keyboard) xhci_handle_keyboard(h->buf);
                else                xhci_handle_mouse(h->buf);
            }
            xhci_queue_report(h);
            break;
        }
    }
}

#endif /* XHCI_H */
