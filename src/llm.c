#include "llm.h"

/*
 * GGUF loading and the arena the weights live in.
 *
 * Built with SSE2 on, unlike the rest of the kernel.  See llm.h.
 */

/* ---- arena ----
 * A bump allocator over the largest usable region Limine reported.
 * Nothing is ever freed: a model is loaded once and lives until reboot,
 * so a free list would be dead weight. */

static uint8_t  *arena_base = 0;
static uint64_t  arena_size = 0;
static uint64_t  arena_head = 0;

void llm_arena_init(void *base, uint64_t size) {
    arena_base = (uint8_t *)base;
    arena_size = size;
    arena_head = 0;
}

uint64_t llm_arena_total(void) { return arena_size; }
uint64_t llm_arena_used(void)  { return arena_head; }

/* used once weight loading lands; the arena itself is already live */
__attribute__((unused))
static void *arena_alloc(uint64_t n) {
    uint64_t aligned = (arena_head + 63) & ~(uint64_t)63;   /* cache line */
    if (!arena_base || aligned + n > arena_size) return 0;
    void *p = arena_base + aligned;
    arena_head = aligned + n;
    return p;
}

/* ---- GGUF ---- */

#define GGUF_MAGIC 0x46554747u      /* "GGUF" little-endian */

/* metadata value types */
enum {
    GT_U8 = 0, GT_I8, GT_U16, GT_I16, GT_U32, GT_I32,
    GT_F32, GT_BOOL, GT_STRING, GT_ARRAY, GT_U64, GT_I64, GT_F64
};

/* tensor quantisation types, in GGUF's own numbering */
static const char *quant_names[] = {
    "F32", "F16", "Q4_0", "Q4_1", "?", "?", "Q5_0", "Q5_1",
    "Q8_0", "Q8_1", "Q2_K", "Q3_K", "Q4_K", "Q5_K", "Q6_K", "Q8_K",
    "IQ2_XXS", "IQ2_XS", "IQ3_XXS", "IQ1_S", "IQ4_NL", "IQ3_S",
    "IQ2_S", "IQ4_XS", "I8", "I16", "I32", "I64", "F64", "IQ1_M",
    "BF16", "?"
};

const char *llm_quant_name(uint32_t t) {
    return t < 32 ? quant_names[t] : "?";
}

/* Elements per block and bytes per block, per quantisation type.  Only
 * what this model actually contains is filled in; anything else is
 * rejected rather than guessed at. */
static int quant_block(uint32_t t, uint32_t *elems, uint32_t *bytes) {
    switch (t) {
    case 0:  *elems = 1;  *bytes = 4;   return 0;   /* F32  */
    case 1:  *elems = 1;  *bytes = 2;   return 0;   /* F16  */
    case 2:  *elems = 32; *bytes = 18;  return 0;   /* Q4_0: d + 16 nibbles  */
    case 3:  *elems = 32; *bytes = 20;  return 0;   /* Q4_1: d, m + nibbles  */
    case 6:  *elems = 32; *bytes = 22;  return 0;   /* Q5_0: d, qh + nibbles */
    case 7:  *elems = 32; *bytes = 24;  return 0;   /* Q5_1: d, m, qh + qs   */
    case 8:  *elems = 32; *bytes = 34;  return 0;   /* Q8_0 */
    case 9:  *elems = 32; *bytes = 40;  return 0;   /* Q8_1 */
    case 12: *elems = 256; *bytes = 144; return 0;  /* Q4_K */
    case 13: *elems = 256; *bytes = 176; return 0;  /* Q5_K */
    case 14: *elems = 256; *bytes = 210; return 0;  /* Q6_K */
    default: return -1;
    }
}

static llm_info_t info;

const llm_info_t *llm_get_info(void) { return &info; }

/* ---- a streaming reader over the callback ---- */

typedef struct {
    llm_read_fn rd;
    void       *ctx;
    uint64_t    pos;
    uint64_t    size;
    int         failed;
    uint8_t     buf[4096];
    uint64_t    buf_off;
    uint32_t    buf_len;
} greader_t;

