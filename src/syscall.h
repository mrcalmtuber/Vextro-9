#ifndef SYSCALL_H
#define SYSCALL_H

/*
 * src/syscall.h — the boundary.
 *
 * There used to be nothing here worth the name. `int 0x80` went to a
 * gate at DPL 0, and SYSCALL went to a stub that ran on the caller's
 * stack and returned with `jmp *%rcx` — both perfectly correct for what
 * they had to do, which was let ring-0 code call ring-0 code through a
 * numbered table. Neither survives contact with an actual privilege
 * change: a ring-3 `int 0x80` into a DPL-0 gate raises #GP before the
 * handler is reached, and SYSCALL from ring 3 arrives with the *user's*
 * stack pointer still in RSP, which is the classic way to let a program
 * choose where the kernel writes.
 *
 * So both entries now do the same three things in the same order:
 * get onto a kernel stack, save every register into a frame, and hand
 * that frame to one C function. Nothing reaches the service routines
 * except through that frame, which is what makes "sanitise the incoming
 * registers" a property of the code rather than a rule handlers have to
 * remember.
 *
 * Interrupts stay off for the whole syscall. IA32_FMASK clears IF on
 * entry and nothing here turns it back on: every service below is
 * bounded and short — a string measured, a rectangle filled, a page
 * mapped — and leaving them off means the frame, the kernel stack
 * pointer and the current address space cannot change underneath a
 * handler that is halfway through validating a pointer.
 */

#include <stdint.h>
#include "gdt.h"
#include "vmm.h"

/* ---- numbers ----
 *
 * 1-3 are what apps/vextro.h has always sent and cannot move. The rest
 * are new. 20 and up are the calls that stand in for symbols the loader
 * used to patch straight into an image as kernel addresses; see the
 * trampoline below for why they exist at all.
 */
#define SYS_PRINT            1
#define SYS_DRAW_PIXEL       2
#define SYS_GET_MOUSE        3
#define SYS_EXIT             4
#define SYS_SBRK             5
#define SYS_WRITE            6
#define SYS_YIELD            7
#define SYS_TICKS            8
#define SYS_CANVAS           9
#define SYS_TTF_TEXT_WIDTH  20
#define SYS_TTF_DRAW_STRING 21
#define SYS_GFX_RECT        22
#define SYS_FORK            23
#define SYS_MEMINFO         24

/*
 * Every register a user thread had, in the order the entry stubs push
 * them. The offsets are load-bearing — the assembly below writes them by
 * position, this reads them by name, and nothing checks that the two
 * agree except that the machine stops working if they do not.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rsp;
} syscall_frame_t;

/*
 * Where the next entry from user mode should put its stack pointer. The
 * scheduler rewrites this and tss.rsp0 together on every switch, because
 * they answer the same question for the two different ways into the
 * kernel.
 *
 * Both are external rather than static because the entry stub below
 * names them, and a name the assembler has to resolve is a name that has
 * to survive into the object file.
 */
uint64_t syscall_kstack = 0;
uint64_t syscall_user_rsp_slot = 0;

/* Implemented in desktop.h, where the things a syscall can ask for
 * actually live. Returns the value the caller finds in RAX. */
static uint64_t syscall_service(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5);

/*
 * The frame the call in progress arrived on, and which door it came
 * through.
 *
 * Only fork needs these, and it needs them absolutely: a child has to
 * resume at the same instruction as its parent, and the only record of
 * where that is lives in the frame. Safe as a global because interrupts
 * are masked for the whole of a syscall, so there is never more than one
 * in progress.
 *
 * The door matters because the two do not carry the same information.
 * SYSCALL puts the return address in RCX and the flags in R11 by
 * architecture; `int 0x80` puts them in the interrupt frame the
 * processor pushed, above what this structure covers, and leaves RCX and
 * R11 holding whatever the program had in them.
 */
static syscall_frame_t *syscall_cur_frame = 0;
static int              syscall_via_fast  = 0;

__attribute__((used))
void syscall_dispatch_frame(syscall_frame_t *f);

void syscall_dispatch_frame(syscall_frame_t *f) {
    syscall_cur_frame = f;
    syscall_via_fast  = 1;
    f->rax = syscall_service(f->rax, f->rdi, f->rsi, f->rdx,
                             f->r10, f->r8, f->r9);
    syscall_cur_frame = 0;
}

