/*
 * src/sched/vls_core.c — the Vextro Linux Subset: the router, the
 * translation table, and the signal matrix.
 *
 * The fourth object beside the composition root, and it earns the
 * separation the same way the other three do: it needs eleven symbols
 * back and exports fifteen. Everything it needs that is not in
 * include/kernel_shared.h or src/sched/sched.h arrives through the hook
 * table in include/vls.h, which the composition root fills in once — the
 * same shape as sched_reap_hook, and for the same reason. A module that
 * included src/desktop.h would compile, link, and get a private copy of
 * the compositor's state.
 *
 * ---- what is actually hard here ----
 *
 * Not the numbering. The renumbering turned out to be nearly free,
 * because this system's own descriptor calls were written against
 * Linux's constants on purpose: O_CREAT is 0100 in src/syscall.h because
 * that is what the code being ported says, and PROT_READ is 1 for the
 * same reason. Thirty of the entries below are therefore a change of
 * call number and nothing else, and the table says so.
 *
 * The hard parts are the four places where Linux has a *concept* this
 * system did not:
 *
 *   Signals, which are the only way a program here can be told anything
 *   it did not ask for. Everything else in this kernel is
 *   call-and-answer.
 *
 *   exec, which is the only operation that changes what a process *is*
 *   without changing which process it is.
 *
 *   wait, which requires that a process have a parent, which requires
 *   that a process have an identity — and until today the only identity
 *   anywhere was a thread's.
 *
 *   clone with CLONE_VM, whose child returns from a call the child never
 *   made. Native SYS_CLONE takes an entry point and cannot express it,
 *   so the table does not pretend it can: the flags are inspected, and a
 *   combination this file does not wholly understand is refused rather
 *   than half-translated.
 */

#include <stdint.h>

/* The seam, the thread table, and this module's own declarations. Note
 * what is absent: src/vfs.h, src/desktop.h, src/vmm.h. The services
 * behind every forwarded call live in those files and are reached
 * through one function pointer, which is the whole of this object's
 * dependency on them. */
#include "kernel_shared.h"
#include "sched/sched.h"
#include "vls.h"

/* Filled in by the composition root before vls_init runs. Every field is
 * called through unchecked *after* vls_init has verified they are all
 * present, which is why the check is worth doing once loudly rather than
 * fifty times quietly. */
vls_host_t vls_host;

static int      vls_ready = 0;
static uint64_t vls_translated = 0;
static uint64_t vls_refused = 0;

uint64_t vls_calls_translated(void) { return vls_translated; }
uint64_t vls_calls_refused(void)    { return vls_refused; }

/* ============================================================
 *  1. saying so, on the serial line, exactly once
 * ============================================================
 *
 * Every refusal in this file goes out under `[VLS]` and none of them
 * stops the machine. That is the whole contract of a subset: a program
 * that asks for something outside it gets ENOSYS and decides for itself
 * what to do, and the operator gets a line naming the call. A kernel
 * that halted instead would turn "this port uses splice" into a boot
 * failure with no message about splice in it.
 *
 * Once per number, though. A program probing for a call it does not
 * find will probe in a loop — that is what a feature test is — and a
 * line per attempt is a serial log with the interesting part scrolled
 * off the top. The bitmap costs sixty-four bytes and is the difference
 * between a report and a flood.
 */
static uint64_t vls_seen[VLS_NR_MAX / 64];

static int vls_first_sighting(uint64_t nr) {
    if (nr >= VLS_NR_MAX) return 1;      /* out of range: always report */
    const uint64_t bit = 1ull << (nr & 63);
    if (vls_seen[nr >> 6] & bit) return 0;
    vls_seen[nr >> 6] |= bit;
    return 1;
}

static void vls_hex64(uint64_t v) {
    static const char d[] = "0123456789abcdef";
    char out[19];
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; i++) out[2 + i] = d[(v >> ((15 - i) * 4)) & 0xF];
    out[18] = '\0';
    serial_puts(out);
}

static void vls_dec(uint64_t v) {
    if (v > 0xFFFFFFFFull) { vls_hex64(v); return; }
    serial_put_dec((uint32_t)v);
}

/*
 * The debug payload the specification asks for, and it is deliberately
 * everything: the number, the name if there is one, all six argument
 * registers, and who was calling. An unmapped vector is a porting
 * question, and a porting question is answered by the arguments as often
 * as by the number — a `clone` whose flags are 0x3d0f00 and a `clone`
 * whose flags are 0x11 are two entirely different pieces of work.
 */
static void vls_report(const char *what, uint64_t nr, const char *name,
                       const uint64_t a[6]) {
    serial_puts("[VLS] ");
    serial_puts(what);
    serial_puts(": linux call ");
    vls_dec(nr);
    if (name) { serial_puts(" ("); serial_puts(name); serial_puts(")"); }
    serial_puts("\n[VLS]   args ");
    for (int i = 0; i < 6; i++) {
        vls_hex64(a[i]);
        if (i != 5) serial_puts(" ");
    }
    serial_puts("\n[VLS]   thread ");
    if (cur_thread) {
        serial_puts(cur_thread->name);
        serial_puts(" (tid ");
        serial_put_dec(cur_thread->pid);
        serial_puts(", pid ");
        serial_put_dec(vls_host.pid ? vls_host.pid() : 0);
        serial_puts(")");
    } else {
        serial_puts("none");
    }
    serial_puts("\n");
}

/* ============================================================
 *  2. finding a process
 * ============================================================
 *
 * By its pid, which is the tid of the thread that created its address
 * space. Linear over the thread table because the table is sixty-four
 * entries and this runs on kill and wait4, neither of which is on any
 * hot path; an index would be a second structure to keep true across
 * fork, exec and reap for no measurable gain.
 */
/* Defined with the child records in section 5; needed by vls_terminate
 * in section 3, which is one of the four places a process can end. */
void vls_report_exit(addr_space_t *as, int exit_code, int killed);

static addr_space_t *vls_find_process(uint32_t pid) {
    if (!pid) return 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == T_FREE || !t->as) continue;
        if (t->as->pid == pid) return t->as;
    }
    return 0;
}

static thread_t *vls_find_thread(uint32_t tid) {
    if (!tid) return 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == T_FREE) continue;
        if (t->pid == tid) return t;
    }
    return 0;
}

/* Does this process still have a child that could report a status? A
 * wait with no such child is ECHILD and must not park. */
static int vls_has_live_child(uint32_t pid) {
    if (!pid) return 0;
    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t->state == T_FREE || !t->as) continue;
        /*
         * A process whose ending has already been recorded is not one
         * that could report again, even though its threads are still in
         * the table waiting to be reaped. Counting it would make a
         * second wait4 — after the first had collected the status —
         * park forever on a child that has nothing left to say.
         */
        if (t->as->exit_reported) continue;
        if (t->as->ppid == pid && t->as->pid != pid) return 1;
    }
    return 0;
}

/* ============================================================
 *  3. signal state
 * ============================================================ */

vls_sig_t *vls_sig_of(addr_space_t *as, int create) {
    if (!as) return 0;
    if (as->sig || !create) return as->sig;

    vls_sig_t *s = (vls_sig_t *)kmalloc(sizeof(vls_sig_t));
    if (!s) return 0;
    for (uint64_t i = 0; i < sizeof(vls_sig_t); i++) ((uint8_t *)s)[i] = 0;
    /* Every disposition starts at the default, which for most signals is
     * "end the process" and for four of them is "do nothing". Zero is
     * VLS_SIG_DFL, so the loop above has already said it; what the
     * defaults *mean* is vls_default_is_fatal below, and keeping the
     * meaning in one function rather than in a table of initial values
     * is what stops a handler reset from having to know it too. */
    as->sig = s;
    return s;
}

void vls_sig_free(addr_space_t *as) {
    if (!as || !as->sig) return;
    kfree(as->sig);
    as->sig = 0;
}

/*
 * A child inherits its parent's handlers and its blocked mask, and does
 * not inherit anything else here.
 *
 * Pending signals are dropped, which is what POSIX says and is worth a
 * line because it looks like an omission: a signal that arrived for the
 * parent was addressed to the parent, and a fork that duplicated it
 * would deliver one signal twice. The child records are dropped for the
 * same reason — they are the parent's children, not the child's.
 */
int vls_sig_clone(addr_space_t *child, addr_space_t *parent) {
    if (!parent || !parent->sig) return 0;      /* nothing to inherit */
    vls_sig_t *d = vls_sig_of(child, 1);
    if (!d) return -1;
    const vls_sig_t *s = parent->sig;
    for (int i = 0; i < VLS_NSIG; i++) d->act[i] = s->act[i];
    d->blocked = s->blocked;
    return 0;
}

/*
 * exec keeps the blocked mask and resets every disposition that pointed
 * at code, because the code is gone.
 *
 * Ignored stays ignored: the new image's address space has nothing at
 * the old handler's address, but "ignore this signal" is a decision
 * about the process rather than about the image, and POSIX is explicit
 * that it survives. A handler that survived would be a jump into
 * whatever the new image happens to have loaded there.
 */
void vls_sig_reset_for_exec(addr_space_t *as) {
    if (!as || !as->sig) return;
    vls_sig_t *s = as->sig;
    for (int i = 0; i < VLS_NSIG; i++) {
        if (s->act[i].handler != VLS_SIG_IGN) s->act[i].handler = VLS_SIG_DFL;
        s->act[i].flags = 0;
        s->act[i].restorer = 0;
        s->act[i].mask = 0;
    }
    s->pending = 0;
    s->fault_addr = 0;
    s->fault_code = 0;
    s->killed_by = 0;
    s->clear_child_tid = 0;
}

/* The four whose default action is to be ignored. Everything else
 * defaults to ending the process, which is why this is a list of
 * exceptions rather than a table of actions. */
static int vls_default_is_ignore(int sig) {
    return sig == VLS_SIGCHLD || sig == VLS_SIGCONT ||
           sig == VLS_SIGWINCH;
}

/*
 * End every thread of a process that is not the one asking.
 *
 * The same mechanism SYS_EXIT_GROUP uses and the same argument for why
 * marking is enough: a thread marked T_ZOMBIE is never chosen by
 * sched_pick again, so it executes no further user instruction from this
 * moment, and the reaper frees its stack later on the compositor thread.
 * Interrupts are masked for the whole of a system call and user threads
 * are pinned to processor zero, so no thread of the target is running
 * anywhere while this executes.
 *
 * The caller's own thread is left alone deliberately. If the target is
 * the caller's own process, the caller returns through
 * vls_signal_dispatch, which answers VLS_DELIVER_FATAL and lets the exit
 * happen on the ordinary path — a thread that zombied itself here would
 * still be holding a kernel stack it has to return along.
 */
static void vls_terminate(addr_space_t *as, int sig) {
    if (!as) return;
    vls_sig_t *s = vls_sig_of(as, 1);
    if (s) s->killed_by = (uint8_t)sig;

    /* Told to its parent here rather than at the reap, because a process
     * killed this way makes no system call on the way out and the reaper
     * may not run for a long time. */
    vls_report_exit(as, sig, 1);

    for (int i = 0; i < SCHED_MAX_THREADS; i++) {
        thread_t *t = threads[i];
        if (!t || t == cur_thread) continue;
        if (t->as != as || t->state == T_FREE) continue;
        t->exit_code = 128 + sig;
        t->state     = T_ZOMBIE;
    }
}

