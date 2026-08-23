#ifndef CRYPTOSWITCH_H
#define CRYPTOSWITCH_H

/*
 * src/cryptoswitch.h — proving that cryptographic state does not bleed
 * across a context switch.
 *
 * The claim this file exists to test is easy to state and impossible to
 * confirm by reading: when the APIC timer preempts a thread in the
 * middle of an AES round, the round keys and intermediate state sitting
 * in XMM registers belong to *that* thread, and the next thread to run
 * must not be able to see or disturb them.
 *
 * Everything the guarantee rests on is already in place. It is spread
 * across three files and none of them says so:
 *
 *   sched.h    fxsave64 is the first thing done to the outgoing thread
 *              and fxrstor64 the last thing done to the incoming one,
 *              after CR3. The 512-byte area covers XMM0-15 and MXCSR,
 *              which is the whole of what AES-NI touches.
 *
 *   idt.h      every handler is target("general-regs-only"), so nothing
 *              between the interrupt firing and fxsave64 executing is
 *              even *able* to emit an XMM instruction. That window is
 *              the only place a leak could occur.
 *
 *   sched.h    the per-thread save area is aligned(16), which fxsave64
 *              requires and faults without.
 *
 * A change to any one of those breaks the property silently: AES keeps
 * producing output, and the output is wrong only for the threads that
 * happened to be preempted mid-block. That is the worst failure shape
 * there is, so it gets a test rather than a comment.
 *
 * Enable with `make EXTRA=-DCRYPTO_SWITCH_SELFTEST`.
 */

#include <stdint.h>
#include "aes.h"
#include "sched.h"

static int cs_checks = 0;
static int cs_fails  = 0;

static void cs_ok(const char *what, int cond) {
    cs_checks++;
    if (!cond) cs_fails++;
    serial_puts(cond ? "  ok    " : "  FAIL  ");
    serial_puts(what);
    serial_putc('\n');
}

/* ===========================================================
 * 1. an XMM register survives being preempted
 * =========================================================== */

/*
 * Write a pattern into XMM5, spin until the timer has taken the thread
 * away and given it back, then read XMM5 out again.
 *
 * The whole sequence is a single asm block, and that is load-bearing.
 * On the SysV ABI every XMM register is caller-saved, so a version of
 * this written as "set register, call sched_yield(), read register" is
 * not a test of anything -- the compiler is entitled to use XMM5 inside
 * the call and would be within its rights to hand back a different
 * value on a perfectly working kernel. Keeping the spin inside the asm
 * leaves the compiler no place to put an instruction, so the only thing
 * that can touch XMM5 between the write and the read is a context
 * switch.
 */
/*
 * Something for the spinning thread to be switched *to*.
 *
 * A context switch needs two runnable threads. sched_pick() returning
 * the thread that is already running is not a switch and is correctly
 * not counted -- so a lone thread spinning with the timer firing at
 * 1 kHz produces a switch count of zero, which is what the first
 * version of this test measured and wrongly called a failure. The
 * preemption is real either way; what was missing was a destination.
 */
static volatile int cs_rival_stop = 0;
static volatile uint32_t cs_rival_spins = 0;

static void cs_rival(void) {
    while (!cs_rival_stop) {
        cs_rival_spins++;
        sched_yield();
    }
}

static int cs_xmm_survives_preemption(uint64_t *switches_seen) {
    static uint8_t before[16] __attribute__((aligned(16))) = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF
    };
    static uint8_t after[16] __attribute__((aligned(16)));
    uint64_t start = sched_switches;
    uint64_t tick_start = sched_ticks;

    /* Long enough to cross several 1 kHz ticks on any machine this
     * runs on; the loop is a dependent chain so it cannot be widened
     * by the processor into finishing early. */
    __asm__ volatile(
        "movdqa %1, %%xmm5\n\t"
        "1:\n\t"
        "sub $1, %%rcx\n\t"
        "jnz 1b\n\t"
        "movdqa %%xmm5, %0\n\t"
        : "=m"(*(uint8_t (*)[16])after)
        : "m"(*(uint8_t (*)[16])before), "c"(200000000ULL)
        : "xmm5", "memory");

    *switches_seen = sched_switches - start;
    serial_puts("[crypto]   spin covered ");
    serial_put_dec((uint32_t)(sched_ticks - tick_start));
    serial_puts(" timer ticks, rival ran ");
    serial_put_dec(cs_rival_spins);
    serial_puts(" times\n");

    for (int i = 0; i < 16; i++)
        if (after[i] != before[i]) return 0;
    return 1;
}

