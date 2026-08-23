#ifndef VXPORT_IMPL_H
#define VXPORT_IMPL_H

/*
 * src/vxport_impl.h — the kernel's half of the contract in vxport.h.
 *
 * Included by kernel.c, once, near the end -- after the heap, the
 * scheduler and the NIC exist, because every function here is a thin
 * non-static wrapper over one of them. Thin on purpose: the point of the
 * seam is that it is auditable, and a wrapper with logic in it is a
 * wrapper someone has to read twice.
 *
 * Nothing in this file may be static. That is the entire reason it
 * exists as a separate file rather than a section of kernel.c: it is the
 * one place where the single-translation-unit rule is deliberately
 * broken, and keeping it in one file makes the exception visible instead
 * of scattered.
 */

#include "vxport.h"

/* ===== memory ===== */

void *vx_alloc(uint64_t bytes) { return kmalloc(bytes); }
void  vx_free(void *p)         { kfree(p); }
uint64_t vx_alloc_size(void *p) { return p ? kheap_usable(p) : 0; }

void *vx_calloc(uint64_t n, uint64_t size) {
    uint64_t total = n * size;
    /* n * size can wrap, and a wrapped allocation is a heap overflow
     * with a certificate parser standing on it. */
    if (n && total / n != size) return 0;
    uint8_t *p = (uint8_t *)kmalloc(total);
    if (!p) return 0;
    for (uint64_t i = 0; i < total; i++) p[i] = 0;
    return p;
}

/* ===== threads ===== */

int vx_thread_start(void (*fn)(void *), void *arg,
                    const char *name, int prio) {
    thread_t *t = sched_spawn_kernel_arg(fn, arg, name, (uint32_t)prio);
    return t ? 1 : 0;
}

void vx_thread_exit(void)       { sched_exit(0); }
void vx_yield(void)             { sched_yield(); }
void vx_sleep_ms(uint32_t ms)   { sched_sleep_ms(ms); }
int  vx_thread_id(void)         { return cur_thread ? (int)cur_thread->pid : 0; }

int  vx_block(void *chan, uint32_t timeout_ms) {
    return sched_block_on(chan, timeout_ms);
}
int  vx_block_locked(void *chan, uint32_t timeout_ms, uint64_t flags) {
    return sched_block_on_locked(chan, timeout_ms, flags);
}
void vx_wake(void *chan, int all) { sched_wake_chan(chan, all); }

uint64_t vx_irq_save(void)              { return irq_save(); }
void     vx_irq_restore(uint64_t flags) { irq_restore(flags); }

/* ===== time =====
 *
 * The scheduler ticks at 1 kHz, so its counter is already milliseconds.
 * Truncated to 32 bits because that is the width lwIP's timer wheel
 * uses, and it handles the wrap itself with serial-number comparisons.
 */
uint32_t vx_now_ms(void) { return (uint32_t)sched_ticks; }

/* ===== the wire ===== */

/*
 * Which link lwIP is actually sitting on.
 *
 * Two can exist at once. Ethernet wins when it is there, because a
 * cable that is plugged in is up the moment the driver loads, whereas a
 * radio is not usable until it has joined a network -- and a default
 * route pointed at an unassociated radio is a machine that looks
 * connected and reaches nothing.
 *
 * The choice is made per call rather than latched at boot so that
 * joining a network after the stack is up works, and so does unplugging
 * a cable. lwIP never learns that any of this happened: one netif, one
 * MAC address, and frames that are Ethernet on this side of the seam
 * whichever radio or wire carried them.
 */
static int vx_use_wifi(void) {
    return !e1000_found && wifi_connected();
}

int  vx_nic_present(void) { return e1000_found || wifi_present(); }
uint32_t vx_nic_mtu(void) { return 1500; }

void vx_nic_mac(uint8_t out[6]) {
    const uint8_t *mac = vx_use_wifi() ? wifi_dev.mac : e1000_mac;
    for (int i = 0; i < 6; i++) out[i] = mac[i];
}

int vx_nic_send(const uint8_t *frame, uint16_t len) {
    if (vx_use_wifi()) return wifi_tx_eth(frame, len);
    if (!e1000_found) return -1;
    return e1000_transmit(frame, len);
}

/*
 * One frame out of the receive ring, copied.
 *
 * e1000_rx_poll hands back a pointer into the DMA ring and immediately
 * releases that descriptor to the card, so the buffer it names is live
 * for as long as it takes the card to wrap the ring -- microseconds
 * under load. lwIP holds a pbuf far longer than that, so the copy is
 * not laziness; handing the pointer up would be a use-after-free with a
 * hardware writer on the other end.
 */