static int g_fill(greader_t *g, uint64_t off) {
    uint32_t got = 0;
    if (g->rd(g->ctx, off, g->buf, sizeof(g->buf), &got) != 0 || got == 0) {
        g->failed = 1;
        return -1;
    }
    g->buf_off = off;
    g->buf_len = got;
    return 0;
}

static int g_bytes(greader_t *g, void *out, uint32_t n) {
    uint8_t *o = (uint8_t *)out;
    while (n > 0) {
        if (g->pos < g->buf_off || g->pos >= g->buf_off + g->buf_len) {
            if (g_fill(g, g->pos) != 0) return -1;
        }
        uint64_t avail = g->buf_off + g->buf_len - g->pos;
        uint32_t take = n < avail ? n : (uint32_t)avail;
        const uint8_t *src = g->buf + (g->pos - g->buf_off);
        for (uint32_t i = 0; i < take; i++) o[i] = src[i];
        o += take;
        g->pos += take;
        n -= take;
    }
    return 0;
}

static uint8_t  g_u8(greader_t *g)  { uint8_t v = 0;  g_bytes(g, &v, 1); return v; }
static uint16_t g_u16(greader_t *g) { uint8_t b[2]; g_bytes(g, b, 2);
    return (uint16_t)(b[0] | (b[1] << 8)); }
static uint32_t g_u32(greader_t *g) { uint8_t b[4]; g_bytes(g, b, 4);
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24); }
static uint64_t g_u64(greader_t *g) {
    uint64_t lo = g_u32(g), hi = g_u32(g);
    return lo | (hi << 32);
}
static float g_f32(greader_t *g) {
    union { uint32_t u; float f; } c;
    c.u = g_u32(g);
    return c.f;
}

