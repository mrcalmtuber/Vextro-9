#ifndef PE_H
#define PE_H

/*
 * src/pe.h — Windows executables.
 *
 * The Portable Executable format is what a .exe and a .dll are: a DOS
 * stub nobody has run since 1995, a signature, a COFF header, an
 * optional header that is not optional, and a table of sections with
 * their own addresses and protections.
 *
 * Three things make it materially different from the ELF loader already
 * here, and all three are the reason this is a separate file rather than
 * a branch inside that one.
 *
 *   Relocations. A PE image states the address it was linked for and
 *   carries a list of every place in itself that depends on it. Load it
 *   somewhere else and each of those gets the difference added. That is
 *   what lets an image move — which is what the ELF path could never do,
 *   and why address space randomisation here stops at the stack.
 *
 *   Imports. A PE names the libraries it needs and the function in each,
 *   and the loader writes the resolved addresses into a table the image
 *   reads through. Nothing is patched into the code; every call goes
 *   through an indirection that the loader owns.
 *
 *   Section protections. Each section says whether it is executable,
 *   writable or read-only, and the mapping honours all three separately.
 *
 * What this does not do is implement Win32. An image that calls
 * CreateWindowExW will find that name unresolved and be refused, with
 * the name printed, which is a far more useful outcome than a jump to
 * zero. What is implemented is the loading, the linking, and a small set
 * of exports from a built-in library so that a program compiled for this
 * system can be linked as a PE and run.
 */

#include <stdint.h>
#include "vmm.h"

#define PE_DOS_MAGIC   0x5A4D          /* "MZ" */
#define PE_NT_MAGIC    0x00004550      /* "PE\0\0" */
#define PE_MACHINE_X64 0x8664
#define PE_MACHINE_X86 0x014C

#define PE_OPT_MAGIC_32 0x010B
#define PE_OPT_MAGIC_64 0x020B

#define PE_DIR_EXPORT   0
#define PE_DIR_IMPORT   1
#define PE_DIR_RESOURCE 2
#define PE_DIR_EXCEPTION 3
#define PE_DIR_BASERELOC 5
#define PE_DIR_TLS      9

#define PE_SCN_CODE      0x00000020
#define PE_SCN_INIT_DATA 0x00000040
#define PE_SCN_UNINIT    0x00000080
#define PE_SCN_EXEC      0x20000000
#define PE_SCN_READ      0x40000000
#define PE_SCN_WRITE     0x80000000

typedef struct {
    uint16_t e_magic;
    uint16_t e_pad[29];
    uint32_t e_lfanew;             /* where the real header starts */
} __attribute__((packed)) pe_dos_t;

typedef struct {
    uint16_t machine;
    uint16_t sections;
    uint32_t timestamp;
    uint32_t sym_table;
    uint32_t sym_count;
    uint16_t opt_size;
    uint16_t characteristics;
} __attribute__((packed)) pe_coff_t;

typedef struct {
    uint32_t rva;
    uint32_t size;
} __attribute__((packed)) pe_dir_t;

typedef struct {
    uint16_t magic;
    uint8_t  linker_major, linker_minor;
    uint32_t code_size, data_size, bss_size;
    uint32_t entry_rva;
    uint32_t code_base;
    uint64_t image_base;
    uint32_t section_align, file_align;
    uint16_t os_major, os_minor, image_major, image_minor;
    uint16_t sub_major, sub_minor;
    uint32_t win32_version;
    uint32_t image_size, headers_size, checksum;
    uint16_t subsystem, dll_flags;
    uint64_t stack_reserve, stack_commit, heap_reserve, heap_commit;
    uint32_t loader_flags, dir_count;
    pe_dir_t dir[16];
} __attribute__((packed)) pe_opt64_t;

typedef struct {
    char     name[8];
    uint32_t virtual_size;
    uint32_t virtual_addr;
    uint32_t raw_size;
    uint32_t raw_ptr;
    uint32_t reloc_ptr, line_ptr;
    uint16_t reloc_count, line_count;
    uint32_t characteristics;
} __attribute__((packed)) pe_section_t;

