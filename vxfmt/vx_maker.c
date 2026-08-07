/*
 * vx_maker — build a .vx executable.
 *
 * Two input modes:
 *
 *   raw    -t text.bin [-d data.bin] [-b bss_bytes]
 *          Packs flat images of x86_64 machine code and initialised
 *          data straight into the container.  This is the primitive
 *          mode: whatever bytes you hand it become the text segment.
 *
 *   elf    -e program.elf
 *          Reads a linked ELF64's program headers and repacks its two
 *          PT_LOAD segments.  This is how the Vextro app store's
 *          packages are built, because the linker is what knows the
 *          real .bss size and the text/data split.
 *
 * The output layout is always page aligned so that a loader can give
 * the text segment PROT_EXEC without also making the data executable.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vx_format.h"

#define DEFAULT_TEXT_VADDR 0x1000u

/* ===== minimal ELF64 view (we only ever read program headers) ===== */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define PT_LOAD    1
#define PF_X       1
#define EM_X86_64  62

/* ===== helpers ===== */

static void die(const char *fmt, ...)
    __attribute__((noreturn, format(printf, 1, 2)));

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("vx_maker: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static unsigned char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s: %s", path, strerror(errno));
    if (fseek(f, 0, SEEK_END) != 0) die("cannot seek %s", path);
    long n = ftell(f);
    if (n < 0) die("cannot size %s", path);
    rewind(f);

    unsigned char *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf) die("out of memory reading %s", path);
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n)
        die("short read on %s", path);
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

static uint64_t parse_u64(const char *s, const char *what) {
    char *end = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 0);
    if (errno || !end || *end) die("bad %s: %s", what, s);
    return (uint64_t)v;
}

static void pad_to(FILE *f, uint64_t target) {
    long cur = ftell(f);
    if (cur < 0) die("ftell failed: %s", strerror(errno));
    for (uint64_t i = (uint64_t)cur; i < target; i++)
        if (fputc(0, f) == EOF) die("write failed: %s", strerror(errno));
}

/* ===== ELF extraction ===== */

typedef struct {
    unsigned char *text;
    uint64_t text_size, text_vaddr;
    unsigned char *data;
    uint64_t data_size, data_vaddr;
    uint64_t bss_size;
    uint64_t entry;
} image_t;

static void from_elf(const char *path, image_t *img) {
    size_t len = 0;
    unsigned char *file = read_file(path, &len);

    if (len < sizeof(Elf64_Ehdr)) die("%s is too small to be an ELF64", path);
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)file;
    if (memcmp(eh->e_ident, "\x7f" "ELF", 4) != 0)
        die("%s is not an ELF file", path);
    if (eh->e_ident[4] != 2) die("%s is not 64-bit", path);
    if (eh->e_ident[5] != 1) die("%s is not little-endian", path);
    if (eh->e_machine != EM_X86_64)
        die("%s targets machine %u, not x86_64", path, eh->e_machine);
    if (eh->e_phoff == 0 || eh->e_phnum == 0)
        die("%s has no program headers", path);
    if (eh->e_phentsize != sizeof(Elf64_Phdr))
        die("%s has unexpected program header size %u", path, eh->e_phentsize);
    if (eh->e_phoff > len ||
        (uint64_t)eh->e_phnum * eh->e_phentsize > len - eh->e_phoff)
        die("%s program header table runs past the end of the file", path);

    const Elf64_Phdr *ph = (const Elf64_Phdr *)(file + eh->e_phoff);
    const Elf64_Phdr *seg[2] = { NULL, NULL };
    int nseg = 0;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (ph[i].p_memsz == 0) continue;
        if (nseg == 2) die("%s has more than two PT_LOAD segments; "
                           "link it with apps/vx.ld", path);
        if (ph[i].p_offset > len || ph[i].p_filesz > len - ph[i].p_offset)
            die("%s: PT_LOAD %u runs past the end of the file", path, i);
        if (ph[i].p_filesz > ph[i].p_memsz)
            die("%s: PT_LOAD %u has filesz > memsz", path, i);
        seg[nseg++] = &ph[i];
    }
    if (nseg == 0) die("%s has no loadable segments", path);

    /* lowest vaddr first, so seg[0] is the text segment */
    if (nseg == 2 && seg[0]->p_vaddr > seg[1]->p_vaddr) {
        const Elf64_Phdr *t = seg[0];
        seg[0] = seg[1];
        seg[1] = t;
    }
    if (!(seg[0]->p_flags & PF_X))
        die("%s: the lowest PT_LOAD segment is not executable", path);
    if (nseg == 2 && (seg[1]->p_flags & PF_X))
        die("%s: the data segment is executable; W^X cannot be honoured",
            path);

    img->text_vaddr = seg[0]->p_vaddr;
    img->text_size  = seg[0]->p_filesz;
    img->text       = malloc(img->text_size ? img->text_size : 1);
    if (!img->text) die("out of memory");
    memcpy(img->text, file + seg[0]->p_offset, img->text_size);

    if (nseg == 2) {
        img->data_vaddr = seg[1]->p_vaddr;
        img->data_size  = seg[1]->p_filesz;
        img->bss_size   = seg[1]->p_memsz - seg[1]->p_filesz;
        img->data       = malloc(img->data_size ? img->data_size : 1);
        if (!img->data) die("out of memory");
        memcpy(img->data, file + seg[1]->p_offset, img->data_size);
    } else {
        /* a single segment may still carry .bss past its file image */
        img->data_vaddr = 0;
        img->data_size  = 0;
        img->bss_size   = seg[0]->p_memsz - seg[0]->p_filesz;
        if (img->bss_size)
            die("%s: .bss is inside the text segment; link it with "
                "apps/vx.ld so .bss lands in a separate PT_LOAD", path);
    }

    img->entry = eh->e_entry;
    free(file);
}