int vx_nic_recv(uint8_t *out, uint16_t max, uint16_t *got) {
    /* The wireless path copies and converts inside wifi_rx_eth(): what
     * comes off the air is an 802.11 frame that may be encrypted, and
     * management frames and the EAPOL rekeys are consumed there rather
     * than being handed up to a stack that has nowhere to put them. */
    if (vx_use_wifi()) return wifi_rx_eth(out, max, got);
    if (!e1000_found) return 0;
    uint8_t *buf = 0;
    uint16_t len = 0;
    if (!e1000_rx_poll(&buf, &len)) return 0;
    if (len > max) len = max;
    for (uint16_t i = 0; i < len; i++) out[i] = buf[i];
    *got = len;
    return 1;
}

/* ===== the certificate authority store =====
 *
 * Read once, on the first connection that asks, and kept. The bundle is
 * a couple of hundred kilobytes of PEM and parsing it into X.509
 * structures costs more than reading it, so Mbed TLS does that once too
 * -- see vxsec_init() in tlsglue.c.
 *
 * A volume with no bundle is not a failure here. It is reported, and
 * the layer above decides what to do about a connection that cannot be
 * authenticated; what it must not do is proceed as though it had been.
 */
/* A full root store is a third of a megabyte of PEM and grows with
 * every new authority; 320 KB was not enough for the bundle this build
 * machine carries and the read was silently refused. */
static uint8_t  vx_ca_buf[512 * 1024];
static uint32_t vx_ca_len = 0;
static int      vx_ca_tried = 0;

uint32_t vx_ca_bundle(const uint8_t **out) {
    if (!vx_ca_tried) {
        uint64_t sz = 0;
        const void *p;

        /*
         * Do not latch a failure that only means "too early".
         *
         * TLS starts before the volume is mounted -- the network comes
         * up long before the disk -- so the first call here happens
         * with no filesystem to read. Recording that as "no bundle"
         * makes the store permanently absent on a machine that has one,
         * which is exactly what it did: certificates went unverified
         * because of an ordering detail rather than a missing file.
         */
        if (!fs_writable()) { *out = vx_ca_buf; return 0; }

        vx_ca_tried = 1;
        p = fs_read_file("/etc/ca-bundle.crt", &sz);
        if (p && sz > 0 && sz < sizeof(vx_ca_buf) - 1) {
            for (uint64_t i = 0; i < sz; i++)
                vx_ca_buf[i] = ((const uint8_t *)p)[i];
            /* Mbed TLS's PEM parser wants a NUL-terminated buffer and
             * counts the terminator in the length it is given. A bundle
             * passed without it parses the first certificate and then
             * runs off the end of the allocation. */
            vx_ca_buf[sz] = 0;
            vx_ca_len = (uint32_t)sz + 1;
        }
    }
    *out = vx_ca_buf;
    return vx_ca_len;
}

/* ===== entropy =====
 *
 * RDRAND. Intel's own guidance is to retry ten times and then treat the
 * generator as broken, because the instruction can legitimately fail
 * when the hardware pool is drained faster than it refills.
 *
 * The failure is reported rather than papered over. A CSPRNG seeded from
 * a buffer that was never written produces the same "random" key every
 * boot, and behaves in every observable way like one that works -- which
 * is the single worst failure mode available to this file.
 */
uint32_t vx_random(uint8_t *out, uint32_t len) {
    uint32_t a, b, c, d;
    cpuid_count(1, 0, &a, &b, &c, &d);
    if (!(c & (1u << 30))) return 0;          /* no RDRAND on this part */

    uint32_t done = 0;
    while (done < len) {
        uint64_t v = 0;
        int ok = 0;
        for (int try = 0; try < 10; try++) {
            uint8_t cf = 0;
            __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(cf) :: "cc");
            if (cf) { ok = 1; break; }
            __asm__ volatile("pause");
        }
        if (!ok) return done;
        for (int i = 0; i < 8 && done < len; i++, done++)
            out[done] = (uint8_t)(v >> (i * 8));
    }
    return done;
}

/* ===== diagnostics ===== */
void vx_log(const char *s)     { serial_puts(s); }
void vx_log_u32(uint32_t v)    { serial_put_dec(v); }

/* ===== floating point =====
 *
 * See the note in vxport.h: this is redundant against the scheduler's
 * own save, and it is here because the brief asked for it and because
 * a local guarantee survives changes to a distant file.
 */
void vx_fpu_save(uint8_t *area) {
    __asm__ volatile("fxsave64 %0" : "=m"(*area) :: "memory");
}
void vx_fpu_restore(const uint8_t *area) {
    __asm__ volatile("fxrstor64 %0" :: "m"(*area) : "memory");
}

#endif /* VXPORT_IMPL_H */
