#ifndef SCHED_H
#define SCHED_H

/*
 * src/sched.h — threads, and the interrupt that switches between them.
 *
 * What this replaces is not a scheduler. An application used to be run
 * by pointing the stack pointer at a static buffer and issuing a CALL;
 * the desktop then stopped existing until the program returned. A
 * Mandelbrot set that takes four seconds to draw froze the pointer, the
 * clock and the compositor for four seconds, and there was no way to
 * write a program that did not do that.
 *
 * Now a program is a thread with its own address space, its own kernel
 * stack, and its own copy of the floating-point state, and the APIC
 * timer takes the processor away from it a thousand times a second.
 *
 * ---- how a switch actually happens ----
 *
 * The timer stub pushes every general-purpose register onto the stack of
 * whichever thread was interrupted, on top of the five words the
 * processor pushed. That block *is* the thread's saved state; the only
 * thing that has to be remembered separately is where it starts, so a
 * context switch is: write the current stack pointer into the current
 * thread, pick another thread, load its stack pointer, and return. The
 * IRETQ at the end of the stub then unwinds into whatever that other
 * thread was doing — user or kernel, the frame says which.
 *
 * The extended state does not fit in that trick, because FXSAVE writes
 * 512 bytes to a fixed address rather than pushing. So each thread
 * carries its own aligned area and the switch copies through it. This is
 * what makes floating point safe to use everywhere: two threads can both
 * be halfway through an expression involving XMM registers, and neither
 * can see the other's.
 *
 * ---- what is deliberately not preemptible ----
 *
 * The compositor mutates a great deal of shared state — the window list,
 * the terminal ring, the notification queue — and a syscall from another
 * thread can reach the same structures. Rather than lock each of them,
 * the frame is a critical section: the render loop raises the preemption
 * count while it draws and drops it before it sleeps. Applications get
 * the rest of the frame, which on this machine is most of it, in
 * millisecond slices.
 */

#include <stdint.h>
#include "gdt.h"
#include "vmm.h"
#include "kheap.h"
#include "apic.h"
#include "syscall.h"

#define SCHED_MAX_THREADS 64
#define SCHED_NAME_LEN    24
#define SCHED_KSTACK      (32 * 1024)

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
    int            exit_code;
    char           name[SCHED_NAME_LEN];
    /* 512 bytes, sixteen-aligned, exactly as FXSAVE64 wants them. The
     * attribute is on the member rather than the struct because the
     * struct is heap-allocated and the heap's own alignment is a
     * separate promise. */
    uint8_t        fx[512] __attribute__((aligned(16)));
} thread_t;

static thread_t  *threads[SCHED_MAX_THREADS];
static thread_t  *cur_thread   = 0;
static thread_t  *idle_thread  = 0;
static uint32_t   sched_next_pid = 1;
static volatile int32_t preempt_count = 0;
static volatile uint64_t sched_ticks = 0;
static uint64_t   sched_switches = 0;
static int        sched_running = 0;
static uint8_t    fx_template[512] __attribute__((aligned(16)));

/*
 * Raised for the length of a software-requested switch.
 *
 * A thread that gives up the processor does it by raising the timer
 * vector, so that there is exactly one piece of code in this kernel that
 * knows how to change threads. But a vector raised by an INSTRUCTION is
 * not an interrupt the APIC delivered: acknowledging it would dismiss
 * whatever genuinely is in service, and counting it would make the tick
 * count — which sleepers are timed against — depend on how often
 * programs happened to yield.
 */
static volatile int sched_soft_yield = 0;

static inline void preempt_disable(void) {
    __atomic_add_fetch(&preempt_count, 1, __ATOMIC_SEQ_CST);
}
static inline void preempt_enable(void) {
    __atomic_sub_fetch(&preempt_count, 1, __ATOMIC_SEQ_CST);
}

static int sched_slot(thread_t *t) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (threads[i] == t) return i;
    return -1;
}

/*
 * Pick.
 *
 * Highest priority wins; among equals it is round robin, which falls out
 * of starting the search one past whoever just ran rather than at zero.
 * Sleepers whose deadline has passed are woken on the way through, since
 * this is the one place that walks every thread anyway.
 */
