/*
 * src/sched/scheduler.c — threads, and the interrupt that switches
 * between them.
 *
 * This is the kernel's second-largest object and the one whose split
 * from the composition root took the most care, because almost
 * everything in the system calls into it: twenty-four symbols leave
 * here and seventy-one files use at least one of them.
 *
 * What it needs *back* is only fifteen — the descriptor table, the page
 * tables, the interrupt controller and the heap — and those are
 * declared in include/kernel_shared.h. That ratio is the argument for
 * the boundary: a great deal depends on the scheduler and the scheduler
 * depends on very little.
 *
 * Three things stayed in the header rather than moving here, and the
 * reason in every case is that they are on a hot path and inlining them
 * is load-bearing rather than an optimisation:
 *
 *   preempt_disable / preempt_enable   the compositor brackets every
 *                                      frame with these
 *   sched_yield                        raises the timer vector, and the
 *                                      INT must be immediate
 *
 * lapic_eoi moved to the seam header for the same reason and is
 * still always_inline and general-regs-only: it is called from the
 * timer stub below, inside the window where the extended state is
 * half-saved, and a cross-object call there would be a new instruction
 * sequence in the most delicate function in this kernel.
 */

#include <stdint.h>

/* The four subsystems a context switch touches. Note what is not here:
 * src/gdt.h, src/vmm.h, src/apic.h and src/kheap.h themselves. Their
 * bodies hold the descriptor table, the page-table roots, the APIC base
 * pointer and the heap free lists; including any of them would give
 * this object a private copy of that state, and a scheduler writing its
 * own TSS while the processor reads another is a machine that takes one
 * timer interrupt and stops. */
#include "kernel_shared.h"
#include "sched/sched.h"

thread_t  *threads[SCHED_MAX_THREADS];
thread_t  *cur_thread   = 0;
static thread_t  *idle_thread  = 0;
static uint32_t   sched_next_pid = 1;
volatile int32_t preempt_count = 0;
volatile uint64_t sched_ticks = 0;
/*
 * Volatile for the same reason sched_ticks above is: this is written
 * inside sched_on_tick, which reaches normal code only through an
 * interrupt the compiler cannot see. Without it, a caller that samples
 * the counter, does some work and samples again has both loads folded
 * into one and measures a difference of exactly zero -- which is what
 * happened to the context-switch selftest in cryptoswitch.h, and would
 * happen to anything else that ever tried to measure switch rate.
 */
volatile uint64_t sched_switches = 0;
int        sched_running = 0;
static uint8_t    fx_template[512] __attribute__((aligned(16)));

/*
 * What IA32_GS_BASE holds, and why it is not per-thread.
 *
 * A Windows program reads its Thread Environment Block through GS, so
 * something has to put an address there. The obvious design is a field
 * in thread_t written by the context switch alongside CR3 -- and that
 * was tried, and it is wrong twice over.
 *
 * Wrong because it is unnecessary: the TEB sits at the same virtual
 * address in every process (WIN_TEB_VA is a constant), and GS_BASE is a
 * *linear* address resolved through whatever CR3 is current. One value
 * therefore serves every process, each seeing its own page. There is
 * nothing to switch.
 *
 * And wrong because sched_on_tick is the most delicate function in this
 * kernel -- hand-tuned, compiled general-regs-only, moving extended
 * state through registers the compiler has been told not to touch.
 * Adding a WRMSR to it produced a #GP on the FXRSTOR two instructions
 * later. Whatever the precise mechanism, a serialising instruction in
 * the middle of that sequence is not worth the risk for a value that
 * never changes.
 *
 * So it is written once, the first time a PE is spawned, and left.
 */
static uint64_t   gs_base_live = 0;

void sched_set_gs_base(uint64_t va) {
    if (gs_base_live == va) return;
    gs_base_live = va;
    wrmsr(0xC0000101u, va);
}