/*
 * The same work, with the result thrown away — and that is not an
 * oversight, it is the older door's contract.
 *
 * apps/vextro.h has said since the beginning that `int 0x80` preserves
 * every general-purpose register, and GCC believes it. Two calls in a
 * row to os_print compile to one `mov eax, 1` and two interrupts,
 * because the compiler can see that nothing between them changes EAX.
 * Return a value in RAX and the second call is made with whatever the
 * first one returned as its number: it does nothing, silently, and the
 * program looks like it skipped a line.
 *
 * That was found exactly that way. So the legacy gate keeps its promise,
 * to the letter and to binaries already installed on somebody's disk,
 * and anything that needs an answer back uses SYSCALL — which has no
 * such history and returns in RAX like every other calling convention on
 * this machine.
 */
__attribute__((used))
void syscall_dispatch_legacy(syscall_frame_t *f);

void syscall_dispatch_legacy(syscall_frame_t *f) {
    syscall_cur_frame = f;
    syscall_via_fast  = 0;
    (void)syscall_service(f->rax, f->rdi, f->rsi, f->rdx,
                          f->r10, f->r8, f->r9);
    syscall_cur_frame = 0;
}

/*
 * SYSCALL entry.
 *
 * On arrival: CPL is already 0, RCX holds the return address, R11 holds
 * the caller's RFLAGS, IF is clear because FMASK says so — and RSP is
 * still whatever the user program had. The first instruction therefore
 * cannot be a push. It parks the user stack pointer in a global, which
 * is safe precisely because interrupts are masked: nothing can arrive
 * between writing it and reading it back.
 *
 * The exit is SYSRETQ, which needs RCX canonical and R11 sane; both come
 * straight back out of the frame the CPU itself filled in.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl syscall_entry\n"
    ".type syscall_entry, @function\n"
    "syscall_entry:\n"
    "  movq %rsp, syscall_user_rsp_slot(%rip)\n"
    "  movq syscall_kstack(%rip), %rsp\n"
    "  pushq syscall_user_rsp_slot(%rip)\n"
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
    /* The ABI wants RSP sixteen-aligned at the call, and how it arrives
     * here depends on how many words the processor pushed, which differs
     * between the two entry paths. Align it explicitly and put it back
     * from RBP, whose own value is already safe in the frame. */
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"
    "  call syscall_dispatch_frame\n"
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
    "  popq %rsp\n"
    "  sysretq\n"
    ".popsection\n"
);
extern void syscall_entry(void);

/*
 * The int 0x80 gate.
 *
 * Slower than SYSCALL and kept because it is what every existing app
 * binary emits, down to the ones already installed on somebody's disk.
 * The processor has done the stack switch by the time this runs — that
 * is what a DPL-3 interrupt gate and a TSS are for — so the only work is
 * to reach up into the frame it pushed for the user's RSP and lay the
 * registers out the same way the SYSCALL path does.
 *
 * The gate is also reachable from ring 0, where no stack switch happens
 * and 24(%rsp) is the kernel's own stack pointer. That is harmless: the
 * value is copied into the frame, never loaded back.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl int80_stub\n"
    ".type int80_stub, @function\n"
    "int80_stub:\n"
    "  pushq 24(%rsp)\n"          /* user RSP out of the interrupt frame */
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
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"
    "  call syscall_dispatch_legacy\n"
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
    "  addq $8, %rsp\n"           /* drop the copied RSP */
    "  iretq\n"
    ".popsection\n"
);
extern void int80_stub(void);

/*
 * ===== the user trampoline page =====
 *
 * A .vx image carries an import table: names the loader fills in with
 * addresses before the image runs. Those addresses used to be kernel
 * function pointers, which worked because applications ran in ring 0 and
 * stops working the moment they do not.
 *
 * The names survive, and so does every image that uses them; what
 * changes is what they resolve to. Each now points at one of the stubs
 * below, which lives on a page mapped into the process's own address
 * space and does nothing but turn a C call into a syscall. The stubs are
 * written by hand because they must be genuinely position independent —
 * no data references at all — so that the same physical page can be
 * mapped at whatever address each process is given.
 *
 * The two eight-argument entries have more parameters than any calling
 * convention passes in registers, so they rebuild their arguments as a
 * contiguous block on the caller's stack and pass a pointer to it. The
 * kernel side validates that pointer like any other.
 *
 * They enter through SYSCALL rather than `int 0x80` because one of them
 * has to return a value, and the legacy gate is bound by an older
 * promise to leave RAX exactly as it found it. SYSCALL clobbers RCX and
 * R11, which is why every argument that arrived in RCX is pushed before
 * the instruction is issued.
 */
