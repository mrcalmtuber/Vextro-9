#ifndef VEXTRO_SCHED_H
#define VEXTRO_SCHED_H

/*
 * src/sched/sched.h — what a thread is, and what the rest of the kernel
 * may ask the scheduler to do.
 *
 * The implementation is src/sched/scheduler.c, compiled as its own
 * object. This file carries the types (because callers hold thread_t
 * pointers and read their fields), the three inline operations that are
 * too hot to become calls, and prototypes for the rest.
 *
 * There is no `struct pcb` here and no process table. The unit of
 * scheduling in this kernel is thread_t and the table is `threads` — a
 * thread carries an address space rather than being owned by one, which
 * is what makes a kernel thread and a ring-3 program the same kind of
 * object to the switch code.
 */

#include <stdint.h>
#include "kernel_shared.h"

#define SCHED_MAX_THREADS 64
#define SCHED_NAME_LEN    24
#define SCHED_KSTACK      (32 * 1024)

/* One ready queue per processor. The bound matches SMP_MAX_CPUS in
 * src/smp.h, which is where processors are actually started; the two are
 * separate constants because this file must compile for the host test
 * suite, where there is no smp.h and no APIC. */
#define SCHED_MAX_CPUS    16

/* Higher runs first. The compositor sits above applications so that a
 * program in a tight loop can never cost the interface its frame. */
#define PRIO_IDLE    0
#define PRIO_NORMAL  4
#define PRIO_UI      8
#define PRIO_MAX     15

typedef enum {
    T_FREE = 0,
    T_READY,
    T_RUNNING,
    T_SLEEPING,
    T_ZOMBIE
} thread_state_t;

/*
 * The frame the timer stub builds. Field order is the push order
 * reversed, and the last five are the processor's own — which is why
 * `rsp` and `ss` are in here at all: in long mode IRET pops them
 * unconditionally, even when the privilege level is not changing.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, cs, rflags, rsp, ss;
} trap_frame_t;

typedef struct thread {
    uint64_t       rsp;                 /* saved kernel stack pointer     */
    uint64_t       cr3;                 /* address space, physical        */
    uint64_t       kstack_top;          /* what goes in TSS.RSP0          */
    void          *kstack;              /* the allocation, for release    */
    addr_space_t  *as;                  /* null for a kernel thread       */
    uint32_t       pid;
    uint32_t       prio;
    thread_state_t state;
    uint64_t       wake_at;             /* scheduler ticks, for sleepers  */
    uint64_t       slices;              /* how much processor it has had  */
    int            user;                /* runs in ring 3                 */
    int            waits_frame;         /* asleep until the frame clock   */
    /* What this thread is blocked on, or null. Any address will do as a
     * channel -- a semaphore's own address is the usual one -- because
     * nothing dereferences it; it is only ever compared. See
     * sched_block_on. */
    void          *wait_chan;
    int            exit_code;
    /*
     * Which processor's ready queue this thread belongs to.
     *
     * Chosen once, when the thread is created, by whichever queue is
     * shortest -- except for the interface, which is pinned to processor
     * zero because that is the one the frame clock interrupts and the
     * one the compositor's shared state lives on.
     *
     * A home is a preference and not a confinement: sched_pick looks
     * through its own queue first and then through every other one, so a
     * thread whose home processor is not dispatching still runs. That
     * fallback is what makes it safe to assign homes today, when exactly
     * one processor dispatches threads at all.
     */
    uint32_t       cpu;
    /*
     * ---- where this thread's own variables are ----
     *
     * The base FS is given when the thread reads a `__thread` variable.
     * Zero on every thread that has never asked for one, which is every
     * thread this system had before pthreads: a program with one thread
     * keeps its globals in .data and never touches FS at all.
     *
     * It has to be per-thread and restored on every switch, which is the
     * whole difference between this and sched_set_gs_base. GS holds a
     * Windows program's Thread Environment Block, and that is written
     * once because the TEB is at the same address in every process — one
     * value serves them all. Thread-local storage is the opposite: the
     * value *is* the identity of the thread, so two threads that shared
     * it would share every variable it exists to keep apart.
     */
    uint64_t       fsbase;
    char           name[SCHED_NAME_LEN];
    /* 512 bytes, sixteen-aligned, exactly as FXSAVE64 wants them. The
     * attribute is on the member rather than the struct because the
     * struct is heap-allocated and the heap's own alignment is a
     * separate promise. */
    uint8_t        fx[512] __attribute__((aligned(16)));
} thread_t;
/* ===== THE THREAD TABLE =====
 *
 * Exported rather than wrapped: the desktop walks it to list what is
 * running, and the process panel reads several fields per entry. An
 * accessor per field would be six functions saying nothing.
 */
extern thread_t  *threads[SCHED_MAX_THREADS];
extern thread_t  *cur_thread;
extern int        sched_running;