/*
 * IA32_FS_BASE, which is per-thread and therefore cannot take the way
 * out that GS did.
 *
 * Two words rather than one, and they mean different things. `want` is
 * what the thread about to run needs, published by sched_on_tick as an
 * ordinary store. `live` is what the machine actually has loaded. The
 * stub compares them and issues WRMSR only when they differ, which on a
 * system with no thread-local storage anywhere is never: every thread
 * that predates pthreads has a base of zero, both words stay zero, and
 * the MSR is not written once.
 *
 * `live` tracks the processor and not the thread, which is what makes
 * clearing work. A switch from a thread with a base to one without moves
 * `want` to zero, the comparison fails, and the base is zeroed — rather
 * than left loaded for a thread that has no TCB and would find somebody
 * else's if it looked.
 *
 * Not static: the stub names them, and a name the assembler resolves has
 * to survive into the object file.
 */
uint64_t sched_fsbase_want = 0;
uint64_t sched_fsbase_live = 0;

/* Install one immediately, for the thread that is running now. The
 * switch would get to it on the next tick anyway; doing it here is what
 * lets a thread set its base and read a __thread variable on the very
 * next instruction, which is what the C ABI expects. */
void sched_set_fsbase(uint64_t va) {
    if (cur_thread) cur_thread->fsbase = va;
    sched_fsbase_want = va;
    sched_fsbase_live = va;
    wrmsr(0xC0000100u, va);
}

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
volatile int sched_soft_yield = 0;


static int sched_slot(thread_t *t) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (threads[i] == t) return i;
    return -1;
}

/* ===== THE PER-CORE READY QUEUES =====
 *
 * A queue here is a depth and a home: `sched_rq[c].n` is how many
 * threads call processor c home, and thread_t.cpu is which one each of
 * them chose. The membership is only ever changed when a thread is
 * created or reaped, which is rare and never on an interrupt path — so
 * it is maintained under the queue's own lock, and read by the picker
 * without one.
 *
 * That asymmetry is deliberate and it is worth being exact about why it
 * is sound, because "takes a lock to write and not to read" is usually a
 * bug. Exactly one processor dispatches threads in this kernel, and it
 * does so from inside a timer interrupt with interrupts disabled. So
 * while the picker is running, nothing else on that processor can be
 * halfway through a mutation, and no other processor mutates at all --
 * the application processors run kernel workers and never touch a
 * thread_t. The lock is there for the writers to serialise against each
 * other the day an application processor does dispatch, and taking it in
 * the picker would put a cross-object call inside sched_on_tick, which
 * is the one function in this kernel where that is not worth doing.
 */
typedef struct {
    spinlock_t lock;
    int        n;               /* threads homed here */
} sched_rq_t;

static sched_rq_t sched_rq[SCHED_MAX_CPUS];

/*
 * Two counts, and they are not the same number.
 *
 * sched_cpus is how many queues the balancer may spread threads across,
 * which is how many processors the machine actually started.
 *
 * sched_dispatchers is how many of them run this scheduler, which is
 * one -- the application processors are a pool of kernel workers and
 * take no threads. See the boundary argued at the top of src/smp.h.
 *
 * Keeping them apart is what lets the balancer do real work now while
 * the picker stays correct: homes are assigned and counted, and the
 * moment a second processor begins dispatching, raising the second
 * number is the whole of what turns affinity on.
 */
static int sched_cpus        = 1;
static int sched_dispatchers = 1;

/*
 * Which processor is asking.
 *
 * Zero, today, and the function exists so that the picker below is
 * written in terms of "this processor" rather than in terms of a
 * constant that would have to be found again later. When an application
 * processor starts dispatching threads this is the one place that has to
 * learn how to answer, and everything above it is already correct.
 */
__attribute__((always_inline, target("general-regs-only")))
static inline int sched_here(void) { return 0; }

void sched_set_cpu_count(int n) {
    if (n < 1) n = 1;
    if (n > SCHED_MAX_CPUS) n = SCHED_MAX_CPUS;
    sched_cpus = n;
}

int sched_cpu_count(void) { return sched_cpus; }

int sched_queue_depth(int cpu) {
    if (cpu < 0 || cpu >= SCHED_MAX_CPUS) return 0;
    return sched_rq[cpu].n;
}

