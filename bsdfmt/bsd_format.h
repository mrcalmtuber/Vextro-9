#ifndef BSD_FORMAT_H
#define BSD_FORMAT_H

#include <stdint.h>

/*
 * .bsd — a minimal executable container for x86_64.
 *
 * The whole format is one fixed header followed by two images:
 *
 *      offset 0        bsd_header_t                    (80 bytes)
 *      text_off        text image      -> mapped R-X
 *      data_off        data image      -> mapped RW-, then bss_size
 *                                         zero bytes appended
 *
 * Design rules, all enforced by bsd_validate():
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

#define BSD_MAGIC0     'B'
#define BSD_MAGIC1     'S'
#define BSD_MAGIC2     'D'
#define BSD_MAGIC3     0x64        /* x86_64 */

#define BSD_VERSION    1u
#define BSD_PAGE_SIZE  4096u

/* Sanity ceiling on the loaded span; a loader may impose a lower one. */
#define BSD_MAX_IMAGE  (64u * 1024u * 1024u)

/* flags */
#define BSD_F_NONE     0u

typedef struct {
    uint8_t  magic[4];     /* 'B', 'S', 'D', 0x64                     */
    uint32_t version;      /* BSD_VERSION; also aligns what follows   */
    uint64_t entry;        /* virtual address of the entry point      */
    uint64_t text_off;     /* file offset of the text image           */
    uint64_t text_size;    /* bytes of text in the file               */
    uint64_t data_off;     /* file offset of the data image           */
    uint64_t data_size;    /* bytes of data in the file               */
    uint64_t text_vaddr;   /* where the text image wants to live      */
    uint64_t data_vaddr;   /* where the data image wants to live      */
    uint64_t bss_size;     /* zero bytes to append after the data     */
    uint64_t flags;        /* BSD_F_*                                 */
} bsd_header_t;

_Static_assert(sizeof(bsd_header_t) == 80, "bsd_header_t must be 80 bytes");
_Static_assert(_Alignof(bsd_header_t) == 8, "bsd_header_t must be 8-aligned");


/* ===== imported symbols =====
 *
 * A `.bsd` image carries no relocations, which is what lets a loader put
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
 * with a static assertion on it, `bsd_validate` is shared verbatim by the
 * host packer, the kernel loader and the store's download path, and all
 * three would have to change together for a feature none of them need to
 * know about. A self-describing table in the data segment costs an eight
 * byte tag and leaves every existing image byte-for-byte valid.
 *
 * The loader searches for the tag rather than requiring it at a fixed
 * offset, so no linker script has to guarantee placement — which is the
 * kind of requirement that works until someone adds a global.
 */
#define BSD_IMPORT_MAGIC 0x53524D50495F4253ULL   /* "SB_IMPRS", LE */
#define BSD_IMPORT_NAMELEN 24
#define BSD_IMPORT_MAX 32          /* refuse anything larger as malformed */

typedef struct {
    char     name[BSD_IMPORT_NAMELEN];
    uint64_t addr;                 /* zero in the file; filled by the loader */
} bsd_import_t;

typedef struct {
    uint64_t magic;                /* BSD_IMPORT_MAGIC                      */
    uint32_t count;                /* how many entries follow               */
    uint32_t reserved;             /* keeps the entries 8-aligned           */
} bsd_import_hdr_t;

_Static_assert(sizeof(bsd_import_t) == 32, "bsd_import_t must be 32 bytes");
_Static_assert(sizeof(bsd_import_hdr_t) == 16, "bsd_import_hdr_t must be 16 bytes");

/* What the kernel offers. Name and address, terminated by a null name. */
typedef struct {
    const char *name;
    uint64_t    addr;
} bsd_export_t;

/* ===== helpers (freestanding: no libc, no allocation) ===== */

static inline int bsd_magic_ok(const bsd_header_t *h) {
    return h->magic[0] == BSD_MAGIC0 && h->magic[1] == BSD_MAGIC1 &&
           h->magic[2] == BSD_MAGIC2 && h->magic[3] == (uint8_t)BSD_MAGIC3;
}

static inline uint64_t bsd_round_up(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

/* Highest virtual address the loaded image touches, exclusive. */
static inline uint64_t bsd_image_end(const bsd_header_t *h) {
    uint64_t end = h->text_vaddr + h->text_size;
    if (h->data_size || h->bss_size) {
        uint64_t d = h->data_vaddr + h->data_size + h->bss_size;
        if (d > end) end = d;
    }
    return end;
}

/* Bytes of address space the image needs, from text_vaddr upwards. */
static inline uint64_t bsd_image_span(const bsd_header_t *h) {
    return bsd_image_end(h) - h->text_vaddr;
}

/*
 * Returns NULL when the header describes an image that is safe to load
 * out of a file of file_size bytes, or a reason string when it does not.
 *
 * Payloads arrive over the network, so every arithmetic step here is
 * written to be overflow-safe rather than merely correct for sane input.
 */
static inline const char *bsd_validate(const bsd_header_t *h,
                                       uint64_t file_size) {
    if (file_size < sizeof(bsd_header_t)) return "file is smaller than the header";
    if (!bsd_magic_ok(h))                 return "bad magic - not a .bsd image";
    if (h->version != BSD_VERSION)        return "unsupported format version";
    if (h->text_size == 0)                return "empty text section";

    /* text lies inside the file */
    if (h->text_off > file_size || h->text_size > file_size - h->text_off)
        return "text section runs past the end of the file";
    if (h->text_off < sizeof(bsd_header_t))
        return "text section overlaps the header";

    /* text is mappable */
    if (h->text_vaddr & (BSD_PAGE_SIZE - 1))
        return "text_vaddr is not page aligned";
    if ((h->text_off & (BSD_PAGE_SIZE - 1)) !=
        (h->text_vaddr & (BSD_PAGE_SIZE - 1)))
        return "text_off is not congruent to text_vaddr";
    if (h->text_size > UINT64_MAX - h->text_vaddr)
        return "text section wraps the address space";

    if (h->data_size) {
        if (h->data_off > file_size || h->data_size > file_size - h->data_off)
            return "data section runs past the end of the file";
        if (h->data_off < sizeof(bsd_header_t))
            return "data section overlaps the header";
        if ((h->data_off & (BSD_PAGE_SIZE - 1)) !=
            (h->data_vaddr & (BSD_PAGE_SIZE - 1)))
            return "data_off is not congruent to data_vaddr";
    }

    if (h->data_size || h->bss_size) {
        if (h->data_vaddr & (BSD_PAGE_SIZE - 1))
            return "data_vaddr is not page aligned";
        if (h->data_size > UINT64_MAX - h->data_vaddr ||
            h->bss_size > UINT64_MAX - h->data_vaddr - h->data_size)
            return "data segment wraps the address space";
        /* a shared page would force RWX on both segments */
        if (h->data_vaddr < h->text_vaddr +
                            bsd_round_up(h->text_size, BSD_PAGE_SIZE))
            return "data segment shares a page with the text segment";
    }

    if (h->entry < h->text_vaddr ||
        h->entry >= h->text_vaddr + h->text_size)
        return "entry point is outside the text section";

    if (bsd_image_span(h) > BSD_MAX_IMAGE)
        return "image is larger than the format allows";

    return 0;
}

#endif /* BSD_FORMAT_H */
