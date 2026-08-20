#ifndef USBMSC_H
#define USBMSC_H

#include <stdint.h>
#include "xhci.h"

/*
 * src/usbmsc.h — USB storage.
 *
 * A memory stick is not a new kind of disk. It is a SCSI disk with a
 * different cable, and the whole of the USB Mass Storage Bulk-Only
 * Transport specification is a description of how to put a SCSI command
 * into a USB bulk transfer. There are exactly three of them per
 * operation:
 *
 *   1. a 31-byte Command Block Wrapper, out, with the SCSI command
 *      inside it
 *   2. the data, in or out, if the command has any
 *   3. a 13-byte Command Status Wrapper, in
 *
 * That is the entire protocol. Everything else in this file is the SCSI
 * underneath -- INQUIRY to find out what it is, READ CAPACITY to find
 * out how big, READ(10) and WRITE(10) to use it -- and the xHCI bulk
 * endpoints to carry them.
 *
 * ---- why this is separate from the HID path in xhci.h ----
 *
 * The two share the controller, the command ring and the event ring,
 * and nothing else. A HID device has one interrupt IN endpoint that is
 * left permanently armed, and reports arrive whether or not anyone
 * asked. Storage has two bulk endpoints driven strictly in lockstep,
 * where every transfer is a question with an answer and the next
 * question cannot be asked until the previous answer has arrived.
 * Trying to express both through one structure produced a set of
 * fields where half were meaningless for either kind, so they are two
 * structures.
 *
 * ---- the tag, and why it is checked ----
 *
 * Each command carries a tag which the device must echo in the status.
 * Checking it is not ceremony: if a data transfer times out and the
 * device later completes it anyway, the *next* command's status read
 * returns the previous command's status. Without the tag check that
 * reads as success, and the caller is handed a buffer that was never
 * filled. With it, the mismatch is caught and the endpoint is reset.
 */

#define USBMSC_MAX 2

/* ---- Bulk-Only Transport ---- */
#define CBW_SIGNATURE  0x43425355u      /* "USBC" */
#define CSW_SIGNATURE  0x53425355u      /* "USBS" */

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_len;
    uint8_t  flags;                     /* 0x80 = device to host */
    uint8_t  lun;
    uint8_t  cb_len;
    uint8_t  cb[16];
} __attribute__((packed)) usbmsc_cbw_t;

typedef struct {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;                    /* 0 good, 1 failed, 2 phase error */
} __attribute__((packed)) usbmsc_csw_t;

typedef struct {
    int        used;
    uint8_t    slot;
    uint32_t   port;
    uint8_t    iface;
    uint8_t    ep_in_id;                /* xHCI endpoint id, 2N+1 */
    uint8_t    ep_out_id;               /* xHCI endpoint id, 2N   */
    uint16_t   mps_in;
    uint16_t   mps_out;
    xhci_trb_t *ring_in;
    xhci_trb_t *ring_out;
    uint32_t   idx_in, idx_out;
    uint8_t    cyc_in, cyc_out;
    uint32_t   tag;

    uint64_t   sectors;                 /* in 512-byte units */
    uint32_t   block_size;              /* what the device actually uses */
    char       vendor[9];
    char       product[17];
} usbmsc_dev_t;

static usbmsc_dev_t usbmsc_devs[USBMSC_MAX];
static int usbmsc_count = 0;

/*
 * Filled in by src/blk.h. A stick is found when it is plugged in, which
 * may be long after the disks were probed -- the block layer had already
 * printed its count by the time the first one came up. So attachment is
 * an event rather than something asked about once at boot, and the same
 * path serves boot and hot-plug.
 */
static void (*usbmsc_attach_hook)(int unit) = 0;

/* Separate contexts from the HID ones: a storage device needs its own
 * slot, and the HID arrays are sized for HID devices. */
static uint8_t    usbmsc_dev_ctx[USBMSC_MAX][2048] __attribute__((aligned(64)));
static uint8_t    usbmsc_in_ctx[USBMSC_MAX][2048]  __attribute__((aligned(64)));
static xhci_trb_t usbmsc_ring_in[USBMSC_MAX][XHCI_RING_TRBS]  __attribute__((aligned(64)));
static xhci_trb_t usbmsc_ring_out[USBMSC_MAX][XHCI_RING_TRBS] __attribute__((aligned(64)));
static uint8_t    usbmsc_ctrl_ring[USBMSC_MAX][XHCI_RING_TRBS * 16] __attribute__((aligned(64)));