/*
 * Where a new thread should live.
 *
 * The interface is pinned to processor zero: it is the processor the
 * frame clock interrupts, and everything the compositor touches --
 * the window list, the terminal ring, the notification queue -- is
 * reachable from a syscall on the same processor and protected by a
 * preemption count rather than by a lock. Moving it would mean making
 * every one of those safe against a second processor first.
 *
 * Everything else goes wherever there is least. Ties go to the lowest
 * numbered queue, which keeps a machine that never starts a second
 * processor behaving exactly as it did.
 */
static uint32_t sched_pick_home(uint32_t prio) {
    if (prio >= PRIO_UI || sched_cpus <= 1) return 0;

    int best = 0;
    for (int c = 1; c < sched_cpus; c++)
        if (sched_rq[c].n < sched_rq[best].n) best = c;
    return (uint32_t)best;
}

static void sched_rq_join(thread_t *t) {
    if (t->cpu >= (uint32_t)SCHED_MAX_CPUS) t->cpu = 0;
    uint64_t f = spin_lock_irq(&sched_rq[t->cpu].lock);
    sched_rq[t->cpu].n++;
    spin_unlock_irq(&sched_rq[t->cpu].lock, f);
}

static void sched_rq_leave(thread_t *t) {
    if (t->cpu >= (uint32_t)SCHED_MAX_CPUS) return;
    uint64_t f = spin_lock_irq(&sched_rq[t->cpu].lock);
    if (sched_rq[t->cpu].n > 0) sched_rq[t->cpu].n--;
    spin_unlock_irq(&sched_rq[t->cpu].lock, f);
}

/*
 * Pick.
 *
 * Highest priority wins; among equals it is round robin, which falls out
 * of starting the search one past whoever just ran rather than at zero.
 * Sleepers whose deadline has passed are woken on the way through, since
 * this is the one place that walks every thread anyway.
 *
 * ---- and now, whose thread it is ----
 *
 * A home only restricts the search when there is more than one
 * processor doing the searching, and getting that condition wrong is
 * not a missed optimisation -- it is starvation.
 *
 * The first attempt here looked right and was badly wrong: consider
 * threads homed to this processor first, and fall back to all of them
 * only if that found nothing. With one dispatching processor the
 * fallback is unreachable, because the compositor is homed to processor
 * zero and is runnable sixty times a second -- so the restricted pass
 * always found *something*, always won, and every thread the balancer
 * had homed elsewhere simply never ran. An application would start,
 * print nothing, and hang, which is exactly what it did.
 *
 * The rule that is actually correct is that priority is global and
 * affinity is a tie-break *between processors*, not within one. So the
 * restriction is applied only when more than one processor dispatches,
 * and until then this is the same single pass over the same table in the
 * same rotated order that it has always been -- which is also why the
 * tie-break among equal priorities is unchanged.
 */
