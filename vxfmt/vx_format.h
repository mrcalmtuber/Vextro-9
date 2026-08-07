#ifndef VX_FORMAT_H
#define VX_FORMAT_H

#include <stdint.h>

/*
 * .vx — a minimal executable container for x86_64.
 *
 * The whole format is one fixed header followed by two images:
 *
 *      offset 0        vx_header_t                    (80 bytes)
 *      text_off        text image      -> mapped R-X
 *      data_off        data image      -> mapped RW-, then bss_size
 *                                         zero bytes appended
 *
 * Design rules, all enforced by vx_validate():
 *
 *   - Every field after the magic is uint64_t, and a uint32_t version
 *     pads the 4-byte magic out to 8, so no field straddles an 8-byte
 *     boundary and the header can be read straight out of a mapping
 *     with no unaligned access on any x86_64 ABI.
 *
 *   - text_vaddr and data_vaddr are page aligned and the data segment
 *     starts on a page after the last text page.  Without that a loader
 *     could not mprotect() the two segments differently: protection has
 *     page granularity, so a shared page would have to be RWX and the
 *     W^X guarantee would be gone.
 *
 *   - File offsets are congruent to their virtual addresses modulo the
 *     page size, so a loader may mmap() the segments directly from the
 *     file descriptor instead of copying.
 *
 * Images are expected to be position independent (RIP-relative, no
 * relocations): there is no relocation table, so a loader is free to
 * place the image anywhere as long as it preserves the distance between
 * the text and data segments.
 *
 * This header is shared by the host tools and by the Vextro kernel,
 * so it stays freestanding: stdint.h and nothing else.
 */

/*
 * 0x7F leads, the way ELF's does, so that a plain text file can never be
 * mistaken for an executable -- no printable byte sequence starts here.
 * Then the format's name, then the machine, so one glance at a hexdump
 * says both what the file is and what it will run on.
 */
#define VX_MAGIC0     0x7F
#define VX_MAGIC1     'V'
#define VX_MAGIC2     'X'
#define VX_MAGIC3     0x64        /* x86_64 */

#define VX_VERSION    1u
#define VX_PAGE_SIZE  4096u

/* Sanity ceiling on the loaded span; a loader may impose a lower one. */
#define VX_MAX_IMAGE  (64u * 1024u * 1024u)

/* flags */
#define VX_F_NONE     0u

typedef struct {
    uint8_t  magic[4];     /* 0x7F, 'V', 'X', 0x64                    */
    uint32_t version;      /* VX_VERSION; also aligns what follows   */
    uint64_t entry;        /* virtual address of the entry point      */
    uint64_t text_off;     /* file offset of the text image           */
    uint64_t text_size;    /* bytes of text in the file               */
    uint64_t data_off;     /* file offset of the data image           */
    uint64_t data_size;    /* bytes of data in the file               */
    uint64_t text_vaddr;   /* where the text image wants to live      */
    uint64_t data_vaddr;   /* where the data image wants to live      */
    uint64_t bss_size;     /* zero bytes to append after the data     */
    uint64_t flags;        /* VX_F_*                                 */
} vx_header_t;

_Static_assert(sizeof(vx_header_t) == 80, "vx_header_t must be 80 bytes");
_Static_assert(_Alignof(vx_header_t) == 8, "vx_header_t must be 8-aligned");


/* ===== imported symbols =====
 *
 * A `.vx` image carries no relocations, which is what lets a loader put
 * it anywhere as long as the text-to-data distance is preserved. That
 * also means it has no way to reference anything outside itself, so every
 * application statically contains whatever it uses — fine for a fractal,
 * expensive for anything that wants the TrueType rasteriser.
 *
 * This is the smallest thing that fixes it without giving up the property
 * that makes the format simple. The application places one initialised
 * structure in its data: a magic word, a count, and that many name/address
 * pairs with the addresses left zero. The loader finds the structure,
 * looks each name up in the kernel's export table, and writes the
 * addresses in. The application then calls through them.
 *
 * Deliberately *not* a header field. The header is a fixed eighty bytes
 * with a static assertion on it, `vx_validate` is shared verbatim by the
 * host packer, the kernel loader and the store's download path, and all
 * three would have to change together for a feature none of them need to
 * know about. A self-describing table in the data segment costs an eight
 * byte tag and leaves every existing image byte-for-byte valid.
 *
 * The loader searches for the tag rather than requiring it at a fixed
 * offset, so no linker script has to guarantee placement — which is the
 * kind of requirement that works until someone adds a global.
 */
