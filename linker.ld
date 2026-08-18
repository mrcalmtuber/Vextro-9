OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

PHDRS {
    text    PT_LOAD FLAGS(5);   /* R-X */
    rodata  PT_LOAD FLAGS(4);   /* R-- */
    data    PT_LOAD FLAGS(6);   /* RW- */
    bss     PT_LOAD FLAGS(7);   /* RWX — app binary execution arena */
}

SECTIONS {
    /* Limine higher-half kernel load address */
    . = 0xffffffff80000000;

    .text : ALIGN(4096) {
        *(.text .text.*)
    } :text

    . = ALIGN(4096);

    /*
     * The syscall trampolines, on pages of their own.
     *
     * This is the only part of the kernel image that is mapped into a
     * user address space, so it has to be separable at page granularity
     * — hence its own output section rather than a corner of .text. It
     * contains code and nothing else: no literals, no relocations, no
     * references outside itself, so the same physical pages can appear
     * at whatever virtual address each process is given.
     */
    .utext : ALIGN(4096) {
        __utext_start = .;
        KEEP(*(.utext))
        . = ALIGN(4096);
        __utext_end = .;
    } :text

    . = ALIGN(4096);

    /* Read-only data, including Limine request sections */
    .rodata : ALIGN(4096) {
        *(.rodata .rodata.*)
    } :rodata

    .limine_reqs_start : ALIGN(4096) {
        KEEP(*(.limine_reqs_start))
    } :rodata

    .limine_reqs : {
        KEEP(*(.limine_reqs))
    } :rodata

    .limine_reqs_end : {
        KEEP(*(.limine_reqs_end))
    } :rodata

    . = ALIGN(4096);

    /* Writable data */
    .data : ALIGN(4096) {
        *(.data .data.*)
    } :data

    . = ALIGN(4096);

    /* BSS — zero-initialized; app execution arena + backbuffer live here.
     * Separate RWX segment so loaded ELF binaries can execute from app_memory. */
    .bss : ALIGN(4096) {
        *(.bss .bss.*)
        *(COMMON)
    } :bss

    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.comment)
    }
}
