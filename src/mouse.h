#ifndef MOUSE_H
#define MOUSE_H

#include <stdint.h>
#include "idt.h"
#include "vmmouse.h"
#include "paccel.h"

/* Global cursor position + button state — written by IRQ12, read by render loop */
volatile int32_t mouse_x = 0;
volatile int32_t mouse_y = 0;
volatile uint8_t mouse_buttons = 0;

/*
 * Scroll notches, positive towards the top of a document.  The render
 * loop drains this each frame; see the read-then-subtract in kmain,
 * which keeps a notch that lands mid-drain rather than dropping it.
 */
volatile int32_t mouse_wheel = 0;

/* Set once the VMware backdoor has agreed to report absolute positions. */
static volatile int mouse_absolute = 0;

static int32_t mouse_max_x = 1023;
static int32_t mouse_max_y = 767;

/* Packet assembly state.  Wheel mice send four bytes, plain ones three. */
/* Whether a pointing device has ever answered on the aux port. Drives
 * hot-plug probing: see ps2_hotplug_poll. */
static int      mouse_seen = 0;
static uint16_t mouse_init_cs = 0;

static uint8_t mouse_buf[4];
static int     mouse_phase = 0;
static int     mouse_pkt_len = 3;

/* ---- PS/2 controller I/O ---- */
static void ps2_wait_write(void) {
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++);
}
static void ps2_wait_read(void) {
    for (int i = 0; i < 100000 && !(inb(0x64) & 1); i++);
}
static void ps2_cmd(uint8_t c)  { ps2_wait_write(); outb(0x64, c); }
static void ps2_data(uint8_t d) { ps2_wait_write(); outb(0x60, d); }
static uint8_t ps2_read(void)   { ps2_wait_read();  return inb(0x60); }

/* Send one byte to the mouse rather than the controller, return its ACK */
static uint8_t ps2_aux_cmd(uint8_t b) {
    ps2_cmd(0xD4);
    ps2_data(b);
    return ps2_read();
}

/*
 * Take every position the backdoor has queued.  Returns how many packets
 * were consumed, which is what tells the caller whether the PS/2 packet
 * that raised this interrupt carried real motion or was only a knock on
 * the door from an absolute device.
 */
static int vmm_drain(void) {
    vmm_regs_t r;
    int got = 0;

    for (int guard = 0; guard < 32; guard++) {
        vmm_call(VMM_CMD_STATUS, 0, &r);
        if (r.ax == VMM_STATUS_ERROR) {
            /* Hand the pointer back before falling through to PS/2: while
             * the host still believes an absolute device is listening it
             * sends nothing to the relative one, and the mouse would die
             * rather than degrade. */
            vmm_disable();
            mouse_absolute = 0;
            return got;
        }
        if ((r.ax & 0xFFFFu) < 4) break;  /* a packet is four words */

        vmm_call(VMM_CMD_DATA, 4, &r);

        uint32_t btn = r.ax & 0xFFFFu;
        uint32_t ax  = r.bx & 0xFFFFu;    /* absolute, full scale */
        uint32_t ay  = r.cx & 0xFFFFu;
        int32_t  dz  = (int32_t)(int8_t)(uint8_t)r.dx;

        int32_t px = (int32_t)((ax * (uint32_t)(mouse_max_x + 1)) >> 16);
        int32_t py = (int32_t)((ay * (uint32_t)(mouse_max_y + 1)) >> 16);
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px > mouse_max_x) px = mouse_max_x;
        if (py > mouse_max_y) py = mouse_max_y;
        mouse_x = px;
        mouse_y = py;

        mouse_buttons = (uint8_t)(((btn & VMM_BTN_LEFT)   ? 1 : 0) |
                                  ((btn & VMM_BTN_RIGHT)  ? 2 : 0) |
                                  ((btn & VMM_BTN_MIDDLE) ? 4 : 0));

        mouse_wheel -= dz;   /* the wire convention is negative-for-up */
        got++;
    }
    return got;
}

