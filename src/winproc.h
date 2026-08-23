#ifndef WINPROC_H
#define WINPROC_H

#include <stdint.h>
#include "pe.h"

/*
 * src/winproc.h — the three things a Windows program expects to find
 * already there.
 *
 * The loader in src/pe.h maps an image and binds its imports, which is
 * enough for a program that only calls the functions it imported. It is
 * not enough for one compiled by a real toolchain, because a real
 * toolchain assumes an environment that was set up before `main` ran:
 *
 *   the TEB and PEB   a block of per-thread state at the GS base, and a
 *                     per-process one behind it. A C runtime reads the
 *                     stack bounds out of the first thing it does, and
 *                     GS:[0x60] is how anything finds the second.
 *
 *   SEH               `__try`/`__except`. On x86-64 this is not a linked
 *                     list on the stack the way it was on 32-bit -- it
 *                     is a *table*, in .pdata and .xdata, and unwinding
 *                     is the operating system's job rather than the
 *                     program's.
 *
 *   string resources  the strings a program displays, kept in .rsrc
 *                     rather than in the code, so that translating it
 *                     means shipping a different resource section.
 *
 * ---- why table-driven exceptions change who does the work ----
 *
 * On 32-bit Windows a `__try` pushed a record onto a chain at FS:[0],
 * and the program carried its own handler list around at run time. The
 * x86-64 convention deleted that: the prologue pushes nothing, the
 * handler list is static data emitted by the compiler, and at fault time
 * *something else* has to look up the faulting address in that table,
 * find the enclosing scope, and transfer control.
 *
 * That something else is the operating system. Which is why this is a
 * kernel file and not a library: a program cannot implement `__except`
 * for itself, because at the moment of the fault the program is not
 * running -- the trap handler is.
 */

/* ---- where the per-thread block lives ----
 *
 * High in user space, clear of the image, the stack, the heap and the
 * two shared mappings. Fixed rather than allocated because the GS base
 * is written into an MSR on every context switch and a constant keeps
 * that path free of lookups.
 */
#define WIN_TEB_VA   0x00007FFF00000000ULL
#define WIN_PEB_VA   (WIN_TEB_VA + 0x1000)
/* A third page: the process parameters the PEB points at, and the
 * strings and environment block they in turn point at. See the
 * RTL_USER_PROCESS_PARAMETERS offsets below. */
#define WIN_PARAMS_VA (WIN_PEB_VA + 0x1000)

/* ---- TEB offsets, as everything compiled for Windows expects them ----
 *
 * These are not ours to choose. A C runtime reads GS:[0x08] for the
 * stack base whether or not we think that is a sensible place to put
 * it, so the layout is the published one and the names are the
 * published names.
 */
#define TEB_EXCEPTION_LIST   0x00   /* NT_TIB.ExceptionList              */
#define TEB_STACK_BASE       0x08
#define TEB_STACK_LIMIT      0x10
#define TEB_SELF             0x30   /* the TEB's own address             */
#define TEB_PROCESS_ID       0x40
#define TEB_THREAD_ID        0x48
#define TEB_PEB              0x60
#define TEB_LAST_ERROR       0x68

#define PEB_BEING_DEBUGGED   0x02
#define PEB_IMAGE_BASE       0x10
#define PEB_LDR              0x18
#define PEB_PROCESS_PARAMS   0x20
#define PEB_NUM_PROCESSORS   0x0B8
#define PEB_OS_MAJOR         0x118
#define PEB_OS_MINOR         0x11C
#define PEB_OS_BUILD         0x120

/* ---- RTL_USER_PROCESS_PARAMETERS ----
 *
 * What PEB_PROCESS_PARAMS points at, and what this system used to write
 * as a literal zero. It is where a process's current directory and its
 * environment actually live: GetCurrentDirectory and GetEnvironmentVariable
 * read through here, and a null pointer is why neither could ever have
 * answered.
 *
 * Published offsets for x86-64, not ours to choose. CurrentDirectory is
 * a CURDIR -- a UNICODE_STRING at 0x38 followed by a handle at 0x48 --
 * which is why DllPath starts at 0x50 and not 0x48.
 */