__attribute__((target("general-regs-only")))
static thread_t *sched_pick(void) {
    const int start = cur_thread ? sched_slot(cur_thread) : -1;
    const uint32_t here = (uint32_t)sched_here();
    const int affine = sched_dispatchers > 1;
    thread_t *best = 0;

    for (int pass = 0; pass < 2; pass++) {
        for (int n = 1; n <= SCHED_MAX_THREADS; n++) {
            int i = (start + n) % SCHED_MAX_THREADS;
            thread_t *t = threads[i];
            if (!t) continue;
            /* Deadlines are honoured on the first pass only: the second
             * walks the same table and would otherwise test a condition
             * the first has already resolved. */
            if (pass == 0 && t->state == T_SLEEPING &&
                sched_ticks >= t->wake_at)
                t->state = T_READY;
            if (t->state != T_READY && t->state != T_RUNNING) continue;
            if (pass == 0 && affine && t->cpu != here) continue;
            if (!best || t->prio > best->prio) best = t;
        }
        /* One pass is the whole search unless a home restricted it. */
        if (best || !affine) break;
    }
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

    /*
     * Thread-local storage, announced rather than installed.
     *
     * A `__thread` variable is addressed as an offset from FS, so the
     * base is part of a thread's register state exactly like RSP is, and
     * has to be restored on every switch or a thread reads somebody
     * else's variables. That much is not optional.
     *
     * What is optional is doing it *here*. The comment on gs_base_live
     * above records what happened the last time a WRMSR was put in this
     * function: a #GP on the FXRSTOR two instructions later. That was for
     * a value that never changes and so was simply moved out. This one
     * genuinely changes per thread, so it cannot be moved out — but it
     * can be moved *later*, past the FXRSTOR entirely, which is the part
     * of the sequence that objected.
     *
     * So the switch publishes the value and the stub below installs it,
     * after this function has returned and the extended state is already
     * back in the registers. A plain store to a global is the one thing
     * this window is definitely safe for; it is what CR3 and RSP0 above
     * are, too.
     */
    sched_fsbase_want = next->fsbase;

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
    /*
     * The thread-local base, installed here and nowhere else.
     *
     * This is past the FXRSTOR inside sched_on_tick, which is the whole
     * reason it is down here rather than beside the CR3 load: see the
     * comment on sched_fsbase_want. RAX, RCX and RDX are destroyed and
     * that is free — the pops immediately below restore all three from
     * the frame, and RSP has already been taken out of RAX.
     *
     * The compare is what makes this cost nothing on a machine with no
     * thread-local storage: both words are zero and the branch is taken
     * every time.
     */
    "  movq sched_fsbase_want(%rip), %rax\n"
    "  cmpq sched_fsbase_live(%rip), %rax\n"
    "  je 1f\n"
    "  movq %rax, sched_fsbase_live(%rip)\n"
    "  movq %rax, %rdx\n"
    "  shrq $32, %rdx\n"
    "  movl $0xC0000100, %ecx\n"
    "  wrmsr\n"
    "1:\n"
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
void sched_init(void) {
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
    /* Processor zero, and not by the balancer: this thread is already
     * executing on it, which is a stronger claim than any preference. */
    t->cpu        = 0;
    t->kstack_top = tss.rsp0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->cr3));
    t->cr3 &= PTE_ADDR_MASK;
    for (int i = 0; i < 512; i++) t->fx[i] = fx_template[i];
    sched_name(t, "compositor");

    sched_register(t);
    sched_rq_join(t);
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