/* Bounce buffers. The controller writes by physical address, so what it
 * fills has to be memory this kernel can hand it a physical address for
 * -- and a caller's buffer might be anywhere. 64 KB covers 128 sectors,
 * which is the largest read the block layer issues. */
static uint8_t usbmsc_data[USBMSC_MAX][65536] __attribute__((aligned(4096)));
static usbmsc_cbw_t usbmsc_cbw[USBMSC_MAX]    __attribute__((aligned(64)));
static usbmsc_csw_t usbmsc_csw[USBMSC_MAX]    __attribute__((aligned(64)));

/*
 * One bulk transfer.
 *
 * A Normal TRB naming a buffer, the doorbell, and then wait for the
 * transfer event that says how much moved. IOC asks for that event; ISP
 * asks for one on a short packet too, which matters because a device
 * answering INQUIRY with fewer bytes than were asked for is normal and
 * not an error -- without ISP that transfer never completes and the
 * driver waits for ever on a device that has already answered.
 */
static int usbmsc_bulk(usbmsc_dev_t *d, int in, void *buf, uint32_t len,
                       uint32_t *moved) {
    xhci_trb_t *ring = in ? d->ring_in : d->ring_out;
    uint32_t   *idx  = in ? &d->idx_in : &d->idx_out;
    uint8_t    *cyc  = in ? &d->cyc_in : &d->cyc_out;
    uint8_t     epid = in ? d->ep_in_id : d->ep_out_id;

    xhci_trb_t *t = &ring[*idx];
    t->param  = kern_virt_to_phys(buf);
    t->status = len;
    t->control = TRB_TYPE(TRB_NORMAL) | (1u << 5) | (1u << 2) | *cyc;
    /*                                   IOC        ISP        cycle  */

    *idx += 1;
    if (*idx >= XHCI_RING_TRBS - 1) {
        /* The link TRB at the end sends the controller back to the
         * start; toggling our cycle bit is what tells it the wrapped
         * entries are new rather than the ones it already consumed. */
        ring[XHCI_RING_TRBS - 1].control =
            (ring[XHCI_RING_TRBS - 1].control & ~1u) | *cyc;
        *idx = 0;
        *cyc ^= 1;
    }

    xhci_db[d->slot] = epid;

    xhci_trb_t ev;
    if (xhci_wait_for(TRB_TRANSFER_EVENT, &ev, 3000000) < 0) return -1;

    uint32_t cc = TRB_GET_CC(ev.status);
    /* SHORT_PACKET is success with a residue, which is exactly what a
     * device does when it has less to say than was asked. */
    if (cc != TRB_CC_SUCCESS && cc != TRB_CC_SHORT_PACKET) return -1;

    if (moved) *moved = len - (ev.status & 0xFFFFFF);
    return 0;
}

/*
 * One SCSI command, wrapped.
 *
 * `dir` is 1 for data coming in, 0 for data going out, and anything
 * with no data at all passes len 0.
 */
static int usbmsc_command(usbmsc_dev_t *d, const uint8_t *cdb, uint8_t cdb_len,
                          int dir, void *data, uint32_t len) {
    int idx = (int)(d - usbmsc_devs);
    usbmsc_cbw_t *cbw = &usbmsc_cbw[idx];
    usbmsc_csw_t *csw = &usbmsc_csw[idx];

    uint32_t tag = ++d->tag;

    for (uint32_t i = 0; i < sizeof(*cbw); i++) ((uint8_t *)cbw)[i] = 0;
    cbw->signature = CBW_SIGNATURE;
    cbw->tag       = tag;
    cbw->data_len  = len;
    cbw->flags     = dir ? 0x80 : 0x00;
    cbw->lun       = 0;
    cbw->cb_len    = cdb_len;
    for (uint8_t i = 0; i < cdb_len && i < 16; i++) cbw->cb[i] = cdb[i];

    if (usbmsc_bulk(d, 0, cbw, 31, 0) != 0) return -1;

    if (len) {
        if (usbmsc_bulk(d, dir, data, len, 0) != 0) {
            /* The data stage failed, but the device still owes a
             * status. Reading it is what keeps the endpoint in phase --
             * skipping it leaves the status of this command waiting to
             * be mistaken for the status of the next. */
            usbmsc_bulk(d, 1, csw, 13, 0);
            return -1;
        }
    }

    if (usbmsc_bulk(d, 1, csw, 13, 0) != 0) return -1;

    if (csw->signature != CSW_SIGNATURE) return -1;
    if (csw->tag != tag) {
        /* See the note at the top of the file: an echoed tag from an
         * earlier command means the two are out of step, and believing
         * it would hand back a buffer nobody filled. */
        serial_puts("[usb-msc] status tag mismatch; the device is out of step\n");
        return -1;
    }
    return csw->status == 0 ? 0 : -1;
}

