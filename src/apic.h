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
#include "kernel_shared.h"
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

/* ===== THE INTERRUPT COMMAND REGISTER =====
 *
 * The one part of the local APIC that talks about a processor other than
 * the one writing it. An IPI is composed in two halves: the high word
 * says who it is for, the low word says what it is, and writing the low
 * word is what sends it — so the high word must be written first, every
 * time, or the message goes to whoever the previous write named.
 *
 * Three delivery modes are used here and nothing else is:
 *
 *   INIT    puts a processor into its reset state and leaves it there,
 *           waiting. It is not a message the target runs any code for.
 *   STARTUP carries an 8-bit vector which the target takes as the top
 *           twelve bits of a physical address: it begins executing in
 *           real mode at vector << 12, with CS set accordingly. That is
 *           why the trampoline has to live in the first megabyte and on
 *           a page boundary — the encoding cannot express anything else.
 *   FIXED   an ordinary vector, used to wake a worker that has parked
 *           itself in HLT.
 *
 * Bit 12 is delivery status and reads as set while a message is still in
 * flight. Sending a second IPI before it clears is not queued; it
 * replaces what was there.
 */
#define APIC_REG_ICR_LO   0x300
#define APIC_REG_ICR_HI   0x310

#define APIC_ICR_FIXED    0x00000000u
#define APIC_ICR_INIT     0x00000500u
#define APIC_ICR_STARTUP  0x00000600u
#define APIC_ICR_ASSERT   0x00004000u
#define APIC_ICR_LEVEL    0x00008000u   /* trigger mode: level           */
#define APIC_ICR_PENDING  0x00001000u   /* delivery status: still in flight */

/* The vector an idle application processor is woken on. Above the
 * scheduler's timer and below the spurious vector, in the range the
 * IDT's default stubs already cover. */
#define APIC_VEC_WAKE     0x41

/* APIC_VEC_SPURIOUS and APIC_VEC_TIMER moved to
 * include/kernel_shared.h, with lapic_eoi — the scheduler installs both
 * vectors and acknowledges from inside the timer stub. */

/* Not static: lapic_eoi is inlined into scheduler.o and reads both. */
volatile uint8_t *lapic_base = 0;
int               lapic_present = 0;
static uint32_t   lapic_ticks_per_ms = 0;

static inline uint32_t lapic_read(uint32_t reg) {
    return *(volatile uint32_t *)(lapic_base + reg);
}
static inline void lapic_write(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

/* lapic_eoi moved to include/kernel_shared.h, unchanged and still
 * always_inline: it is called from the timer stub in scheduler.o. */

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

/* ===== WHO AM I, AND SENDING TO SOMEBODY ELSE =====
 *
 * Every processor sees its own local APIC at the same physical address,
 * so lapic_read here answers about whichever processor is executing it.
 * That is the whole mechanism by which a woken core discovers which core
 * it is, and it is why the ID is read rather than passed in.
 */
static uint32_t lapic_id(void) {
    if (!lapic_present) return 0;
    return lapic_read(APIC_REG_ID) >> 24;
}

/* Wait for the last message to leave. Bounded, because a local APIC that
 * never clears delivery status is a machine that would otherwise hang
 * here with nothing said. */
static int lapic_ipi_wait(void) {
    for (int i = 0; i < 1000000; i++) {
        if (!(lapic_read(APIC_REG_ICR_LO) & APIC_ICR_PENDING)) return 0;
        __asm__ volatile("pause");
    }
    return -1;
}

/*
 * Compose and send. High word first — see the note on the ICR above; the
 * low word is the trigger and must be the last thing written.
 */
static int lapic_ipi(uint32_t apic_id, uint32_t low) {
    if (!lapic_present) return -1;
    if (lapic_ipi_wait() != 0) return -1;
    lapic_write(APIC_REG_ICR_HI, apic_id << 24);
    lapic_write(APIC_REG_ICR_LO, low);
    return lapic_ipi_wait();
}

static int lapic_send_init(uint32_t apic_id) {
    return lapic_ipi(apic_id, APIC_ICR_INIT | APIC_ICR_ASSERT |
                              APIC_ICR_LEVEL);
}

/* The vector is the page number of the trampoline: the target begins
 * executing at vector << 12 in real mode. */
static int lapic_send_startup(uint32_t apic_id, uint8_t vector) {
    return lapic_ipi(apic_id, APIC_ICR_STARTUP | APIC_ICR_ASSERT | vector);
}

static int lapic_send_wake(uint32_t apic_id) {
    return lapic_ipi(apic_id, APIC_ICR_FIXED | APIC_ICR_ASSERT |
                              APIC_VEC_WAKE);
}

/*
 * Bring up the local APIC of the processor that is executing this.
 *
 * Everything lapic_init does *once for the machine* — finding the
 * registers, mapping them, measuring the bus clock — has already been
 * done by the time an application processor runs. What has not been done
 * for that processor is the part that is per-processor state: the task
 * priority, which silently drops every vector below it if it is left at
 * whatever reset put there, and the spurious-interrupt register, whose
 * enable bit is what makes the APIC deliver anything at all.
 *
 * Deliberately absent: the timer. One processor drives the scheduler's
 * clock and it is the boot processor; a second timer ticking into the
 * same handler would double the tick rate and halve every sleep in the
 * system.
 */
static void lapic_init_ap(void) {
    if (!lapic_present) return;
    uint64_t base_msr = rdmsr(MSR_APIC_BASE);
    wrmsr(MSR_APIC_BASE, base_msr | APIC_BASE_ENABLE);
    lapic_write(APIC_REG_TPR, 0);
    lapic_write(APIC_REG_SVR, APIC_SVR_ENABLE | APIC_VEC_SPURIOUS);
    lapic_write(APIC_REG_LVT_TMR, APIC_LVT_MASKED);
    lapic_write(APIC_REG_TMR_INIT, 0);
}

#endif /* APIC_H */
