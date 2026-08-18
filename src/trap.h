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
#include "sched.h"

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
    "  call exception_handle\n"
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
    for (int v = 0; v <= 21; v++)
        idt_set_gate_ex(v, stubs[v], GDT_KCODE, v == 8 ? 1 : 0, 0);
    idt_set_gate_ex(29, (void *)exc_stub_29, GDT_KCODE, 0, 0);
    idt_set_gate_ex(30, (void *)exc_stub_30, GDT_KCODE, 0, 0);

    serial_puts("[trap] 24 exception vectors installed, #DF on its own stack\n");
}

#endif /* TRAP_H */
