/*
 * example_data.s — a payload with both segments.
 *
 * ABI:  long entry(long ignored)   result in RAX.
 * Sums an eight-entry table that lives in the data segment and returns
 * the total (36), then bumps a counter in .bss to prove the loader
 * really did hand us writable, zeroed memory.
 *
 * The table is reached RIP-relatively, so the code does not care where
 * it is loaded — only that the loader preserves the distance between
 * the text and data segments, which the format requires.
 */
    .text
    .globl  _start
_start:
    leaq    table(%rip), %rsi
    xorq    %rax, %rax
    movq    $8, %rcx
1:
    addq    (%rsi), %rax
    addq    $8, %rsi
    decq    %rcx
    jnz     1b

    /* .bss starts zeroed; incrementing it must yield exactly 1 */
    incq    calls(%rip)
    addq    calls(%rip), %rax
    decq    %rax                    /* undo that, so the answer is the sum */

    ret

    .data
    .align  8
table:
    .quad   1, 2, 3, 4, 5, 6, 7, 8

    .bss
    .align  8
calls:
    .quad   0
