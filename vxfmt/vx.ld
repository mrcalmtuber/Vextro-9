/*
 * Linker script for .vx programs.
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
 * .vx file itself is repacked by vx_maker either way.
 */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);   /* R-X */
    data PT_LOAD FLAGS(6);   /* RW- */
    /*
     * The thread-local template.
     *
     * PT_TLS does not describe memory to be mapped -- .tdata is already
     * inside the data segment above, and this header points at the same
     * bytes. What it describes is a *template*: the size and alignment of
     * the block each thread needs, and where the initialised part of it
     * comes from. The linker needs that to resolve a thread-local
     * variable's offset from the FS segment base, and libc/pthread.c
     * needs it to know how much to allocate and copy per thread.
     *
     * The .vx packer ignores it, because vx_maker walks only PT_LOAD.
     */
    tls  PT_TLS  FLAGS(4);   /* R-- */
}

SECTIONS {
    . = 0x1000;

    .text   : { *(.text .text.*) }     :text
    .rodata : { *(.rodata .rodata.*) } :text

    . = ALIGN(4096);

    /*
     * ---- the thread-local template ----
     *
     * .tdata holds the initialised thread-local variables and occupies
     * real bytes; .tbss holds the zero-initialised ones and occupies
     * none. Together they are the image copied into every new thread.
     *
     * Both are named in two segments at once, which is what makes the
     * template work: :data so the bytes are actually loaded, and :tls so
     * the linker records their size and alignment where pthread_create
     * and the compiler's TLS relocations can both find them.
     *
     * The three symbols are what libc/pthread.c reads. They are always
     * defined, even in a program with no thread-local variables at all,
     * in which case they are equal and the image size is zero.
     */
    .tdata : {
        PROVIDE_HIDDEN(__tdata_start = .);
        *(.tdata .tdata.* .gnu.linkonce.td.*)
        PROVIDE_HIDDEN(__tdata_end = .);
    } :data :tls

    .tbss : {
        *(.tbss .tbss.* .gnu.linkonce.tb.*)
        *(.tcommon)
        PROVIDE_HIDDEN(__tbss_end = .);
    } :data :tls

    /*
     * ---- what runs before and after main ----
     *
     * A C++ program records the constructor for every namespace-scope
     * object here and expects something to walk the array before main.
     * Nothing did, because nothing had to: a C program's array is empty.
     * libc/crt0.c walks it, and these symbols are how it finds the ends.
     *
     * .ctors and .dtors are the older spelling of the same thing, still
     * emitted by some toolchains, and are gathered into the same arrays
     * so that a program does not silently skip half its constructors
     * depending on which convention its compiler used.
     */
    .init_array : {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)))
        KEEP(*(.init_array))
        KEEP(*(SORT_BY_NAME(.ctors.*)))
        KEEP(*(.ctors))
        PROVIDE_HIDDEN(__init_array_end = .);
    } :data

    .fini_array : {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*)))
        KEEP(*(.fini_array))
        KEEP(*(SORT_BY_NAME(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } :data

    .data   : { *(.data .data.*) }        :data
    .bss    : { *(.bss .bss.* COMMON) }   :data

    /DISCARD/ : {
        *(.note .note.*)
        *(.eh_frame .eh_frame.*)
        *(.comment)
    }
}