typedef struct {
    uint32_t lookup_rva;           /* the names being asked for */
    uint32_t timestamp;
    uint32_t forwarder;
    uint32_t name_rva;             /* the library's own name    */
    uint32_t address_rva;          /* where to write the answers */
} __attribute__((packed)) pe_import_desc_t;

typedef struct {
    uint32_t flags, timestamp;
    uint16_t major, minor;
    uint32_t name_rva, ordinal_base;
    uint32_t addr_count, name_count;
    uint32_t addr_rva, name_rva_table, ordinal_rva;
} __attribute__((packed)) pe_export_t;

/* ---- what the loader produces ---- */
typedef struct {
    uint64_t base;                 /* where it actually landed  */
    uint64_t entry;                /* absolute, after relocation */
    uint64_t image_size;
    int      sections;
    int      relocations;
    int      imports_resolved;
    int      imports_missing;
    char     missing[64];          /* the first name that was not found */
} pe_image_t;

/* ===== THE BUILT-IN LIBRARY =====
 *
 * What a PE compiled for this system can import. The names are the ones
 * a C runtime would ask for, so an ordinary program links against them
 * without knowing it is not on Windows.
 *
 * A name that is not here is not guessed at. The loader refuses the
 * image and prints what it wanted, which turns "it crashed" into "it
 * wanted GetProcAddress and there isn't one".
 */
typedef struct {
    const char *library;
    const char *name;
    uint64_t    stub;              /* filled with a trampoline address */
} pe_export_entry_t;

static pe_export_entry_t pe_exports[] = {
    { "vextro.dll", "VxPrint",      0 },
    { "vextro.dll", "VxExit",       0 },
    { "vextro.dll", "VxDrawPixel",  0 },
    { "vextro.dll", "VxGetMouse",   0 },
    { "vextro.dll", "VxYield",      0 },
    { "vextro.dll", "VxCanvas",     0 },
    { "vextro.dll", "VxTicks",      0 },
    { "vextro.dll", "VxTextWidth",  0 },
    { "vextro.dll", "VxDrawString", 0 },
    { "vextro.dll", "VxFillRect",   0 },
    { "vextro.dll", "VxAlloc",      0 },
    { 0, 0, 0 }
};

static int pe_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        a++; b++;
    }
    return *a == *b;
}

static uint64_t pe_lookup_export(const char *lib, const char *fn) {
    for (int i = 0; pe_exports[i].name; i++) {
        if (lib && !pe_name_eq(pe_exports[i].library, lib)) continue;
        if (pe_name_eq(pe_exports[i].name, fn)) return pe_exports[i].stub;
    }
    return 0;
}

/* ---- staging ----
 *
 * The image is assembled in one flat buffer at its own relative
 * addresses, relocated and linked there, and only then copied into the
 * address space that will run it. Same shape as the ELF path, for the
 * same reason: a malformed header is rejected before any page of the new
 * address space has been touched.
 */
static uint8_t *pe_stage = 0;
static uint64_t pe_stage_size = 0;
static uint8_t *pe_page_prot = 0;

static int pe_stage_ready(uint64_t need) {
    if (pe_stage && pe_stage_size >= need) return 1;
    if (pe_stage) { kfree(pe_stage); kfree(pe_page_prot); }
    pe_stage = (uint8_t *)kmalloc_paged(need);
    pe_page_prot = (uint8_t *)kmalloc_paged(need / 4096 + 1);
    pe_stage_size = pe_stage ? need : 0;
    return pe_stage && pe_page_prot;
}

static void *pe_rva(uint64_t rva) {
    if (rva >= pe_stage_size) return 0;
    return pe_stage + rva;
}

/*
 * Apply the base relocation table.
 *
 * A block per 4 KB page: the page's own address, the size of the block,
 * then one 16-bit entry per fixup — four bits of type and twelve of
 * offset within the page. Type 10 is the only one that occurs in a
 * 64-bit image and means "add the delta to the eight bytes here".
 *
 * An image with no relocation table can only be loaded at the address it
 * was linked for. That is not an error; it is what /FIXED means.
 */
