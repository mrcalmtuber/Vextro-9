#ifndef TRAP_H
#define TRAP_H

/*
 * src/trap.h — what happens when the processor objects.
 *
 * The previous answer was two lines long: vectors that push an error
 * code halted the machine forever, and every other vector returned
 * silently as though nothing had happened. Both were defensible when
 * nothing could fault except the kernel itself and a fault meant the
 * kernel was already wrong. Neither survives ring 3, where a faulting
 * program is an ordinary event that must not take the machine with it,
 * and where "the screen froze" is the only diagnosis a halt provides.
 *
 * So: every exception is reported with the things that actually identify
 * it — vector, error code, the faulting address for a page fault, the
 * instruction pointer, and which thread was running — and then one of
 * two things happens.
 *
 * A fault from ring 3 kills the thread and nothing else. It cannot
 * simply return, because IRETQ would go straight back to the
 * instruction that faulted and fault again forever; so the frame is
 * rewritten to return into the kernel instead, on the thread's own
 * kernel stack, at a function that asks the scheduler to let it go.
 * Rewriting the frame rather than switching stacks by hand means the one
 * path out of an interrupt is still the only path out.
 *
 * A fault from ring 0 is a bug in this kernel and stops it, with
 * everything known printed first.
 */

#include <stdint.h>
#include "gdt.h"
#include "sched/sched.h"
/* A ring-3 fault is the second of the two places a caught signal can be
 * delivered, and the only place a *synchronous* one can be. */
#include "vls.h"

typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;
    uint64_t rip, cs, rflags, rsp, ss;
} exc_frame_t;

/* The stubs differ only in whether the processor pushed an error code.
 * Where it did not, one is pushed for it, so that everything downstream
 * sees a frame of one shape. */
#define EXC_STUB_NOERR(n)                    \
    ".align 16\n"                            \
    ".globl exc_stub_" #n "\n"               \
    "exc_stub_" #n ":\n"                     \
    "  pushq $0\n"                           \
    "  pushq $" #n "\n"                      \
    "  jmp exc_common\n"

#define EXC_STUB_ERR(n)                      \
    ".align 16\n"                            \
    ".globl exc_stub_" #n "\n"               \
    "exc_stub_" #n ":\n"                     \
    "  pushq $" #n "\n"                      \
    "  jmp exc_common\n"

__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    EXC_STUB_NOERR(0)  EXC_STUB_NOERR(1)  EXC_STUB_NOERR(2)
    EXC_STUB_NOERR(3)  EXC_STUB_NOERR(4)  EXC_STUB_NOERR(5)
    EXC_STUB_NOERR(6)  EXC_STUB_NOERR(7)
    EXC_STUB_ERR(8)
    EXC_STUB_NOERR(9)
    EXC_STUB_ERR(10)   EXC_STUB_ERR(11)   EXC_STUB_ERR(12)
    EXC_STUB_ERR(13)   EXC_STUB_ERR(14)
    EXC_STUB_NOERR(15) EXC_STUB_NOERR(16)
    EXC_STUB_ERR(17)
    EXC_STUB_NOERR(18) EXC_STUB_NOERR(19) EXC_STUB_NOERR(20)
    EXC_STUB_ERR(21)
    EXC_STUB_ERR(29)   EXC_STUB_ERR(30)

    ".align 16\n"
    "exc_common:\n"
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
    /*
     * ---- and the vector registers ----
     *
     * The fifteen pushes above are the general-purpose set, and for most
     * of this kernel's life that was the whole of a thread's state that
     * an exception could disturb. It is not, and the difference is
     * invisible until the day it is not.
     *
     * exception_handle is ordinary C, compiled with SSE available. It
     * uses vector registers — a structure copy, a zeroing loop the
     * compiler turned into stores of sixteen bytes at a time — and it
     * does not put them back. That is exactly what the syscall path
     * already accounts for, and there it is accounted for on the *other*
     * side: apps/vextro.h declares all sixteen XMM registers clobbered,
     * because the program asked for the system call and knows where it
     * is.
     *
     * A fault is not asked for. It happens in the middle of an
     * instruction the program never annotated and cannot annotate, and
     * the processor re-executes that instruction on return. If the
     * instruction was a vector store — which is what GCC compiles a
     * loop that fills an array into — then it re-executes with a source
     * register the handler has since overwritten, and stores whatever
     * the handler left there.
     *
     * The symptom is a program that writes an array and finds the first
     * few elements correct and the rest wrong, with no fault, no
     * refusal, and nothing on the wire. That is how this was found: a
     * lazily backed mapping faults *inside* such a loop by design, so
     * the case that used to require a copy-on-write page in exactly the
     * wrong place is now the ordinary one.
     *
     * The save area is on this exception's own kernel stack, sixteen-
     * aligned because FXSAVE64 faults on anything else. RBP carries the
     * unaligned stack pointer across, and RBP's own value is already
     * safe in the frame above.
     */
    "  movq %rsp, %rbp\n"
    "  subq $512, %rsp\n"
    "  andq $-16, %rsp\n"
    "  fxsave64 (%rsp)\n"
    "  call exception_handle\n"
    "  fxrstor64 (%rsp)\n"
    "  movq %rbp, %rsp\n"
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
    "  addq $16, %rsp\n"        /* vector and error code */
    "  iretq\n"
    ".popsection\n"
);

