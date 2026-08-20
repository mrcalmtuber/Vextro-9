#ifndef USBHUB_H
#define USBHUB_H

#include <stdint.h>
#include "xhci.h"
#include "usbmsc.h"

/*
 * src/usbhub.h — devices that are not plugged into the machine.
 *
 * Everything before this assumed one cable from the port to the device.
 * That is not how anyone's desk works: a laptop has two sockets, a
 * monitor has a hub in it, and a keyboard has a hub in it, so the mouse
 * is plugged into the keyboard which is plugged into the monitor which
 * is plugged into the machine. Three tiers, and none of the three
 * devices was visible to this system.
 *
 * ---- the route string ----
 *
 * xHCI has one idea that makes this tractable, and it is worth stating
 * plainly because everything else follows from it: the controller does
 * the routing. The driver never forwards a packet through a hub. It
 * tells the controller *where* the device is, as a twenty-bit route
 * string -- five four-bit port numbers, one per tier, read from the
 * root outwards -- and from then on the device is addressed exactly
 * like one plugged into the machine directly. There is no per-hub
 * transfer path and no store and forward.
 *
 * So a hub driver is much smaller than it sounds. It has to:
 *
 *   - notice a device is a hub, from its class
 *   - read the hub descriptor to find out how many ports it has
 *   - turn each port's power on and wait the settling time the
 *     descriptor asks for
 *   - reset any port with something on it, and read back the speed
 *   - hand the port number to the ordinary setup path, with one more
 *     nibble in the route string
 *
 * and that is all of it. The recursion is genuine -- a hub found on a
 * hub's port is set up the same way, one tier deeper -- and bounded at
 * five, which is what twenty bits of route string holds and also the
 * limit USB itself imposes.
 *
 * ---- the transaction translator ----
 *
 * The one place this is not uniform. A full or low speed device behind
 * a high speed hub cannot talk to the controller directly; the hub
 * translates for it, and the controller has to be told which hub is
 * doing the translating. That is the parent slot and port written into
 * the slot context. Getting it wrong does not fail loudly -- the device
 * is addressed successfully and then never answers -- which is why it
 * is set here rather than left for later.
 */

#define USBHUB_MAX      4
#define USBHUB_MAX_TIER 5

/* Hub class requests. The recipient is the *port*, which is why these
 * are 0x23 and 0xA3 rather than the device-recipient forms. */
#define HUB_GET_DESCRIPTOR   0xA0
#define HUB_GET_PORT_STATUS  0xA3
#define HUB_SET_PORT_FEATURE 0x23
#define HUB_CLR_PORT_FEATURE 0x23

#define HUB_FEAT_PORT_RESET       4
#define HUB_FEAT_PORT_POWER       8
#define HUB_FEAT_C_PORT_CONNECTION 16
#define HUB_FEAT_C_PORT_RESET      20

#define HUB_PORT_CONNECTED  0x0001
#define HUB_PORT_ENABLED    0x0002
#define HUB_PORT_LOWSPEED   0x0200
#define HUB_PORT_HIGHSPEED  0x0400

typedef struct {
    int      used;
    uint8_t  slot;
    uint32_t root_port;
    uint32_t route;
    uint8_t  tier;
    uint8_t  nports;
    uint8_t  pwr_good_2ms;      /* bPwrOn2PwrGood, in 2 ms units */
    uint32_t speed;
} usbhub_t;

static usbhub_t usbhub_hubs[USBHUB_MAX];
static int usbhub_count = 0;

static uint8_t    usbhub_dev_ctx[USBHUB_MAX][2048] __attribute__((aligned(64)));
static uint8_t    usbhub_in_ctx[USBHUB_MAX][2048]  __attribute__((aligned(64)));
static uint8_t    usbhub_ctrl_ring[USBHUB_MAX][XHCI_RING_TRBS * 16] __attribute__((aligned(64)));
static uint8_t    usbhub_buf[USBHUB_MAX][256] __attribute__((aligned(64)));

/* Forward: a hub's port may hold another hub. */
static int usbhub_setup(uint32_t root_port, uint32_t route, uint8_t tier,
                        uint8_t parent_slot, uint8_t parent_port,
                        uint32_t speed_hint);

/*
 * Address a device and read enough of its descriptors to know what it
 * is. Shared by everything below, because "what is on this port" has
 * exactly one answer and three possible consequences.
 *
 * Returns the device class from the interface descriptor, or -1.
 * On success the slot is left addressed and `out_slot` names it; the
 * caller either keeps it or gives it back.
 */
