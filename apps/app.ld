OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    code PT_LOAD FLAGS(7);
}

SECTIONS {
    . = 0x1000;

    .text   : { *(.text .text.*) }   :code
    .rodata : { *(.rodata .rodata.*) } :code
    .data   : { *(.data .data.*) }   :code
    .bss    : { *(.bss .bss.* COMMON) } :code

    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.comment)
    }
}