/* ===== usage ===== */

static void usage(FILE *out) {
    fputs(
"usage: vx_maker -o OUT.vx -t TEXT.bin [-d DATA.bin] [-b BSS_BYTES] [opts]\n"
"       vx_maker -o OUT.vx -e PROGRAM.elf [opts]\n"
"\n"
"  -o, --output FILE     .vx image to write (required)\n"
"  -t, --text FILE       raw x86_64 machine code for the text segment\n"
"  -d, --data FILE       raw bytes for the data segment\n"
"  -b, --bss N           zero-filled bytes to append after the data\n"
"  -e, --elf FILE        take both segments from a linked ELF64 instead\n"
"      --text-vaddr A    where the text segment loads (default 0x1000)\n"
"      --data-vaddr A    where the data segment loads (default: the page\n"
"                        after the text segment)\n"
"      --entry A         entry point address (default: --text-vaddr)\n"
"  -v, --verbose         print the resulting header\n"
"  -h, --help            this message\n"
"\n"
"Addresses accept 0x, 0 and decimal forms.\n", out);
}

int main(int argc, char **argv) {
    const char *out_path = NULL, *text_path = NULL, *data_path = NULL;
    const char *elf_path = NULL;
    uint64_t bss_size = 0;
    uint64_t text_vaddr = 0, data_vaddr = 0, entry = 0;
    int have_text_vaddr = 0, have_data_vaddr = 0, have_entry = 0;
    int verbose = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT(what) (++i < argc ? argv[i] : (die("%s needs a value", what), ""))
        if (!strcmp(a, "-o") || !strcmp(a, "--output"))      out_path = NEXT(a);
        else if (!strcmp(a, "-t") || !strcmp(a, "--text"))   text_path = NEXT(a);
        else if (!strcmp(a, "-d") || !strcmp(a, "--data"))   data_path = NEXT(a);
        else if (!strcmp(a, "-e") || !strcmp(a, "--elf"))    elf_path = NEXT(a);
        else if (!strcmp(a, "-b") || !strcmp(a, "--bss"))
            bss_size = parse_u64(NEXT(a), "bss size");
        else if (!strcmp(a, "--text-vaddr")) {
            text_vaddr = parse_u64(NEXT(a), "text vaddr"); have_text_vaddr = 1;
        } else if (!strcmp(a, "--data-vaddr")) {
            data_vaddr = parse_u64(NEXT(a), "data vaddr"); have_data_vaddr = 1;
        } else if (!strcmp(a, "--entry")) {
            entry = parse_u64(NEXT(a), "entry"); have_entry = 1;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) verbose = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
        else die("unknown option %s (try --help)", a);
        #undef NEXT
    }

    if (!out_path) { usage(stderr); die("no output file given"); }
    if (!elf_path && !text_path) { usage(stderr); die("need -t or -e"); }
    if (elf_path && (text_path || data_path || bss_size))
        die("-e is exclusive with -t/-d/-b");

    image_t img;
    memset(&img, 0, sizeof(img));

    if (elf_path) {
        from_elf(elf_path, &img);
        if (have_text_vaddr || have_data_vaddr)
            die("--text-vaddr/--data-vaddr come from the ELF in -e mode");
        if (have_entry) img.entry = entry;
    } else {
        size_t n = 0;
        img.text = read_file(text_path, &n);
        img.text_size = n;
        if (img.text_size == 0) die("%s is empty", text_path);

        if (data_path) {
            img.data = read_file(data_path, &n);
            img.data_size = n;
        }
        img.bss_size = bss_size;

        img.text_vaddr = have_text_vaddr ? text_vaddr : DEFAULT_TEXT_VADDR;
        img.data_vaddr = have_data_vaddr
            ? data_vaddr
            : img.text_vaddr + vx_round_up(img.text_size, VX_PAGE_SIZE);
        img.entry = have_entry ? entry : img.text_vaddr;
    }

    /* ---- lay the file out ---- */
    vx_header_t h;
    memset(&h, 0, sizeof(h));
    h.magic[0] = VX_MAGIC0;
    h.magic[1] = VX_MAGIC1;
    h.magic[2] = VX_MAGIC2;
    h.magic[3] = (uint8_t)VX_MAGIC3;
    h.version    = VX_VERSION;
    h.entry      = img.entry;
    h.text_vaddr = img.text_vaddr;
    h.text_size  = img.text_size;
    h.data_vaddr = img.data_vaddr;
    h.data_size  = img.data_size;
    h.bss_size   = img.bss_size;
    h.flags      = VX_F_NONE;

    /* Segments start on a page, and each file offset is congruent to
     * its virtual address, so a loader can mmap() straight from the fd. */
    h.text_off = VX_PAGE_SIZE;
    /* A pure-.bss data segment contributes nothing to the file, so its
     * offset stays 0 rather than pointing past the end of it. */
    h.data_off = img.data_size
        ? h.text_off + vx_round_up(img.text_size, VX_PAGE_SIZE)
        : 0;

    uint64_t file_size = img.data_size ? h.data_off + img.data_size
                                       : h.text_off + img.text_size;

    const char *bad = vx_validate(&h, file_size);
    if (bad) die("refusing to write a bad image: %s", bad);

    FILE *f = fopen(out_path, "wb");
    if (!f) die("cannot create %s: %s", out_path, strerror(errno));

    if (fwrite(&h, sizeof(h), 1, f) != 1) die("write failed");
    pad_to(f, h.text_off);
    if (fwrite(img.text, 1, img.text_size, f) != img.text_size)
        die("write failed");
    if (img.data_size) {
        pad_to(f, h.data_off);
        if (fwrite(img.data, 1, img.data_size, f) != img.data_size)
            die("write failed");
    }
    if (fclose(f) != 0) die("cannot close %s: %s", out_path, strerror(errno));

    if (verbose) {
        printf("%s\n", out_path);
        printf("  magic       %c%c%c 0x%02X\n", h.magic[0], h.magic[1],
               h.magic[2], h.magic[3]);
        printf("  version     %u\n", h.version);
        printf("  entry       0x%016llx\n", (unsigned long long)h.entry);
        printf("  text        vaddr 0x%08llx  off %6llu  size %6llu\n",
               (unsigned long long)h.text_vaddr,
               (unsigned long long)h.text_off,
               (unsigned long long)h.text_size);
        printf("  data        vaddr 0x%08llx  off %6llu  size %6llu\n",
               (unsigned long long)h.data_vaddr,
               (unsigned long long)h.data_off,
               (unsigned long long)h.data_size);
        printf("  bss         %llu bytes\n", (unsigned long long)h.bss_size);
        printf("  image span  %llu bytes\n",
               (unsigned long long)vx_image_span(&h));
    } else {
        printf("vx_maker: %s (text %llu, data %llu, bss %llu)\n", out_path,
               (unsigned long long)h.text_size,
               (unsigned long long)h.data_size,
               (unsigned long long)h.bss_size);
    }

    free(img.text);
    free(img.data);
    return 0;
}