/*
 * Post a signal to a process.
 *
 * Two outcomes, and which one applies is decided here rather than at
 * delivery, because they need different machinery. A signal that will be
 * *caught* is recorded as pending and delivered when the target next
 * crosses a boundary where its registers can be rewritten. A signal that
 * will *kill* cannot wait for that: the target may be parked in a sleep
 * it will not leave for a minute, and "kill -9 took a minute" is not a
 * kill. So a fatal signal acts immediately, and only the caller's own
 * process is left to the ordinary path.
 */
int vls_signal_post(addr_space_t *as, int sig, int from_kernel) {
    (void)from_kernel;
    if (!as || !as->live) return -VLS_ESRCH;
    if (sig < 0 || sig >= VLS_NSIG) return -VLS_EINVAL;
    if (sig == 0) return 0;             /* the existence probe: kill(pid, 0) */

    vls_sig_t *s = vls_sig_of(as, 1);
    if (!s) return -VLS_ENOMEM;

    const uint64_t bit = VLS_SIGBIT(sig);
    const uint64_t disp = s->act[sig].handler;

    /* Uncatchable, unblockable, unignorable, and it says so in one
     * place: include/vls.h's VLS_SIG_UNBLOCKABLE. */
    if (bit & VLS_SIG_UNBLOCKABLE) {
        s->pending |= bit;
        vls_terminate(as, sig);
        return 0;
    }

    if (disp == VLS_SIG_IGN) return 0;
    if (disp == VLS_SIG_DFL && vls_default_is_ignore(sig)) return 0;

    s->pending |= bit;

    /* A default disposition that is not "ignore" is "terminate", and
     * that is acted on now for the reason above — unless the target is
     * this very process, whose exit belongs on its own return path. */
    if (disp == VLS_SIG_DFL && !(s->blocked & bit) && as != vmm_current)
        vls_terminate(as, sig);

    /* A caught signal on a process that is parked: nudge it, so it
     * leaves the sleep and reaches a delivery point. The channel a
     * futex or a wait is parked on is not knowable from here, so what
     * this can do is wake a wait4 — which is the one park this file
     * owns — and leave the rest to the bounded re-entry every other
     * park in this kernel already does. */
    sched_wake_chan(s, 1);
    return 0;
}

/*
 * The synchronous half: a fault has happened and the processor has
 * already said what and where.
 *
 * Separate from vls_signal_post because a fault carries an address, and
 * because a fault must never be turned into a termination *here* — the
 * trap handler has a report to print and a thread to end, and it does
 * both better than this file can. So this only records; the answer comes
 * from the dispatch that immediately follows.
 */
int vls_signal_fault(addr_space_t *as, int sig, uint64_t addr, uint32_t code,
                     uint64_t trapno, uint64_t err) {
    (void)trapno; (void)err;
    if (!as || sig <= 0 || sig >= VLS_NSIG) return 0;
    vls_sig_t *s = as->sig;          /* never created here: see below */
    if (!s) return 0;
    /*
     * Deliberately not vls_sig_of(as, 1). A process that has never
     * installed a handler cannot catch this, so allocating state for it
     * would be a kmalloc inside a page-fault handler to record something
     * nothing will read. The trap handler's existing path — print
     * everything and end the thread — is the right answer for that
     * process and is what happens when this returns zero.
     */
    const uint64_t bit = VLS_SIGBIT(sig);
    if (s->act[sig].handler == VLS_SIG_DFL ||
        s->act[sig].handler == VLS_SIG_IGN)
        return 0;
    /*
     * A synchronous fault cannot be honoured while blocked.
     *
     * Linux's rule, and the reason is that the alternative is a loop:
     * returning from the handler resumes the instruction that faulted,
     * which faults again, forever. Linux forces the disposition back to
     * default; this reports and lets the thread die, which reaches the
     * same place with a line saying why.
     */
    if (s->blocked & bit) {
        serial_puts("[VLS] fault signal ");
        serial_put_dec((uint32_t)sig);
        serial_puts(" is blocked; the handler will not be run\n");
        return 0;
    }
    s->pending    |= bit;
    s->fault_addr  = addr;
    s->fault_code  = code;
    return 1;
}

/* ============================================================
 *  4. building the frame a handler runs on
 * ============================================================ */

static uint64_t vls_lowest_pending(uint64_t set) {
    for (int i = 1; i < VLS_NSIG; i++)
        if (set & VLS_SIGBIT(i)) return (uint64_t)i;
    return 0;
}

/*
 * Lay a signal frame on the interrupted thread's own stack and point it
 * at the handler.
 *
 * Where the frame goes is the whole of the delicacy. Below the stack
 * pointer, past a hundred and twenty-eight bytes that this kernel's own
 * programs do not use — they are compiled -mno-red-zone — but a ported
 * one may, and skipping the red zone costs a hundred and twenty-eight
 * bytes where not skipping it costs a leaf function its locals. Then
 * aligned down twice: once so the frame itself is sixteen-aligned, and
 * once more after the return address is pushed, because the System V ABI
 * says a function is entered with RSP eight past a boundary and a
 * handler that takes a __m128 local will fault on the first MOVAPS
 * otherwise.
 *
 * Returns 1 on success. On failure the process has a stack it cannot
 * write, which is the one case where a caught signal still ends the
 * program — and it says so, because "the handler never ran" is
 * otherwise indistinguishable from "the handler did nothing".
 */
static int vls_build_frame(vls_sig_t *s, int sig, vls_regs_t *r) {
    vls_sigframe_t fr;
    for (uint64_t i = 0; i < sizeof(fr); i++) ((uint8_t *)&fr)[i] = 0;

    const vls_sigaction_t act = s->act[sig];

    uint64_t sp = r->rsp;
    sp -= 128;                                   /* the red zone */
    sp &= ~15ull;
    sp -= sizeof(vls_sigframe_t);
    sp &= ~15ull;
    const uint64_t frame_va = sp;
    sp -= 8;                                     /* the return address */

    if (sp < 4096 || !vls_host.range_ok(sp, sizeof(vls_sigframe_t) + 8)) {
        serial_puts("[VLS] no room on the stack for a signal frame\n");
        return 0;
    }
    if (!vls_host.range_mapped(sp, sizeof(vls_sigframe_t) + 8, 1)) {
        serial_puts("[VLS] the signal stack is not writable\n");
        return 0;
    }

    fr.magic      = VLS_SIGFRAME_MAGIC;
    fr.saved_mask = s->blocked;
    fr.sig        = (uint64_t)sig;

    fr.info.si_signo = sig;
    fr.info.si_code  = (sig == VLS_SIGSEGV || sig == VLS_SIGBUS ||
                        sig == VLS_SIGILL  || sig == VLS_SIGFPE)
                       ? (int32_t)s->fault_code : VLS_SI_USER;
    fr.info.si_addr  = (sig == VLS_SIGSEGV || sig == VLS_SIGBUS ||
                        sig == VLS_SIGILL  || sig == VLS_SIGFPE)
                       ? s->fault_addr : 0;

    /*
     * The register file, in Linux's order.
     *
     * This is the copy a handler is allowed to *modify*: sigreturn below
     * restores from here rather than from a private snapshot, which is
     * what makes the one thing a JIT actually does with a SIGSEGV
     * handler work — set gregs[REG_RIP] to a bail-out stub and return,
     * and execution continues there instead of re-faulting forever. A
     * private snapshot would have been simpler and would have silently
     * ignored the assignment.
     */
    uint64_t *g = fr.uc.uc_mcontext.gregs;
    g[VLS_REG_R8]  = r->r8;   g[VLS_REG_R9]  = r->r9;
    g[VLS_REG_R10] = r->r10;  g[VLS_REG_R11] = r->r11;
    g[VLS_REG_R12] = r->r12;  g[VLS_REG_R13] = r->r13;
    g[VLS_REG_R14] = r->r14;  g[VLS_REG_R15] = r->r15;
    g[VLS_REG_RDI] = r->rdi;  g[VLS_REG_RSI] = r->rsi;
    g[VLS_REG_RBP] = r->rbp;  g[VLS_REG_RBX] = r->rbx;
    g[VLS_REG_RDX] = r->rdx;  g[VLS_REG_RAX] = r->rax;
    g[VLS_REG_RCX] = r->rcx;  g[VLS_REG_RSP] = r->rsp;
    g[VLS_REG_RIP] = r->rip;  g[VLS_REG_EFL] = r->rflags;
    g[VLS_REG_ERR]     = r->err;
    g[VLS_REG_TRAPNO]  = r->trapno;
    g[VLS_REG_OLDMASK] = s->blocked;
    g[VLS_REG_CR2]     = r->fault_addr;
    /* Null, and Linux does the same when there is no extended state to
     * record. include/vls.h explains why there is none to record here:
     * a signal is delivered at a system call or a fault, and the
     * thread's FPU state is already in thread_t.fx either way. */
    fr.uc.uc_mcontext.fpregs = 0;

    fr.uc.ss_sp    = r->rsp;
    fr.uc.ss_size  = 0;
    fr.uc.ss_flags = 2;                  /* SS_DISABLE: no alternate stack */
    fr.uc.uc_sigmask[0] = s->blocked;

    if (vls_host.copy_out(frame_va, &fr, sizeof(fr)) != 0) return 0;

    /*
     * What the handler returns *to*.
     *
     * A program that set SA_RESTORER supplied its own trampoline and
     * gets it, because a libc that did so has a sigreturn stub whose
     * exact instructions it may depend on. Everything else gets this
     * system's, on the shared trampoline page — which is the reason the
     * call bias in include/vls.h exists as well as the personality: one
     * stub, issuing one biased number, serves a native process that
     * caught a signal and a Linux one that did.
     */
    const uint64_t ret_to = (act.flags & VLS_SA_RESTORER) && act.restorer
                            ? act.restorer : vls_host.sigreturn_va();
    if (!ret_to) {
        serial_puts("[VLS] no signal trampoline in this process\n");
        return 0;
    }
    if (vls_host.copy_out(sp, &ret_to, 8) != 0) return 0;

    /* From here the mask is the handler's, not the interrupted code's.
     * SA_NODEFER is the program asking for its own signal to remain
     * deliverable inside its own handler, which is a request for
     * re-entrancy and is granted rather than second-guessed. */
    s->blocked |= act.mask;
    if (!(act.flags & VLS_SA_NODEFER)) s->blocked |= VLS_SIGBIT(sig);
    s->blocked &= ~VLS_SIG_UNBLOCKABLE;

    if (act.flags & VLS_SA_RESETHAND) s->act[sig].handler = VLS_SIG_DFL;

    r->rsp = sp;
    r->rip = act.handler;
    r->rdi = (uint64_t)sig;
    /* The three-argument form, when the program asked for it. Both
     * pointers are real structures on the program's own stack rather
     * than null, because a handler registered with SA_SIGINFO
     * dereferences them without checking — that is what the flag means.
     */
    r->rsi = (act.flags & VLS_SA_SIGINFO)
             ? frame_va + (uint64_t)__builtin_offsetof(vls_sigframe_t, info) : 0;
    r->rdx = (act.flags & VLS_SA_SIGINFO)
             ? frame_va + (uint64_t)__builtin_offsetof(vls_sigframe_t, uc) : 0;
    r->rax = 0;
    /* Direction flag clear, trap flag clear: the ABI requires the first
     * of a function's caller and the second would single-step the
     * handler. Interrupts stay enabled. */
    r->rflags &= ~0x500ull;
    r->rflags |= 0x2ull;
    return 1;
}