#define RUPP_MAX_LENGTH      0x00
#define RUPP_LENGTH          0x04
#define RUPP_FLAGS           0x08
#define RUPP_CURDIR_PATH     0x38   /* UNICODE_STRING */
#define RUPP_CURDIR_HANDLE   0x48
#define RUPP_DLL_PATH        0x50   /* UNICODE_STRING */
#define RUPP_IMAGE_PATH      0x60   /* UNICODE_STRING */
#define RUPP_COMMAND_LINE    0x70   /* UNICODE_STRING */
#define RUPP_ENVIRONMENT     0x80   /* PVOID          */
#define RUPP_SIZE            0x90

/* A UNICODE_STRING is { USHORT Length; USHORT MaximumLength; ULONG pad;
 * PWSTR Buffer; } -- sixteen bytes, the pointer eight in. Length counts
 * bytes and excludes the terminator; MaximumLength includes it. */
#define UNISTR_LENGTH        0x00
#define UNISTR_MAX_LENGTH    0x02
#define UNISTR_BUFFER        0x08

/* Where inside the parameters page each variable-length part is put.
 * Fixed rather than packed, because a layout computed at load time is a
 * layout that has to be recomputed to be checked. */
#define WIN_PARAMS_CURDIR_OFF  0x0100
#define WIN_PARAMS_IMAGE_OFF   0x0300
#define WIN_PARAMS_CMDLINE_OFF 0x0500
#define WIN_PARAMS_ENV_OFF     0x0700
#define WIN_PARAMS_ENV_CAP     0x0900   /* to the end of the page */

/* ---- the exception directory ---- */

#define PE_DIR_EXCEPTION  3
#define PE_DIR_RESOURCE   2

/*
 * One entry per function that has any prologue worth unwinding. The
 * array is sorted by begin_rva, which is what makes the lookup a binary
 * search rather than a walk -- and a fault handler is exactly the place
 * that matters, because it may run while the machine is already in
 * trouble.
 */
typedef struct {
    uint32_t begin_rva;
    uint32_t end_rva;
    uint32_t unwind_rva;
} __attribute__((packed)) pe_runtime_fn_t;

/*
 * The unwind information a RUNTIME_FUNCTION points at.
 *
 * `version_flags` packs a 3-bit version and 5 bits of flags. What this
 * file cares about is UNW_FLAG_EHANDLER: if it is set, an exception
 * handler RVA and its data follow the unwind codes, and that data is
 * the scope table `__try` compiles into.
 */
typedef struct {
    uint8_t version_flags;
    uint8_t prolog_size;
    uint8_t code_count;
    uint8_t frame_reg_off;
} __attribute__((packed)) pe_unwind_t;

#define UNW_FLAG_EHANDLER   0x01
#define UNW_FLAG_UHANDLER   0x02
#define UNW_FLAG_CHAININFO  0x04

/*
 * The scope table `__C_specific_handler` reads. One entry per `__try`.
 *
 * `filter` is 1 for a bare `__except(EXCEPTION_EXECUTE_HANDLER)` and
 * otherwise the RVA of a filter function that decides. `target` is where
 * to resume -- the first instruction of the `__except` block.
 */
typedef struct {
    uint32_t begin_rva;
    uint32_t end_rva;
    uint32_t filter_rva;
    uint32_t target_rva;
} __attribute__((packed)) pe_scope_entry_t;

/* ---- what the loader records about an image, for the fault path ---- */
typedef struct {
    int      valid;
    uint64_t base;
    uint64_t image_size;
    uint32_t pdata_rva;
    uint32_t pdata_size;
    uint32_t rsrc_rva;
    uint32_t rsrc_size;
    uint32_t entry_rva;
} win_image_t;

static win_image_t win_image;