/* ---- IRQ12 handler: one assembled PS/2 packet ---- */
__attribute__((target("general-regs-only")))
static void mouse_packet(void) {
    /*
     * An absolute pointer answers here first.  QEMU raises IRQ12 by
     * poking a placeholder packet through the PS/2 port, so if the
     * backdoor had anything queued this packet's deltas are not motion
     * and must not be added to anything.
     */
    if (mouse_absolute && vmm_drain()) return;

    uint8_t flags = mouse_buf[0];

    mouse_buttons = flags & 0x07;

    /* Discard packets that report coordinate overflow */
    if (flags & 0xC0) return;

    int32_t dx = (int32_t)(uint32_t)mouse_buf[1];
    int32_t dy = (int32_t)(uint32_t)mouse_buf[2];

    /* Sign-extend using the sign bits in the flags byte */
    if (flags & 0x10) dx |= (int32_t)0xFFFFFF00;
    if (flags & 0x20) dy |= (int32_t)0xFFFFFF00;

    /*
     * Accelerate. This is the relative path, which is the only one where
     * acceleration means anything -- the VMware backdoor above reports an
     * absolute position and is deliberately left alone.
     *
     * dy is negated first so the curve sees the motion in screen terms;
     * the sign does not matter to it, but the pairing does.
     */
    dy = -dy;                    /* screen y grows downwards */
    paccel_apply(&dx, &dy);

    mouse_x += dx;
    mouse_y += dy;

    if (mouse_x < 0)           mouse_x = 0;
    if (mouse_y < 0)           mouse_y = 0;
    if (mouse_x > mouse_max_x) mouse_x = mouse_max_x;
    if (mouse_y > mouse_max_y) mouse_y = mouse_max_y;

    if (mouse_pkt_len == 4)
        mouse_wheel -= (int32_t)(int8_t)mouse_buf[3];
}

