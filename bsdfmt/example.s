/*
 * example.s — the smallest useful .bsd payload: text only.
 *
 * ABI:  long entry(long n)   n in RDI, result in RAX.
 * Returns n! (exact up to n = 20, which is where 64 bits runs out).
 *
 * There are no memory references at all, so the assembled bytes are
 * position independent by construction and can be fed to bsd_maker in
 * raw mode without a link step.
 */
    .text
    .globl  _start
_start:
    movq    $1, %rax
    testq   %rdi, %rdi
    jz      2f
1:
    imulq   %rdi, %rax
    decq    %rdi
    jnz     1b
2:
    ret