static int pe_relocate(const pe_opt64_t *opt, uint64_t new_base) {
    int64_t delta = (int64_t)(new_base - opt->image_base);
    if (delta == 0) return 0;

    const pe_dir_t *d = &opt->dir[PE_DIR_BASERELOC];
    if (!d->rva || !d->size) return -1;      /* cannot move: no table */

    uint32_t done = 0, fixups = 0;
    while (done + 8 <= d->size) {
        const uint32_t *blk = (const uint32_t *)pe_rva(d->rva + done);
        if (!blk) break;
        uint32_t page  = blk[0];
        uint32_t bsize = blk[1];
        if (bsize < 8 || done + bsize > d->size) break;

        uint32_t n = (bsize - 8) / 2;
        const uint16_t *ent = (const uint16_t *)(blk + 2);
        for (uint32_t i = 0; i < n; i++) {
            uint16_t type = ent[i] >> 12;
            uint16_t off  = ent[i] & 0xFFF;
            if (type == 0) continue;                     /* padding */
            if (type != 10) continue;                    /* not 64-bit */
            uint64_t *p = (uint64_t *)pe_rva(page + off);
            if (!p) continue;
            *p = (uint64_t)((int64_t)*p + delta);
            fixups++;
        }
        done += bsize;
    }
    return (int)fixups;
}

/*
 * Fill in the import address table.
 *
 * Each descriptor names a library and points at two parallel arrays: the
 * lookup table, which says what is wanted, and the address table, which
 * is where the answers go. In a file on disk they usually hold the same
 * values; the loader overwrites the second.
 *
 * An entry with the top bit set is an ordinal rather than a name. This
 * refuses those: importing by ordinal means the caller and the library
 * agreed on a number, and nothing here has ever published one.
 */
static int pe_link_imports(const pe_opt64_t *opt, pe_image_t *img) {
    const pe_dir_t *d = &opt->dir[PE_DIR_IMPORT];
    if (!d->rva || !d->size) return 0;

    int resolved = 0, missing = 0;
    for (uint32_t k = 0; ; k++) {
        const pe_import_desc_t *desc =
            (const pe_import_desc_t *)pe_rva(d->rva + k * sizeof(pe_import_desc_t));
        if (!desc || !desc->name_rva) break;

        const char *lib = (const char *)pe_rva(desc->name_rva);
        if (!lib) break;

        uint32_t lookup = desc->lookup_rva ? desc->lookup_rva : desc->address_rva;
        for (uint32_t i = 0; ; i++) {
            const uint64_t *want = (const uint64_t *)pe_rva(lookup + i * 8);
            uint64_t *slot = (uint64_t *)pe_rva(desc->address_rva + i * 8);
            if (!want || !slot || !*want) break;

            uint64_t addr = 0;
            if (*want & (1ULL << 63)) {
                /* by ordinal */
                addr = 0;
            } else {
                const char *fn = (const char *)pe_rva((uint32_t)*want + 2);
                if (fn) addr = pe_lookup_export(lib, fn);
                if (!addr && fn && !img->missing[0]) {
                    int n = 0;
                    while (lib[n] && n < 24) { img->missing[n] = lib[n]; n++; }
                    img->missing[n++] = '!';
                    int m = 0;
                    while (fn[m] && n < 62) img->missing[n++] = fn[m++];
                    img->missing[n] = '\0';
                }
            }
            if (addr) { *slot = addr; resolved++; }
            else      { *slot = 0;    missing++;  }

            /* Say what was bound to what. An import table is the one
             * place where a wrong answer looks exactly like a right one
             * until the program jumps through it. */
            serial_puts("[pe]   ");
            serial_puts(lib);
            serial_puts("!");
            {
                const char *fn = (*want & (1ULL << 63)) ? "(ordinal)"
                               : (const char *)pe_rva((uint32_t)*want + 2);
                serial_puts(fn ? fn : "(bad name)");
            }
            serial_puts(" -> ");
            serial_put_hex32((uint32_t)addr);
            serial_puts(" at slot ");
            serial_put_hex32(desc->address_rva + i * 8);
            serial_puts("\n");
        }
    }
    img->imports_resolved = resolved;
    img->imports_missing  = missing;
    return missing ? -1 : resolved;
}