__asm__(
    ".pushsection .utext, \"ax\", @progbits\n"
    ".globl utramp_start\n"
    "utramp_start:\n"

    ".globl utramp_ttf_text_width\n"
    "utramp_ttf_text_width:\n"
    "  movl $20, %eax\n"
    "  syscall\n"
    "  ret\n"

    ".align 16\n"
    ".globl utramp_ttf_draw_string\n"
    "utramp_ttf_draw_string:\n"
    "  movq 16(%rsp), %rax\n"     /* arg8: size  */
    "  pushq %rax\n"
    "  movq 16(%rsp), %rax\n"     /* arg7: color */
    "  pushq %rax\n"
    "  pushq %r9\n"               /* arg6: s     */
    "  pushq %r8\n"               /* arg5: topY  */
    "  pushq %rcx\n"              /* arg4: topX  */
    "  pushq %rdx\n"              /* arg3: bh    */
    "  pushq %rsi\n"              /* arg2: bw    */
    "  pushq %rdi\n"              /* arg1: buf   */
    "  movq %rsp, %rdi\n"
    "  movl $21, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  ret\n"

    ".align 16\n"
    ".globl utramp_gfx_rect\n"
    "utramp_gfx_rect:\n"
    "  movq 16(%rsp), %rax\n"     /* arg8: color */
    "  pushq %rax\n"
    "  movq 16(%rsp), %rax\n"     /* arg7: h     */
    "  pushq %rax\n"
    "  pushq %r9\n"               /* arg6: w     */
    "  pushq %r8\n"               /* arg5: y     */
    "  pushq %rcx\n"              /* arg4: x     */
    "  pushq %rdx\n"              /* arg3: bh    */
    "  pushq %rsi\n"              /* arg2: bw    */
    "  pushq %rdi\n"              /* arg1: buf   */
    "  movq %rsp, %rdi\n"
    "  movl $22, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  ret\n"

    /*
     * Where a program lands when it returns from _start.
     *
     * Applications here are written as ordinary functions that end by
     * returning, which was fine while the kernel was the thing that
     * called them. Nothing calls them now. The loader writes the address
     * of this stub onto the new stack as the return address, so falling
     * off the end of main is an orderly exit rather than a jump to
     * whatever happened to be on the stack.
     */
    ".align 16\n"
    ".globl utramp_exit\n"
    "utramp_exit:\n"
    /* Zero, not EAX. `_start` returns void, so nothing has put a status
     * there — and the legacy gate preserves RAX, so what is actually in
     * it is the number of the last syscall the program made. Passing
     * that on made every clean exit report a failure. */
    "  xorl %edi, %edi\n"
    "  movl $4, %eax\n"
    "  int $0x80\n"
    "1:\n"
    "  jmp 1b\n"

    ".globl utramp_end\n"
    "utramp_end:\n"
    ".popsection\n"
);

extern uint8_t utramp_start[], utramp_end[];
extern uint8_t utramp_ttf_text_width[], utramp_ttf_draw_string[],
               utramp_gfx_rect[], utramp_exit[];

/* Where a stub ends up once the page is mapped into a process. */
static inline uint64_t utramp_user_addr(const uint8_t *stub) {
    return USER_TRAMP_VA + (uint64_t)(stub - utramp_start);
}

/*
 * Program the MSRs.
 *
 * STAR's two halves are read by different instructions and mean
 * different things: [47:32] is the code selector SYSCALL loads, and
 * [63:48] is the base SYSRET does arithmetic on. Getting the second one
 * wrong is not diagnosable from the fault it produces, which is a #GP at
 * an address in user space with no obvious cause.
 *
 * FMASK clears more than the interrupt flag. DF matters because the C
 * ABI says string operations start forward and a user program can leave
 * it set; TF because a single-step flag surviving into ring 0 turns
 * every kernel instruction into a debug exception; AC because alignment
 * checking enabled by a user is not something kernel code is written to
 * survive. NT and IOPL follow for the same reason.
 */
static void syscall_init(void) {
    /* SCE enables the SYSCALL instruction. NXE enables bit 63 of a page
     * table entry to mean "no execute"; without it that bit is reserved
     * and setting it makes every access to the page a reserved-bit page
     * fault — so the no-execute mappings the loader creates depend on
     * this line as much as on the loader. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ULL << 0) | (1ULL << 11));
    wrmsr(MSR_STAR, (STAR_SYSRET_BASE << 48) | (STAR_SYSCALL_CS << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000 | 0x3000);

    syscall_kstack = tss.rsp0;

    /* And the legacy door, at DPL 3 so ring 3 can actually knock. */
    idt_set_gate_ex(0x80, (void *)(uintptr_t)int80_stub, GDT_KCODE, 0, 3);

    serial_puts("[syscall] SYSCALL/SYSRET armed, int 0x80 gate open to ring 3\n");
}

#endif /* SYSCALL_H */
