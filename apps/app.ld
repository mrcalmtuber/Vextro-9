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