/*
 * Load a PE64 image into a staging buffer and tell the caller where its
 * pages go. `load_base` is where it will be mapped, which the caller
 * chooses -- randomised, since relocations make that possible.
 */
static int pe_load(const uint8_t *file, uint64_t fsize, uint64_t load_base,
                   pe_image_t *img) {
    if (fsize < sizeof(pe_dos_t)) return -1;
    const pe_dos_t *dos = (const pe_dos_t *)file;
    if (dos->e_magic != PE_DOS_MAGIC) return -1;
    if (dos->e_lfanew + 24 > fsize) return -1;

    const uint8_t *nt = file + dos->e_lfanew;
    if (*(const uint32_t *)nt != PE_NT_MAGIC) return -1;

    const pe_coff_t *coff = (const pe_coff_t *)(nt + 4);
    if (coff->machine != PE_MACHINE_X64) return -2;   /* 32-bit: not here */
    if (coff->sections == 0 || coff->sections > 96) return -1;

    const pe_opt64_t *opt = (const pe_opt64_t *)(nt + 4 + sizeof(pe_coff_t));
    if (opt->magic != PE_OPT_MAGIC_64) return -2;
    if (opt->image_size < opt->headers_size) return -1;
    if (opt->image_size > 64u * 1024 * 1024) return -1;

    if (!pe_stage_ready(opt->image_size)) return -1;
    for (uint64_t i = 0; i < opt->image_size; i++) pe_stage[i] = 0;
    for (uint64_t i = 0; i < opt->image_size / 4096 + 1; i++) pe_page_prot[i] = 0;
    pe_stage_size = opt->image_size;

    /* Headers first: an image may read its own. */
    for (uint32_t i = 0; i < opt->headers_size && i < fsize; i++)
        pe_stage[i] = file[i];
    for (uint32_t p = 0; p < (opt->headers_size + 4095) / 4096; p++)
        pe_page_prot[p] = APROT_READ;

    const pe_section_t *sec =
        (const pe_section_t *)(nt + 4 + sizeof(pe_coff_t) + coff->opt_size);

    for (int i = 0; i < coff->sections; i++) {
        const pe_section_t *sc = &sec[i];
        if (sc->virtual_addr + sc->virtual_size > opt->image_size) continue;

        uint32_t n = sc->raw_size < sc->virtual_size ? sc->raw_size
                                                     : sc->virtual_size;
        if (sc->raw_ptr + n <= fsize)
            for (uint32_t k = 0; k < n; k++)
                pe_stage[sc->virtual_addr + k] = file[sc->raw_ptr + k];

        uint8_t prot = APROT_READ;
        if (sc->characteristics & PE_SCN_WRITE) prot |= APROT_WRITE;
        if (sc->characteristics & PE_SCN_EXEC)  prot |= APROT_EXEC;
        uint32_t p0 = sc->virtual_addr / 4096;
        uint32_t p1 = (sc->virtual_addr + sc->virtual_size + 4095) / 4096;
        for (uint32_t p = p0; p < p1; p++) pe_page_prot[p] |= prot;
    }

    img->base       = load_base;
    img->image_size = opt->image_size;
    img->sections   = coff->sections;
    img->missing[0] = '\0';

    int rel = pe_relocate(opt, load_base);
    if (rel < 0) {
        /* No table: it has to go where it was linked. */
        img->base = opt->image_base;
        rel = 0;
    }
    img->relocations = rel;

    if (pe_link_imports(opt, img) < 0 && img->imports_missing)
        return -3;

    img->entry = img->base + opt->entry_rva;
    return 0;
}

static const char *pe_error(int rc) {
    switch (rc) {
    case -1: return "malformed or truncated PE image";
    case -2: return "not a 64-bit PE image (32-bit is not supported here)";
    case -3: return "imports this system does not provide";
    default: return "unknown PE error";
    }
}

#endif /* PE_H */
