#ifndef VMMOUSE_H
#define VMMOUSE_H

#include <stdint.h>

/*
 * VMware backdoor absolute pointer.
 *
 * A PS/2 mouse only ever reports *relative* motion, and a host UI can
 * only turn that back into a pointer position if it captures the real
 * cursor first.  QEMU's Cocoa backend does exactly that: until you click
 * to grab, it forwards no motion at all, so a guest that has nothing but
 * a PS/2 mouse looks like it has no mouse.
 *
 * The way out is an absolute device, and there is one already sitting on
 * QEMU's default `pc` machine: `vmmouse`, which speaks the VMware
 * backdoor protocol.  Once the guest asks for absolute mode, QEMU hands
 * over host pointer coordinates directly and stops needing the grab — the
 * cursor simply tracks, in a window or full screen, with no capture.
 *
 * The protocol is a hypervisor call disguised as a port read: load the
 * magic into EAX, a command into ECX and 0x5658 into DX, then execute
 * `in`.  The hypervisor traps it and writes its answer back into the
 * general registers.  On real hardware nothing decodes that port, the
 * read comes back as all-ones, the version handshake fails, and we stay
 * on PS/2 — which is why detection is safe to attempt unconditionally.
 *
 * Data still arrives via IRQ12: the device queues the packet here and
 * pokes the PS/2 controller so the existing interrupt path wakes up.
 */

#define VMM_MAGIC          0x564D5868u   /* "VMXh", little-endian */
#define VMM_PORT           0x5658

#define VMM_CMD_GETVERSION 10
#define VMM_CMD_DATA       39
#define VMM_CMD_STATUS     40
#define VMM_CMD_COMMAND    41

#define VMM_ENABLE         0x45414552u
#define VMM_DISABLE        0x000000F5u
#define VMM_ABSOLUTE       0x53424152u
#define VMM_RELATIVE       0x4C455252u

#define VMM_VERSION_ID     0x3442554Au
#define VMM_STATUS_ERROR   0xFFFF0000u

/* Button bits as the backdoor reports them (not the PS/2 order) */
#define VMM_BTN_LEFT       0x20
#define VMM_BTN_RIGHT      0x10
#define VMM_BTN_MIDDLE     0x08

typedef struct { uint32_t ax, bx, cx, dx; } vmm_regs_t;

/*
 * One backdoor call.  ESI/EDI are listed as outputs because the protocol
 * permits the hypervisor to write them; naming them keeps the compiler
 * from assuming they survive.
 */
static inline void vmm_call(uint32_t cmd, uint32_t in_bx, vmm_regs_t *r) {
    uint32_t ax, bx, cx, dx, si, di;
    __asm__ volatile("inl (%%dx), %%eax"
                     : "=a"(ax), "=b"(bx), "=c"(cx), "=d"(dx),
                       "=S"(si), "=D"(di)
                     : "a"(VMM_MAGIC), "b"(in_bx),
                       "c"(cmd), "d"(VMM_PORT)
                     : "memory");
    (void)si; (void)di;
    r->ax = ax; r->bx = bx; r->cx = cx; r->dx = dx;
}

/* Is anything answering on the backdoor port at all? */
static int vmm_detect(void) {
    vmm_regs_t r;
    vmm_call(VMM_CMD_GETVERSION, 0, &r);
    /* A hypervisor echoes the magic back in EBX; bare metal floats high. */
    return (r.bx == VMM_MAGIC && r.ax != 0xFFFFFFFFu);
}

/*
 * Switch the pointer into absolute mode.  Returns 1 on success.
 *
 * ENABLE pushes the protocol version onto the device's queue rather than
 * returning it, so the handshake is: enable, check the queue grew, read
 * the version out of it, then ask for absolute reporting.
 */
static int vmm_enable_absolute(void) {
    vmm_regs_t r;

    if (!vmm_detect()) return 0;

    vmm_call(VMM_CMD_COMMAND, VMM_ENABLE, &r);

    vmm_call(VMM_CMD_STATUS, 0, &r);
    if (r.ax == VMM_STATUS_ERROR || (r.ax & 0xFFFFu) == 0) return 0;

    vmm_call(VMM_CMD_DATA, 1, &r);
    if (r.ax != VMM_VERSION_ID) return 0;

    vmm_call(VMM_CMD_COMMAND, VMM_ABSOLUTE, &r);
    return 1;
}

static void vmm_disable(void) {
    vmm_regs_t r;
    vmm_call(VMM_CMD_COMMAND, VMM_DISABLE, &r);
}

#endif /* VMMOUSE_H */