extern void exc_stub_0(void),  exc_stub_1(void),  exc_stub_2(void);
extern void exc_stub_3(void),  exc_stub_4(void),  exc_stub_5(void);
extern void exc_stub_6(void),  exc_stub_7(void),  exc_stub_8(void);
extern void exc_stub_9(void),  exc_stub_10(void), exc_stub_11(void);
extern void exc_stub_12(void), exc_stub_13(void), exc_stub_14(void);
extern void exc_stub_15(void), exc_stub_16(void), exc_stub_17(void);
extern void exc_stub_18(void), exc_stub_19(void), exc_stub_20(void);
extern void exc_stub_21(void), exc_stub_29(void), exc_stub_30(void);

static const char *exc_name(uint64_t v) {
    switch (v) {
    case 0:  return "divide error";
    case 1:  return "debug";
    case 2:  return "non-maskable interrupt";
    case 3:  return "breakpoint";
    case 4:  return "overflow";
    case 5:  return "bound range exceeded";
    case 6:  return "invalid opcode";
    case 7:  return "device not available";
    case 8:  return "double fault";
    case 10: return "invalid TSS";
    case 11: return "segment not present";
    case 12: return "stack-segment fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "x87 floating-point error";
    case 17: return "alignment check";
    case 18: return "machine check";
    case 19: return "SIMD floating-point error";
    case 21: return "control protection";
    case 29: return "VMM communication";
    case 30: return "security exception";
    default: return "exception";
    }
}

static void serial_put_hex64(uint64_t v) {
    serial_puts("0x");
    for (int i = 15; i >= 0; i--) {
        uint8_t nib = (uint8_t)((v >> (i * 4)) & 0xF);
        serial_putc((char)(nib < 10 ? '0' + nib : 'a' + nib - 10));
    }
}

/*
 * Structured exception handling, if the faulting thread is running a PE
 * that has any.
 *
 * A hook rather than a call, because the tables live in the staged image
 * that src/desktop.h owns and this file is included long before it. Set
 * by win_seh_install(); null on a system that has never loaded a PE.
 *
 * Returns 1 and fills *resume with an absolute address to continue at.
 */
static int (*trap_seh_hook)(uint64_t rip, int vector, uint64_t *resume) = 0;

/* The pager. See the call site in exception_handle for why this is
 * declared here and defined in swap.h. */
static int swap_handle_fault(uint64_t cr2, uint64_t error);

/* Where a killed user thread lands. It runs in ring 0 on its own kernel
 * stack with a frame the handler wrote, and never returns. */
__attribute__((used))
void trap_thread_died(void);