static int usb_probe_device(uint32_t root_port, uint32_t route, uint32_t speed,
                            uint8_t parent_slot, uint8_t parent_port,
                            uint8_t *ctx_dev, uint8_t *ctx_in,
                            uint8_t *ctrl_ring, uint8_t *buf,
                            xhci_hid_t *ctl, uint16_t *out_total) {
    xhci_cmd_push(0, 0, TRB_TYPE(TRB_ENABLE_SLOT));
    xhci_trb_t ev;
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return -1;
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return -1;
    uint8_t slot = (uint8_t)(ev.control >> 24);
    if (slot == 0 || slot >= 64) return -1;

    xhci_zero(ctx_dev, 2048);
    xhci_zero(ctx_in, 2048);
    xhci_ring_init((xhci_trb_t *)ctrl_ring, XHCI_RING_TRBS);
    xhci_dcbaa[slot] = kern_virt_to_phys(ctx_dev);

    uint32_t *icc = xhci_ctx_at(ctx_in, 0);
    icc[1] = 0x3;
    uint32_t *sc = xhci_ctx_at(ctx_in, 1);
    sc[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF);
    sc[1] = (root_port + 1) << 16;
    if (route) sc[2] = (uint32_t)parent_slot | ((uint32_t)parent_port << 8);

    uint32_t mps = (speed == 4) ? 512 : (speed == 3) ? 64 : 8;
    uint32_t *ep0 = xhci_ctx_at(ctx_in, 2);
    ep0[1] = (4u << 3) | (mps << 16) | (3u << 1);
    ((uint64_t *)ep0)[1] = kern_virt_to_phys(ctrl_ring) | 1;

    xhci_cmd_push(kern_virt_to_phys(ctx_in), 0,
                  TRB_TYPE(TRB_ADDRESS_DEVICE) | ((uint32_t)slot << 24));
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0) return -1;
    if (TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) return -1;

    /*
     * The handle belongs to the caller and is carried onwards.
     *
     * Building a fresh one for each stage was the bug that made a hub
     * detectable and unusable: xhci_control advances ring_idx, and a new
     * handle starts at zero, so the next transfer is written where the
     * controller has already been while its dequeue pointer sits further
     * along looking at an empty entry. The device is found, named, and
     * then never answers again.
     */
    for (uint32_t k = 0; k < sizeof(*ctl); k++) ((uint8_t *)ctl)[k] = 0;
    ctl->slot = slot;
    ctl->ring = (xhci_trb_t *)ctrl_ring;
    ctl->cycle = 1;
    ctl->buf = buf;

    if (xhci_control(ctl, 0x80, 6, 0x0200, 0, buf, 64) != 0) return -1;
    uint16_t total = (uint16_t)(buf[2] | (buf[3] << 8));
    if (total > 255) total = 255;
    if (total > 64 && xhci_control(ctl, 0x80, 6, 0x0200, 0, buf, total) != 0)
        return -1;

    *out_total = total;

    /* The interface class of the first interface. A composite device
     * has several and this takes the first, which is the same choice
     * the HID path already makes. */
    for (uint32_t o = 0; o + 1 < total; ) {
        uint8_t dl = buf[o], dt = buf[o + 1];
        if (dl == 0) break;
        if (dt == 4 && o + 8 < total) return buf[o + 5];
        o += dl;
    }
    return 0;
}

/* ---- hub control helpers ---- */

static int hub_port_feature(xhci_hid_t *ctl, uint8_t feature, uint8_t port,
                            int set) {
    return xhci_control(ctl, set ? HUB_SET_PORT_FEATURE : 0x23,
                        set ? 3 : 1, feature, port, 0, 0);
}

static int hub_port_status(xhci_hid_t *ctl, uint8_t port, uint8_t *out4) {
    return xhci_control(ctl, HUB_GET_PORT_STATUS, 0, 0, port, out4, 4);
}

/*
 * Walk a configured hub's ports and bring up whatever is on them.
 */