/* Windows exception codes, for the ones a processor fault maps to. */
#define EXC_ACCESS_VIOLATION      0xC0000005u
#define EXC_ILLEGAL_INSTRUCTION   0xC000001Du
#define EXC_INT_DIVIDE_BY_ZERO    0xC0000094u
#define EXC_PRIV_INSTRUCTION      0xC0000096u
#define EXC_STACK_OVERFLOW        0xC00000FDu
#define EXC_BREAKPOINT            0x80000003u

static uint32_t win_exception_code(int vector) {
    switch (vector) {
    case 0:  return EXC_INT_DIVIDE_BY_ZERO;
    case 3:  return EXC_BREAKPOINT;
    case 6:  return EXC_ILLEGAL_INSTRUCTION;
    case 13: return EXC_PRIV_INSTRUCTION;
    case 14: return EXC_ACCESS_VIOLATION;
    default: return EXC_ACCESS_VIOLATION;
    }
}

static const char *win_exception_name(uint32_t code) {
    switch (code) {
    case EXC_ACCESS_VIOLATION:    return "access violation";
    case EXC_ILLEGAL_INSTRUCTION: return "illegal instruction";
    case EXC_INT_DIVIDE_BY_ZERO:  return "integer divide by zero";
    case EXC_PRIV_INSTRUCTION:    return "privileged instruction";
    case EXC_STACK_OVERFLOW:      return "stack overflow";
    case EXC_BREAKPOINT:          return "breakpoint";
    default:                      return "exception";
    }
}

/*
 * Remember where this image's tables are.
 *
 * Called by the loader. The fault path cannot go looking for them: by
 * then the faulting thread's address space may not even be the one
 * mapped, and re-parsing a PE header inside a trap handler is exactly
 * the kind of work that turns one fault into two.
 */
static void win_image_record(const pe_image_t *img) {
    win_image.valid      = 1;
    win_image.base       = img->base;
    win_image.image_size = img->image_size;
    win_image.entry_rva  = (uint32_t)(img->entry - img->base);
    win_image.pdata_rva  = img->pdata_rva;
    win_image.pdata_size = img->pdata_size;
    win_image.rsrc_rva   = img->rsrc_rva;
    win_image.rsrc_size  = img->rsrc_size;
}

static void win_image_forget(void) { win_image.valid = 0; }

/*
 * Find the RUNTIME_FUNCTION covering an address.
 *
 * Binary search, because .pdata is required to be sorted by begin_rva
 * and because this runs from a fault handler -- where a linear walk of
 * a few thousand entries is time spent with the machine in an unknown
 * state.
 *
 * `stage` is the loaded image as the kernel can see it, which is not
 * necessarily where the process sees it.
 */
static const pe_runtime_fn_t *win_lookup_function(const uint8_t *stage,
                                                  uint32_t rva) {
    if (!win_image.valid || !win_image.pdata_rva || !win_image.pdata_size)
        return 0;

    const pe_runtime_fn_t *fns =
        (const pe_runtime_fn_t *)(stage + win_image.pdata_rva);
    int n = (int)(win_image.pdata_size / sizeof(pe_runtime_fn_t));
    if (n <= 0) return 0;

    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (rva < fns[mid].begin_rva)      hi = mid - 1;
        else if (rva >= fns[mid].end_rva)  lo = mid + 1;
        else                               return &fns[mid];
    }
    return 0;
}

/*
 * Follow chained unwind info to the entry that actually carries the
 * handler.
 *
 * A function split across several ranges -- which the compiler does for
 * cold paths -- has UNW_FLAG_CHAININFO set and a RUNTIME_FUNCTION at
 * the end of its codes pointing at the parent. Without following it, a
 * fault in the cold half of a function finds no handler and the
 * `__except` around it never runs.
 */