#define VX_IMPORT_MAGIC 0x5352504D495F5856ULL   /* "VX_IMPRS", LE */
#define VX_IMPORT_NAMELEN 24
#define VX_IMPORT_MAX 32          /* refuse anything larger as malformed */

typedef struct {
    char     name[VX_IMPORT_NAMELEN];
    uint64_t addr;                 /* zero in the file; filled by the loader */
} vx_import_t;

typedef struct {
    uint64_t magic;                /* VX_IMPORT_MAGIC                      */
    uint32_t count;                /* how many entries follow               */
    uint32_t reserved;             /* keeps the entries 8-aligned           */
} vx_import_hdr_t;

_Static_assert(sizeof(vx_import_t) == 32, "vx_import_t must be 32 bytes");
_Static_assert(sizeof(vx_import_hdr_t) == 16, "vx_import_hdr_t must be 16 bytes");

/* What the kernel offers. Name and address, terminated by a null name. */
typedef struct {
    const char *name;
    uint64_t    addr;
} vx_export_t;

/* ===== helpers (freestanding: no libc, no allocation) ===== */

static inline int vx_magic_ok(const vx_header_t *h) {
    return h->magic[0] == VX_MAGIC0 && h->magic[1] == VX_MAGIC1 &&
           h->magic[2] == VX_MAGIC2 && h->magic[3] == (uint8_t)VX_MAGIC3;
}

static inline uint64_t vx_round_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Highest virtual address the loaded image touches, exclusive. */
static inline uint64_t vx_image_end(const vx_header_t *h) {
    uint64_t end = h->text_vaddr + h->text_size;
    if (h->data_size || h->bss_size) {
        uint64_t d = h->data_vaddr + h->data_size + h->bss_size;
        if (d > end) end = d;
    }
    return end;
}

/* Bytes of address space the image needs, from text_vaddr upwards. */
static inline uint64_t vx_image_span(const vx_header_t *h) {
    return vx_image_end(h) - h->text_vaddr;
}

/*
 * Returns NULL when the header describes an image that is safe to load
 * out of a file of file_size bytes, or a reason string when it does not.
 *
 * Payloads arrive over the network, so every arithmetic step here is
 * written to be overflow-safe rather than merely correct for sane input.
 */
static inline const char *vx_validate(const vx_header_t *h,
                                       uint64_t file_size) {
    if (file_size < sizeof(vx_header_t)) return "file is smaller than the header";
    if (!vx_magic_ok(h))                 return "bad magic - not a .vx image";
    if (h->version != VX_VERSION)        return "unsupported format version";
    if (h->text_size == 0)                return "empty text section";

    /* text lies inside the file */
    if (h->text_off > file_size || h->text_size > file_size - h->text_off)
        return "text section runs past the end of the file";
    if (h->text_off < sizeof(vx_header_t))
        return "text section overlaps the header";

    /* text is mappable */
    if (h->text_vaddr & (VX_PAGE_SIZE - 1))
        return "text_vaddr is not page aligned";
    if ((h->text_off & (VX_PAGE_SIZE - 1)) !=
        (h->text_vaddr & (VX_PAGE_SIZE - 1)))
        return "text_off is not congruent to text_vaddr";
    if (h->text_size > UINT64_MAX - h->text_vaddr)
        return "text section wraps the address space";

    if (h->data_size) {
        if (h->data_off > file_size || h->data_size > file_size - h->data_off)
            return "data section runs past the end of the file";
        if (h->data_off < sizeof(vx_header_t))
            return "data section overlaps the header";
        if ((h->data_off & (VX_PAGE_SIZE - 1)) !=
            (h->data_vaddr & (VX_PAGE_SIZE - 1)))
            return "data_off is not congruent to data_vaddr";
    }

    if (h->data_size || h->bss_size) {
        if (h->data_vaddr & (VX_PAGE_SIZE - 1))
            return "data_vaddr is not page aligned";
        if (h->data_size > UINT64_MAX - h->data_vaddr ||
            h->bss_size > UINT64_MAX - h->data_vaddr - h->data_size)
            return "data segment wraps the address space";
        /* a shared page would force RWX on both segments */
        if (h->data_vaddr < h->text_vaddr +
                            vx_round_up(h->text_size, VX_PAGE_SIZE))
            return "data segment shares a page with the text segment";
    }

    if (h->entry < h->text_vaddr ||
        h->entry >= h->text_vaddr + h->text_size)
        return "entry point is outside the text section";

    if (vx_image_span(h) > VX_MAX_IMAGE)
        return "image is larger than the format allows";

    return 0;
}

#endif /* VX_FORMAT_H */