__attribute__((target("general-regs-only")))
static thread_t *sched_pick(void) {
    int start = cur_thread ? sched_slot(cur_thread) : -1;
    thread_t *best = 0;
    int best_i = -1;

    for (int n = 1; n <= SCHED_MAX_THREADS; n++) {
        int i = (start + n) % SCHED_MAX_THREADS;
        thread_t *t = threads[i];
        if (!t) continue;
        if (t->state == T_SLEEPING && sched_ticks >= t->wake_at)
            t->state = T_READY;
        if (t->state != T_READY && t->state != T_RUNNING) continue;
        if (!best || t->prio > best->prio) { best = t; best_i = i; }
    }
    (void)best_i;
    return best;
}

/*
 * The tick.
 *
 * Returns the stack pointer the stub should resume on: the one it was
 * handed if nothing is switching, the next thread's otherwise. Compiled
 * without vector registers on purpose — the extended state is being
 * moved through here, and a compiler that decided to stage a struct copy
 * through XMM would be writing into the very registers this is trying to
 * preserve.
 */
__attribute__((used, target("general-regs-only")))
uint64_t sched_on_tick(uint64_t rsp);

__attribute__((target("general-regs-only")))
uint64_t sched_on_tick(uint64_t rsp) {
    int soft = sched_soft_yield;
    sched_soft_yield = 0;
    if (!soft) {
        sched_ticks++;
        lapic_eoi();
    }

    if (!sched_running || !cur_thread) return rsp;
    /* A yield is a request, not a suggestion: honouring the preemption
     * count here would leave a thread that asked to step aside spinning
     * on a switch that never happens. */
    if (preempt_count > 0 && !soft) return rsp;

    thread_t *next = sched_pick();
    if (!next || next == cur_thread) {
        if (cur_thread->state == T_READY) cur_thread->state = T_RUNNING;
        return rsp;
    }

    cur_thread->rsp = rsp;
    __asm__ volatile("fxsave64 %0" : "=m"(*cur_thread->fx) :: "memory");
    if (cur_thread->state == T_RUNNING) cur_thread->state = T_READY;

    cur_thread = next;
    next->state = T_RUNNING;
    next->slices++;
    sched_switches++;

    tss.rsp0       = next->kstack_top;
    syscall_kstack = next->kstack_top;
    vmm_current    = next->as;

    uint64_t want = next->cr3;
    uint64_t have;
    __asm__ volatile("mov %%cr3, %0" : "=r"(have));
    if ((have & PTE_ADDR_MASK) != want)
        __asm__ volatile("mov %0, %%cr3" :: "r"(want) : "memory");

    __asm__ volatile("fxrstor64 %0" :: "m"(*next->fx) : "memory");
    return next->rsp;
}

/*
 * The timer entry. Raw assembly rather than __attribute__((interrupt))
 * because the whole point is to change which stack IRETQ unwinds from,
 * and a compiler-generated prologue leaves no way to say so.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl sched_timer_stub\n"
    ".type sched_timer_stub, @function\n"
    "sched_timer_stub:\n"
    "  pushq %rax\n"
    "  pushq %rbx\n"
    "  pushq %rcx\n"
    "  pushq %rdx\n"
    "  pushq %rsi\n"
    "  pushq %rdi\n"
    "  pushq %rbp\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  pushq %r10\n"
    "  pushq %r11\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  cld\n"
    "  movq %rsp, %rdi\n"
    /* An interrupt taken inside ring 0 does not realign the stack the way
     * one that changes privilege does, so where RSP lands here depends on
     * the code that was interrupted. Align it for the call by hand; the
     * result comes back in RAX and is loaded straight into RSP, so nothing
     * needs restoring afterwards. */
    "  andq $-16, %rsp\n"
    "  call sched_on_tick\n"
    "  movq %rax, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %r11\n"
    "  popq %r10\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rbp\n"
    "  popq %rdi\n"
    "  popq %rsi\n"
    "  popq %rdx\n"
    "  popq %rcx\n"
    "  popq %rbx\n"
    "  popq %rax\n"
    "  iretq\n"
    ".popsection\n"
);
extern void sched_timer_stub(void);

/* The spurious vector. The APIC raises it when an interrupt is
 * withdrawn between being raised and being accepted, and it is the one
 * vector that must *not* be acknowledged. */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl apic_spurious_stub\n"
    ".type apic_spurious_stub, @function\n"
    "apic_spurious_stub:\n"
    "  iretq\n"
    ".popsection\n"
);
extern void apic_spurious_stub(void);

static void sched_name(thread_t *t, const char *n) {
    int i = 0;
    for (; n && n[i] && i < SCHED_NAME_LEN - 1; i++) t->name[i] = n[i];
    t->name[i] = '\0';
}