static const pe_unwind_t *win_resolve_unwind(const uint8_t *stage,
                                             uint32_t unwind_rva,
                                             int depth) {
    if (depth > 8 || unwind_rva == 0) return 0;
    if (unwind_rva + sizeof(pe_unwind_t) > win_image.image_size) return 0;

    const pe_unwind_t *u = (const pe_unwind_t *)(stage + unwind_rva);
    if ((u->version_flags & 0x07) != 1) return 0;      /* version 1 only */

    if (u->version_flags & (UNW_FLAG_CHAININFO << 3)) {
        uint32_t codes = ((uint32_t)u->code_count + 1u) & ~1u;
        uint32_t off = unwind_rva + 4 + codes * 2;
        const pe_runtime_fn_t *parent = (const pe_runtime_fn_t *)(stage + off);
        return win_resolve_unwind(stage, parent->unwind_rva, depth + 1);
    }
    return u;
}

/*
 * The dispatcher.
 *
 * Given a fault at `rip` in a loaded PE, decide whether the program has
 * a `__try` around it. Returns 1 and fills `*resume_rva` if it does.
 *
 * The filter is *not* called. A real Windows dispatcher evaluates the
 * filter expression to get EXCEPTION_EXECUTE_HANDLER, _CONTINUE_SEARCH
 * or _CONTINUE_EXECUTION, and evaluating it means running program code
 * on the faulting thread's stack from inside a trap handler. That is
 * doable and it is not done here: this treats a filter as "handle it",
 * which is right for the bare `__except(EXCEPTION_EXECUTE_HANDLER)`
 * form and wrong for a filter that would have declined. Said plainly
 * rather than hidden, because a program whose filter declines will be
 * resumed into a handler it did not want.
 */
static int win_dispatch_exception(const uint8_t *stage, uint64_t rip,
                                  uint32_t code, uint32_t *resume_rva) {
    if (!win_image.valid) return 0;
    if (rip < win_image.base || rip >= win_image.base + win_image.image_size)
        return 0;

    uint32_t rva = (uint32_t)(rip - win_image.base);
    const pe_runtime_fn_t *fn = win_lookup_function(stage, rva);
    if (!fn) return 0;

    const pe_unwind_t *u = win_resolve_unwind(stage, fn->unwind_rva, 0);
    if (!u) return 0;
    if (!(u->version_flags & (UNW_FLAG_EHANDLER << 3))) return 0;

    /* The handler RVA sits after the unwind codes, which are padded to
     * an even count -- the array is addressed in pairs. */
    uint32_t codes = ((uint32_t)u->code_count + 1u) & ~1u;
    uint32_t off = (uint32_t)(((const uint8_t *)u - stage)) + 4 + codes * 2;
    if (off + 8 > win_image.image_size) return 0;

    /* handler RVA, then the handler's own data: for __C_specific_handler
     * that data is a count followed by that many scope entries. */
    uint32_t scope_off = off + 4;
    const uint32_t *count_p = (const uint32_t *)(stage + scope_off);
    uint32_t count = *count_p;
    if (count == 0 || count > 256) return 0;
    if (scope_off + 4 + count * sizeof(pe_scope_entry_t) > win_image.image_size)
        return 0;

    const pe_scope_entry_t *sc =
        (const pe_scope_entry_t *)(stage + scope_off + 4);

    /*
     * Innermost wins. The table is emitted outermost-first, so the last
     * entry that contains the address is the tightest `__try` around it
     * -- taking the first would run the outer handler and skip the
     * inner one, which is the opposite of what nesting means.
     */
    const pe_scope_entry_t *best = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (rva >= sc[i].begin_rva && rva < sc[i].end_rva) best = &sc[i];
    }
    if (!best || best->target_rva == 0) return 0;

    (void)code;
    *resume_rva = best->target_rva;
    return 1;
}

/* ===========================================================
 * string resources
 * =========================================================== */

typedef struct {
    uint32_t characteristics;
    uint32_t timestamp;
    uint16_t major, minor;
    uint16_t named_count;
    uint16_t id_count;
} __attribute__((packed)) rsrc_dir_t;

typedef struct {
    uint32_t name_or_id;
    uint32_t offset;            /* high bit set: another directory */
} __attribute__((packed)) rsrc_entry_t;