int vls_signal_dispatch(addr_space_t *as, vls_regs_t *r) {
    if (!as || !as->sig || !vls_ready) return VLS_DELIVER_NONE;
    vls_sig_t *s = as->sig;

    /*
     * Loop, because several signals may be pending and the first two
     * outcomes do not consume the return to user mode. An ignored signal
     * is dropped and the next one considered; a fatal one ends the
     * process and there is nothing after it; a caught one rewrites the
     * frame and must be the last thing this does, since the frame can
     * only point at one handler.
     */
    for (;;) {
        uint64_t ready = s->pending & ~s->blocked;
        ready |= s->pending & VLS_SIG_UNBLOCKABLE;
        const uint64_t sig = vls_lowest_pending(ready);
        if (!sig) return VLS_DELIVER_NONE;

        s->pending &= ~VLS_SIGBIT(sig);
        const uint64_t h = s->act[sig].handler;

        /* Recorded before every fatal return below, because the caller
         * has to put "killed by 11" in the status word its parent will
         * read and an exit code has no room left to carry it. */
        s->killed_by = (uint8_t)sig;

        if (VLS_SIGBIT(sig) & VLS_SIG_UNBLOCKABLE) return VLS_DELIVER_FATAL;
        if (h == VLS_SIG_IGN) { s->killed_by = 0; continue; }
        if (h == VLS_SIG_DFL) {
            if (vls_default_is_ignore((int)sig)) { s->killed_by = 0; continue; }
            return VLS_DELIVER_FATAL;
        }
        if (!vls_host.range_ok(h, 1)) {
            serial_puts("[VLS] handler address is outside user space; "
                        "treating the signal as fatal\n");
            return VLS_DELIVER_FATAL;
        }
        if (vls_build_frame(s, (int)sig, r)) {
            s->killed_by = 0;
            return VLS_DELIVER_HANDLER;
        }
        return VLS_DELIVER_FATAL;
    }
}

/*
 * The way back.
 *
 * Everything restored comes out of the frame the program has had write
 * access to for the whole of the handler, so everything restored has to
 * be something the program was already allowed to set. Twenty of the
 * twenty-three general registers are; RIP and RSP are checked for being
 * in user space, which is the only property that matters, since a
 * program may already jump anywhere in its own image and move its own
 * stack. RFLAGS is the one that is *not* taken as given: IOPL, IF and
 * the trap flag are the program's business only in the sense that it
 * must not be able to change them, so they are taken from the kernel's
 * side and the rest of the word from the program's.
 */
static uint64_t vls_sigreturn(uint64_t uptr) {
    addr_space_t *as = vmm_current;
    if (!as || !as->sig) return (uint64_t)(int64_t)-VLS_EPERM;
    /*
     * Straight into the frame the syscall will return along, rather than
     * into a snapshot handed back up.
     *
     * It has to be the frame, because a snapshot would have to be
     * threaded through the router and every one of the other sixty-odd
     * handlers would carry a parameter it never touches. And it can be
     * the frame, because sigreturn is reachable from exactly one place:
     * the trampoline this kernel put on the program's stack, which is a
     * SYSCALL. Anything arriving here by another door has no return
     * address this code could restore anyway, which is what the second
     * condition says.
     */
    syscall_frame_t *f = cur_thread ? cur_thread->sframe : 0;
    if (!f || !cur_thread->sfast) {
        vls_host.refuse("sigreturn: not returning from a signal handler");
        return (uint64_t)(int64_t)-VLS_EPERM;
    }
    if (!vls_host.range_mapped(uptr, sizeof(vls_sigframe_t), 0))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    vls_sigframe_t fr;
    if (vls_host.copy_in(uptr, &fr, sizeof(fr)) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;

    if (fr.magic != VLS_SIGFRAME_MAGIC) {
        vls_host.refuse("sigreturn: that is not a signal frame this "
                        "kernel built");
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }

    const uint64_t *g = fr.uc.uc_mcontext.gregs;
    if (!vls_host.range_ok(g[VLS_REG_RIP], 1) ||
        !vls_host.range_ok(g[VLS_REG_RSP], 8)) {
        vls_host.refuse("sigreturn: the frame resumes outside user space");
        return (uint64_t)(int64_t)-VLS_EFAULT;
    }

    vls_sig_t *s = as->sig;
    s->blocked = fr.saved_mask & ~VLS_SIG_UNBLOCKABLE;

    f->r8  = g[VLS_REG_R8];   f->r9  = g[VLS_REG_R9];
    f->r10 = g[VLS_REG_R10];
    f->r12 = g[VLS_REG_R12];  f->r13 = g[VLS_REG_R13];
    f->r14 = g[VLS_REG_R14];  f->r15 = g[VLS_REG_R15];
    f->rdi = g[VLS_REG_RDI];  f->rsi = g[VLS_REG_RSI];
    f->rbp = g[VLS_REG_RBP];  f->rbx = g[VLS_REG_RBX];
    f->rdx = g[VLS_REG_RDX];

    /*
     * The three that are not stored under their own names.
     *
     * SYSRETQ takes RIP out of RCX and RFLAGS out of R11 — that is the
     * instruction's definition, not this kernel's convention — so the
     * resumption address and the flags go into those two fields, and the
     * program's own RCX and R11 are lost. They were already lost: the
     * SYSCALL that entered here clobbered both, which is why the calling
     * convention lists them as destroyed and why no compiler expects
     * them to survive a system call.
     *
     * IF on, IOPL zero, TF and NT off, reserved bit one; the rest of the
     * flags as the program left them. Those four are not the program's
     * to set, and this frame has been writable by the program for the
     * whole of the handler.
     */
    f->rcx      = g[VLS_REG_RIP];
    f->user_rsp = g[VLS_REG_RSP];
    f->r11      = (g[VLS_REG_EFL] & 0x0000000000000CD5ull) | 0x202ull;

    /* And the value the interrupted call was about to return. It goes
     * back through RAX the ordinary way, which is why this is a return
     * value and not an assignment. */
    return g[VLS_REG_RAX];
}

/* ============================================================
 *  5. children, and waiting for them
 * ============================================================ */

void vls_child_exited(uint32_t pid, uint32_t ppid, int exit_code, int killed) {
    if (!pid || !ppid) return;
    addr_space_t *parent = vls_find_process(ppid);
    if (!parent) return;                 /* orphaned: nobody to tell */

    vls_sig_t *s = vls_sig_of(parent, 1);
    if (!s) return;

    for (int i = 0; i < VLS_MAX_CHILDREN; i++) {
        if (s->child[i].used) continue;
        s->child[i].pid    = pid;
        s->child[i].status = vls_encode_status(exit_code, killed);
        s->child[i].used   = 1;
        break;
    }
    /*
     * If every slot was taken the status is dropped, and that is
     * reported rather than silent: sixteen unreaped children means a
     * parent that has stopped calling wait, and the number it would
     * eventually read is less interesting than the fact that it stopped.
     */
    int room = 0;
    for (int i = 0; i < VLS_MAX_CHILDREN; i++) if (!s->child[i].used) room = 1;
    if (!room) {
        serial_puts("[VLS] pid ");
        serial_put_dec(ppid);
        serial_puts(" has sixteen unreaped children; the next status "
                    "will be dropped\n");
    }

    /* SIGCHLD, and then a nudge for whoever is parked in wait4. The
     * post is what a program that installed a handler is waiting for;
     * the wake is what a program that called wait is waiting for, and a
     * program may reasonably do both. */
    vls_signal_post(parent, VLS_SIGCHLD, 1);
    sched_wake_chan(s, 1);
}

/*
 * Once, whichever of the four ways out this process took.
 *
 * The four are exit, exit_group, a fatal signal and a fault, and they
 * do not share a code path — three are system calls made by the process
 * itself and the fourth is a trap handler that ends a thread which never
 * called anything. The reaper is the one point all four pass through and
 * would be the obvious single place to do this; it is the backstop
 * instead, for the reason written beside addr_space_t.exit_reported.
 */
void vls_report_exit(addr_space_t *as, int exit_code, int killed) {
    if (!as || as->exit_reported || !as->pid) return;
    as->exit_reported = 1;
    vls_child_exited(as->pid, as->ppid, exit_code, killed);
}

int64_t vls_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                  uint64_t rusage_ptr) {
    addr_space_t *as = vmm_current;
    if (!as || !as->live) return -VLS_ECHILD;
    const uint32_t me = as->pid;
    if (!me) return -VLS_ECHILD;

    /*
     * Resource usage is not accounted for a process here — the scheduler
     * counts slices per thread and nothing sums them per address space —
     * so a request for it is refused rather than answered with zeros. A
     * zeroed rusage is the kind of wrong answer a program builds a
     * report out of.
     */
    if (rusage_ptr) {
        vls_host.refuse("wait4: this system does not account rusage");
        return -VLS_EINVAL;
    }

    vls_sig_t *s = vls_sig_of(as, 1);
    if (!s) return -VLS_ENOMEM;

    const int64_t want = (int64_t)pid;

    for (;;) {
        for (int i = 0; i < VLS_MAX_CHILDREN; i++) {
            if (!s->child[i].used) continue;
            /* -1 and 0 both mean "any child" here: this system has no
             * process groups, so the group forms degrade to the same
             * answer rather than to a refusal. */
            if (want > 0 && s->child[i].pid != (uint32_t)want) continue;

            const uint32_t got = s->child[i].pid;
            const int32_t  st  = s->child[i].status;
            s->child[i].used = 0;

            if (status_ptr) {
                if (!vls_host.range_mapped(status_ptr, 4, 1)) {
                    vls_host.refuse("wait4: unwritable status");
                    return -VLS_EFAULT;
                }
                if (vls_host.copy_out(status_ptr, &st, 4) != 0)
                    return -VLS_EFAULT;
            }
            return (int64_t)got;
        }

        if (!vls_has_live_child(me)) return -VLS_ECHILD;
        if (options & LNX_WNOHANG) return 0;

        /*
         * A bounded park, re-entered by this loop — the same shape the
         * futex service uses and for the same reason. The wake comes
         * from vls_child_exited on the reaper's thread; the bound is
         * what keeps a missed wake from becoming a thread this kernel
         * can never reap, and re-checking is free because the check is
         * a scan of sixteen slots.
         */
        sched_block_on(s, 50);
    }
}

/* ============================================================
 *  6. the translation table
 * ============================================================ */

typedef uint64_t (*vls_fn_t)(const uint64_t a[6]);

