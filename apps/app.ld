/*
 * Linker script for plain ELF64 programs.
 *
 * Two segments, split on a page boundary, for the same reason vx.ld has
 * always had two: the kernel's loader now maps an image page by page and
 * gives each page the protection its segment asked for. One RWX segment
 * would mean every page of the program is both writable and executable,
 * which is the state W^X exists to prevent — and there is no longer any
 * excuse for it, because there are page tables to say otherwise.
 *
 * Link with -z max-page-size=0x1000; ld's 2 MB default would pad the gap
 * between the segments to two megabytes of image.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X */
    data PT_LOAD FLAGS(6);   /* RW- */
}

SECTIONS {
    . = 0x1000;

    .text   : { *(.text .text.*) }     :text
    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(4096);

    .data   : { *(.data .data.*) }        :data
    .bss    : { *(.bss .bss.* COMMON) }   :data

    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.comment)
    }
}