/* ---- the SCSI commands this needs, and no others ---- */

static int usbmsc_test_unit_ready(usbmsc_dev_t *d) {
    uint8_t cdb[6] = { 0x00, 0, 0, 0, 0, 0 };
    return usbmsc_command(d, cdb, 6, 1, 0, 0);
}

static int usbmsc_inquiry(usbmsc_dev_t *d) {
    int idx = (int)(d - usbmsc_devs);
    uint8_t cdb[6] = { 0x12, 0, 0, 0, 36, 0 };
    if (usbmsc_command(d, cdb, 6, 1, usbmsc_data[idx], 36) != 0) return -1;

    const uint8_t *r = usbmsc_data[idx];
    for (int i = 0; i < 8; i++)  d->vendor[i]  = (char)r[8 + i];
    for (int i = 0; i < 16; i++) d->product[i] = (char)r[16 + i];
    d->vendor[8] = 0;
    d->product[16] = 0;
    /* Trailing spaces are how SCSI pads these fields, not part of the
     * name. */
    for (int i = 7; i >= 0 && d->vendor[i] == ' '; i--)  d->vendor[i] = 0;
    for (int i = 15; i >= 0 && d->product[i] == ' '; i--) d->product[i] = 0;
    return 0;
}

static int usbmsc_read_capacity(usbmsc_dev_t *d) {
    int idx = (int)(d - usbmsc_devs);
    uint8_t cdb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    if (usbmsc_command(d, cdb, 10, 1, usbmsc_data[idx], 8) != 0) return -1;

    const uint8_t *r = usbmsc_data[idx];
    /* Big-endian, because SCSI is. */
    uint32_t last  = ((uint32_t)r[0] << 24) | ((uint32_t)r[1] << 16) |
                     ((uint32_t)r[2] << 8)  |  (uint32_t)r[3];
    uint32_t bsize = ((uint32_t)r[4] << 24) | ((uint32_t)r[5] << 16) |
                     ((uint32_t)r[6] << 8)  |  (uint32_t)r[7];
    if (bsize == 0) bsize = 512;

    d->block_size = bsize;
    /* The block layer above addresses everything in 512-byte sectors,
     * whatever the device uses, so the capacity is converted here and
     * the reads below do the arithmetic. A 4 KB-block stick is
     * ordinary. */
    d->sectors = ((uint64_t)last + 1) * (bsize / 512);
    return 0;
}

/*
 * Read and write, in the 512-byte sectors the block layer speaks.
 *
 * A device with 4 KB blocks cannot be asked for one 512-byte sector, so
 * the request is converted to whole device blocks and the wanted part
 * copied out. Writing the same way would need a read-modify-write; this
 * refuses instead, because a partial-block write that silently zeroed
 * the rest of the block would be worse than an error.
 */
static int usbmsc_read(uint8_t unit, uint64_t lba, uint32_t count, void *buf) {
    if (unit >= usbmsc_count || !usbmsc_devs[unit].used) return -1;
    usbmsc_dev_t *d = &usbmsc_devs[unit];
    uint32_t per = d->block_size / 512;
    if (per == 0) per = 1;

    uint8_t *out = (uint8_t *)buf;
    while (count) {
        uint64_t dev_lba = lba / per;
        uint32_t skip    = (uint32_t)(lba % per);
        uint32_t blocks  = (skip + count + per - 1) / per;
        if (blocks * d->block_size > sizeof(usbmsc_data[0]))
            blocks = (uint32_t)(sizeof(usbmsc_data[0]) / d->block_size);
        if (blocks == 0) return -1;

        uint32_t bytes = blocks * d->block_size;
        uint8_t cdb[10] = {
            0x28, 0,
            (uint8_t)(dev_lba >> 24), (uint8_t)(dev_lba >> 16),
            (uint8_t)(dev_lba >> 8),  (uint8_t)dev_lba,
            0, (uint8_t)(blocks >> 8), (uint8_t)blocks, 0
        };
        if (usbmsc_command(d, cdb, 10, 1, usbmsc_data[unit], bytes) != 0)
            return -1;

        uint32_t avail = blocks * per - skip;
        uint32_t take  = avail < count ? avail : count;
        const uint8_t *src = usbmsc_data[unit] + (uint64_t)skip * 512;
        for (uint32_t i = 0; i < take * 512u; i++) out[i] = src[i];

        out   += take * 512u;
        lba   += take;
        count -= take;
    }
    return 0;
}

