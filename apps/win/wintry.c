/*
 * wintry — a Windows executable that faults on purpose and survives it.
 *
 * This is the test for the structured exception handling in
 * src/winproc.h, and it exists because that feature cannot be checked
 * any other way. `__except` is not a library call and not a syscall;
 * it is a claim about what happens when a program does something
 * illegal, and the only way to test the claim is to do the illegal
 * thing.
 *
 * ---- why the assembly ----
 *
 * `__try`/`__except` is Microsoft syntax and GCC does not compile it.
 * What GCC's assembler *does* speak is the SEH directive family --
 * `.seh_proc`, `.seh_handler`, `.seh_handlerdata` -- which emit exactly
 * the .pdata and .xdata a Microsoft compiler emits for a `__try`. So
 * the tables under test are genuine even though the syntax above them
 * is not: same RUNTIME_FUNCTION, same UNWIND_INFO with UNW_FLAG_EHANDLER
 * set, same scope table naming __C_specific_handler.
 *
 * A hand-built table would have proved only that the kernel can read a
 * table this repository also wrote. This proves it can read one the
 * toolchain wrote.
 *
 * ---- what it does ----
 *
 * seh_probe() writes through a null pointer inside a guarded range. On
 * a system without SEH the thread dies there and nothing after it runs.
 * With it, the kernel finds the scope entry covering the faulting
 * address and resumes at the handler, which returns 1.
 *
 * The frame is arranged so resumption works: the handler's epilogue
 * unwinds exactly the prologue the fault happened under, which is the
 * case the kernel supports and the case a `__try` in the same function
 * always produces.
 */

typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

__declspec(dllimport) void VxPrint(const char *s);
__declspec(dllimport) void VxExit(int code);

int seh_probe(void);
int seh_probe_ok(void);

__asm__(
    ".text\n"
    ".globl seh_probe\n"
    ".def seh_probe; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_probe\n"
"seh_probe:\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  movq %rsp, %rbp\n"
    "  .seh_setframe %rbp, 0\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    /* The handler, and the scope table it reads. `@except` is what sets
     * UNW_FLAG_EHANDLER in the unwind info -- the single bit the kernel
     * tests before it will look for a scope table at all. */
    "  .seh_handler __C_specific_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"                    /* one __try in this function     */
    "  .rva .Lseh_try_begin\n"
    "  .rva .Lseh_try_end\n"
    "  .long 1\n"                    /* EXCEPTION_EXECUTE_HANDLER      */
    "  .rva .Lseh_handler\n"
    "  .text\n"
    "  .seh_endprologue\n"
".Lseh_try_begin:\n"
    /* Write through a null pointer. A page fault, from ring 3, inside
     * the guarded range. */
    "  xorq %rax, %rax\n"
    "  movq $42, (%rax)\n"
".Lseh_try_end:\n"
    "  xorl %eax, %eax\n"            /* not reached: the fault is real */
    "  jmp .Lseh_done\n"
".Lseh_handler:\n"
    "  movl $1, %eax\n"              /* the __except block             */
".Lseh_done:\n"
    "  addq $32, %rsp\n"
    "  popq %rbp\n"
    "  ret\n"
    "  .seh_endproc\n"
);

/*
 * A second function with the same shape that does *not* fault, so a
 * pass tells the two cases apart. If the kernel resumed at a handler
 * for a fault that never happened, this would return 1 and the test
 * would say so.
 */
__asm__(
    ".text\n"
    ".globl seh_probe_ok\n"
    ".def seh_probe_ok; .scl 2; .type 32; .endef\n"
    ".seh_proc seh_probe_ok\n"
"seh_probe_ok:\n"
    "  pushq %rbp\n"
    "  .seh_pushreg %rbp\n"
    "  movq %rsp, %rbp\n"
    "  .seh_setframe %rbp, 0\n"
    "  subq $32, %rsp\n"
    "  .seh_stackalloc 32\n"
    "  .seh_handler __C_specific_handler, @except\n"
    "  .seh_handlerdata\n"
    "  .long 1\n"
    "  .rva .Lok_try_begin\n"
    "  .rva .Lok_try_end\n"
    "  .long 1\n"
    "  .rva .Lok_handler\n"
    "  .text\n"
    "  .seh_endprologue\n"
".Lok_try_begin:\n"
    "  movl $7, %eax\n"
".Lok_try_end:\n"
    "  jmp .Lok_done\n"
".Lok_handler:\n"
    "  movl $99, %eax\n"
".Lok_done:\n"
    "  addq $32, %rsp\n"
    "  popq %rbp\n"
    "  ret\n"
    "  .seh_endproc\n"
);

/*
 * __C_specific_handler is named by the tables and never called by this
 * kernel -- the dispatcher reads the scope table itself rather than
 * running the language handler. It exists so the image links.
 */
long __C_specific_handler(void *rec, void *frame, void *ctx, void *disp) {
    (void)rec; (void)frame; (void)ctx; (void)disp;
    return 1;                       /* ExceptionContinueSearch */
}

/*
 * Read the Thread Environment Block the way a Windows program does:
 * straight through GS, with no call and no import.
 *
 * This is the whole point of the TEB being at the GS base rather than
 * somewhere a function could return -- a C runtime reads GS:[0x08] for
 * the stack base in the middle of a bounds check, with no opportunity
 * to ask anyone anything. If the segment base is wrong these reads do
 * not fail, they return whatever is at that address, which is why the
 * values are checked against each other rather than merely for being
 * non-zero.
 */
static unsigned long long teb_read(unsigned int off) {
    unsigned long long v;
    __asm__ volatile("movq %%gs:(%1), %0" : "=r"(v) : "r"((unsigned long long)off));
    return v;
}

static int teb_ok(void) {
    unsigned long long self  = teb_read(0x30);
    unsigned long long peb   = teb_read(0x60);
    unsigned long long base  = teb_read(0x08);
    unsigned long long limit = teb_read(0x10);

    /* Self-consistency, not plausibility: the TEB must name itself, the
     * PEB must be the page after it, and the stack must run downwards
     * from base to limit. Random memory satisfies none of those. */
    if (self == 0 || peb != self + 0x1000) return 0;
    if (base <= limit) return 0;
    return 1;
}

static void report(const char *label, int got, int want) {
    VxPrint(got == want ? "wintry:   ok   " : "wintry:   FAIL ");
    VxPrint(label);
    VxPrint("\n");
}

void PeMain(void) {
    VxPrint("wintry: structured exception handling\n");

    /* Checked first: everything below assumes a working environment,
     * and exiting non-zero is how the kernel's selftest learns that the
     * TEB was wrong -- the program's own output goes to the terminal,
     * not the serial line the test reads. */
    if (!teb_ok()) {
        VxPrint("wintry:   FAIL the TEB at the GS base is not self-consistent\n");
        VxExit(2);
    }
    VxPrint("wintry:   ok   the TEB reads correctly through GS\n");

    int quiet = seh_probe_ok();
    report("a guarded range that does not fault returns its own value",
           quiet, 7);

    /* The one that matters. Everything after this line only runs
     * because the kernel resumed the thread at the handler. */
    int caught = seh_probe();
    report("a fault inside __try resumes at __except", caught, 1);

    VxPrint("wintry: the program is still running after a null write\n");
    VxExit(0);
}