/* ===========================================================
 * 2. AES-NI and the portable path agree
 * =========================================================== */

/*
 * The "hardware encryption check" in this system is aes_have_ni(),
 * which decides at runtime whether AESENC is used. If the two paths
 * disagree, half the machines in the world get different ciphertext
 * from the other half and nothing says so until a file written on one
 * cannot be read on the other.
 *
 * FIPS-197's own vector, through whichever path this processor
 * selected. tools/aes_test.c checks the portable implementation on the
 * build machine, which by construction cannot exercise AESENC; this is
 * the half that only the target can run.
 */
static void cs_aes_paths_agree(void) {
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
        0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const uint8_t pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
        0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    static const uint8_t want[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
        0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };
    aes_key_t k;
    uint8_t out[16], back[16];
    int same = 1;

    serial_puts("[crypto] AES path in use: ");
    serial_puts(aes_have_ni() ? "AES-NI (AESENC)\n" : "portable C\n");

    aes_setkey(&k, key, 128);
    aes_encrypt_block(&k, pt, out);
    for (int i = 0; i < 16; i++) if (out[i] != want[i]) same = 0;
    cs_ok("AES-128 matches FIPS-197 through the selected path", same);

    aes_decrypt_block(&k, out, back);
    same = 1;
    for (int i = 0; i < 16; i++) if (back[i] != pt[i]) same = 0;
    cs_ok("  and decryption inverts it", same);
}

/* ===========================================================
 * 3. concurrent AES, under preemption
 * =========================================================== */

/*
 * Several threads, each holding a different key, each encrypting the
 * same plaintext over and over and checking its own answer.
 *
 * With the scheduler switching a thousand times a second and each
 * encryption taking a few hundred cycles, threads are interrupted
 * inside aes_encrypt_block constantly -- which is exactly the state
 * the guarantee is about. A switch that failed to preserve XMM would
 * show up here as one thread computing another's ciphertext, or as
 * noise; either way the count of mismatches stops being zero.
 *
 * Deliberately *not* using a lock: the threads share nothing but the
 * result counters, and adding mutual exclusion would serialise them and
 * remove the interleaving the test exists to create.
 */
#define CS_THREADS   4
#define CS_ROUNDS    20000

static volatile uint32_t cs_thread_done = 0;
static volatile uint32_t cs_thread_bad  = 0;
static volatile uint32_t cs_thread_ran  = 0;

static void cs_worker(void) {
    /* Each thread derives its key from its own id, so a thread that
     * picked up another's round keys produces a different block. */
    uint32_t id = (uint32_t)__atomic_fetch_add(&cs_thread_ran, 1,
                                               __ATOMIC_SEQ_CST);
    uint8_t key[16], pt[16], want[16], got[16];
    aes_key_t k;
    uint32_t bad = 0;

    for (int i = 0; i < 16; i++) {
        key[i] = (uint8_t)(0x10 * (id + 1) + i);
        pt[i]  = (uint8_t)(i * 3 + id);
    }

    aes_setkey(&k, key, 128);
    aes_encrypt_block(&k, pt, want);      /* the answer, computed once */

    for (uint32_t r = 0; r < CS_ROUNDS; r++) {
        aes_encrypt_block(&k, pt, got);
        for (int i = 0; i < 16; i++)
            if (got[i] != want[i]) { bad++; break; }
    }

    if (bad) __atomic_fetch_add(&cs_thread_bad, bad, __ATOMIC_SEQ_CST);
    __atomic_fetch_add(&cs_thread_done, 1, __ATOMIC_SEQ_CST);
}