static int usbmsc_write(uint8_t unit, uint64_t lba, uint32_t count,
                        const void *buf) {
    if (unit >= usbmsc_count || !usbmsc_devs[unit].used) return -1;
    usbmsc_dev_t *d = &usbmsc_devs[unit];
    uint32_t per = d->block_size / 512;
    if (per == 0) per = 1;

    if (per != 1 && ((lba % per) || (count % per))) {
        /* See above: silently reading, patching and writing back would
         * be the usual answer, and it turns a torn write into corrupt
         * data in blocks the caller never mentioned. */
        serial_puts("[usb-msc] refusing a write that is not a whole device block\n");
        return -1;
    }

    const uint8_t *in = (const uint8_t *)buf;
    while (count) {
        uint32_t blocks = count / per;
        if (blocks * d->block_size > sizeof(usbmsc_data[0]))
            blocks = (uint32_t)(sizeof(usbmsc_data[0]) / d->block_size);
        if (blocks == 0) return -1;

        uint32_t bytes = blocks * d->block_size;
        for (uint32_t i = 0; i < bytes; i++) usbmsc_data[unit][i] = in[i];

        uint64_t dev_lba = lba / per;
        uint8_t cdb[10] = {
            0x2A, 0,
            (uint8_t)(dev_lba >> 24), (uint8_t)(dev_lba >> 16),
            (uint8_t)(dev_lba >> 8),  (uint8_t)dev_lba,
            0, (uint8_t)(blocks >> 8), (uint8_t)blocks, 0
        };
        if (usbmsc_command(d, cdb, 10, 0, usbmsc_data[unit], bytes) != 0)
            return -1;

        uint32_t did = blocks * per;
        in    += did * 512u;
        lba   += did;
        count -= did;
    }
    return 0;
}

/*
 * Bring up a storage device found on a port.
 *
 * Everything up to reading the configuration descriptor is the same
 * dance as a HID device -- reset, enable a slot, address it -- and is
 * repeated here rather than shared because the HID version writes into
 * arrays indexed by HID device number.
 */
/* Says which step gave up. A storage device that fails to come up
 * otherwise looks exactly like a port with nothing on it. */
static int usbmsc_fail(const char *why) {
    serial_puts("[usb-msc] gave up at ");
    serial_puts(why);
    serial_putc('\n');
    return 0;
}

/* The same, once a slot has been taken. Handing it back is what lets the
 * next driver offered this port address the device -- see the note on
 * xhci_release_slot. Not doing so is why a hub behind this path was
 * invisible. */
static int usbmsc_fail_slot(const char *why, uint8_t slot) {
    xhci_release_slot(slot);
    return usbmsc_fail(why);
}

