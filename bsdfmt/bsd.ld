/*
 * Linker script for .bsd programs.
 *
 * Two PT_LOAD segments, and the data one starts on a fresh page.  That
 * page alignment is not cosmetic: mprotect() works at page granularity,
 * so if .data shared a page with .text the loader would have to make
 * that page both writable and executable and W^X would be lost.
 *
 * .rodata rides along in the executable segment, where it is read-only
 * without needing a third mapping.
 *
 * Link with -z max-page-size=0x1000, otherwise ld pads the segments out
 * to its 2 MB default and the intermediate ELF becomes enormous.  The
 * .bsd file itself is repacked by bsd_maker either way.
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