enum {
    VK_NATIVE,   /* forward to a native number, arguments permuted   */
    VK_LOCAL,    /* a function below, because a value has to change  */
    VK_CONST,    /* a constant is the whole and correct answer       */
    VK_ERRNO     /* a fixed refusal that is a real answer, not a gap */
};

/*
 * One row per call. `map[i]` names which Linux argument feeds the i'th
 * native argument, or -1 for zero — so a permutation and a truncation
 * are both expressible and neither needs a function.
 *
 * The rule the advisory review insisted on, and it is the right rule: a
 * row is either a complete translation or it is not in the table.
 * Nothing here forwards a call whose arguments it has only partly
 * understood. `clone` is the reason — its flags word decides what kind
 * of object is being made, and a row that dropped it would have made a
 * process every time a thread was asked for.
 */
typedef struct {
    uint16_t    nr;
    uint8_t     kind;
    uint16_t    native;
    int8_t      map[6];
    int64_t     konst;
    vls_fn_t    fn;
    const char *name;
} vls_call_t;

#define M(a, b, c, d, e, f) { a, b, c, d, e, f }
#define MNONE               { -1, -1, -1, -1, -1, -1 }

/* ---- the local handlers ---- */

static uint64_t vn(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                   uint64_t a3, uint64_t a4, uint64_t a5) {
    return vls_host.native(nr, a0, a1, a2, a3, a4, a5);
}

/* A native refusal of -1 turned into an error number a Linux program can
 * act on. The older half of this kernel's calls answer -1 for every
 * failure; the descriptor calls answer -errno. Both go through here so
 * that the boundary between the two conventions is crossed in one place
 * rather than in thirty. */
static uint64_t vls_norm(uint64_t v, int64_t as_errno) {
    const int64_t sv = (int64_t)v;
    if (sv == -1) return (uint64_t)as_errno;
    return v;
}

static uint64_t l_brk(const uint64_t a[6]) {
    /* Native sbrk answers the *old* break and moves by a delta; Linux
     * brk takes an absolute address and answers the *new* one. A brk
     * that cannot be satisfied returns the break unchanged, which is how
     * a caller detects failure — there is no errno in it. */
    const uint64_t cur = vn(VXN_SBRK, 0, 0, 0, 0, 0, 0);
    if ((int64_t)cur == -1) return 0;
    if (!a[0] || a[0] <= cur) return cur;
    const uint64_t delta = a[0] - cur;
    const uint64_t got = vn(VXN_SBRK, delta, 0, 0, 0, 0, 0);
    if ((int64_t)got == -1) return cur;
    return a[0];
}

static uint64_t l_mmap(const uint64_t a[6]) {
    /* a: addr, length, prot, flags, fd, offset. The protection bits and
     * the flag bits are already this system's, deliberately — see the
     * note beside VX_PROT_READ. What has to be checked rather than
     * translated is the descriptor: a file mapping is not something this
     * kernel can do, and a port that asks for one must be told so
     * instead of handed anonymous zeroes where it expected a file. */
    const int64_t fd = (int64_t)(int32_t)a[4];
    if (fd != -1 && !(a[3] & 0x20u /* MAP_ANONYMOUS */)) {
        vls_host.refuse("mmap: only anonymous mappings exist here");
        return (uint64_t)(int64_t)-VLS_ENODEV;
    }
    return vls_norm(vn(VXN_MMAP, a[0], a[1], a[2], a[3], 0, 0), -VLS_ENOMEM);
}

static uint64_t l_futex(const uint64_t a[6]) {
    /* Linux's operation word carries two modifiers above the operation
     * itself: FUTEX_PRIVATE_FLAG, which says the futex is not shared
     * between processes, and FUTEX_CLOCK_REALTIME, which chooses the
     * clock a timeout is against. Neither changes what this kernel does
     * — every futex here is keyed on the word's *physical* address, so
     * private and shared are handled identically and correctly — so both
     * are masked off rather than refused. */
    const uint64_t op = a[1] & 0x7Full;
    if (op != 0 /* FUTEX_WAIT */ && op != 1 /* FUTEX_WAKE */) {
        uint64_t rep[6] = { a[0], a[1], a[2], a[3], a[4], a[5] };
        if (vls_first_sighting(LNX_futex))
            vls_report("no translation for this futex operation",
                       LNX_futex, "futex", rep);
        return (uint64_t)(int64_t)-VLS_ENOSYS;
    }
    return vls_norm(vn(VXN_FUTEX, a[0], op, a[2], 0, 0, 0), -VLS_EINVAL);
}

static uint64_t l_nanosleep(const uint64_t a[6]) {
    /* Linux passes a timespec; the native call takes milliseconds. The
     * remainder pointer is filled with zero when it is given, which is
     * the truthful answer: nothing interrupts a sleep here, so a sleep
     * that returns has always completed. */
    struct { int64_t sec; int64_t nsec; } ts = { 0, 0 };
    if (!a[0]) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (!vls_host.range_mapped(a[0], 16, 0)) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (vls_host.copy_in(a[0], &ts, 16) != 0) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
        return (uint64_t)(int64_t)-VLS_EINVAL;

    uint64_t ms = (uint64_t)ts.sec * 1000ull + (uint64_t)ts.nsec / 1000000ull;
    /* A sub-millisecond sleep is a yield rather than a no-op: a program
     * that sleeps for a microsecond in a loop is asking to be
     * descheduled, and answering immediately turns it into a spin. */
    if (!ms && (ts.sec || ts.nsec)) ms = 1;
    vn(VXN_NANOSLEEP, ms, 0, 0, 0, 0, 0);

    if (a[1] && vls_host.range_mapped(a[1], 16, 1)) {
        const int64_t zero[2] = { 0, 0 };
        vls_host.copy_out(a[1], zero, 16);
    }
    return 0;
}