static void usbhub_walk(usbhub_t *h, xhci_hid_t *ctlp) {
    xhci_hid_t ctl = *ctlp;   /* carries the live ring index and cycle */

    for (uint8_t p = 1; p <= h->nports; p++) {
        hub_port_feature(&ctl, HUB_FEAT_PORT_POWER, p, 1);
    }
    /* bPwrOn2PwrGood is in 2 ms units and is the hub telling us how long
     * its own power switches take to settle. Reading a port before then
     * reports nothing connected on a port that has a device on it. */
    xhci_delay(h->pwr_good_2ms * 2 * 1000);

    for (uint8_t p = 1; p <= h->nports; p++) {
        uint8_t st[4] = { 0, 0, 0, 0 };
        if (hub_port_status(&ctl, p, st) != 0) continue;
        uint16_t status = (uint16_t)(st[0] | (st[1] << 8));
        if (!(status & HUB_PORT_CONNECTED)) continue;

        hub_port_feature(&ctl, HUB_FEAT_PORT_RESET, p, 1);

        int ok = 0;
        for (int i = 0; i < 50; i++) {
            xhci_delay(2000);
            if (hub_port_status(&ctl, p, st) != 0) break;
            status = (uint16_t)(st[0] | (st[1] << 8));
            if (status & HUB_PORT_ENABLED) { ok = 1; break; }
        }
        if (!ok) continue;

        /* Acknowledge the change bits, or the hub keeps reporting them
         * and a later poll re-resets a port that is already working. */
        hub_port_feature(&ctl, HUB_FEAT_C_PORT_RESET, p, 0);
        hub_port_feature(&ctl, HUB_FEAT_C_PORT_CONNECTION, p, 0);

        /* xHCI speed IDs: 1 full, 2 low, 3 high, 4 super. The hub
         * reports low and high as flags and full as the absence of
         * both. */
        uint32_t speed = (status & HUB_PORT_LOWSPEED)  ? 2 :
                         (status & HUB_PORT_HIGHSPEED) ? 3 : 1;

        /* One more nibble, at this tier's position. */
        uint32_t route = h->route | ((uint32_t)p << (4 * (h->tier - 1)));

        /*
         * Which hub translates for this device.
         *
         * A high speed device behind a high speed hub needs no
         * translation and the parent fields must stay zero -- setting
         * them anyway makes the controller route split transactions for
         * a device that does not use them, and it stops answering.
         * A slower device inherits its parent's translator if the
         * parent is itself slow, which is what the second branch says.
         */
        uint8_t tt_slot = 0, tt_port = 0;
        if (speed < 3 && h->speed >= 3) {
            tt_slot = h->slot;
            tt_port = p;
        }

        if (usbhub_setup(h->root_port, route, (uint8_t)(h->tier + 1),
                         tt_slot, tt_port, speed))
            continue;
    }
}

/*
 * Bring up whatever is at `route`, which may be a hub, a HID device or
 * a disk. Returns 1 if it was a hub (and therefore recursed).
 */