__attribute__((interrupt, target("general-regs-only")))
static void irq12_handler(interrupt_frame_t *frame) {
    (void)frame;

    /* Bit 5 of status port: output came from aux port (mouse) */
    if (!(inb(0x64) & 0x20)) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    uint8_t data = inb(0x60);

    /* Packet byte 0 must always have bit 3 set — resync if lost */
    if (mouse_phase == 0 && !(data & 0x08)) {
        outb(PIC2_CMD, PIC_EOI);
        outb(PIC1_CMD, PIC_EOI);
        return;
    }

    mouse_buf[mouse_phase++] = data;

    if (mouse_phase >= mouse_pkt_len) {
        mouse_phase = 0;
        mouse_packet();
    }

    outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* ---- Initialize PS/2 mouse and hook IRQ12 ---- */
static void mouse_init(uint16_t cs, int32_t max_x, int32_t max_y) {
    mouse_init_cs = cs;
    mouse_max_x = max_x;
    mouse_max_y = max_y;
    mouse_x     = max_x / 2;
    mouse_y     = max_y / 2;

    ps2_cmd(0xA8);         /* Enable aux (mouse) port */

    ps2_cmd(0x20);         /* Read controller config byte */
    uint8_t cfg = ps2_read();
    cfg |=  (uint8_t)(1 << 1);   /* enable IRQ12 */
    cfg &= (uint8_t)~(1 << 5);   /* clear: enable aux clock */
    ps2_cmd(0x60);         /* Write config byte */
    ps2_data(cfg);

    /*
     * The IntelliMouse knock: a wheel mouse watches for the sample rate
     * being set to 200, then 100, then 80, and answers device id 3
     * afterwards — at which point it starts sending a fourth byte
     * carrying the wheel.  A plain two-button mouse ignores all of it
     * and still answers 0, so this is safe to try either way.
     */
    ps2_aux_cmd(0xF3); ps2_aux_cmd(200);
    ps2_aux_cmd(0xF3); ps2_aux_cmd(100);
    ps2_aux_cmd(0xF3); ps2_aux_cmd(80);

    /* The acknowledgement to identify is also the presence test: an
     * empty port cannot produce 0xFA. */
    uint8_t id_ack = ps2_aux_cmd(0xF2);
    uint8_t id     = ps2_read();
    if (id == 3) mouse_pkt_len = 4;

    ps2_aux_cmd(0xF3); ps2_aux_cmd(100);  /* back to a sane report rate */
    ps2_aux_cmd(0xF4);                    /* enable packet reporting */

    /* Prefer an absolute pointer if the host offers one: it needs no
     * grab, so the cursor tracks straight away and stays exact. */
    mouse_absolute = vmm_enable_absolute();

    /* Install the IRQ12 handler at vector 0x2C (slave IRQ4 = IRQ12) */
    idt_set_gate(0x2C, irq12_handler, cs);

    /* Unmask: IRQ2 (cascade) on master, IRQ4 (IRQ12) on slave */
    outb(PIC1_DATA, (uint8_t)~(1 << 2));
    outb(PIC2_DATA, (uint8_t)~(1 << 4));

    mouse_seen = (id_ack == 0xFA);
}

/* ===== HOT-PLUG =====
 *
 * PS/2 has no hot-plug. There is no presence pin, no insertion
 * interrupt, and nothing in the specification that says a device may
 * arrive after boot -- the port was designed on the assumption that
 * unplugging a mouse from a running machine was something you did not
 * do. So detection here means asking, and asking is not free.
 *
 * The rule this follows is: only probe a port believed to be *empty*.
 *
 * That asymmetry is the whole design. Sending an identify command to a
 * mouse that is working means the next byte the controller returns is
 * the answer to that command rather than the next third of a movement
 * packet, and the packet decoder -- which has no way to tell the two
 * apart -- loses its phase. The symptom is a pointer that jumps across
 * the screen every few seconds, caused entirely by the code checking
 * whether the pointer is still there.
 *
 * A mouse that is unplugged simply stops sending. Nothing detects that,
 * and nothing needs to: an absent mouse and a still mouse look the same
 * to everything above this file. What matters is that plugging one *in*
 * works, and that is what this does.
 */
static uint32_t ps2_probe_tick = 0;

/* Drain anything the controller has buffered without interpreting it.
 * Used before a probe so the reply cannot be confused with stale data. */
static void ps2_flush(void) {
    for (int i = 0; i < 32 && (inb(0x64) & 0x01); i++) (void)inb(0x60);
}

/*
 * Called from the frame loop. Returns 1 if a device has just appeared.
 *
 * Once a second, not every frame: an identify command with its
 * acknowledgement is a handful of microseconds of port I/O each of
 * which stalls the processor, and a device that arrives is noticed
 * within a second either way -- which is faster than the hand that
 * plugged it in can reach the desk.
 */
static int ps2_hotplug_poll(void) {
    if (mouse_seen) return 0;
    if (++ps2_probe_tick < 60) return 0;
    ps2_probe_tick = 0;

    ps2_flush();

    /* 0xF2 is identify. A port with nothing on it times out in
     * ps2_wait_read and returns whatever the data port held, so the
     * answer is only believed when it is one of the values a real
     * pointing device gives: 0x00 for a plain mouse, 0x03 for a wheel
     * mouse, 0x04 for a five-button one. */
    ps2_cmd(0xD4);
    ps2_data(0xF2);
    uint8_t ack = ps2_read();
    if (ack != 0xFA) return 0;

    uint8_t id = ps2_read();
    if (id != 0x00 && id != 0x03 && id != 0x04) return 0;

    serial_puts("[ps2] pointer plugged in (id ");
    serial_put_dec(id);
    serial_puts(")\n");

    /* Re-run the full initialisation rather than only enabling
     * reporting: a device that has just been powered up has none of the
     * sample rate, resolution or wheel negotiation that mouse_init
     * performs, and its defaults are not the ones this system wants. */
    mouse_init(mouse_init_cs, mouse_max_x, mouse_max_y);
    return 1;
}

#endif /* MOUSE_H */