static int sched_register(thread_t *t) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        if (!threads[i]) { threads[i] = t; return i; }
    }
    return -1;
}

/*
 * Adopt the running context as thread zero.
 *
 * There is no way to create the first thread — it is already executing.
 * So it is described rather than built: a control block with no saved
 * state, because the state is live in the registers, and it will be
 * written down the first time the timer takes the processor away.
 */
static void sched_init(void) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) threads[i] = 0;

    /* A known-good starting FPU state, taken once from a unit that has
     * just been reset, and stamped into every thread created afterwards.
     * A zeroed area is not the same thing: it sets the control word to
     * unmasked exceptions and MXCSR to a reserved encoding, and FXRSTOR
     * refuses the latter with #GP. */
    __asm__ volatile("fninit");
    __asm__ volatile("fxsave64 %0" : "=m"(*fx_template) :: "memory");

    thread_t *t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) { serial_puts("[sched] no memory for the first thread\n"); return; }
    for (uint64_t i = 0; i < sizeof(thread_t); i++) ((uint8_t *)t)[i] = 0;

    t->pid        = sched_next_pid++;
    t->prio       = PRIO_UI;
    t->state      = T_RUNNING;
    t->user       = 0;
    t->as         = 0;
    t->kstack     = 0;
    t->kstack_top = tss.rsp0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->cr3));
    t->cr3 &= PTE_ADDR_MASK;
    for (int i = 0; i < 512; i++) t->fx[i] = fx_template[i];
    sched_name(t, "compositor");

    sched_register(t);
    cur_thread = t;

    idt_set_gate_ex(APIC_VEC_TIMER, (void *)(uintptr_t)sched_timer_stub,
                    GDT_KCODE, 0, 0);
    idt_set_gate_ex(APIC_VEC_SPURIOUS, (void *)(uintptr_t)apic_spurious_stub,
                    GDT_KCODE, 0, 0);

    serial_puts("[sched] thread 1 is the compositor\n");
}

/*
 * Somewhere for the processor to be when there is nothing to do.
 *
 * Without it, a scheduler pass that finds every thread asleep has to
 * leave the sleeping thread running, which works only because that
 * thread happened to be sitting in a HLT. An idle thread makes the
 * answer to "what runs now" total, and gives the halt an owner.
 */
static void sched_idle_loop(void) {
    for (;;) __asm__ volatile("hlt");
}

static thread_t *sched_spawn_kernel(void (*fn)(void), const char *name,
                                    uint32_t prio);

/*
 * Start switching — but only if there is something to switch on.
 *
 * Every blocking point in this scheduler works by marking a thread as
 * not-runnable and waiting for a timer to put it back. On a machine with
 * no local APIC there is no such timer, so the compositor would stand
 * down for the first frame and nothing would ever wake it: a desktop
 * that draws one frame and stops. Better to keep the whole thing dormant
 * and let the render loop run exactly as it did before threads existed.
 */
static void sched_start(void) {
    if (!lapic_present) {
        serial_puts("[sched] no APIC timer: staying single-threaded\n");
        return;
    }
    idle_thread = sched_spawn_kernel(sched_idle_loop, "idle", PRIO_IDLE);
    if (!idle_thread) {
        serial_puts("[sched] no memory for the idle thread\n");
        return;
    }
    sched_running = 1;
}

/*
 * Lay a starting frame on a fresh kernel stack.
 *
 * A new thread has never been interrupted, so it has no saved state to
 * resume — which is solved by writing the state it *would* have had. The
 * frame put here is exactly what the timer stub expects to find, so the
 * first switch into this thread returns from an interrupt that never
 * happened, into its entry point.
 */
static uint64_t sched_build_frame(thread_t *t, uint64_t entry,
                                  uint64_t stack_ptr, int user) {
    uint64_t top = (uint64_t)(uintptr_t)t->kstack + SCHED_KSTACK;
    top &= ~15ULL;
    trap_frame_t *f = (trap_frame_t *)(uintptr_t)(top - sizeof(trap_frame_t));

    for (uint64_t i = 0; i < sizeof(trap_frame_t) / 8; i++)
        ((uint64_t *)f)[i] = 0;

    f->rip    = entry;
    f->cs     = user ? SEL_UCODE : GDT_KCODE;
    f->ss     = user ? SEL_UDATA : GDT_KDATA;
    f->rsp    = stack_ptr;
    /* Bit 1 is reserved and must be set; 0x200 is IF, without which the
     * thread would run with interrupts off and never be preempted. */
    f->rflags = 0x202;
    return (uint64_t)(uintptr_t)f;
}