static int usbmsc_setup_port(uint32_t port, uint32_t route, uint8_t parent_slot,
                             uint8_t parent_port) {
    if (usbmsc_count >= USBMSC_MAX) return 0;

    uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
    uint32_t speed;

    if (route == 0) {
        if (!(psc & XHCI_PORTSC_CCS)) return 0;
        xhci_wr32(xhci_op, XHCI_PORTSC(port),
                  (psc & ~0x80FFu) | XHCI_PORTSC_PR | XHCI_PORTSC_PP);
        int ok = 0;
        for (int i = 0; i < 1000; i++) {
            psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
            if (psc & XHCI_PORTSC_PRC) { ok = 1; break; }
            xhci_delay(2);
        }
        if (!ok || !(psc & XHCI_PORTSC_PED)) return usbmsc_fail("port reset");
        xhci_wr32(xhci_op, XHCI_PORTSC(port), psc);
        speed = (psc >> 10) & 0xF;
    } else {
        /* Behind a hub: the hub has already reset the port and reported
         * the speed, which arrives in parent_port's high nibble. */
        speed = parent_port >> 4;
        parent_port &= 0x0F;
    }

    xhci_cmd_push(0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    xhci_trb_t ev;
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return usbmsc_fail("enable slot");
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return usbmsc_fail("enable slot");
    uint8_t slot = (uint8_t)(ev.control >> 24);
    if (slot == 0 || slot >= 64) return usbmsc_fail("slot number");

    int i = usbmsc_count;
    usbmsc_dev_t *d = &usbmsc_devs[i];
    for (uint32_t k = 0; k < sizeof(*d); k++) ((uint8_t *)d)[k] = 0;
    d->slot = slot;
    d->port = port;
    d->ring_in  = usbmsc_ring_in[i];
    d->ring_out = usbmsc_ring_out[i];
    d->cyc_in = d->cyc_out = 1;
    d->block_size = 512;

    xhci_zero(usbmsc_dev_ctx[i], 2048);
    xhci_zero(usbmsc_in_ctx[i], 2048);
    xhci_ring_init((xhci_trb_t *)usbmsc_ctrl_ring[i], XHCI_RING_TRBS);
    xhci_ring_init(d->ring_in, XHCI_RING_TRBS);
    xhci_ring_init(d->ring_out, XHCI_RING_TRBS);
    xhci_dcbaa[slot] = kern_virt_to_phys(usbmsc_dev_ctx[i]);

    uint32_t *icc = xhci_ctx_at(usbmsc_in_ctx[i], 0);
    icc[1] = 0x3;
    uint32_t *sc = xhci_ctx_at(usbmsc_in_ctx[i], 1);
    sc[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);
    sc[1] = (port + 1) << 16;
    if (route) {
        /* A full or low speed device behind a high speed hub needs the
         * hub's own slot and port recorded, so the controller knows
         * which transaction translator to route split transactions
         * through. Without it the device is addressed and then never
         * answers. */
        sc[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8);
    }

    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
    uint32_t *ep0 = xhci_ctx_at(usbmsc_in_ctx[i], 2);
    ep0[1] = (4u << 3) | (mps << 16) | (3u << 1);
    ((uint64_t *)ep0)[1] = kern_virt_to_phys(usbmsc_ctrl_ring[i]) | 1;

    xhci_cmd_push(kern_virt_to_phys(usbmsc_in_ctx[i]), 0,
                  TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot << 24));
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return usbmsc_fail_slot("address device", slot);
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return usbmsc_fail_slot("address device", slot);

    /* A control transfer needs a HID-shaped handle, because xhci_control
     * was written against one. The fields it touches are the slot and
     * the ring, both of which are meaningful here. */
    xhci_hid_t ctl;
    for (uint32_t k = 0; k < sizeof(ctl); k++) ((uint8_t *)&ctl)[k] = 0;
    ctl.slot = slot;
    ctl.ring = (xhci_trb_t *)usbmsc_ctrl_ring[i];
    ctl.cycle = 1;
    ctl.buf = usbmsc_data[i];

    uint8_t *cfg = usbmsc_data[i];
    if (xhci_control(&ctl, 0x80, 6, 0x0200, 0, cfg, 64) != 0)
        return usbmsc_fail_slot("first config descriptor read", slot);
    uint16_t total = (uint16_t)(cfg[2] | (cfg[3] << 8));
    if (total > 512) total = 512;
    if (total > 64 && xhci_control(&ctl, 0x80, 6, 0x0200, 0, cfg, total) != 0)
        return usbmsc_fail_slot("full config descriptor read", slot);

    /* Class 8 subclass 6 protocol 0x50: SCSI transparent command set
     * over Bulk-Only Transport. Anything else -- CBI, UFI, a floppy
     * emulation -- is a different protocol and is not handled. */
    int found = 0;
    uint8_t cfg_value = cfg[5];
    int in_msc = 0;
    int ep_in = -1, ep_out = -1;
    uint16_t mps_in = 512, mps_out = 512;

    for (uint32_t o = 0; o + 1 < total; ) {
        uint8_t dl = cfg[o], dt = cfg[o + 1];
        if (dl == 0) break;
        if (dt == 4 && o + 8 < total) {
            in_msc = (cfg[o + 5] == 0x08 && cfg[o + 6] == 0x06 &&
                      cfg[o + 7] == 0x50);
            if (in_msc) d->iface = cfg[o + 2];
        } else if (dt == 5 && in_msc && o + 6 < total) {
            uint8_t addr = cfg[o + 2];
            uint8_t attr = cfg[o + 3] & 0x3;
            uint16_t m   = (uint16_t)(cfg[o + 4] | (cfg[o + 5] << 8));
            if (attr == 2) {                       /* bulk */
                if (addr & 0x80) { ep_in  = addr & 0xF; mps_in  = m; }
                else             { ep_out = addr & 0xF; mps_out = m; }
            }
            if (ep_in >= 0 && ep_out >= 0) { found = 1; break; }
        }
        o += dl;
    }
    if (!found) {
        /* Not a disk. That is an ordinary answer for a port with a hub
         * or a keyboard on it, so it is silent -- the driver is offered
         * every port and most of them are not its business. */
        xhci_release_slot(slot);
        return 0;
    }

    if (xhci_control(&ctl, 0x00, 9, cfg_value, 0, 0, 0) != 0)
        return usbmsc_fail_slot("set configuration", slot);

    d->ep_in_id  = (uint8_t)(ep_in * 2 + 1);
    d->ep_out_id = (uint8_t)(ep_out * 2);
    d->mps_in = mps_in;
    d->mps_out = mps_out;

    /* Configure both bulk endpoints in one command. */
    uint8_t hi = d->ep_in_id > d->ep_out_id ? d->ep_in_id : d->ep_out_id;
    xhci_zero(usbmsc_in_ctx[i], 2048);
    icc = xhci_ctx_at(usbmsc_in_ctx[i], 0);
    icc[1] = 1u | (1u << d->ep_in_id) | (1u << d->ep_out_id);
    sc = xhci_ctx_at(usbmsc_in_ctx[i], 1);
    sc[0] = ((uint32_t)hi << 27) | (speed << 20) | (route & 0xFFFFF);
    sc[1] = (port + 1) << 16;
    if (route) sc[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8);

    uint32_t *epi = xhci_ctx_at(usbmsc_in_ctx[i], d->ep_in_id + 1);
    epi[1] = (6u << 3) | ((uint32_t)mps_in << 16) | (3u << 1);   /* bulk IN */
    ((uint64_t *)epi)[1] = kern_virt_to_phys(d->ring_in) | 1;
    epi[4] = mps_in;

    uint32_t *epo = xhci_ctx_at(usbmsc_in_ctx[i], d->ep_out_id + 1);
    epo[1] = (2u << 3) | ((uint32_t)mps_out << 16) | (3u << 1);  /* bulk OUT */
    ((uint64_t *)epo)[1] = kern_virt_to_phys(d->ring_out) | 1;
    epo[4] = mps_out;

    xhci_cmd_push(kern_virt_to_phys(usbmsc_in_ctx[i]), 0,
                  TRB_TYPE(TRB_CONFIGURE_ENDPOINT) | ((uint32_t)slot << 24));
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return usbmsc_fail_slot("configure endpoints", slot);
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return usbmsc_fail_slot("configure endpoints", slot);

    d->used = 1;
    usbmsc_count++;

    /*
     * A stick that has just been powered up answers TEST UNIT READY
     * with "not ready" for a second or two while it spins up its
     * controller. Asking once and giving up reports no disk on a disk
     * that is about to work.
     */
    int ready = 0;
    for (int t = 0; t < 40; t++) {
        if (usbmsc_test_unit_ready(d) == 0) { ready = 1; break; }
        xhci_delay(50);
    }
    if (!ready) {
        d->used = 0; usbmsc_count--;
        return usbmsc_fail_slot("test unit ready", slot);
    }

    usbmsc_inquiry(d);
    if (usbmsc_read_capacity(d) != 0) {
        d->used = 0; usbmsc_count--;
        return usbmsc_fail_slot("read capacity", slot);
    }

    serial_puts("[usb-msc] ");
    serial_puts(d->vendor);
    serial_putc(' ');
    serial_puts(d->product);
    serial_puts(", ");
    serial_put_dec((uint32_t)(d->sectors / 2048));
    serial_puts(" MB, ");
    serial_put_dec(d->block_size);
    serial_puts("-byte blocks\n");

    if (usbmsc_attach_hook) usbmsc_attach_hook(i);
    return 1;
}

static int usbmsc_present(void) { return usbmsc_count > 0; }

/* Let the controller call us for anything that is not a HID device. */
static void usbmsc_init(void) { xhci_storage_hook = usbmsc_setup_port; }

#endif /* USBMSC_H */