void trap_thread_died(void) {
    sched_exit(-1);
    for (;;) __asm__ volatile("hlt");
}

__attribute__((used))
void exception_handle(exc_frame_t *f);

void exception_handle(exc_frame_t *f) {
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

    int from_user = (f->cs & 3) == 3;

    /*
     * A write to a copy-on-write page is not an error. It is the whole
     * mechanism: the page was made read-only on purpose so that the
     * first write would arrive here and be given a private copy.
     *
     * Checked before anything is printed, because on a process that
     * forked and then ran, this is the common case and reporting it
     * would bury every real fault in noise.
     */
    if (f->vector == 14 && (f->error & 1) && (f->error & 2)) {
        if (vmm_resolve_cow(vmm_current, cr2)) return;
    }

    /*
     * Nor is a fault on a page that was swapped out. It is the pager
     * being asked to do the one thing it exists for.
     *
     * The two conditions cannot overlap: copy-on-write is a *protection*
     * fault, error bit 0 set, on a page that is present; this is bit 0
     * clear, on a page that is not. The processor distinguishes them
     * before either line runs.
     *
     * Checked here, before anything is printed, for the same reason the
     * copy-on-write case is: on a machine that is paging this is the
     * common outcome of a fault, not an error, and reporting each one
     * would bury every genuine fault in a scroll of them.
     *
     * swap_handle_fault is a plain call rather than a hook because the
     * pager is a static function in the same translation unit -- but it
     * is declared and not defined here, since it needs a block device
     * and a mounted volume and this file is compiled before either
     * exists. It answers 0 until swap_init() has run.
     */
    if (f->vector == 14 && !(f->error & 1)) {
        if (swap_handle_fault(cr2, f->error)) return;

        /*
         * Nor is the first touch of an anonymous reservation.
         *
         * mmap does not map anything; it records that a range has been
         * promised and returns, because a program that reserves gigabytes
         * to use megabytes of them should not be charged for the
         * difference. This is where the bill is finally presented, one
         * page at a time, by whichever access actually needed it.
         *
         * After the pager rather than before, and the order is free
         * rather than delicate: the two cannot both be true of one
         * address. A swapped page has an entry with the pagefile slot in
         * it; a reserved page has no entry at all, and often no page
         * table to hold one. But asking the pager first keeps the common
         * case on a machine under memory pressure first, and vmm_area_find
         * is a linear scan that a resident program should not pay for.
         */
        if (vmm_current && (f->error & 4) && vmm_fault_anon(vmm_current, cr2))
            return;
    }

    /* A fault on the page below a kernel stack has exactly one cause,
     * and saying which stack is most of the diagnosis. */
    if (f->vector == 14 && vmm_is_guard(cr2)) {
        serial_puts("\n[trap] kernel stack overflow in thread ");
        serial_puts(cur_thread ? cur_thread->name : "unknown");
        serial_puts(" - it ran off the end of its stack and hit the "
                    "guard page\n[trap]   guard at ");
        serial_put_hex64(cr2);
        serial_puts(", rip ");
        serial_put_hex64(f->rip);
        serial_puts("\n[trap] halted\n");
        __asm__ volatile("cli");
        for (;;) __asm__ volatile("hlt");
    }

    /*
     * ---- did the program ask to be told about this? ----
     *
     * Before the report and before the kill, and the placement is the
     * whole of it. A JIT plants faults on purpose — a guard page it
     * expects to hit, a write to a page it protected to detect a store —
     * and catches SIGSEGV to deal with them. Printing eleven lines of
     * register dump for each one would bury every genuine fault in a
     * scroll of intentional ones, exactly as an unfiltered copy-on-write
     * fault would.
     *
     * This is the second of the two places a caught signal can be
     * delivered; the other is on the way out of a system call. Both are
     * points where the thread is about to return to ring 3 with a full
     * register set in a frame that can be rewritten, and the timer
     * interrupt is deliberately not a third — see the note beside
     * syscall_deliver_signals.
     *
     * Only for a process that has already installed a handler:
     * vls_signal_fault answers zero for one that has not, and the
     * existing path below — print everything, end the thread — is the
     * right answer for that program and has not changed.
     */
    if (from_user && cur_thread && cur_thread->user && vmm_current &&
        vmm_current->sig) {
        int sig = 0;
        switch (f->vector) {
        case 0:  case 16: case 19: sig = VLS_SIGFPE;  break;  /* #DE, x87, SIMD */
        case 1:  case 3:           sig = VLS_SIGTRAP; break;  /* #DB, #BP      */
        case 6:                    sig = VLS_SIGILL;  break;  /* #UD           */
        case 17:                   sig = VLS_SIGBUS;  break;  /* #AC           */
        case 4:  case 5:  case 13: sig = VLS_SIGSEGV; break;  /* #OF, #BR, #GP */
        case 14:                   sig = VLS_SIGSEGV; break;  /* #PF           */
        default: break;
        }
        if (sig) {
            /* The page-fault error code already says which of the two
             * si_code values applies, and a crash reporter that prints
             * "address not mapped" rather than "permissions" is reading
             * exactly this bit. */
            const uint32_t code = (f->vector == 14 && !(f->error & 1))
                                  ? VLS_SEGV_MAPERR
                                  : (f->vector == 14 ? VLS_SEGV_ACCERR
                                                     : VLS_SI_KERNEL);
            if (vls_signal_fault(vmm_current, sig, cr2, code,
                                 f->vector, f->error)) {
                vls_regs_t r;
                for (uint64_t i = 0; i < sizeof(r); i++)
                    ((uint8_t *)&r)[i] = 0;
                /* exc_frame_t and vls_regs_t open with the same fifteen
                 * registers in the same order, which include/vls.h
                 * arranges so that this is a copy rather than fifteen
                 * assignments that could be mis-ordered. */
                for (int i = 0; i < 15; i++)
                    ((uint64_t *)&r)[i] = ((const uint64_t *)f)[i];
                r.rip        = f->rip;
                r.rsp        = f->rsp;
                r.rflags     = f->rflags;
                r.fault_addr = cr2;
                r.trapno     = f->vector;
                r.err        = f->error;

                if (vls_signal_dispatch(vmm_current, &r) ==
                    VLS_DELIVER_HANDLER) {
                    for (int i = 0; i < 15; i++)
                        ((uint64_t *)f)[i] = ((const uint64_t *)&r)[i];
                    f->rip    = r.rip;
                    f->rsp    = r.rsp;
                    f->rflags = r.rflags;
                    serial_puts("[VLS] ");
                    serial_puts(exc_name(f->vector));
                    serial_puts(" delivered to the program's handler\n");
                    return;
                }
                /* Not delivered. Falls through to the report below,
                 * which is where a fault that nobody could catch has
                 * always gone. */
            }
        }
    }

    serial_puts("\n[trap] ");
    serial_puts(exc_name(f->vector));
    serial_puts(" (vector ");
    serial_put_dec((uint32_t)f->vector);
    serial_puts(", error ");
    serial_put_hex64(f->error);
    serial_puts(")\n[trap]   rip ");
    serial_put_hex64(f->rip);
    serial_puts("  cs ");
    serial_put_hex64(f->cs);
    serial_puts("  rsp ");
    serial_put_hex64(f->rsp);
    serial_puts("\n[trap]   rflags ");
    serial_put_hex64(f->rflags);
    if (f->vector == 14) {
        serial_puts("  faulting address ");
        serial_put_hex64(cr2);
        /* The low bits of a page-fault error code are the whole
         * diagnosis and are worth spelling out, because "error 7" and
         * "wrote to a read-only page it was allowed to see" are the same
         * fact and only one of them is useful at three in the morning. */
        serial_puts("\n[trap]   ");
        serial_puts((f->error & 1) ? "protection violation" : "page not present");
        serial_puts((f->error & 2) ? ", on a write" : ", on a read");
        if (f->error & 4)  serial_puts(", from user mode");
        if (f->error & 8)  serial_puts(", reserved bit set");
        if (f->error & 16) serial_puts(", on an instruction fetch");
    }
    serial_puts("\n[trap]   thread ");
    if (cur_thread) {
        serial_puts(cur_thread->name);
        serial_puts(" (pid ");
        serial_put_dec(cur_thread->pid);
        serial_puts(")");
    } else {
        serial_puts("none");
    }
    serial_puts(from_user ? " in ring 3\n" : " in ring 0\n");

    if (from_user && cur_thread && cur_thread->user) {
        /*
         * Before killing it: does the program have a `__try` around the
         * instruction that faulted?
         *
         * On x86-64 that question can only be answered here. The 32-bit
         * convention kept a handler chain on the stack, so a program
         * could catch its own faults; the 64-bit one replaced that with
         * static tables in .pdata and .xdata, and looking an address up
         * in them at fault time is the operating system's job. At the
         * moment of the fault the program is not running -- this is.
         */
        uint64_t resume = 0;
        if (trap_seh_hook && trap_seh_hook(f->rip, f->vector, &resume)) {
            serial_puts("[seh] handled by the program, resuming at ");
            serial_put_hex64(resume);
            serial_putc('\n');
            f->rip = resume;
            return;
        }

        /*
         * Return into the kernel instead of into the instruction that
         * just faulted. Everything the processor pops from here on is
         * something written above rather than something it pushed, which
         * is the only way out of an exception that must not resume.
         */
        f->rip    = (uint64_t)(uintptr_t)trap_thread_died;
        f->cs     = GDT_KCODE;
        f->ss     = GDT_KDATA;
        f->rsp    = cur_thread->kstack_top - 256;
        f->rflags = 0x202;
        return;
    }

    /* A fault in the kernel. There is nothing safe left to do. */
    serial_puts("[trap] halted\n");
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

/*
 * Point the first thirty-two vectors at the stubs above.
 *
 * Every gate stays at DPL 0: a program that can raise a page fault with
 * `int 14` can hand the kernel an error code it invented, and the
 * handler has no way to tell that from one the processor pushed.
 *
 * The double fault is the exception to the pattern, taking IST slot 1.
 * The whole reason it exists as a vector is that the stack in hand may
 * be the problem, and a handler that cannot push its own frame produces
 * a triple fault and a reset with nothing printed at all.
 */
static void trap_install(void) {
    void *stubs[] = {
        (void *)exc_stub_0,  (void *)exc_stub_1,  (void *)exc_stub_2,
        (void *)exc_stub_3,  (void *)exc_stub_4,  (void *)exc_stub_5,
        (void *)exc_stub_6,  (void *)exc_stub_7,  (void *)exc_stub_8,
        (void *)exc_stub_9,  (void *)exc_stub_10, (void *)exc_stub_11,
        (void *)exc_stub_12, (void *)exc_stub_13, (void *)exc_stub_14,
        (void *)exc_stub_15, (void *)exc_stub_16, (void *)exc_stub_17,
        (void *)exc_stub_18, (void *)exc_stub_19, (void *)exc_stub_20,
        (void *)exc_stub_21
    };
    for (int v = 0; v <= 21; v++) {
        uint8_t ist = 0;
        if (v == 8)  ist = IST_DF;    /* double fault      */
        if (v == 2)  ist = IST_NMI;   /* non-maskable      */
        if (v == 12) ist = IST_SS;    /* stack-segment     */
        if (v == 18) ist = IST_MC;    /* machine check     */
        idt_set_gate_ex(v, stubs[v], GDT_KCODE, ist, 0);
    }
    idt_set_gate_ex(29, (void *)exc_stub_29, GDT_KCODE, 0, 0);
    idt_set_gate_ex(30, (void *)exc_stub_30, GDT_KCODE, 0, 0);

    serial_puts("[trap] 24 exception vectors, four on interrupt stacks "
                "(#DF #SS #MC NMI)\n");
}

#endif /* TRAP_H */