static int str_same(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

/* Read a length-prefixed string into out (truncating), always consuming
 * the whole field. */
static void g_str(greader_t *g, char *out, int max) {
    uint64_t n = g_u64(g);
    uint64_t i = 0;
    int o = 0;
    while (i < n) {
        uint8_t c = g_u8(g);
        if (o < max - 1) out[o++] = (char)c;
        i++;
        if (g->failed) break;
    }
    if (out && max > 0) out[o] = '\0';
}

static void g_skip_value(greader_t *g, uint32_t type);

static void g_skip_one(greader_t *g, uint32_t type) {
    switch (type) {
    case GT_U8: case GT_I8: case GT_BOOL: g_u8(g);  break;
    case GT_U16: case GT_I16:             g_u16(g); break;
    case GT_U32: case GT_I32: case GT_F32: g_u32(g); break;
    case GT_U64: case GT_I64: case GT_F64: g_u64(g); break;
    case GT_STRING: { char tmp[2]; g_str(g, tmp, 2); break; }
    default: g->failed = 1; break;
    }
}

static void g_skip_value(greader_t *g, uint32_t type) {
    if (type == GT_ARRAY) {
        uint32_t et = g_u32(g);
        uint64_t n = g_u64(g);
        for (uint64_t i = 0; i < n && !g->failed; i++) g_skip_one(g, et);
        return;
    }
    g_skip_one(g, type);
}

int llm_load(llm_read_fn rd, void *ctx, uint64_t file_size, const char **err) {
    static greader_t g;
    for (uint32_t i = 0; i < sizeof(g); i++) ((uint8_t *)&g)[i] = 0;
    g.rd = rd;
    g.ctx = ctx;
    g.size = file_size;
    g.buf_len = 0;
    g.buf_off = 0xFFFFFFFFFFFFFFFFull;

    for (uint32_t i = 0; i < sizeof(info); i++) ((uint8_t *)&info)[i] = 0;
    info.file_size = file_size;

    if (g_u32(&g) != GGUF_MAGIC) { *err = "not a GGUF file"; return -1; }
    uint32_t version = g_u32(&g);
    if (version != 2 && version != 3) {
        *err = "unsupported GGUF version";
        return -1;
    }

    uint64_t n_tensors = g_u64(&g);
    uint64_t n_kv = g_u64(&g);
    if (g.failed) { *err = "truncated GGUF header"; return -1; }
    if (n_tensors == 0 || n_tensors > 4096) { *err = "implausible tensor count"; return -1; }
    info.n_tensors = n_tensors;

    /* ---- metadata ---- */
    for (uint64_t i = 0; i < n_kv && !g.failed; i++) {
        char key[96];
        g_str(&g, key, sizeof(key));
        uint32_t type = g_u32(&g);

        if (str_same(key, "general.architecture") && type == GT_STRING) {
            g_str(&g, info.arch, sizeof(info.arch));
        } else if (str_same(key, "general.name") && type == GT_STRING) {
            g_str(&g, info.name, sizeof(info.name));
        } else if (type == GT_U32 || type == GT_I32) {
            uint32_t v = g_u32(&g);
            if      (str_same(key, "qwen2.block_count"))            info.n_layer = v;
            else if (str_same(key, "qwen2.embedding_length"))       info.n_embd = v;
            else if (str_same(key, "qwen2.attention.head_count"))   info.n_head = v;
            else if (str_same(key, "qwen2.attention.head_count_kv")) info.n_head_kv = v;
            else if (str_same(key, "qwen2.feed_forward_length"))    info.n_ff = v;
            else if (str_same(key, "qwen2.context_length"))         info.n_ctx_train = v;
        } else if (type == GT_F32) {
            float v = g_f32(&g);
            if      (str_same(key, "qwen2.rope.freq_base"))                 info.rope_freq_base = v;
            else if (str_same(key, "qwen2.attention.layer_norm_rms_epsilon")) info.rms_eps = v;
        } else if (type == GT_ARRAY) {
            /* the tokenizer arrays are the only big ones; note the vocab
             * size and skip the payload for now */
            uint32_t et = g_u32(&g);
            uint64_t n = g_u64(&g);
            if (str_same(key, "tokenizer.ggml.tokens")) info.n_vocab = (uint32_t)n;
            for (uint64_t k = 0; k < n && !g.failed; k++) g_skip_one(&g, et);
        } else {
            g_skip_value(&g, type);
        }
    }
    if (g.failed) { *err = "truncated GGUF metadata"; return -1; }

    if (!str_same(info.arch, "qwen2")) {
        *err = "only the qwen2 architecture is implemented";
        return -1;
    }
    if (info.n_layer == 0 || info.n_embd == 0 || info.n_head == 0) {
        *err = "model metadata is missing its shape";
        return -1;
    }

    /* ---- tensor table ---- */
    uint64_t total_bytes = 0;
    for (uint64_t i = 0; i < n_tensors && !g.failed; i++) {
        char tname[LLM_NAME_MAX];
        g_str(&g, tname, sizeof(tname));
        uint32_t ndim = g_u32(&g);
        if (ndim == 0 || ndim > 4) { *err = "bad tensor rank"; return -1; }
        uint64_t elems = 1;
        for (uint32_t d = 0; d < ndim; d++) elems *= g_u64(&g);
        uint32_t qt = g_u32(&g);
        (void)g_u64(&g);                       /* offset within the blob */

        uint32_t be, bb;
        if (quant_block(qt, &be, &bb) != 0) {
            *err = "tensor uses a quantisation this build cannot read";
            return -1;
        }
        if (elems % be) { *err = "tensor size is not a whole number of blocks"; return -1; }
        total_bytes += (elems / be) * bb;
        if (qt < 32) info.quant_counts[qt]++;
    }
    if (g.failed) { *err = "truncated tensor table"; return -1; }

    info.weight_bytes = total_bytes;
    info.loaded = 1;
    return 0;
}

/*
 * A deliberately float-heavy calculation, so a caller can prove the FPU
 * is live rather than assuming it.  Sums a series that converges on
 * pi^2/6, which is wrong in an obvious way if SSE is not really enabled.
 */
int llm_fpu_selftest(uint32_t *scaled) {
    volatile float acc = 0.0f;
    for (int i = 1; i <= 20000; i++) {
        float x = (float)i;
        acc += 1.0f / (x * x);
    }
    *scaled = (uint32_t)(acc * 10000.0f);
    /* expect ~1.6449; allow slack for float32 accumulation order */
    return (acc > 1.6f && acc < 1.7f) ? 0 : -1;
}