static int usbhub_setup(uint32_t root_port, uint32_t route, uint8_t tier,
                        uint8_t parent_slot, uint8_t parent_port,
                        uint32_t speed) {
    if (tier > USBHUB_MAX_TIER) {
        serial_puts("[usb-hub] too deep; USB allows five tiers\n");
        return 0;
    }
    if (usbhub_count >= USBHUB_MAX) return 0;

    int i = usbhub_count;
    uint16_t total = 0;
    xhci_hid_t ctl;

    int cls = usb_probe_device(root_port, route, speed,
                               parent_slot, parent_port,
                               usbhub_dev_ctx[i], usbhub_in_ctx[i],
                               usbhub_ctrl_ring[i], usbhub_buf[i],
                               &ctl, &total);
    uint8_t slot = ctl.slot;
    if (cls < 0) {
        serial_puts("[usb-hub] could not probe port ");
        serial_put_dec(root_port);
        serial_puts(" at tier ");
        serial_put_dec(tier);
        serial_putc('\n');
        return 0;
    }


    if (cls != 0x09) {
        /* Not a hub. Give the slot back and let the ordinary paths
         * enumerate it from scratch -- they set up endpoints this
         * function knows nothing about. */
        xhci_release_slot(slot);
        if (route == 0) return 0;

        /*
         * Behind a hub. Neither path may reset the root port -- doing so
         * would knock the hub itself off the bus, and every other device
         * on it with the hub -- so both are handed the route string and
         * the speed the hub already reported.
         */
        if (cls == 0x03) {
            if (xhci_setup_routed(root_port, route, parent_slot,
                                  parent_port, speed))
                xhci_queue_report(&xhci_hid[xhci_hid_count - 1]);
            return 0;
        }
        if (cls == 0x08 && xhci_storage_hook)
            xhci_storage_hook(root_port, route, parent_slot,
                              (uint8_t)(parent_port | (speed << 4)));
        return 0;
    }

    usbhub_t *h = &usbhub_hubs[i];
    for (uint32_t k = 0; k < sizeof(*h); k++) ((uint8_t *)h)[k] = 0;
    h->slot = slot;
    h->root_port = root_port;
    h->route = route;
    h->tier = tier;
    h->speed = speed;

    /* SET_CONFIGURATION, then the hub descriptor. Descriptor type 0x29
     * for a USB 2 hub, 0x2A for a USB 3 one; both put the port count at
     * offset 2 and the power-on delay at offset 5. */
    uint8_t cfg_value = usbhub_buf[i][5];
    if (xhci_control(&ctl, 0x00, 9, cfg_value, 0, 0, 0) != 0) {
        xhci_release_slot(slot);
        return 0;
    }

    uint8_t desc_type = (speed == 4) ? 0x2A : 0x29;
    if (xhci_control(&ctl, HUB_GET_DESCRIPTOR, 6,
                     (uint16_t)(desc_type << 8), 0, usbhub_buf[i], 8) != 0) {
        serial_puts("[usb-hub] no hub descriptor\n");
        xhci_release_slot(slot);
        return 0;
    }
    h->nports       = usbhub_buf[i][2];
    h->pwr_good_2ms = usbhub_buf[i][5];
    if (h->nports == 0 || h->nports > 15) h->nports = 4;

    /*
     * Tell the controller this slot is a hub.
     *
     * Bit 26 of the first slot context word, with the port count in the
     * second. Without it the controller will not route anything through
     * this device: a route string naming a port on a slot it does not
     * believe is a hub is rejected, and every device behind it fails to
     * address with no indication why.
     */
    xhci_zero(usbhub_in_ctx[i], 2048);
    uint32_t *icc = xhci_ctx_at(usbhub_in_ctx[i], 0);
    icc[1] = 0x1;                                  /* evaluate the slot */
    uint32_t *sc = xhci_ctx_at(usbhub_in_ctx[i], 1);
    sc[0] = (1u << 27) | (speed << 20) | (route & 0xFFFFF) | (1u << 26);
    sc[1] = ((root_port + 1) << 16) | ((uint32_t)h->nports << 24);
    /* Think time and multi-TT live in the third word for a USB 2 hub. */
    if (speed == 3) sc[2] = (uint32_t)parent_slot |
                            ((uint32_t)parent_port << 8);

    xhci_cmd_push(kern_virt_to_phys(usbhub_in_ctx[i]), 0,
                  TRB_TYPE(TRB_EVAL_CONTEXT) | ((uint32_t)slot << 24));
    xhci_trb_t ev;
    if (xhci_wait_for(TRB_COMMAND_COMPLETE, &ev, 200000) < 0 ||
        TRB_GET_CC(ev.status) != TRB_CC_SUCCESS) {
        serial_puts("[usb-hub] the controller would not accept it as a hub\n");
        xhci_release_slot(slot);
        return 0;
    }

    h->used = 1;
    usbhub_count++;

    serial_puts("[usb-hub] tier ");
    serial_put_dec(tier);
    serial_puts(" hub on slot ");
    serial_put_dec(slot);
    serial_puts(", ");
    serial_put_dec(h->nports);
    serial_puts(" ports\n");

    usbhub_walk(h, &ctl);
    return 1;
}

/*
 * Offered every port that turned out to hold neither a keyboard, a
 * mouse nor a disk. A hub is what is left.
 */
static int usbhub_try(uint32_t port, uint32_t route, uint8_t parent_slot,
                      uint8_t parent_port) {
    (void)route; (void)parent_slot; (void)parent_port;

    uint32_t psc = xhci_rd32(xhci_op, XHCI_PORTSC(port));
    if (!(psc & XHCI_PORTSC_CCS)) return 0;
    uint32_t speed = (psc >> 10) & 0xF;
    if (speed == 0) speed = 3;

    return usbhub_setup(port, 0, 1, 0, 0, speed);
}

static void usbhub_init(void) { xhci_hub_hook = usbhub_try; }

#endif /* USBHUB_H */