/* Written by the timer interrupt, read from ordinary code — so both are
 * volatile, and the extern must say so as loudly as the definition.
 * A mismatch here folds the difference between two samples to zero,
 * which is what once made a context-switch selftest pass by measuring
 * nothing at all. */
extern volatile uint64_t sched_switches;
extern volatile int32_t  preempt_count;
extern volatile int      sched_soft_yield;

/* ===== THE PREEMPTION COUNT =====
 *
 * The compositor mutates the window list, the terminal ring and the
 * notification queue, and a syscall from another thread reaches the
 * same structures. Rather than lock each one, the frame is a critical
 * section: raised while drawing, dropped before sleeping.
 *
 * Inline because it brackets every frame and every short critical
 * region in the kernel; a call per bracket would be pure overhead for
 * an atomic increment.
 */
static inline void preempt_disable(void) {
    __atomic_add_fetch(&preempt_count, 1, __ATOMIC_SEQ_CST);
}
static inline void preempt_enable(void) {
    __atomic_sub_fetch(&preempt_count, 1, __ATOMIC_SEQ_CST);
}

/*
 * Give up the rest of this slice.
 *
 * Software-generates the timer vector, so that there is exactly one
 * piece of code in this kernel that knows how to change threads. Inline
 * because the INT has to be the instruction that immediately follows
 * setting the flag — with a call in between, the timer could arrive
 * first and take the switch down the interrupt path with sched_soft_yield
 * already set, acknowledging an interrupt the APIC never delivered.
 */
static inline void sched_yield(void) {
    uint64_t flags = irq_save();
    sched_soft_yield = 1;
    __asm__ volatile("int %0" :: "i"(APIC_VEC_TIMER) : "memory");
    irq_restore(flags);
}

/* ===== STARTING AND STOPPING ===== */

void      sched_init(void);
void      sched_start(void);
thread_t *sched_spawn_kernel(void (*fn)(void), const char *name,
                             uint32_t prio);
thread_t *sched_spawn_kernel_arg(void (*fn)(void *), void *arg,
                                 const char *name, uint32_t prio);
thread_t *sched_spawn_user(addr_space_t *as, uint64_t entry,
                           uint64_t user_stack, const char *name,
                           uint32_t prio);
thread_t *sched_fork_thread(thread_t *parent, addr_space_t *child_as);

/*
 * A second thread in an address space that already has one.
 *
 * The difference from sched_spawn_user is the argument and the pinning,
 * and both matter. The argument, because a thread function takes one and
 * there is nowhere else to put it: a new thread is started by IRETQ-ing
 * into a frame, so the argument goes into the frame's RDI and arrives in
 * the first parameter register exactly as the ABI says it should.
 *
 * The pinning, because src/syscall.h keeps the kernel stack pointer for
 * the next entry from user mode in one global word. Two ring-3 threads
 * entering SYSCALL on two processors at the same instant would both load
 * it, and the second would build its frame on the first one's stack. One
 * processor runs user threads, so that cannot happen; this is what makes
 * it stay true now that a process can have more than one.
 */
thread_t *sched_spawn_thread(addr_space_t *as, uint64_t entry,
                             uint64_t user_stack, uint64_t arg,
                             uint64_t fsbase, const char *name);
void      sched_exit(int code);
int       sched_join(thread_t *t, uint32_t timeout_ms);
void      sched_reap(void);

/* Called for each thread as it is reaped, or null. The desktop installs
 * its own here so the scheduler need not know what an application is —
 * it is the one place a higher layer reaches into the switch code, and
 * a function pointer keeps the dependency pointing the right way. */
extern void (*sched_reap_hook)(thread_t *t);
int       sched_thread_count(void);

/* ===== THE PER-CORE READY QUEUES =====
 *
 * How many processors the balancer may spread work across. Set once by
 * the composition root after the application processors have answered;
 * one until then, which is what makes every decision below correct on a
 * machine that never starts a second one.
 */
void sched_set_cpu_count(int n);
int  sched_cpu_count(void);
int  sched_queue_depth(int cpu);

/* ===== SLEEPING AND WAKING =====
 *
 * A channel is any address: nothing dereferences it, it is only ever
 * compared, so a semaphore's own address is the usual choice.
 */
void sched_sleep_ms(uint64_t ms);
int  sched_block_on(void *chan, uint32_t timeout_ms);
int  sched_block_on_locked(void *chan, uint32_t timeout_ms, uint64_t flags);
void sched_wake_chan(void *chan, int all);

/* The frame clock, which the compositor pulses and UI threads wait on. */
void sched_wait_frame(void);
void sched_frame_pulse(void);

/* Written once, the first time a PE is spawned: a Windows program reads
 * its Thread Environment Block through GS, and the TEB is at the same
 * virtual address in every process, so one value serves them all. */
void sched_set_gs_base(uint64_t va);

/* And the one that is genuinely per-thread. Takes effect on this thread
 * immediately and on every later switch to it. */
void sched_set_fsbase(uint64_t va);

#endif /* VEXTRO_SCHED_H */