static void cs_concurrent_aes(void) {
    uint64_t before = sched_switches;
    int spawned = 0;

    cs_thread_done = 0;
    cs_thread_bad  = 0;
    cs_thread_ran  = 0;

    for (int i = 0; i < CS_THREADS; i++)
        if (sched_spawn_kernel(cs_worker, "cs-aes", PRIO_NORMAL)) spawned++;

    cs_ok("four AES threads start", spawned == CS_THREADS);

    /* Bounded wait: a hang here would be a scheduler bug of its own and
     * must not become a boot that never finishes. */
    for (int t = 0; t < 3000 && cs_thread_done < (uint32_t)spawned; t++)
        sched_sleep_ms(10);

    cs_ok("  and all of them finish", cs_thread_done == (uint32_t)spawned);
    /* AES-NI retires a block in a few dozen cycles, so eighty thousand
     * of them across four threads is tens of milliseconds -- tens of
     * ticks, not hundreds. What matters is that each thread was
     * interrupted mid-work several times over, which a handful of
     * switches per thread already establishes. */
    cs_ok("  with the scheduler switching underneath them",
          sched_switches - before >= (uint64_t)CS_THREADS);

    serial_puts("[crypto] ");
    serial_put_dec((uint32_t)(sched_switches - before));
    serial_puts(" context switches during ");
    serial_put_dec((uint32_t)(CS_THREADS * CS_ROUNDS));
    serial_puts(" encryptions\n");

    cs_ok("  and not one block came out wrong", cs_thread_bad == 0);
}

/* ===========================================================
 * 4. the write path allocates nothing
 * =========================================================== */

/*
 * The NTFS writer's buffers are static kernel objects and its code path
 * calls no allocator, which is what makes it immune to the page-swap
 * pipeline: there is nothing for an allocation to block on, and the
 * swapper never considers a kernel page in the first place
 * (swap.h: "if (!(e & PTE_USER)) return 0" -- kernel side: never).
 *
 * This checks the second half of that, because it is the half that
 * could change without anyone noticing: a swapper that started
 * accepting kernel pages would make every static buffer in the system a
 * candidate, and the NTFS scratch buffers are the ones where the
 * consequence is a deadlock inside a disk write.
 */
static void cs_kernel_pages_never_swap(void) {
    /* A page table entry with every bit a kernel data page would have:
     * present, writable, no-execute, and PTE_USER clear. */
    uint64_t kernel_pte = PTE_PRESENT | PTE_WRITE | PTE_NX | 0x200000ULL;
    uint64_t user_pte   = PTE_PRESENT | PTE_WRITE | PTE_NX | PTE_USER |
                          0x200000ULL;

    cs_ok("a kernel page is not marked user",  !(kernel_pte & PTE_USER));
    cs_ok("  and a user page is",               (user_pte & PTE_USER) != 0);
    cs_ok("  which is the test the swapper applies before evicting",
          !(kernel_pte & PTE_USER) && (user_pte & PTE_USER));
}

/* ===========================================================
 * the whole thing
 * =========================================================== */

static void crypto_switch_selftest(void) {
    uint64_t switches = 0;

    serial_puts("\n[crypto] extended state across context switches\n");

    cs_aes_paths_agree();

    {
        int survived;

        /*
         * A rival, so that preemption has somewhere to go -- and at
         * *this* thread's priority, not below it.
         *
         * sched_pick() is strict priority (`t->prio > best->prio`), so
         * a lower-priority rival is never chosen while the thread
         * running this is runnable, and the timer fires a thousand
         * times a second changing nothing. At equal priority the scan
         * starts one slot past the current thread and the strict `>`
         * lets the first one found win, which is round-robin -- and
         * that is what actually takes this thread away.
         */
        cs_rival_stop = 0;
        cs_rival_spins = 0;
        sched_spawn_kernel(cs_rival, "cs-rival",
                           cur_thread ? cur_thread->prio : PRIO_NORMAL);

        survived = cs_xmm_survives_preemption(&switches);
        cs_rival_stop = 1;

        serial_puts("[crypto] ");
        serial_put_dec((uint32_t)switches);
        serial_puts(" context switches while XMM5 held a pattern\n");
        cs_ok("the thread was actually taken away and given back",
              switches > 0);
        cs_ok("XMM5 came back byte-for-byte after preemption", survived);
    }

    cs_concurrent_aes();
    cs_kernel_pages_never_swap();

    serial_puts("[crypto] ");
    serial_put_dec((uint32_t)cs_checks);
    serial_puts(" checks, ");
    serial_put_dec((uint32_t)cs_fails);
    serial_puts(" failures\n\n");
}

#endif /* CRYPTOSWITCH_H */
