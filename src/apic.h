#ifndef APIC_H
#define APIC_H

/*
 * src/apic.h — the local APIC, for one thing: a timer fast enough to
 * schedule with.
 *
 * The PIT stays exactly where it is, at 60 Hz, because that is the frame
 * clock and every animation in this system is written against it. What
 * it cannot also be is the scheduler's tick. A preemption granularity of
 * sixteen milliseconds means a program that wants the processor waits up
 * to a whole frame for it, and the desktop and one application end up
 * trading whole frames back and forth — thirty frames a second each,
 * visibly.
 *
 * The local APIC's timer runs off the bus clock and can be set to a
 * millisecond without touching the PIT at all, so the two coexist: the
 * PIT still says when to draw, the APIC says when to switch. Device
 * interrupts stay on the 8259 where the drivers already expect them.
 * Moving mouse, keyboard and network to an I/O APIC in the same change
 * would mean that if the machine came up dead there would be no way to
 * tell which half did it.
 *
 * The count is calibrated rather than assumed. The bus frequency is not
 * architecturally discoverable on the processors this runs on, and it
 * differs between an emulator and real silicon by an order of magnitude.
 */

#include <stdint.h>
#include "pci.h"

#define MSR_APIC_BASE     0x1B
#define APIC_BASE_ENABLE  (1ULL << 11)

#define APIC_REG_ID       0x020
#define APIC_REG_VERSION  0x030
#define APIC_REG_TPR      0x080
#define APIC_REG_EOI      0x0B0
#define APIC_REG_SVR      0x0F0
#define APIC_REG_LVT_TMR  0x320
#define APIC_REG_LVT_LINT0 0x350
#define APIC_REG_LVT_LINT1 0x360
#define APIC_REG_TMR_INIT 0x380
#define APIC_REG_TMR_CURR 0x390
#define APIC_REG_TMR_DIV  0x3E0

#define APIC_SVR_ENABLE   0x100
#define APIC_LVT_MASKED   0x10000
#define APIC_TIMER_PERIODIC 0x20000

#define APIC_VEC_SPURIOUS 0xFF
#define APIC_VEC_TIMER    0x40

static volatile uint8_t *lapic_base = 0;
static int      lapic_present = 0;
static uint32_t lapic_ticks_per_ms = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_base + reg);
}
static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

/* Acknowledge. Every APIC-delivered interrupt ends with this, and one
 * that does not blocks every interrupt at or below its priority for
 * good — which presents as the machine freezing some seconds after
 * boot, with no fault and nothing on the wire. */
static inline void lapic_eoi(void) {
    if (lapic_present) lapic_write(APIC_REG_EOI, 0);
}

static int cpu_has_apic(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                             : "a"(1));
    (void)eax; (void)ebx; (void)ecx;
    return (edx >> 9) & 1;
}

/*
 * How many APIC ticks are there in a millisecond?
 *
 * Measured against the time stamp counter, which tsc_calibrate() has
 * already pinned to the PIT. Ten milliseconds of counting is long enough
 * that the cost of the reads themselves disappears into the result, and
 * short enough not to be felt at boot.
 */
static uint32_t lapic_calibrate(void) {
    lapic_write(APIC_REG_TMR_DIV, 0x3);              /* divide by 16 */
    lapic_write(APIC_REG_LVT_TMR, APIC_LVT_MASKED);
    lapic_write(APIC_REG_TMR_INIT, 0xFFFFFFFFu);

    uint64_t start = cycle_now();
    uint64_t target = cycles_per_ms ? cycles_per_ms * 10 : 0;
    if (!target) {
        /* The TSC was never calibrated. Fall back to a PIT-free spin of
         * a fixed length, which is wrong on any particular machine but
         * wrong by a bounded factor and still leaves a usable tick. */
        for (volatile uint32_t i = 0; i < 20000000u; i++) { }
    } else {
        while (cycle_now() - start < target) __asm__ volatile("pause");
    }

    uint32_t remaining = lapic_read(APIC_REG_TMR_CURR);
    lapic_write(APIC_REG_TMR_INIT, 0);
    uint32_t elapsed = 0xFFFFFFFFu - remaining;
    uint32_t per_ms = target ? elapsed / 10 : elapsed / 10;
    return per_ms ? per_ms : 1;
}

/*
 * Bring it up and start the tick.
 *
 * `hz` is the scheduler's rate, not the display's. A thousand is a
 * millisecond of granularity: fine enough that an application sharing
 * the machine with the compositor never waits a visible length of time
 * for the processor, coarse enough that the switch itself — some
 * hundreds of cycles, plus a 512-byte FPU save — stays far below one
 * percent of it.
 */
static void lapic_init(uint32_t hz) {
    if (!cpu_has_apic()) {
        serial_puts("[apic] no local APIC on this processor\n");
        return;
    }

    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    uint64_t phys = base_msr & 0xFFFFFF000ULL;
    wrmsr(MSR_APIC_BASE, base_msr | APIC_BASE_ENABLE);

    /* Uncacheable, which mmio_map already guarantees. The registers are
     * memory only in the sense that they are addressed like it. */
    lapic_base = mmio_map(phys, 0x1000);
    if (!lapic_base) {
        serial_puts("[apic] could not map the register page\n");
        return;
    }
    lapic_present = 1;

    /* Accept everything: a task priority above zero silently drops the
     * vectors below it, and the timer's is low. */
    lapic_write(APIC_REG_TPR, 0);
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_VEC_SPURIOUS);

    lapic_ticks_per_ms = lapic_calibrate();

    uint32_t count = lapic_ticks_per_ms * 1000u / (hz ? hz : 1000u);
    if (!count) count = 1;

    lapic_write(APIC_REG_TMR_DIV, 0x3);
    lapic_write(APIC_REG_LVT_TMR, APIC_VEC_TIMER | APIC_TIMER_PERIODIC);
    lapic_write(APIC_REG_TMR_INIT, count);

    serial_puts("[apic] local APIC at ");
    serial_put_hex32((uint32_t)phys);
    serial_puts(", ");
    serial_put_dec(lapic_ticks_per_ms);
    serial_puts(" ticks/ms, scheduling at ");
    serial_put_dec(hz);
    serial_puts(" Hz\n");
}

static void lapic_stop(void) {
    if (!lapic_present) return;
    lapic_write(APIC_REG_LVT_TMR, APIC_LVT_MASKED);
    lapic_write(APIC_REG_TMR_INIT, 0);
}

#endif /* APIC_H */