thread_t *sched_spawn_kernel(void (*fn)(void), const char *name,
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
void sched_start(void) {
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

/*
 * Where a thread lands if it falls off the end of its function.
 *
 * There is no caller to return to -- the frame a thread starts on was
 * written by sched_build_frame and has no return address in it -- so
 * before this existed, returning meant popping whatever happened to be
 * on the stack and jumping to it. Naming that is worth eight bytes.
 */
void sched_exit(int code);

static void sched_thread_fell_off(void) {
    serial_puts("[sched] thread returned from its entry point\n");
    sched_exit(0);
}

/*
 * Set a new thread's stack pointer so the ABI holds when it starts.
 *
 * The subtlety here cost a general protection fault in the middle of a
 * TLS handshake, and it is worth writing down because it is invisible
 * until the day it is not.
 *
 * System V says that at the first instruction of a function, RSP is
 * congruent to 8 modulo 16 -- because the CALL that got there pushed an
 * eight-byte return address onto an aligned stack. Every compiler
 * relies on this: it is how a function knows that placing a local at a
 * particular offset from RBP makes that local sixteen-byte aligned,
 * which is what lets it use MOVAPS instead of MOVUPS.
 *
 * A thread here does not arrive by CALL. It arrives by IRETQ, which
 * loads RSP from the frame -- and that frame was sixteen-byte aligned,
 * so the thread began life with RSP aligned rather than eight past it.
 * Every stack local in every kernel thread has therefore been eight
 * bytes off its declared alignment since threads were introduced.
 *
 * Nothing noticed, because nothing had asked for an aligned local yet.
 * Then GCC turned the zeroing of a `struct sockaddr_in` into a single
 * MOVAPS, and MOVAPS to an unaligned address is #GP -- in a thread
 * doing nothing more exotic than opening a socket.
 *
 * The eight bytes are spent on a return address rather than simply
 * subtracted, so the fix also gives the fall-off-the-end case somewhere
 * to land.
 */
static uint64_t sched_entry_sp(uint64_t frame) {
    uint64_t sp = frame - 8;
    *(uint64_t *)(uintptr_t)sp = (uint64_t)(uintptr_t)sched_thread_fell_off;
    return sp;
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
    t->cpu        = sched_pick_home(prio);
    for (int i = 0; i < 512; i++) t->fx[i] = fx_template[i];
    sched_name(t, name);
    sched_rq_join(t);
    return t;
}

/* A thread that runs in the kernel's own address space. */
thread_t *sched_spawn_kernel(void (*fn)(void), const char *name,
                             uint32_t prio) {
    thread_t *t = sched_new(name, prio);
    if (!t) return 0;
    t->user = 0;
    t->as   = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->cr3));
    t->cr3 &= PTE_ADDR_MASK;
    /* Its stack pointer at entry is whatever is left below the frame,
     * offset by eight so the System V alignment rule holds. See
     * sched_entry_sp. */
    uint64_t frame = sched_build_frame(t, (uint64_t)(uintptr_t)fn, 0, 0);
    ((trap_frame_t *)(uintptr_t)frame)->rsp = sched_entry_sp(frame);
    t->rsp = frame;

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        /* sched_new counted it into a queue before the table was known
         * to have room. Give the place back, or a full table leaks a
         * queue slot per refused spawn and the balancer slowly comes to
         * believe every processor is busy. */
        sched_rq_leave(t);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/*
 * The same, for an entry point that takes an argument.
 *
 * There is no trampoline and no side table here, and there does not need
 * to be one: a new thread is started by IRETQ-ing into a frame this
 * kernel wrote itself, and the System V ABI puts the first argument in
 * RDI. Setting that one field in the frame *is* passing the argument.
 *
 * lwIP needs this -- sys_thread_new hands every thread a void* and its
 * tcpip thread is useless without it -- and so does anything else that
 * wants two workers running the same function over different state.
 */
thread_t *sched_spawn_kernel_arg(void (*fn)(void *), void *arg,
                                 const char *name, uint32_t prio) {
    thread_t *t = sched_new(name, prio);
    if (!t) return 0;
    t->user = 0;
    t->as   = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(t->cr3));
    t->cr3 &= PTE_ADDR_MASK;

    uint64_t frame = sched_build_frame(t, (uint64_t)(uintptr_t)fn, 0, 0);
    trap_frame_t *f = (trap_frame_t *)(uintptr_t)frame;
    f->rsp = sched_entry_sp(frame);
    f->rdi = (uint64_t)(uintptr_t)arg;
    t->rsp = frame;

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        /* sched_new counted it into a queue before the table was known
         * to have room. Give the place back, or a full table leaks a
         * queue slot per refused spawn and the balancer slowly comes to
         * believe every processor is busy. */
        sched_rq_leave(t);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/* A thread that runs in ring 3, in an address space of its own. */
thread_t *sched_spawn_user(addr_space_t *as, uint64_t entry,
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
        /* sched_new counted it into a queue before the table was known
         * to have room. Give the place back, or a full table leaks a
         * queue slot per refused spawn and the balancer slowly comes to
         * believe every processor is busy. */
        sched_rq_leave(t);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/*
 * A second thread in an address space that already has one.
 *
 * Everything a pthread needs and nothing a fork does. There is no page
 * table copy and no copy-on-write pass, because there is nothing to
 * separate: the new thread runs on its creator's mappings, sees its
 * creator's heap, and writes its creator's globals. That is the contract
 * a threading library is built to provide, and it is also why the
 * address space now has to be counted — see the reaper.
 *
 * The argument travels in the frame's RDI, which is where the System V
 * ABI puts a first parameter. A thread is started by IRETQ-ing into a
 * frame this function wrote, so there is no call to pass it through and
 * no trampoline needed to hold it: the register is simply set before the
 * thread ever runs.
 *
 * ---- why it is pinned to processor zero ----
 *
 * src/syscall.h keeps the kernel stack for the next entry from user mode
 * in `syscall_kstack`, one global word for the machine. That is sound
 * while exactly one processor ever runs ring-3 code: the word is
 * rewritten by every switch, and only the processor doing the switching
 * reads it. Two user threads entering SYSCALL simultaneously on two
 * processors would both load that one word and the second would build
 * its register frame on top of the first one's kernel stack.
 *
 * The futex comment in src/desktop.h already relies on this being true.
 * A process with several threads is the first thing in this system that
 * could make it false by accident, so the home is set rather than
 * chosen. sched_pick's fallback still lets another processor run the
 * thread if its own queue is empty — that is a separate question, and
 * one src/smp.h lists among the things a real lock would have to cover
 * before application processors dispatch user work at all.
 */
thread_t *sched_spawn_thread(addr_space_t *as, uint64_t entry,
                             uint64_t user_stack, uint64_t arg,
                             uint64_t fsbase, const char *name) {
    if (!as) return 0;

    thread_t *t = sched_new(name, PRIO_NORMAL);
    if (!t) return 0;
    t->user   = 1;
    t->as     = as;
    t->cr3    = as->pml4_phys;
    t->fsbase = fsbase;

    /* Off the queue it was put on by name, and onto processor zero's.
     * sched_new has already counted it somewhere, so the move has to be
     * a leave and a join rather than an assignment. */
    if (t->cpu != 0) {
        sched_rq_leave(t);
        t->cpu = 0;
        sched_rq_join(t);
    }

    uint64_t frame = sched_build_frame(t, entry, user_stack, 1);
    ((trap_frame_t *)(uintptr_t)frame)->rdi = arg;
    t->rsp = frame;

    uint64_t flags = irq_save();
    if (sched_register(t) < 0) {
        irq_restore(flags);
        sched_rq_leave(t);
        kstack_free(t->kstack, SCHED_KSTACK);
        kfree(t);
        return 0;
    }
    /* Counted only once the thread is certain to exist. A failed
     * registration that had already incremented would leave a process
     * whose last real thread could never take the count to zero, and the
     * address space would outlive every thread in it. */
    as->refs++;
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
thread_t *sched_fork_thread(thread_t *parent, addr_space_t *child_as) {
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
        /* sched_new counted it into a queue before the table was known
         * to have room. Give the place back, or a full table leaks a
         * queue slot per refused spawn and the balancer slowly comes to
         * believe every processor is busy. */
        sched_rq_leave(t);
        kstack_free(t->kstack, SCHED_KSTACK); kfree(t);
        return 0;
    }
    irq_restore(flags);
    return t;
}

/* Give up the rest of this slice. Software-generates the timer vector,
 * which means the one switch path is the only switch path. */

void sched_sleep_ms(uint64_t ms) {
    if (!cur_thread || !sched_running) return;
    cur_thread->wake_at = sched_ticks + (ms ? ms : 1);
    cur_thread->state   = T_SLEEPING;
    while (cur_thread->state == T_SLEEPING) sched_yield();
}

/*
 * ---- wait channels ----
 *
 * Everything above blocks on a deadline. A semaphore has to block on an
 * *event*, and there was nothing here that could: sleeping a millisecond
 * at a time and re-checking works, but it puts a millisecond of latency
 * on every packet and burns a slice per poll doing it.
 *
 * A channel is just an address. A thread parks itself against one and
 * whoever satisfies the condition wakes everything parked on it. No
 * queue is kept, because the scheduler already walks every thread on
 * each tick and the cost of testing one more field there is nothing
 * against the cost of maintaining a list under interrupts.
 *
 * Returns 1 if woken by sched_wake_chan, 0 if the timeout expired. The
 * caller must re-test its own condition either way: two threads can be
 * woken for one item, and the loser has to go back to sleep. That is a
 * property of every wakeup primitive worth having, not a shortcut.
 */
/*
 * The version that takes the interrupt flags from its caller, because
 * for a semaphore the check and the parking must be one atomic step.
 *
 * A semaphore wait is "if the count is zero, sleep". Written as two
 * statements with interrupts on between them, there is a window in
 * which the count is raised and the sleeper woken *before* it is
 * asleep -- so it then sleeps, and the wakeup it needed has already
 * happened. That is the lost-wakeup race, it hangs one connection out
 * of thousands, and it is unreproducible by construction.
 *
 * Closing it needs no lock on a kernel that schedules one processor:
 * the caller disables interrupts, tests its own condition, and hands
 * the flags here still disabled. Nothing can run in between because
 * nothing can interrupt.
 */
int sched_block_on_locked(void *chan, uint32_t timeout_ms,
                          uint64_t flags) {
    if (!cur_thread || !sched_running) {
        irq_restore(flags);
        /* Before the scheduler exists there is nobody to wake us, so the
         * honest thing is to spin briefly rather than block forever. */
        for (volatile int i = 0; i < 10000; i++) __asm__ volatile("pause");
        return 0;
    }
    cur_thread->wait_chan = chan;
    cur_thread->wake_at   = timeout_ms ? sched_ticks + timeout_ms
                                       : ~(uint64_t)0;
    cur_thread->state     = T_SLEEPING;
    irq_restore(flags);

    while (cur_thread->state == T_SLEEPING) sched_yield();

    /* Still flagged means the tick released us on the deadline rather
     * than a poster clearing it. */
    int woken = (cur_thread->wait_chan == 0);
    cur_thread->wait_chan = 0;
    return woken;
}

int sched_block_on(void *chan, uint32_t timeout_ms) {
    return sched_block_on_locked(chan, timeout_ms, irq_save());
}

/*
 * Release what is parked on a channel. Safe from an interrupt: it only
 * writes two fields and never allocates.
 */
void sched_wake_chan(void *chan, int all) {
    uint64_t flags = irq_save();
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->wait_chan != chan) continue;
        t->wait_chan = 0;
        if (t->state == T_SLEEPING) t->state = T_READY;
        if (!all) break;
    }
    irq_restore(flags);
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
void sched_wait_frame(void) {
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
void sched_frame_pulse(void) {
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
void sched_exit(int code) {
    if (!cur_thread) return;
    cur_thread->exit_code = code;
    cur_thread->state = T_ZOMBIE;
    for (;;) sched_yield();
}

/*
 * Collect what has finished. Called from the compositor thread, which is
 * by construction not any of the threads being freed.
 */
/* Not static: kernel.c installs the desktop's reaper here, so that
 * scheduler.o need not know what an application is. */
void (*sched_reap_hook)(thread_t *t) = 0;

void sched_reap(void) {
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state != T_ZOMBIE) continue;
        if (t == cur_thread) continue;

        if (sched_reap_hook) sched_reap_hook(t);

        uint64_t flags = irq_save();
        threads[i] = 0;
        irq_restore(flags);

        /* Off its queue before the control block goes, so the balancer
         * never counts a thread that no longer exists. */
        sched_rq_leave(t);

        /*
         * The address space goes when the last thread standing in it
         * does, and not before.
         *
         * This used to be an unconditional destroy, which was right for
         * as long as an address space had exactly one thread — a process
         * was a thread that carried one. SYS_CLONE breaks that: a pthread
         * shares its creator's space, because sharing the heap and the
         * globals is what threading *is*. Destroying on the first exit
         * would unmap the page tables out from under every sibling still
         * running on them.
         *
         * A count of zero rather than one is the guard for spaces that
         * were never counted at all. Nothing creates one now — vmm_create
         * sets it to one — but a zero here would wrap to four billion and
         * leak the space forever, and a subtraction that can wrap is
         * worth one comparison to prevent.
         *
         * No atomic and no lock: this runs on the compositor thread,
         * which is the only caller of sched_reap, and the increment side
         * runs inside a system call with interrupts masked. There is
         * never a second writer.
         */
        if (t->as) {
            if (t->as->refs > 1) {
                t->as->refs--;
            } else {
                vmm_destroy(t->as);
                kfree(t->as);
            }
        }
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
int sched_join(thread_t *t, uint32_t timeout_ms) {
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

int sched_thread_count(void) {
    int n = 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++)
        if (threads[i] && threads[i]->state != T_FREE) n++;
    return n;
}

