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

int  vx_nic_present(void) { return e1000_found; }
uint32_t vx_nic_mtu(void) { return 1500; }

void vx_nic_mac(uint8_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = e1000_mac[i];
}

int vx_nic_send(const uint8_t *frame, uint16_t len) {
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
    if (!e1000_found) return 0;
    uint8_t *buf = 0;
    uint16_t len = 0;
    if (!e1000_rx_poll(&buf, &len)) return 0;
    if (len > max) len = max;
    for (uint16_t i = 0; i < len; i++) out[i] = buf[i];
    *got = len;
    return 1;
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