static thread_t *sched_new(const char *name, uint32_t prio) {
    thread_t *t = (thread_t *)kmalloc(sizeof(thread_t));
    if (!t) return 0;
    for (uint64_t i = 0; i < sizeof(thread_t); i++) ((uint8_t *)t)[i] = 0;

    /* Not from the heap: the heap lives in the direct map, where every
     * page of RAM is mapped and a hole cannot be made. A stack there has
     * nothing below it but whatever the allocator handed out next, so an
     * overflow corrupts silently. kstack_alloc puts an unmapped page
     * under each one. */
    t->kstack = kstack_alloc(SCHED_KSTACK);
    if (!t->kstack) { kfree(t); return 0; }

    t->pid        = sched_next_pid++;
    t->prio       = prio;
    t->state      = T_READY;
    t->kstack_top = ((uint64_t)(uintptr_t)t->kstack + SCHED_KSTACK) & ~15ULL;
    for (int i = 0; i < 512; i++) t->fx[i] = fx_template[i];
    sched_name(t, name);
    return t;
}

/* A thread that runs in the kernel's own address space. */
static thread_t *sched_spawn_kernel(void (*fn)(void), const char *name,
                                    uint32_t prio) {
    thread_t *t = sched_new(name, prio);
    if (!t) return 0;
    t->user = 0;
    t->as   = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->cr3));
    t->cr3 &= PTE_ADDR_MASK;
    /* Its stack pointer at entry is whatever is left below the frame. */
    uint64_t frame = sched_build_frame(t, (uint64_t)(uintptr_t)fn, 0, 0);
    ((trap_frame_t *)(uintptr_t)frame)->rsp = frame;
    t->rsp = frame;

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/* A thread that runs in ring 3, in an address space of its own. */
static thread_t *sched_spawn_user(addr_space_t *as, uint64_t entry,
                                  uint64_t user_stack, const char *name,
                                  uint32_t prio) {
    thread_t *t = sched_new(name, prio);
    if (!t) return 0;
    t->user = 1;
    t->as   = as;
    t->cr3  = as->pml4_phys;
    t->rsp  = sched_build_frame(t, entry, user_stack, 1);

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/*
 * A thread that resumes where its parent is, in a different address
 * space.
 *
 * The child's kernel stack is given a trap frame built out of the
 * parent's syscall frame: same instruction pointer, same flags, same
 * user stack pointer, every general-purpose register the same — and RAX
 * zero, which is the only way the two of them can tell each other apart
 * on the way back out.
 *
 * Only from the SYSCALL door. The legacy gate does not put the return
 * address in RCX, so there would be nothing to build a frame from.
 */
static thread_t *sched_fork_thread(thread_t *parent, addr_space_t *child_as) {
    if (!parent || !child_as || !syscall_cur_frame || !syscall_via_fast)
        return 0;

    thread_t *t = sched_new(parent->name, parent->prio);
    if (!t) return 0;
    t->user = 1;
    t->as   = child_as;
    t->cr3  = child_as->pml4_phys;

    uint64_t top = ((uint64_t)(uintptr_t)t->kstack + SCHED_KSTACK) & ~15ULL;
    trap_frame_t *f = (trap_frame_t *)(uintptr_t)(top - sizeof(trap_frame_t));
    const syscall_frame_t *p = syscall_cur_frame;

    f->r15 = p->r15; f->r14 = p->r14; f->r13 = p->r13; f->r12 = p->r12;
    f->r11 = p->r11; f->r10 = p->r10; f->r9  = p->r9;  f->r8  = p->r8;
    f->rbp = p->rbp; f->rdi = p->rdi; f->rsi = p->rsi; f->rdx = p->rdx;
    f->rcx = p->rcx; f->rbx = p->rbx;
    f->rax = 0;                       /* this is the child */

    f->rip    = p->rcx;               /* SYSCALL left it there */
    f->cs     = SEL_UCODE;
    f->ss     = SEL_UDATA;
    f->rsp    = p->user_rsp;
    f->rflags = (p->r11 | 0x202ULL) & ~0x8ULL;   /* IF on, TF off */

    /* The parent's own extended state, so the child starts with the same
     * floating-point registers rather than a freshly reset unit. */
    for (int i = 0; i < 512; i++) t->fx[i] = parent->fx[i];

    t->rsp = (uint64_t)(uintptr_t)f;

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/* Give up the rest of this slice. Software-generates the timer vector,
 * which means the one switch path is the only switch path. */
static inline void sched_yield(void) {
    uint64_t flags = irq_save();
    sched_soft_yield = 1;
    __asm__ volatile("int %0" :: "i"(APIC_VEC_TIMER) : "memory");
    irq_restore(flags);
}

static void sched_sleep_ms(uint64_t ms) {
    if (!cur_thread || !sched_running) return;
    cur_thread->wake_at = sched_ticks + (ms ? ms : 1);
    cur_thread->state   = T_SLEEPING;
    while (cur_thread->state == T_SLEEPING) sched_yield();
}

/*
 * Sleep until the frame clock says there is something to draw.
 *
 * This is the compositor's one blocking point and the reason
 * applications get any processor at all. Strict priority means a thread
 * that is merely *runnable* at PRIO_UI keeps everything below it off the
 * machine forever — the old render loop's HLT looked like idling but the
 * scheduler could not tell it apart from work. Saying so explicitly is
 * the difference between a desktop that shares the machine and one that
 * only appears to.
 *
 * The wake comes from the PIT handler at 60 Hz, not from a deadline in
 * scheduler ticks, so the interface stays locked to the display's clock
 * however the scheduler is tuned.
 */
static void sched_wait_frame(void) {
    if (!sched_running || !cur_thread) {
        __asm__ volatile("hlt");
        return;
    }
    cur_thread->waits_frame = 1;
    cur_thread->wake_at     = ~(uint64_t)0;
    cur_thread->state       = T_SLEEPING;
    while (cur_thread->state == T_SLEEPING) sched_yield();
}

/* Called from the 60 Hz timer. Anything waiting on a frame is now
 * runnable; the next scheduler tick, at most a millisecond away, will
 * pick the highest-priority one of them. */
__attribute__((target("general-regs-only")))
static void sched_frame_pulse(void) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (t && t->waits_frame) {
            t->waits_frame = 0;
            if (t->state == T_SLEEPING) t->state = T_READY;
        }
    }
}