typedef struct {
    uint32_t data_rva;
    uint32_t size;
    uint32_t codepage;
    uint32_t reserved;
} __attribute__((packed)) rsrc_data_t;

#define RT_STRING 6

/*
 * Fetch a string by id.
 *
 * The layout is the reason this is more than an array lookup, and it is
 * a genuinely odd one: strings are stored in *bundles* of sixteen, the
 * bundle's resource id is `id / 16 + 1`, and inside the bundle the
 * strings are laid end to end -- each a 16-bit length followed by that
 * many UTF-16 code units, with empty slots present as a zero length.
 * So reaching string 33 means finding bundle 3 and then skipping the
 * first `33 % 16` entries by walking their lengths. There is no index.
 *
 * Converted to Latin-1 on the way out, because this system has no
 * UTF-16 anywhere else and a code unit above 0xFF has no representation
 * in the fonts it would be drawn with. Those become '?', which is
 * visibly wrong rather than silently truncated.
 */
static int win_load_string(const uint8_t *stage, uint32_t id,
                           char *out, int max) {
    if (!win_image.valid || !win_image.rsrc_rva || max <= 0) return 0;
    out[0] = 0;

    const uint8_t *root = stage + win_image.rsrc_rva;
    uint32_t bundle = id / 16 + 1;
    uint32_t within = id % 16;

    /* level 1: by type */
    const rsrc_dir_t *d = (const rsrc_dir_t *)root;
    const rsrc_entry_t *e = (const rsrc_entry_t *)(root + sizeof(rsrc_dir_t));
    int n = d->named_count + d->id_count;

    const uint8_t *type_dir = 0;
    for (int i = 0; i < n; i++) {
        if ((e[i].name_or_id & 0x80000000u) == 0 &&
            e[i].name_or_id == RT_STRING &&
            (e[i].offset & 0x80000000u)) {
            type_dir = root + (e[i].offset & 0x7FFFFFFF);
            break;
        }
    }
    if (!type_dir) return 0;

    /* level 2: by resource id -- the bundle */
    d = (const rsrc_dir_t *)type_dir;
    e = (const rsrc_entry_t *)(type_dir + sizeof(rsrc_dir_t));
    n = d->named_count + d->id_count;

    const uint8_t *lang_dir = 0;
    for (int i = 0; i < n; i++) {
        if ((e[i].name_or_id & 0x80000000u) == 0 &&
            e[i].name_or_id == bundle && (e[i].offset & 0x80000000u)) {
            lang_dir = root + (e[i].offset & 0x7FFFFFFF);
            break;
        }
    }
    if (!lang_dir) return 0;

    /* level 3: by language. The first is taken -- this system has one
     * locale, and picking it here rather than failing means a program
     * translated into languages we cannot display still shows *a*
     * string. */
    d = (const rsrc_dir_t *)lang_dir;
    e = (const rsrc_entry_t *)(lang_dir + sizeof(rsrc_dir_t));
    n = d->named_count + d->id_count;
    if (n <= 0 || (e[0].offset & 0x80000000u)) return 0;

    const rsrc_data_t *data = (const rsrc_data_t *)(root + e[0].offset);
    if (data->data_rva + data->size > win_image.image_size) return 0;

    const uint8_t *p   = stage + data->data_rva;
    const uint8_t *end = p + data->size;

    for (uint32_t i = 0; i < within; i++) {
        if (p + 2 > end) return 0;
        uint16_t len = (uint16_t)(p[0] | (p[1] << 8));
        p += 2 + (uint32_t)len * 2;
    }
    if (p + 2 > end) return 0;

    uint16_t len = (uint16_t)(p[0] | (p[1] << 8));
    p += 2;

    int w = 0;
    for (uint16_t i = 0; i < len && w < max - 1; i++) {
        if (p + 2 > end) break;
        uint16_t u = (uint16_t)(p[0] | (p[1] << 8));
        out[w++] = (u < 0x100) ? (char)u : '?';
        p += 2;
    }
    out[w] = 0;
    return w;
}

#endif /* WINPROC_H */