static uint64_t l_clock_gettime(const uint64_t a[6]) {
    struct { int64_t sec; int64_t nsec; } ts;
    if (!a[1] || !vls_host.range_mapped(a[1], 16, 1))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    /* CLOCK_REALTIME 0, CLOCK_MONOTONIC 1, and the coarse forms 5 and 6
     * which differ only in precision this system does not have anyway.
     * The two are genuinely different quantities here and are answered
     * from different places: the wall clock came off the CMOS at boot,
     * the monotonic one is the scheduler tick. */
    if (a[0] == 0 || a[0] == 5 || a[0] == 8) {
        const uint64_t secs = vn(VXN_WALLCLOCK, 0, 0, 0, 0, 0, 0);
        const uint64_t ns   = vn(VXN_CLOCK, 0, 0, 0, 0, 0, 0);
        ts.sec  = (int64_t)secs;
        ts.nsec = (int64_t)(ns % 1000000000ull);
    } else if (a[0] == 1 || a[0] == 4 || a[0] == 6 || a[0] == 7) {
        const uint64_t ns = vn(VXN_CLOCK, 0, 0, 0, 0, 0, 0);
        ts.sec  = (int64_t)(ns / 1000000000ull);
        ts.nsec = (int64_t)(ns % 1000000000ull);
    } else {
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }
    if (vls_host.copy_out(a[1], &ts, 16) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;
    return 0;
}

static uint64_t l_gettimeofday(const uint64_t a[6]) {
    if (a[0]) {
        struct { int64_t sec; int64_t usec; } tv;
        if (!vls_host.range_mapped(a[0], 16, 1))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        const uint64_t secs = vn(VXN_WALLCLOCK, 0, 0, 0, 0, 0, 0);
        const uint64_t ns   = vn(VXN_CLOCK, 0, 0, 0, 0, 0, 0);
        tv.sec  = (int64_t)secs;
        tv.usec = (int64_t)((ns % 1000000000ull) / 1000ull);
        if (vls_host.copy_out(a[0], &tv, 16) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
    }
    /* The timezone argument is obsolete on every system that still
     * accepts it, and this one has no zone to report — src/gfx.h reads
     * the CMOS as UTC and nothing records an offset. Zeroing it says
     * "UTC, no daylight rule", which is exactly true here. */
    if (a[1] && vls_host.range_mapped(a[1], 8, 1)) {
        const int32_t tz[2] = { 0, 0 };
        vls_host.copy_out(a[1], tz, 8);
    }
    return 0;
}

static uint64_t l_time(const uint64_t a[6]) {
    const uint64_t secs = vn(VXN_WALLCLOCK, 0, 0, 0, 0, 0, 0);
    if (a[0]) {
        if (!vls_host.range_mapped(a[0], 8, 1))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        if (vls_host.copy_out(a[0], &secs, 8) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
    }
    return secs;
}

/*
 * stat, in Linux's shape.
 *
 * The native call fills a thirty-two byte vx_stat_t; Linux's struct stat
 * is a hundred and forty-four bytes with the fields in fixed places. So
 * the translation is a real one: the native answer is taken into a
 * kernel buffer and re-laid, with the fields this system does not know
 * left at zero rather than invented. st_mode carries the kind and a
 * permission word, and the permission word is a statement about this
 * system rather than a guess — there are no permission bits on the
 * volume, every file is readable and writable by whoever may open it at
 * all, so 0666 and 0777 are what the volume actually offers.
 */
static uint64_t vls_fill_stat(uint64_t out, const uint64_t vst[4]) {
    uint8_t st[144];
    for (int i = 0; i < 144; i++) st[i] = 0;

    const uint64_t size = vst[0];
    const uint64_t ino  = vst[1];
    const uint32_t mode = (uint32_t)vst[2];
    const uint32_t nlnk = (uint32_t)(vst[2] >> 32);
    const uint64_t mtim = vst[3];

    uint32_t lmode = mode & 0170000u;
    lmode |= (mode & 0040000u) ? 0777u : 0666u;

    uint64_t *q = (uint64_t *)(void *)st;
    uint32_t *w = (uint32_t *)(void *)st;
    q[0] = 0;                       /*   0 st_dev                        */
    q[1] = ino;                     /*   8 st_ino                        */
    q[2] = nlnk ? nlnk : 1;         /*  16 st_nlink                      */
    w[6] = lmode;                   /*  24 st_mode                       */
    w[7] = 0;                       /*  28 st_uid                        */
    w[8] = 0;                       /*  32 st_gid                        */
    w[9] = 0;                       /*  36 padding                       */
    q[5] = 0;                       /*  40 st_rdev                       */
    q[6] = size;                    /*  48 st_size                       */
    q[7] = 4096;                    /*  56 st_blksize                    */
    q[8] = (size + 511) / 512;      /*  64 st_blocks                     */
    q[9]  = (int64_t)(mtim / 1000000000ull);   /*  72 st_atime           */
    q[10] = 0;                                 /*  80 st_atime_nsec      */
    q[11] = (int64_t)(mtim / 1000000000ull);   /*  88 st_mtime           */
    q[12] = 0;                                 /*  96                    */
    q[13] = (int64_t)(mtim / 1000000000ull);   /* 104 st_ctime           */
    q[14] = 0;                                 /* 112                    */

    if (!vls_host.range_mapped(out, 144, 1)) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (vls_host.copy_out(out, st, 144) != 0) return (uint64_t)(int64_t)-VLS_EFAULT;
    return 0;
}

/* The native call needs somewhere in *user* memory to put its answer,
 * because that is what its interface is. Borrowing the caller's own
 * output buffer is safe and is what makes this need no scratch mapping:
 * the Linux structure is a hundred and forty-four bytes and the native
 * one is thirty-two, so the native answer lands inside the space the
 * caller already provided and is read back out before the wider
 * structure is written over it. */
static uint64_t vls_stat_via(uint64_t native_nr, uint64_t a0, uint64_t out) {
    if (!out || !vls_host.range_mapped(out, 144, 1))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    const uint64_t rc = vn(native_nr, a0, out, 0, 0, 0, 0);
    if ((int64_t)rc < 0) return rc;

    uint64_t vst[4];
    if (vls_host.copy_in(out, vst, 32) != 0) return (uint64_t)(int64_t)-VLS_EFAULT;
    return vls_fill_stat(out, vst);
}

static uint64_t l_stat(const uint64_t a[6]) {
    return vls_stat_via(VXN_STAT, a[0], a[1]);
}
static uint64_t l_fstat(const uint64_t a[6]) {
    return vls_stat_via(VXN_FSTAT, a[0], a[1]);
}
static uint64_t l_newfstatat(const uint64_t a[6]) {
    if ((int32_t)a[0] != LNX_AT_FDCWD) {
        vls_host.refuse("fstatat: this system has one working directory, "
                        "not one per descriptor");
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    }
    /* AT_EMPTY_PATH (0x1000) with an empty name means "stat the
     * descriptor", which is fstat and is answered as fstat rather than
     * refused. */
    char probe[2] = { 0, 0 };
    if ((a[3] & 0x1000u) && vls_host.copy_string(a[1], probe, 2) >= 0 &&
        probe[0] == '\0')
        return vls_stat_via(VXN_FSTAT, a[0], a[2]);
    return vls_stat_via(VXN_STAT, a[1], a[2]);
}

/*
 * getdents64, repacked.
 *
 * The native call answers fixed-length two-hundred-and-seventy-two byte
 * records and returns a count of them; Linux answers variable-length
 * self-describing records and returns a count of bytes. src/syscall.h
 * says why the native side is fixed — a variable record means the kernel
 * writes a length that user space then trusts to step through a buffer —
 * and that argument does not stop applying because a Linux program
 * expects otherwise. So the repacking happens here, on the kernel side
 * of the boundary, and the lengths a program steps through are ones this
 * function computed rather than ones it was handed.
 *
 * One native record at a time, through a single stack slot, because a
 * buffer large enough for a directory would be a kilobyte of kernel
 * stack per call.
 */
static uint64_t l_getdents64(const uint64_t a[6]) {
    const uint64_t ubuf = a[1];
    const uint64_t ucap = a[2];
    if (!ubuf || !ucap) return (uint64_t)(int64_t)-VLS_EINVAL;
    if (!vls_host.range_mapped(ubuf, ucap, 1)) return (uint64_t)(int64_t)-VLS_EFAULT;

    /* The native record, read into the caller's own buffer for the same
     * reason stat is: it is the only user memory in hand, it is large
     * enough, and it is overwritten by the answer immediately after. */
    if (ucap < 272) return (uint64_t)(int64_t)-VLS_EINVAL;

    uint64_t written = 0;
    for (;;) {
        const uint64_t got = vn(VXN_GETDENTS, a[0], ubuf + written, 272,
                                0, 0, 0);
        if ((int64_t)got < 0) return written ? written : got;
        if (got == 0) break;

        /* size, type, namelen, name[256] */
        uint8_t rec[272];
        if (vls_host.copy_in(ubuf + written, rec, 272) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;

        const uint32_t type    = *(const uint32_t *)(const void *)(rec + 8);
        uint32_t namelen = *(const uint32_t *)(const void *)(rec + 12);
        if (namelen > 255) namelen = 255;

        /* d_ino 8, d_off 8, d_reclen 2, d_type 1, name, NUL, padded to
         * eight so the next record is aligned — which is what a program
         * stepping the buffer with a pointer cast relies on. */
        uint64_t reclen = 19 + namelen + 1;
        reclen = (reclen + 7) & ~7ull;

        if (written + reclen > ucap) {
            /*
             * The buffer is full and one record has already been taken
             * out of the directory. Putting it back is the whole
             * difficulty of this call, and it is done through the one
             * thing that can: the directory's own position, which lseek
             * moves. Without this a caller with a small buffer loses a
             * file per call, silently.
             */
            vn(VXN_LSEEK, a[0], (uint64_t)(int64_t)-1, 1 /* SEEK_CUR */,
               0, 0, 0);
            if (!written) return (uint64_t)(int64_t)-VLS_EINVAL;
            break;
        }

        uint8_t out[272 + 16];
        for (uint64_t i = 0; i < reclen; i++) out[i] = 0;
        *(uint64_t *)(void *)(out + 0)  = 1;              /* d_ino  */
        *(uint64_t *)(void *)(out + 8)  = written + reclen; /* d_off */
        *(uint16_t *)(void *)(out + 16) = (uint16_t)reclen;
        /* VX_DT_DIR is 2 and Linux's DT_DIR is 4; VX_DT_REG is 1 and
         * Linux's DT_REG is 8. Two small numbers that mean different
         * things is exactly the kind of difference a table hides, so it
         * is spelled out. */
        out[18] = (type == 2) ? 4 : (type == 1) ? 8 : 0;
        for (uint32_t i = 0; i < namelen; i++) out[19 + i] = rec[16 + i];

        if (vls_host.copy_out(ubuf + written, out, reclen) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
        written += reclen;
    }
    return written;
}

static uint64_t l_open(const uint64_t a[6]) {
    /* The flags are already this system's numbers; src/syscall.h chose
     * them from Linux's on purpose. What is not shared is O_NOFOLLOW,
     * O_NONBLOCK and O_SYNC, none of which this volume can honour and
     * all of which are harmless to drop: there are no symbolic links, no
     * non-blocking file reads, and every write already reaches the disk
     * through the write-back image at close. */
    return vn(VXN_OPEN, a[0], a[1] & ~0x4800u, a[2], 0, 0, 0);
}

static uint64_t l_openat(const uint64_t a[6]) {
    if ((int32_t)a[0] != LNX_AT_FDCWD) {
        vls_host.refuse("openat: this system has one working directory, "
                        "not one per descriptor");
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    }
    const uint64_t b[6] = { a[1], a[2], a[3], 0, 0, 0 };
    return l_open(b);
}

static uint64_t l_unlinkat(const uint64_t a[6]) {
    if ((int32_t)a[0] != LNX_AT_FDCWD)
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    /* AT_REMOVEDIR selects rmdir, which this system does not have as a
     * separate call: a directory is removed by the same path that
     * removes a file, and the filesystem refuses a non-empty one. */
    return vn(VXN_UNLINK, a[1], 0, 0, 0, 0, 0);
}

static uint64_t l_access(const uint64_t a[6]) {
    /*
     * Answered by opening the file read-only and closing it again —
     * never for write, and that restraint is the whole design of this
     * one.
     *
     * Opening for write is what would actually answer W_OK, and it would
     * do it by putting a prompt on the screen: uac_guard freezes the
     * thread and asks a person whether this program may write this file.
     * A program calling access(path, W_OK) has not decided to write
     * anything yet — it is looking around — and turning a look into a
     * question is how somebody is trained to click yes.
     *
     * So what comes back is whether the file can be reached at all, and
     * the write question is deferred to the open that means it. POSIX
     * is unusually direct about this being acceptable: access() is
     * advisory, its answer may be stale by the time it is used, and a
     * program that acts on it instead of on the open it wants has a race
     * whatever the kernel does.
     *
     * F_OK is 0, X_OK is 1, W_OK is 2, R_OK is 4. X_OK degrades to R_OK
     * because there is no execute bit on this volume — every readable
     * file can be handed to the loader.
     */
    (void)a[1];
    const uint64_t fd = vn(VXN_OPEN, a[0], 0 /* O_RDONLY */, 0, 0, 0, 0);
    if ((int64_t)fd < 0) return fd;
    vn(VXN_CLOSE, fd, 0, 0, 0, 0, 0);
    return 0;
}

static uint64_t l_readlink(const uint64_t a[6]) {
    /*
     * There are no symbolic links on this volume, and that makes both
     * answers here exact rather than approximate: a path that exists is
     * not a link, which is EINVAL, and a path that does not exist is
     * ENOENT. A port that walks a chain of links terminates immediately
     * and correctly on the first EINVAL, which is what it would do on a
     * Linux filesystem with no links in it either.
     */
    const uint64_t fd = vn(VXN_OPEN, a[0], 0, 0, 0, 0, 0);
    if ((int64_t)fd < 0) return (uint64_t)(int64_t)-VLS_ENOENT;
    vn(VXN_CLOSE, fd, 0, 0, 0, 0, 0);
    return (uint64_t)(int64_t)-VLS_EINVAL;
}

static uint64_t l_getcwd(const uint64_t a[6]) {
    /* One working directory per machine rather than per process, and it
     * is the volume root as far as a ring-3 program is concerned: every
     * path a program passes is made absolute against it by fs_abs before
     * anything looks at it. Saying so is better than refusing, because a
     * getcwd that fails stops a program that only wanted to build an
     * absolute path. */
    static const char root[2] = { '/', '\0' };
    if (a[1] < 2) return (uint64_t)(int64_t)-VLS_ERANGE;
    if (!vls_host.range_mapped(a[0], 2, 1)) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (vls_host.copy_out(a[0], root, 2) != 0) return (uint64_t)(int64_t)-VLS_EFAULT;
    return 2;
}

static uint64_t l_uname(const uint64_t a[6]) {
    /*
     * Six sixty-five-byte fields, and what goes in them is a decision
     * rather than a formality. A ported program reads sysname to find
     * out which kernel's behaviour to expect and release to find out
     * whether a call it wants exists; answering "Vextro" would be
     * truthful and would send every one of them down a path written for
     * no system at all. So sysname is Linux, because that is the ABI
     * being spoken, and the version string carries the truth in the one
     * field nothing branches on.
     */
    static const char *field[6] = {
        "Linux",                    /* sysname                        */
        "vextro",                   /* nodename                       */
        "5.15.0-vls",               /* release                        */
        "#1 Vextro Linux Subset",   /* version -- the honest field    */
        "x86_64",                   /* machine                        */
        "(none)"                    /* domainname                     */
    };
    char buf[65 * 6];
    for (int i = 0; i < 65 * 6; i++) buf[i] = 0;
    for (int i = 0; i < 6; i++) {
        const char *s = field[i];
        for (int j = 0; j < 64 && s[j]; j++) buf[i * 65 + j] = s[j];
    }
    if (!a[0] || !vls_host.range_mapped(a[0], sizeof(buf), 1))
        return (uint64_t)(int64_t)-VLS_EFAULT;
    if (vls_host.copy_out(a[0], buf, sizeof(buf)) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;
    return 0;
}

static uint64_t l_arch_prctl(const uint64_t a[6]) {
    if (a[0] == LNX_ARCH_SET_FS)
        return vls_norm(vn(VXN_SET_FSBASE, a[1], 0, 0, 0, 0, 0), -VLS_EPERM);
    if (a[0] == LNX_ARCH_GET_FS) {
        if (!cur_thread) return (uint64_t)(int64_t)-VLS_EPERM;
        if (!vls_host.range_mapped(a[1], 8, 1))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        const uint64_t base = cur_thread->fsbase;
        if (vls_host.copy_out(a[1], &base, 8) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
        return 0;
    }
    /* GS is the Thread Environment Block on this machine and is written
     * once for every process — see sched_set_gs_base. A program that
     * moved it would move it for the Windows subsystem too. */
    return (uint64_t)(int64_t)-VLS_EINVAL;
}

/* ---- the scatter/gather pair ---- */

static uint64_t l_iov(const uint64_t a[6], int writing) {
    const uint64_t iov = a[1];
    const uint64_t cnt = a[2];
    if (cnt > 1024) return (uint64_t)(int64_t)-VLS_EINVAL;
    if (!cnt) return 0;
    if (!vls_host.range_mapped(iov, cnt * 16, 0))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    uint64_t total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t e[2];
        if (vls_host.copy_in(iov + i * 16, e, 16) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
        if (!e[1]) continue;
        const uint64_t rc = vn(writing ? VXN_WRITE : VXN_READ,
                               a[0], e[0], e[1], 0, 0, 0);
        if ((int64_t)rc < 0) return total ? total : rc;
        total += rc;
        /* A short transfer ends the whole call, which is what writev and
         * readv are defined to do: the caller is told how much moved and
         * decides what to do about the rest. Carrying on to the next
         * vector would put later bytes in front of earlier ones. */
        if (rc < e[1]) break;
    }
    return total;
}

static uint64_t l_readv(const uint64_t a[6])  { return l_iov(a, 0); }
static uint64_t l_writev(const uint64_t a[6]) { return l_iov(a, 1); }

/* ---- sockets ---- */

static uint64_t l_socket(const uint64_t a[6]) {
    /* AF_INET is 2 on both sides; SOCK_STREAM is 1 on both. Linux ORs
     * SOCK_NONBLOCK and SOCK_CLOEXEC into the type, and neither is
     * something this kernel's sockets can be — they are always blocking
     * and never inherited across an exec that has no descriptor
     * inheritance rule for them — so a request for either is refused
     * rather than accepted and then not honoured. */
    if (a[1] & ~0xFFull) {
        vls_host.refuse("socket: SOCK_NONBLOCK and SOCK_CLOEXEC are not "
                        "available on this system's sockets");
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }
    return vls_norm(vn(VXN_SOCKET, a[0], a[1], a[2], 0, 0, 0), -VLS_EINVAL);
}

static uint64_t l_connect(const uint64_t a[6]) {
    /*
     * Linux passes a sockaddr; the native call takes four bytes of
     * address and a port in host order, because src/syscall.h decided
     * that a structure two systems can disagree about should not cross a
     * privilege boundary. So the disagreement is resolved here, once,
     * where a mistake is the kernel's rather than a program's:
     * sockaddr_in is sa_family at 0, sin_port at 2 in network order, and
     * sin_addr at 4.
     */
    uint8_t sa[16];
    if (a[2] < 8) return (uint64_t)(int64_t)-VLS_EINVAL;
    if (!vls_host.range_mapped(a[1], 8, 0)) return (uint64_t)(int64_t)-VLS_EFAULT;
    if (vls_host.copy_in(a[1], sa, 8) != 0) return (uint64_t)(int64_t)-VLS_EFAULT;

    const uint16_t family = (uint16_t)(sa[0] | ((uint16_t)sa[1] << 8));
    if (family != 2 /* AF_INET */) {
        vls_host.refuse("connect: only IPv4 exists on this system");
        return (uint64_t)(int64_t)-97 /* EAFNOSUPPORT */;
    }
    const uint16_t port = (uint16_t)((sa[2] << 8) | sa[3]);   /* network order */

    /* The four address bytes have to reach the native call through user
     * memory, because that is its interface. They are already there —
     * inside the caller's own sockaddr, at offset four — so the pointer
     * is simply advanced rather than a scratch buffer found. */
    return vn(VXN_CONNECT, a[0], a[1] + 4, port, 0, 0, 0);
}

static uint64_t l_sendto(const uint64_t a[6]) {
    /* With an address this would be a datagram send, and there are no
     * datagram sockets here. Without one it is send(), which is what
     * every ported stream user actually calls. */
    if (a[4]) {
        vls_host.refuse("sendto: this system has no datagram sockets");
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    }
    return vn(VXN_SEND, a[0], a[1], a[2], a[3], 0, 0);
}

static uint64_t l_recvfrom(const uint64_t a[6]) {
    if (a[4]) {
        vls_host.refuse("recvfrom: this system has no datagram sockets");
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    }
    return vn(VXN_RECV, a[0], a[1], a[2], a[3], 0, 0);
}

static uint64_t l_setsockopt(const uint64_t a[6]) {
    /*
     * The BSD interface names an option by a (level, name) pair whose
     * numbering differs between every two systems that implement it, and
     * src/syscall.h flattened that to three numbers on the native side
     * for exactly that reason. So this is where Linux's pairs are mapped
     * onto them, and an option outside the three is *accepted and
     * ignored* rather than refused — which is the one place in this file
     * that happens and is worth defending: SO_REUSEADDR and TCP_CORK are
     * hints, a program that cannot set them still works, and a program
     * that gets EINVAL from setsockopt usually stops.
     */
    const uint64_t level = a[1], name = a[2];
    uint64_t which = 0;
    if (level == 1 /* SOL_SOCKET */ && name == 20 /* SO_RCVTIMEO */) which = 1;
    else if (level == 1 && name == 21 /* SO_SNDTIMEO */) which = 2;
    else if (level == 6 /* IPPROTO_TCP */ && name == 1 /* TCP_NODELAY */) which = 3;
    else return 0;

    if (which == 3) {
        if (a[4] < 4 || !vls_host.range_mapped(a[3], 4, 0))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        int32_t on = 0;
        if (vls_host.copy_in(a[3], &on, 4) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
        return vls_norm(vn(VXN_SOCKOPT, a[0], which, (uint64_t)(int64_t)on,
                           0, 0, 0), -VLS_EINVAL);
    }
    /* A timeout arrives as a timeval; the native call wants
     * milliseconds. */
    if (a[4] < 16 || !vls_host.range_mapped(a[3], 16, 0))
        return (uint64_t)(int64_t)-VLS_EFAULT;
    int64_t tv[2];
    if (vls_host.copy_in(a[3], tv, 16) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;
    const uint64_t ms = (uint64_t)tv[0] * 1000ull + (uint64_t)(tv[1] / 1000);
    return vls_norm(vn(VXN_SOCKOPT, a[0], which, ms, 0, 0, 0), -VLS_EINVAL);
}

/* ---- descriptors ---- */

static uint64_t l_fcntl(const uint64_t a[6]) {
    switch (a[1]) {
    case 0:  /* F_DUPFD */
    case 1030: /* F_DUPFD_CLOEXEC */
        /*
         * "The lowest free descriptor at or above this number." The
         * native dup hands back the lowest free one, and the lowest free
         * one is never below three because zero, one and two are the
         * console streams of every process. So a minimum of three or
         * less is satisfied exactly; a higher one is not something this
         * table can express and is refused rather than answered with a
         * number below what was asked for.
         */
        if (a[2] > 3) {
            vls_host.refuse("fcntl: F_DUPFD cannot place a descriptor "
                            "above the lowest free one");
            return (uint64_t)(int64_t)-VLS_EINVAL;
        }
        return vls_norm(vn(VXN_DUP, a[0], 0, 0, 0, 0, 0), -VLS_EBADF);
    case 1:  /* F_GETFD */
    case 2:  /* F_SETFD -- close-on-exec, which the descriptor already
              * carries; setting it is honoured through DUP2's own path
              * and reading it back is what a port checks. */
        return 0;
    case 3:  /* F_GETFL */
        return 2; /* O_RDWR: the honest answer for a descriptor this
                   * system opened, since the access mode is not stored
                   * anywhere a program can be told about separately. */
    case 4:  /* F_SETFL */
        return 0;
    default:
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }
}

/* ---- process ---- */

static uint64_t l_getpid(const uint64_t a[6]) {
    (void)a; return vls_host.pid ? vls_host.pid() : 0;
}
static uint64_t l_getppid(const uint64_t a[6]) {
    (void)a; return vls_host.ppid ? vls_host.ppid() : 0;
}
static uint64_t l_getuid(const uint64_t a[6]) {
    (void)a;
    /*
     * Not zero, and that is the whole point of answering at all.
     *
     * A great deal of ported code asks getuid() and takes "0" to mean
     * "this process may do anything" — skipping a permission check,
     * writing to a system directory, declining to drop privilege. None
     * of that is true here: every process starts with
     * UAC_TOKEN_RESTRICTED whoever launched it, and the account
     * identifier is what actually decides what it may touch. So the
     * account identifier is what comes back, and it is never zero.
     */
    return vls_host.uid ? vls_host.uid() : 1;
}

static uint64_t l_gettid(const uint64_t a[6]) {
    (void)a; return cur_thread ? cur_thread->pid : 0;
}

static uint64_t l_set_tid_address(const uint64_t a[6]) {
    addr_space_t *as = vmm_current;
    vls_sig_t *s = vls_sig_of(as, 1);
    if (s) s->clear_child_tid = a[0];
    return cur_thread ? cur_thread->pid : 0;
}

static uint64_t l_kill(const uint64_t a[6]) {
    const int64_t pid = (int64_t)a[0];
    const int     sig = (int)a[1];
    if (sig < 0 || sig >= VLS_NSIG) return (uint64_t)(int64_t)-VLS_EINVAL;

    /* Negative and zero pids name process groups, which this system does
     * not have. Refusing is better than treating them as "everybody":
     * kill(0, SIGTERM) meaning "every process on the machine" is how a
     * port takes the desktop down with it. */
    if (pid <= 0) {
        vls_host.refuse("kill: this system has no process groups");
        return (uint64_t)(int64_t)-VLS_EOPNOTSUPP;
    }
    addr_space_t *target = vls_find_process((uint32_t)pid);
    if (!target) return (uint64_t)(int64_t)-VLS_ESRCH;

    /* Same account or nothing. There is no other check available and
     * this is the right one: sid is which account owns the process, and
     * a program signalling another account's process is the thing a
     * permission model exists to stop. */
    addr_space_t *me = vmm_current;
    if (me && target->sid != me->sid) {
        vls_host.refuse("kill: that process belongs to another account");
        return (uint64_t)(int64_t)-VLS_EPERM;
    }
    return (uint64_t)(int64_t)vls_signal_post(target, sig, 0);
}

static uint64_t l_tkill(const uint64_t a[6]) {
    /*
     * Directed at a thread rather than a process. The disposition is
     * still the process's — POSIX puts handlers on the process — so what
     * `tkill` actually buys is that the *right* thread is the one nudged
     * out of a sleep. Since delivery here happens at whichever thread
     * next crosses a boundary, this posts to the thread's process and
     * says so once rather than pretending to a precision it does not
     * have.
     */
    thread_t *t = vls_find_thread((uint32_t)a[0]);
    if (!t || !t->as) return (uint64_t)(int64_t)-VLS_ESRCH;
    const int sig = (int)a[1];
    if (sig < 0 || sig >= VLS_NSIG) return (uint64_t)(int64_t)-VLS_EINVAL;
    return (uint64_t)(int64_t)vls_signal_post(t->as, sig, 0);
}

static uint64_t l_tgkill(const uint64_t a[6]) {
    const uint64_t b[6] = { a[1], a[2], 0, 0, 0, 0 };
    return l_tkill(b);
}

static uint64_t l_rt_sigaction(const uint64_t a[6]) {
    const int sig = (int)a[0];
    if (sig <= 0 || sig >= VLS_NSIG) return (uint64_t)(int64_t)-VLS_EINVAL;
    /* The mask argument is a sigset_t and Linux checks its size. Eight
     * bytes is the only value any libc passes and the only one the
     * structure above can hold. */
    if (a[3] && a[3] != 8) return (uint64_t)(int64_t)-VLS_EINVAL;

    addr_space_t *as = vmm_current;
    vls_sig_t *s = vls_sig_of(as, 1);
    if (!s) return (uint64_t)(int64_t)-VLS_ENOMEM;

    if (a[2]) {
        if (!vls_host.range_mapped(a[2], sizeof(vls_sigaction_t), 1))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        if (vls_host.copy_out(a[2], &s->act[sig], sizeof(vls_sigaction_t)) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
    }
    if (!a[1]) return 0;

    if (VLS_SIGBIT(sig) & VLS_SIG_UNBLOCKABLE) {
        vls_host.refuse("sigaction: SIGKILL and SIGSTOP cannot be caught");
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }
    if (!vls_host.range_mapped(a[1], sizeof(vls_sigaction_t), 0))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    vls_sigaction_t na;
    if (vls_host.copy_in(a[1], &na, sizeof(na)) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;
    if (na.handler != VLS_SIG_DFL && na.handler != VLS_SIG_IGN &&
        !vls_host.range_ok(na.handler, 1)) {
        vls_host.refuse("sigaction: the handler is outside user space");
        return (uint64_t)(int64_t)-VLS_EFAULT;
    }
    na.mask &= ~VLS_SIG_UNBLOCKABLE;
    s->act[sig] = na;

    /* A signal that has just been told to be ignored is not delivered
     * later because it happened to be pending already. POSIX requires
     * this and it is not obvious: without it, SIG_IGN installed at
     * startup still fires once. */
    if (na.handler == VLS_SIG_IGN) s->pending &= ~VLS_SIGBIT(sig);
    return 0;
}

static uint64_t l_rt_sigprocmask(const uint64_t a[6]) {
    if (a[3] && a[3] != 8) return (uint64_t)(int64_t)-VLS_EINVAL;
    addr_space_t *as = vmm_current;
    vls_sig_t *s = vls_sig_of(as, 1);
    if (!s) return (uint64_t)(int64_t)-VLS_ENOMEM;

    if (a[2]) {
        if (!vls_host.range_mapped(a[2], 8, 1))
            return (uint64_t)(int64_t)-VLS_EFAULT;
        if (vls_host.copy_out(a[2], &s->blocked, 8) != 0)
            return (uint64_t)(int64_t)-VLS_EFAULT;
    }
    if (!a[1]) return 0;
    if (!vls_host.range_mapped(a[1], 8, 0))
        return (uint64_t)(int64_t)-VLS_EFAULT;

    uint64_t set = 0;
    if (vls_host.copy_in(a[1], &set, 8) != 0)
        return (uint64_t)(int64_t)-VLS_EFAULT;

    switch (a[0]) {
    case VLS_SIG_BLOCK:   s->blocked |= set;  break;
    case VLS_SIG_UNBLOCK: s->blocked &= ~set; break;
    case VLS_SIG_SETMASK: s->blocked  = set;  break;
    default: return (uint64_t)(int64_t)-VLS_EINVAL;
    }
    s->blocked &= ~VLS_SIG_UNBLOCKABLE;
    return 0;
}

static uint64_t l_rt_sigreturn(const uint64_t a[6]) {
    return vls_sigreturn(a[0]);
}

static uint64_t l_wait4(const uint64_t a[6]) {
    return (uint64_t)vls_wait4(a[0], a[1], a[2], a[3]);
}

static uint64_t l_execve(const uint64_t a[6]) {
    return (uint64_t)vls_host.execve(a[0], a[1], a[2]);
}

/*
 * clone, which is where a translation layer either does the work or
 * admits it cannot.
 *
 * Linux's clone makes anything from a process to a thread depending on
 * one flags word, and the child returns from a call it never made — at
 * the parent's instruction, with RAX zero and a stack of its own. Native
 * SYS_CLONE cannot express that: it takes an entry point, because every
 * thread this system had before today was started by naming a function.
 *
 * So this reads the flags and dispatches to whichever of the two things
 * this kernel really has, and refuses the rest by name. The combination
 * that matters is the one glibc and musl use for pthread_create, and it
 * is checked for rather than assumed.
 */
static uint64_t l_clone(const uint64_t a[6]) {
    const uint32_t flags = (uint32_t)a[0];
    const uint64_t stack = a[1];
    const uint64_t tls   = a[4];

    /* No CLONE_VM: this is a fork, whatever else was asked for. The
     * exit signal in the low byte is the only other thing that can be
     * honoured, and SIGCHLD is what fork means. */
    if (!(flags & LNX_CLONE_VM)) {
        if ((flags & ~(LNX_CSIGNAL | LNX_CLONE_CHILD_CLEARTID |
                       LNX_CLONE_CHILD_SETTID | LNX_CLONE_PARENT_SETTID)) != 0) {
            uint64_t rep[6] = { a[0], a[1], a[2], a[3], a[4], a[5] };
            if (vls_first_sighting(LNX_clone))
                vls_report("clone without CLONE_VM asks for sharing this "
                           "system cannot give a separate address space",
                           LNX_clone, "clone", rep);
            return (uint64_t)(int64_t)-VLS_ENOSYS;
        }
        return vls_norm(vn(VXN_FORK, 0, 0, 0, 0, 0, 0), -VLS_EAGAIN);
    }

    /*
     * A thread. Everything a thread must share is shared here by
     * construction — one address space means one heap, one set of
     * globals, one descriptor table and one set of signal dispositions —
     * so the flags that ask for exactly that are honoured, and a flag
     * asking for something *not* implied by sharing an address space is
     * refused rather than ignored.
     */
    const uint32_t understood =
        LNX_CLONE_VM | LNX_CLONE_FS | LNX_CLONE_FILES | LNX_CLONE_SIGHAND |
        LNX_CLONE_THREAD | LNX_CLONE_SYSVSEM | LNX_CLONE_SETTLS |
        LNX_CLONE_PARENT_SETTID | LNX_CLONE_CHILD_CLEARTID |
        LNX_CLONE_CHILD_SETTID | LNX_CLONE_DETACHED | LNX_CLONE_IO;
    if (flags & ~understood) {
        uint64_t rep[6] = { a[0], a[1], a[2], a[3], a[4], a[5] };
        if (vls_first_sighting(LNX_clone))
            vls_report("clone flags outside the subset", LNX_clone,
                       "clone", rep);
        return (uint64_t)(int64_t)-VLS_ENOSYS;
    }
    if (!stack) {
        vls_host.refuse("clone: a thread must be given a stack");
        return (uint64_t)(int64_t)-VLS_EINVAL;
    }
    if (!cur_thread || !vmm_current)
        return (uint64_t)(int64_t)-VLS_EPERM;
    if (!vls_host.range_ok(stack, 1) ||
        !vls_host.range_mapped(stack - 16, 16, 1)) {
        vls_host.refuse("clone: the new thread's stack is not writable");
        return (uint64_t)(int64_t)-VLS_EFAULT;
    }

    thread_t *t = sched_clone_thread(cur_thread, stack,
                                     (flags & LNX_CLONE_SETTLS) ? tls : 0);
    if (!t) return (uint64_t)(int64_t)-VLS_EAGAIN;

    /* The child's tid, where the parent asked for it. CLONE_CHILD_SETTID
     * writes the same value into the child's address space, which is the
     * same address space, so one write serves both. */
    if (flags & LNX_CLONE_PARENT_SETTID) {
        const int32_t tid = (int32_t)t->pid;
        if (vls_host.range_mapped(a[2], 4, 1))
            vls_host.copy_out(a[2], &tid, 4);
    }
    if (flags & LNX_CLONE_CHILD_SETTID) {
        const int32_t tid = (int32_t)t->pid;
        if (vls_host.range_mapped(a[3], 4, 1))
            vls_host.copy_out(a[3], &tid, 4);
    }
    return t->pid;
}

/* ============================================================
 *  7. the table itself
 * ============================================================
 *
 * Sorted by number, searched by halving. The order is checked at boot
 * rather than trusted, because a row inserted in the wrong place would
 * make a binary search miss calls that are present — which looks
 * exactly like a call that was never implemented and is a great deal
 * harder to find.
 */
static const vls_call_t vls_table[] = {
{ LNX_read,          VK_NATIVE, VXN_READ,      M(0,1,2,-1,-1,-1), 0, 0, "read" },
{ LNX_write,         VK_NATIVE, VXN_WRITE,     M(0,1,2,-1,-1,-1), 0, 0, "write" },
{ LNX_open,          VK_LOCAL,  0,             MNONE, 0, l_open,   "open" },
{ LNX_close,         VK_NATIVE, VXN_CLOSE,     M(0,-1,-1,-1,-1,-1), 0, 0, "close" },
{ LNX_stat,          VK_LOCAL,  0,             MNONE, 0, l_stat,   "stat" },
{ LNX_fstat,         VK_LOCAL,  0,             MNONE, 0, l_fstat,  "fstat" },
{ LNX_lstat,         VK_LOCAL,  0,             MNONE, 0, l_stat,   "lstat" },
{ LNX_lseek,         VK_NATIVE, VXN_LSEEK,     M(0,1,2,-1,-1,-1), 0, 0, "lseek" },
{ LNX_mmap,          VK_LOCAL,  0,             MNONE, 0, l_mmap,   "mmap" },
{ LNX_mprotect,      VK_NATIVE, VXN_MPROTECT,  M(0,1,2,-1,-1,-1), 0, 0, "mprotect" },
{ LNX_munmap,        VK_NATIVE, VXN_MUNMAP,    M(0,1,-1,-1,-1,-1), 0, 0, "munmap" },
{ LNX_brk,           VK_LOCAL,  0,             MNONE, 0, l_brk,    "brk" },
{ LNX_rt_sigaction,  VK_LOCAL,  0,             MNONE, 0, l_rt_sigaction, "rt_sigaction" },
{ LNX_rt_sigprocmask,VK_LOCAL,  0,             MNONE, 0, l_rt_sigprocmask, "rt_sigprocmask" },
{ LNX_rt_sigreturn,  VK_LOCAL,  0,             MNONE, 0, l_rt_sigreturn, "rt_sigreturn" },
/* Every terminal question a program can ask this kernel has the same
 * answer, and ENOTTY is that answer rather than ENOSYS: a descriptor
 * here is a file, a socket or a device shim, and none of them is a
 * terminal. A port checks isatty() early and takes ENOTTY as "not a
 * terminal, carry on", where ENOSYS makes it conclude the system is
 * broken. */
{ LNX_ioctl,         VK_ERRNO,  0,             MNONE, -VLS_ENOTTY, 0, "ioctl" },
{ LNX_readv,         VK_LOCAL,  0,             MNONE, 0, l_readv,  "readv" },
{ LNX_writev,        VK_LOCAL,  0,             MNONE, 0, l_writev, "writev" },
{ LNX_access,        VK_LOCAL,  0,             MNONE, 0, l_access, "access" },
{ LNX_sched_yield,   VK_NATIVE, VXN_YIELD,     MNONE, 0, 0, "sched_yield" },
{ LNX_dup,           VK_NATIVE, VXN_DUP,       M(0,-1,-1,-1,-1,-1), 0, 0, "dup" },
{ LNX_dup2,          VK_NATIVE, VXN_DUP2,      M(0,1,-1,-1,-1,-1), 0, 0, "dup2" },
{ LNX_nanosleep,     VK_LOCAL,  0,             MNONE, 0, l_nanosleep, "nanosleep" },
{ LNX_getpid,        VK_LOCAL,  0,             MNONE, 0, l_getpid, "getpid" },
{ LNX_socket,        VK_LOCAL,  0,             MNONE, 0, l_socket, "socket" },
{ LNX_connect,       VK_LOCAL,  0,             MNONE, 0, l_connect, "connect" },
{ LNX_sendto,        VK_LOCAL,  0,             MNONE, 0, l_sendto, "sendto" },
{ LNX_recvfrom,      VK_LOCAL,  0,             MNONE, 0, l_recvfrom, "recvfrom" },
{ LNX_shutdown,      VK_NATIVE, VXN_SHUTDOWN,  M(0,1,-1,-1,-1,-1), 0, 0, "shutdown" },
{ LNX_setsockopt,    VK_LOCAL,  0,             MNONE, 0, l_setsockopt, "setsockopt" },
{ LNX_clone,         VK_LOCAL,  0,             MNONE, 0, l_clone,  "clone" },
{ LNX_fork,          VK_NATIVE, VXN_FORK,      MNONE, 0, 0, "fork" },
/* vfork is a fork whose parent is suspended until the child execs, and
 * suspending the parent is an optimisation for a system with expensive
 * page-table copies. This one has copy-on-write, so a plain fork is the
 * same thing without the hazard, and answering with one is correct
 * rather than approximate. */
{ LNX_vfork,         VK_NATIVE, VXN_FORK,      MNONE, 0, 0, "vfork" },
{ LNX_execve,        VK_LOCAL,  0,             MNONE, 0, l_execve, "execve" },
{ LNX_exit,          VK_NATIVE, VXN_THREAD_EXIT, M(0,-1,-1,-1,-1,-1), 0, 0, "exit" },
{ LNX_wait4,         VK_LOCAL,  0,             MNONE, 0, l_wait4,  "wait4" },
{ LNX_kill,          VK_LOCAL,  0,             MNONE, 0, l_kill,   "kill" },
{ LNX_uname,         VK_LOCAL,  0,             MNONE, 0, l_uname,  "uname" },
{ LNX_fcntl,         VK_LOCAL,  0,             MNONE, 0, l_fcntl,  "fcntl" },
{ LNX_fsync,         VK_NATIVE, VXN_FSYNC,     M(0,-1,-1,-1,-1,-1), 0, 0, "fsync" },
{ LNX_ftruncate,     VK_NATIVE, VXN_FTRUNCATE, M(0,1,-1,-1,-1,-1), 0, 0, "ftruncate" },
{ LNX_getcwd,        VK_LOCAL,  0,             MNONE, 0, l_getcwd, "getcwd" },
{ LNX_mkdir,         VK_NATIVE, VXN_MKDIR,     M(0,-1,-1,-1,-1,-1), 0, 0, "mkdir" },
{ LNX_rmdir,         VK_NATIVE, VXN_UNLINK,    M(0,-1,-1,-1,-1,-1), 0, 0, "rmdir" },
{ LNX_unlink,        VK_NATIVE, VXN_UNLINK,    M(0,-1,-1,-1,-1,-1), 0, 0, "unlink" },
{ LNX_readlink,      VK_LOCAL,  0,             MNONE, 0, l_readlink, "readlink" },
{ LNX_gettimeofday,  VK_LOCAL,  0,             MNONE, 0, l_gettimeofday, "gettimeofday" },
{ LNX_getuid,        VK_LOCAL,  0,             MNONE, 0, l_getuid, "getuid" },
{ LNX_getgid,        VK_LOCAL,  0,             MNONE, 0, l_getuid, "getgid" },
{ LNX_geteuid,       VK_LOCAL,  0,             MNONE, 0, l_getuid, "geteuid" },
{ LNX_getegid,       VK_LOCAL,  0,             MNONE, 0, l_getuid, "getegid" },
{ LNX_getppid,       VK_LOCAL,  0,             MNONE, 0, l_getppid, "getppid" },
/* An alternate signal stack is what a program uses to survive a stack
 * overflow, and this kernel cannot give it one: the frame is laid on the
 * interrupted stack by vls_build_frame and there is nowhere else to put
 * it. ENOSYS rather than a recorded-and-ignored stack, because a program
 * told its alternate stack was accepted will rely on it exactly once. */
{ LNX_sigaltstack,   VK_ERRNO,  0,             MNONE, -VLS_ENOSYS, 0, "sigaltstack" },
{ LNX_arch_prctl,    VK_LOCAL,  0,             MNONE, 0, l_arch_prctl, "arch_prctl" },
{ LNX_gettid,        VK_LOCAL,  0,             MNONE, 0, l_gettid, "gettid" },
{ LNX_tkill,         VK_LOCAL,  0,             MNONE, 0, l_tkill,  "tkill" },
{ LNX_time,          VK_LOCAL,  0,             MNONE, 0, l_time,   "time" },
{ LNX_futex,         VK_LOCAL,  0,             MNONE, 0, l_futex,  "futex" },
{ LNX_getdents64,    VK_LOCAL,  0,             MNONE, 0, l_getdents64, "getdents64" },
{ LNX_set_tid_address, VK_LOCAL, 0,            MNONE, 0, l_set_tid_address, "set_tid_address" },
{ LNX_clock_gettime, VK_LOCAL,  0,             MNONE, 0, l_clock_gettime, "clock_gettime" },
{ LNX_exit_group,    VK_NATIVE, VXN_EXIT_GROUP, M(0,-1,-1,-1,-1,-1), 0, 0, "exit_group" },
{ LNX_tgkill,        VK_LOCAL,  0,             MNONE, 0, l_tgkill, "tgkill" },
{ LNX_openat,        VK_LOCAL,  0,             MNONE, 0, l_openat, "openat" },
{ LNX_newfstatat,    VK_LOCAL,  0,             MNONE, 0, l_newfstatat, "newfstatat" },
{ LNX_unlinkat,      VK_LOCAL,  0,             MNONE, 0, l_unlinkat, "unlinkat" },
{ LNX_getrandom,     VK_NATIVE, VXN_RANDOM,    M(0,1,-1,-1,-1,-1), 0, 0, "getrandom" },
};

#define VLS_TABLE_N ((int)(sizeof(vls_table) / sizeof(vls_table[0])))

static const vls_call_t *vls_lookup(uint64_t nr) {
    int lo = 0, hi = VLS_TABLE_N - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        if (vls_table[mid].nr == nr) return &vls_table[mid];
        if (vls_table[mid].nr < nr) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* ============================================================
 *  8. the router
 * ============================================================ */

uint64_t vls_syscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                     uint64_t a3, uint64_t a4, uint64_t a5) {
    const uint64_t a[6] = { a0, a1, a2, a3, a4, a5 };

    if (!vls_ready) {
        serial_puts("[VLS] a linux call arrived before the subset was "
                    "initialised\n");
        return (uint64_t)(int64_t)-VLS_ENOSYS;
    }

    const vls_call_t *c = vls_lookup(nr);
    if (!c) {
        vls_refused++;
        if (vls_first_sighting(nr))
            vls_report("no translation", nr, 0, a);
        /*
         * ENOSYS and not a halt, which is the whole point of the layer.
         * A program that finds a call missing takes its own fallback —
         * and the operator gets one line naming what to build next,
         * which is how the list of things this subset still lacks is
         * written by the programs that need them rather than guessed at
         * in advance.
         */
        return (uint64_t)(int64_t)-VLS_ENOSYS;
    }

    vls_translated++;

    switch (c->kind) {
    case VK_NATIVE: {
        uint64_t n[6];
        for (int i = 0; i < 6; i++)
            n[i] = (c->map[i] >= 0) ? a[c->map[i]] : 0;
        return vls_host.native(c->native, n[0], n[1], n[2], n[3], n[4], n[5]);
    }
    case VK_LOCAL:
        /*
         * Six arguments and nothing else. The two handlers that need the
         * caller's register file — sigreturn, which restores one, and
         * clone, which copies one — reach syscall_cur_frame directly,
         * because it is a global that is valid for exactly the duration
         * of a system call and threading a pointer to it through sixty
         * handlers that ignore it would be worse in every way.
         */
        return c->fn(a);
    case VK_CONST:
        return (uint64_t)c->konst;
    case VK_ERRNO:
    default:
        if (vls_first_sighting(nr))
            vls_report("answered with a fixed refusal", nr, c->name, a);
        return (uint64_t)c->konst;
    }
}

/* ============================================================
 *  9. starting up
 * ============================================================ */

void vls_init(void) {
    /*
     * Two checks, both of which have to happen before the first Linux
     * call rather than at it.
     *
     * The hooks, because a null one is a jump to zero inside a system
     * call, in a kernel with no null-page mapping — which is a page
     * fault in ring 0 and a halted machine, several seconds after the
     * mistake that caused it.
     *
     * And the table's order, because a binary search over an unsorted
     * table does not fail: it silently misses rows, and a call that is
     * present but unreachable is indistinguishable from one that was
     * never written.
     */
    if (!vls_host.native || !vls_host.range_ok || !vls_host.range_mapped ||
        !vls_host.copy_in || !vls_host.copy_out || !vls_host.copy_string ||
        !vls_host.execve || !vls_host.sigreturn_va || !vls_host.refuse ||
        !vls_host.pid || !vls_host.ppid || !vls_host.uid) {
        serial_puts("[VLS] the host hook table is incomplete; the subset "
                    "is not available\n");
        return;
    }

    for (int i = 1; i < VLS_TABLE_N; i++) {
        if (vls_table[i - 1].nr >= vls_table[i].nr) {
            serial_puts("[VLS] the call table is out of order at ");
            serial_puts(vls_table[i].name);
            serial_puts("; the subset is not available\n");
            return;
        }
    }

    vls_ready = 1;
    serial_puts("[VLS] linux subset up: ");
    serial_put_dec((uint32_t)VLS_TABLE_N);
    serial_puts(" calls translated, signals 1-64, bias 0x40000000\n");
}