/*
 * A thread ends here. It cannot free its own stack — it is standing on
 * it — so it becomes a zombie and the compositor collects it.
 */
static void sched_exit(int code) {
    if (!cur_thread) return;
    cur_thread->exit_code = code;
    cur_thread->state = T_ZOMBIE;
    for (;;) sched_yield();
}

/*
 * Collect what has finished. Called from the compositor thread, which is
 * by construction not any of the threads being freed.
 */
static void (*sched_reap_hook)(thread_t *t) = 0;

static void sched_reap(void) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state != T_ZOMBIE) continue;
        if (t == cur_thread) continue;

        if (sched_reap_hook) sched_reap_hook(t);

        uint64_t flags = irq_save();
        threads[i] = 0;
        irq_restore(flags);

        if (t->as) { vmm_destroy(t->as); kfree(t->as); }
        if (t->kstack) kstack_free(t->kstack, SCHED_KSTACK);
        serial_puts("[sched] reaped ");
        serial_puts(t->name);
        serial_puts("\n");
        kfree(t);
    }
}

/*
 * Wait for one thread to finish, from a context that is allowed to
 * block. Used at boot, where the compositor loop does not exist yet and
 * a self-test still wants an answer before it prints one.
 *
 * Halting would not do. The waiter is the compositor, at PRIO_UI, and
 * the thread it is waiting for is below it — so as long as the waiter is
 * merely runnable, the thread it wants to finish never starts. It has to
 * stand down, and for longer than one tick: a sleeper due to wake on the
 * very next tick is woken by the same scheduler pass that would have
 * switched away from it, and the two spin against each other.
 */
static int sched_join(thread_t *t, uint32_t timeout_ms) {
    if (!t) return -1;
    /* Without a scheduler clock the deadline below never advances, so a
     * wait would be forever rather than bounded. */
    if (!sched_running) return -1;
    uint64_t deadline = sched_ticks + timeout_ms;
    __asm__ volatile("sti" ::: "memory");
    while (t->state != T_ZOMBIE && t->state != T_FREE) {
        if (sched_ticks > deadline) return -1;
        if (cur_thread && sched_running) {
            cur_thread->wake_at = sched_ticks + 2;
            cur_thread->state   = T_SLEEPING;
            while (cur_thread->state == T_SLEEPING) sched_yield();
        } else {
            __asm__ volatile("hlt");
        }
    }
    return t->exit_code;
}

static int sched_thread_count(void) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (threads[i] && threads[i]->state != T_FREE) n++;
    return n;
}

#endif /* SCHED_H */
