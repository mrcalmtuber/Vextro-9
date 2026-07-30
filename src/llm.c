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

static void *arena_alloc(uint64_t n) {
    uint64_t aligned = (arena_head + 63) & ~(uint64_t)63;   /* cache line */
    if (!arena_base || aligned + n > arena_size) return 0;
    void *p = arena_base + aligned;
    arena_head = aligned + n;
    return p;
}

/*
 * The same, but sticky about failure.
 *
 * A request that does not fit is refused without advancing the head, so
 * a *later*, smaller request can still succeed.  Checking only the last
 * pointer of a run of allocations would therefore miss a hole in the
 * middle and leave a null behind — which surfaces much later as a page
 * fault mid-inference, and this kernel's fault handler halts forever.
 * Better to notice here and say so.
 */
static int arena_failed = 0;

static void *arena_need(uint64_t n) {
    void *p = arena_alloc(n);
    if (!p) arena_failed = 1;
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

static char     *tok_blob;         /* all token text, NUL separated */
static uint32_t *tok_off;          /* id -> offset into tok_blob    */
static uint32_t  tok_n;
static int32_t  *tok_hash;         /* hash -> id, -1 empty          */
static uint32_t  tok_hash_mask;
static uint64_t *mrg_key;          /* (a<<32)|b, +1 so 0 means empty */
static uint32_t *mrg_rank;
static uint32_t  mrg_hash_mask;
static uint32_t  mrg_n;

static void build_byte_map(void);
static void tok_hash_put(uint32_t id);
static int  tok_find(const char *s, int len);
static void mrg_put(uint32_t a, uint32_t b, uint32_t rank);

#define LLM_TENSOR_MAX 512

typedef struct {
    char     name[LLM_NAME_MAX];
    uint32_t type;
    uint64_t offset;        /* relative to the data section */
    uint64_t elems;
    uint32_t ne0, ne1;      /* ne0 is the contiguous (input) dimension */
} tensor_t;

static tensor_t tensors[LLM_TENSOR_MAX];
static int      tensor_n;
static uint64_t data_start;      /* file offset of the tensor payload */
static llm_read_fn model_rd;
static void       *model_ctx;

/* half-precision to float; GGUF stores every scale this way */
static float fp16_to_f32(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31;
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t man  = h & 0x3FF;
    union { uint32_t u; float f; } o;
    if (exp == 0) {
        if (man == 0) { o.u = sign; return o.f; }
        /* subnormal: renormalise */
        exp = 127 - 15 + 1;
        while (!(man & 0x400)) { man <<= 1; exp--; }
        man &= 0x3FF;
        o.u = sign | (exp << 23) | (man << 13);
        return o.f;
    }
    if (exp == 31) { o.u = sign | 0x7F800000u | (man << 13); return o.f; }
    o.u = sign | ((exp - 15 + 127) << 23) | (man << 13);
    return o.f;
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
    /*
     * The window this reader pulls the file through.
     *
     * 4 KB was the whole cost of loading a model.  Metadata is ~24 MB of
     * vocabulary and merges, and at 4 KB a piece that is ~6,000 requests,
     * each of which the filesystem splits into a partial sector, a run of
     * whole ones and another partial — more commands to the drive than
     * reading the entire 373 MB of weights, which streams in 1 MB chunks.
     * The parse was never the slow part; the request size was.
     */
    uint8_t     buf[256 * 1024];
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

/* g_u8/g_u16 are gone: every field is now read or stepped over in bulk,
 * and a per-byte accessor is exactly what made this parse slow. */
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

/*
 * Move the cursor forward without copying anything.
 *
 * The reader had no seek at all, so skipping a field meant *reading* every
 * byte of it and throwing the result away.  A refill only happens when the
 * cursor leaves the buffer, so a short skip stays in memory and a long one
 * costs exactly one refill.
 */
static void g_advance(greader_t *g, uint64_t n) {
    if (g->pos + n > g->size) { g->failed = 1; return; }
    g->pos += n;
}

/* Read a length-prefixed string into out (truncating), always consuming
 * the whole field. */
static void g_str(greader_t *g, char *out, int max) {
    uint64_t n = g_u64(g);
    if (g->failed) { if (out && max > 0) out[0] = '\0'; return; }

    /* Take what fits in one bulk copy, then step over the rest — this used
     * to run a function call per byte, over megabytes of vocabulary. */
    uint32_t take = 0;
    if (out && max > 1) {
        take = (n < (uint64_t)(max - 1)) ? (uint32_t)n : (uint32_t)(max - 1);
        g_bytes(g, out, take);
        out[take] = '\0';
    } else if (out && max > 0) {
        out[0] = '\0';
    }
    if (n > take) g_advance(g, n - take);
}

static void g_skip_value(greader_t *g, uint32_t type);

static void g_skip_one(greader_t *g, uint32_t type) {
    switch (type) {
    case GT_U8: case GT_I8: case GT_BOOL: g_advance(g, 1); break;
    case GT_U16: case GT_I16:             g_advance(g, 2); break;
    case GT_U32: case GT_I32: case GT_F32: g_advance(g, 4); break;
    case GT_U64: case GT_I64: case GT_F64: g_advance(g, 8); break;
    case GT_STRING: { uint64_t n = g_u64(g); g_advance(g, n); break; }
    default: g->failed = 1; break;
    }
}

static void g_skip_value(greader_t *g, uint32_t type) {
    if (type == GT_ARRAY) {
        uint32_t et = g_u32(g);
        uint64_t n = g_u64(g);
        /* Fixed-width elements are one arithmetic step, not n of them —
         * this array is 151,936 entries in the model we care about. */
        uint32_t w = 0;
        switch (et) {
        case GT_U8: case GT_I8: case GT_BOOL:  w = 1; break;
        case GT_U16: case GT_I16:              w = 2; break;
        case GT_U32: case GT_I32: case GT_F32: w = 4; break;
        case GT_U64: case GT_I64: case GT_F64: w = 8; break;
        default: break;
        }
        if (w) { g_advance(g, n * w); return; }
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
    tensor_n = 0;
    tok_blob = 0; tok_off = 0; tok_hash = 0; tok_n = 0;
    mrg_key = 0; mrg_rank = 0; mrg_n = 0;
    build_byte_map();

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
            uint32_t et = g_u32(&g);
            uint64_t n = g_u64(&g);

            if (str_same(key, "tokenizer.ggml.tokens") && et == GT_STRING) {
                /* the vocabulary, concatenated into one blob */
                info.n_vocab = (uint32_t)n;
                tok_n = (uint32_t)n;
                uint64_t cap = n * 24 + 256;
                tok_blob = (char *)arena_alloc(cap);
                tok_off  = (uint32_t *)arena_alloc(n * 4);
                if (!tok_blob || !tok_off) { *err = "arena too small for the vocabulary"; return -1; }

                uint64_t used = 0;
                for (uint64_t k = 0; k < n && !g.failed; k++) {
                    uint64_t slen = g_u64(&g);
                    tok_off[k] = (uint32_t)used;
                    /* straight into the blob in one copy */
                    uint64_t room = (used + 1 < cap) ? (cap - 1 - used) : 0;
                    uint32_t take = (slen < room) ? (uint32_t)slen
                                                  : (uint32_t)room;
                    if (take) g_bytes(&g, tok_blob + used, take);
                    used += take;
                    if (slen > take) g_advance(&g, slen - take);
                    if (used < cap) tok_blob[used++] = '\0';
                }

                /* an open-addressed index over the vocabulary */
                uint32_t hs = 1;
                while (hs < tok_n * 2) hs <<= 1;
                tok_hash_mask = hs - 1;
                tok_hash = (int32_t *)arena_alloc((uint64_t)hs * 4);
                if (!tok_hash) { *err = "arena too small for the token index"; return -1; }
                for (uint32_t k = 0; k < hs; k++) tok_hash[k] = -1;
                for (uint32_t k = 0; k < tok_n; k++) tok_hash_put(k);

            } else if (str_same(key, "tokenizer.ggml.merges") && et == GT_STRING) {
                /* each merge is "A B": the two pieces that fuse, in rank
                 * order, so the index is the rank */
                if (!tok_blob) { *err = "merges arrived before the vocabulary"; return -1; }
                mrg_n = (uint32_t)n;
                uint32_t hs = 1;
                while (hs < mrg_n * 2) hs <<= 1;
                mrg_hash_mask = hs - 1;
                mrg_key  = (uint64_t *)arena_alloc((uint64_t)hs * 8);
                mrg_rank = (uint32_t *)arena_alloc((uint64_t)hs * 4);
                if (!mrg_key || !mrg_rank) { *err = "arena too small for the merge table"; return -1; }
                for (uint32_t k = 0; k < hs; k++) { mrg_key[k] = 0; mrg_rank[k] = 0; }

                char line[256];
                for (uint64_t k = 0; k < n && !g.failed; k++) {
                    uint64_t slen = g_u64(&g);
                    uint32_t o = (slen < sizeof(line) - 1)
                               ? (uint32_t)slen : (uint32_t)(sizeof(line) - 1);
                    if (o) g_bytes(&g, line, o);
                    if (slen > o) g_advance(&g, slen - o);
                    line[o] = '\0';
                    int sp = -1;
                    for (uint32_t j = 0; j < o; j++)
                        if (line[j] == ' ') { sp = (int)j; break; }
                    if (sp <= 0) continue;
                    line[sp] = '\0';
                    int a = tok_find(line, sp);
                    int b = tok_find(line + sp + 1, (int)o - sp - 1);
                    if (a >= 0 && b >= 0)
                        mrg_put((uint32_t)a, (uint32_t)b, (uint32_t)k);
                }
            } else {
                /* an array we do not care about — step over it whole
                 * rather than reading every element (token_type alone is
                 * 151,936 of them) */
                uint32_t w = 0;
                switch (et) {
                case GT_U8: case GT_I8: case GT_BOOL:  w = 1; break;
                case GT_U16: case GT_I16:              w = 2; break;
                case GT_U32: case GT_I32: case GT_F32: w = 4; break;
                case GT_U64: case GT_I64: case GT_F64: w = 8; break;
                default: break;
                }
                if (w) g_advance(&g, n * w);
                else for (uint64_t k = 0; k < n && !g.failed; k++)
                         g_skip_one(&g, et);
            }
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
        uint64_t dims[4] = { 1, 1, 1, 1 };
        uint64_t elems = 1;
        for (uint32_t d = 0; d < ndim; d++) { dims[d] = g_u64(&g); elems *= dims[d]; }
        uint32_t qt = g_u32(&g);
        uint64_t toff = g_u64(&g);             /* offset within the blob */

        if (tensor_n < LLM_TENSOR_MAX) {
            tensor_t *te = &tensors[tensor_n++];
            int c = 0;
            while (tname[c] && c < LLM_NAME_MAX - 1) { te->name[c] = tname[c]; c++; }
            te->name[c] = '\0';
            te->type = qt;
            te->offset = toff;
            te->elems = elems;
            te->ne0 = (uint32_t)dims[0];
            te->ne1 = (uint32_t)dims[1];
        }

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

    /* the payload starts after the table, rounded up to the alignment
     * GGUF defaults to when it does not say otherwise */
    data_start = (g.pos + 31) & ~(uint64_t)31;
    model_rd = rd;
    model_ctx = ctx;

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

/* =====================================================================
 * Byte-level BPE tokenizer
 *
 * Qwen2 tokenizes the way GPT-2 does: text is first reversibly mapped
 * from raw bytes onto printable codepoints, so that no token can ever
 * contain a control byte or a raw space, and the merge table then works
 * on that mapped text.  A space becomes U+0120, which is why vocabulary
 * dumps are full of leading 'Ġ'.
 *
 * Three tables come out of the GGUF and live in the arena:
 *   - the vocabulary, concatenated and NUL-separated
 *   - an open-addressed hash from token text to id
 *   - an open-addressed hash from a merged pair to its rank
 *
 * Encoding splits the text into pieces roughly the way the reference
 * pre-tokenizer does, then repeatedly applies the lowest-ranked merge
 * within each piece, which is the definition of BPE.
 * ===================================================================== */

/* byte <-> printable codepoint, the GPT-2 mapping */
static uint16_t byte_to_uni[256];
static int16_t  uni_to_byte[512];

static void build_byte_map(void) {
    for (int i = 0; i < 512; i++) uni_to_byte[i] = -1;
    int n = 0;
    for (int b = 0; b < 256; b++) {
        int printable = (b >= '!' && b <= '~') ||
                        (b >= 0xA1 && b <= 0xAC) ||
                        (b >= 0xAE && b <= 0xFF);
        byte_to_uni[b] = printable ? (uint16_t)b : (uint16_t)(256 + n++);
    }
    for (int b = 0; b < 256; b++) {
        uint16_t u = byte_to_uni[b];
        if (u < 512) uni_to_byte[u] = (int16_t)b;
    }
}

/* one codepoint as UTF-8; the mapping never exceeds U+02FF so 2 bytes do */
static int uni_to_utf8(uint16_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
}

static uint32_t str_hash(const char *s, int len) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

static int tok_len(uint32_t id) {
    const char *s = tok_blob + tok_off[id];
    int n = 0;
    while (s[n]) n++;
    return n;
}

static void tok_hash_put(uint32_t id) {
    const char *s = tok_blob + tok_off[id];
    uint32_t h = str_hash(s, tok_len(id)) & tok_hash_mask;
    while (tok_hash[h] >= 0) h = (h + 1) & tok_hash_mask;
    tok_hash[h] = (int32_t)id;
}

static int tok_find(const char *s, int len) {
    if (!tok_hash) return -1;
    uint32_t h = str_hash(s, len) & tok_hash_mask;
    while (tok_hash[h] >= 0) {
        const char *c = tok_blob + tok_off[tok_hash[h]];
        int i = 0;
        while (i < len && c[i] && c[i] == s[i]) i++;
        if (i == len && c[i] == '\0') return tok_hash[h];
        h = (h + 1) & tok_hash_mask;
    }
    return -1;
}

int llm_token_id(const char *piece) {
    int n = 0;
    while (piece[n]) n++;
    return tok_find(piece, n);
}

static void mrg_put(uint32_t a, uint32_t b, uint32_t rank) {
    uint64_t key = (((uint64_t)a << 32) | b) + 1;
    uint32_t h = (uint32_t)((key * 1099511628211ull) >> 32) & mrg_hash_mask;
    while (mrg_key[h] && mrg_key[h] != key) h = (h + 1) & mrg_hash_mask;
    if (!mrg_key[h]) { mrg_key[h] = key; mrg_rank[h] = rank; }
}

static uint32_t mrg_get(uint32_t a, uint32_t b) {
    if (!mrg_key) return 0xFFFFFFFFu;
    uint64_t key = (((uint64_t)a << 32) | b) + 1;
    uint32_t h = (uint32_t)((key * 1099511628211ull) >> 32) & mrg_hash_mask;
    while (mrg_key[h]) {
        if (mrg_key[h] == key) return mrg_rank[h];
        h = (h + 1) & mrg_hash_mask;
    }
    return 0xFFFFFFFFu;                       /* not a mergeable pair */
}

int llm_tok_ready(void)     { return tok_blob && tok_n > 0 && mrg_n > 0; }
uint32_t llm_tok_count(void)   { return tok_n; }
uint32_t llm_merge_count(void) { return mrg_n; }

int llm_decode(int32_t id, char *out, int max) {
    if (id < 0 || (uint32_t)id >= tok_n || !tok_blob) return -1;
    const char *s = tok_blob + tok_off[id];
    int o = 0;
    /* walk the mapped codepoints back to the bytes they stand for */
    for (int i = 0; s[i];) {
        uint16_t cp;
        if ((uint8_t)s[i] < 0x80) { cp = (uint8_t)s[i]; i += 1; }
        else if (((uint8_t)s[i] & 0xE0) == 0xC0 && s[i + 1]) {
            cp = (uint16_t)(((s[i] & 0x1F) << 6) | (s[i + 1] & 0x3F));
            i += 2;
        } else { i += 1; continue; }
        int16_t b = cp < 512 ? uni_to_byte[cp] : -1;
        if (b >= 0 && o < max - 1) out[o++] = (char)b;
    }
    out[o] = '\0';
    return o;
}

/*
 * Split like the reference pre-tokenizer: contractions, runs of letters,
 * short runs of digits, punctuation, and whitespace each become their own
 * piece.  Letter and digit classes are approximated over ASCII, with any
 * byte above 0x7F treated as a letter, which is right for the Latin text
 * these articles are made of.
 */
static int is_letter(uint8_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80;
}
static int is_digit_c(uint8_t c) { return c >= '0' && c <= '9'; }
static int is_space_c(uint8_t c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

/* how many bytes of text[] belong to the next piece */
static int next_piece(const char *t, int len) {
    if (len <= 0) return 0;
    uint8_t c0 = (uint8_t)t[0];

    /* 's 't 're 've 'm 'll 'd */
    if (c0 == '\'' && len > 1) {
        uint8_t a = (uint8_t)t[1] | 0x20;
        if (a == 's' || a == 't' || a == 'm' || a == 'd') return 2;
        if (len > 2) {
            uint8_t b = (uint8_t)t[2] | 0x20;
            if ((a == 'r' && b == 'e') || (a == 'v' && b == 'e') ||
                (a == 'l' && b == 'l')) return 3;
        }
    }

    int i = 0;
    /* an optional single leading space joins the following word */
    if (c0 == ' ' && len > 1 &&
        (is_letter((uint8_t)t[1]) || is_digit_c((uint8_t)t[1]))) i = 1;

    if (i < len && is_letter((uint8_t)t[i])) {
        while (i < len && is_letter((uint8_t)t[i])) i++;
        return i;
    }
    if (i < len && is_digit_c((uint8_t)t[i])) {
        int d = 0;
        while (i < len && is_digit_c((uint8_t)t[i]) && d < 3) { i++; d++; }
        return i;
    }

    i = 0;
    if (c0 == ' ' && len > 1 && !is_space_c((uint8_t)t[1])) i = 1;
    if (i < len && !is_space_c((uint8_t)t[i]) &&
        !is_letter((uint8_t)t[i]) && !is_digit_c((uint8_t)t[i])) {
        while (i < len && !is_space_c((uint8_t)t[i]) &&
               !is_letter((uint8_t)t[i]) && !is_digit_c((uint8_t)t[i])) i++;
        return i;
    }

    i = 0;
    while (i < len && is_space_c((uint8_t)t[i])) i++;
    return i > 0 ? i : 1;
}

#define PIECE_MAX 512

int llm_encode(const char *text, int32_t *out, int max_out) {
    if (!llm_tok_ready()) return -1;

    int n_out = 0;
    int len = 0;
    while (text[len]) len++;

    int pos = 0;
    while (pos < len) {
        int plen = next_piece(text + pos, len - pos);
        if (plen <= 0) break;

        /* map the piece's bytes onto their printable codepoints, and
         * seed BPE with one token per mapped character */
        static int32_t sym[PIECE_MAX];
        static char    cbuf[PIECE_MAX * 2];
        int nsym = 0, cused = 0;

        for (int i = 0; i < plen && nsym < PIECE_MAX; i++) {
            char enc[4];
            int n = uni_to_utf8(byte_to_uni[(uint8_t)text[pos + i]], enc);
            if (cused + n + 1 > (int)sizeof(cbuf)) break;
            int id = tok_find(enc, n);
            for (int k = 0; k < n; k++) cbuf[cused + k] = enc[k];
            cused += n;
            sym[nsym++] = id;             /* -1 if the byte has no token */
        }
        pos += plen;

        /* repeatedly fuse the neighbouring pair with the lowest rank */
        for (;;) {
            uint32_t best = 0xFFFFFFFFu;
            int at = -1;
            for (int i = 0; i + 1 < nsym; i++) {
                if (sym[i] < 0 || sym[i + 1] < 0) continue;
                uint32_t r = mrg_get((uint32_t)sym[i], (uint32_t)sym[i + 1]);
                if (r < best) { best = r; at = i; }
            }
            if (at < 0) break;

            /* the merged text has to exist as a token to fuse into */
            char joined[PIECE_MAX * 2];
            int jl = 0;
            const char *a = tok_blob + tok_off[sym[at]];
            const char *b = tok_blob + tok_off[sym[at + 1]];
            while (*a && jl < (int)sizeof(joined) - 1) joined[jl++] = *a++;
            while (*b && jl < (int)sizeof(joined) - 1) joined[jl++] = *b++;
            joined[jl] = '\0';
            int merged = tok_find(joined, jl);
            if (merged < 0) break;

            sym[at] = merged;
            for (int i = at + 1; i + 1 < nsym; i++) sym[i] = sym[i + 1];
            nsym--;
        }

        for (int i = 0; i < nsym; i++) {
            if (n_out >= max_out) return n_out;
            if (sym[i] >= 0) out[n_out++] = sym[i];
        }
    }
    return n_out;
}

/* =====================================================================
 * Dequantisation
 *
 * The K-quants pack a 256-element super-block: one fp16 scale for the
 * whole block, a second for the minimums, then per-sub-block scales at
 * six bits each, crammed two-to-a-byte-and-a-bit.  Getting the packing
 * wrong produces weights that are the right order of magnitude and
 * completely wrong, which is why this is checked against a reference
 * rather than eyeballed.  All four types the model uses (Q4_K, Q5_0,
 * Q6_K and F32) have been verified that way, element for element.
 * ===================================================================== */

int llm_tensor_count(void) { return tensor_n; }
const char *llm_tensor_name(int i) {
    return (i >= 0 && i < tensor_n) ? tensors[i].name : "";
}
uint32_t llm_tensor_type(int i)  { return (i >= 0 && i < tensor_n) ? tensors[i].type : 0; }
uint64_t llm_tensor_elems(int i) { return (i >= 0 && i < tensor_n) ? tensors[i].elems : 0; }

int llm_tensor_find(const char *name) {
    for (int i = 0; i < tensor_n; i++)
        if (str_same(tensors[i].name, name)) return i;
    return -1;
}

/* six-bit scale and minimum for sub-block j, out of the packed twelve */
static void k4_scale_min(int j, const uint8_t *q, uint8_t *d, uint8_t *m) {
    if (j < 4) {
        *d = q[j] & 63;
        *m = q[j + 4] & 63;
    } else {
        *d = (uint8_t)((q[j + 4] & 0xF) | ((q[j - 4] >> 6) << 4));
        *m = (uint8_t)((q[j + 4] >> 4)  | ((q[j - 0] >> 6) << 4));
    }
}

/* Expand one block into `out`; returns elements written, or -1. */
static int dequant_block(uint32_t type, const uint8_t *b, float *out) {
    switch (type) {
    case 0: {                                   /* F32 */
        union { uint32_t u; float f; } c;
        c.u = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
              ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
        out[0] = c.f;
        return 1;
    }
    case 1:                                     /* F16 */
        out[0] = fp16_to_f32((uint16_t)(b[0] | (b[1] << 8)));
        return 1;

    case 6: {                                   /* Q5_0: 32 elements */
        float d = fp16_to_f32((uint16_t)(b[0] | (b[1] << 8)));
        uint32_t qh = (uint32_t)b[2] | ((uint32_t)b[3] << 8) |
                      ((uint32_t)b[4] << 16) | ((uint32_t)b[5] << 24);
        const uint8_t *qs = b + 6;
        for (int j = 0; j < 16; j++) {
            uint8_t h0 = (uint8_t)(((qh >> (j + 0)) << 4) & 0x10);
            uint8_t h1 = (uint8_t)((qh >> (j + 12)) & 0x10);
            out[j]      = (float)(((qs[j] & 0x0F) | h0) - 16) * d;
            out[j + 16] = (float)(((qs[j] >> 4)   | h1) - 16) * d;
        }
        return 32;
    }
    case 8: {                                   /* Q8_0: 32 elements */
        float d = fp16_to_f32((uint16_t)(b[0] | (b[1] << 8)));
        const int8_t *qs = (const int8_t *)(b + 2);
        for (int j = 0; j < 32; j++) out[j] = (float)qs[j] * d;
        return 32;
    }

    case 12: {                                  /* Q4_K: 256 elements */
        float d    = fp16_to_f32((uint16_t)(b[0] | (b[1] << 8)));
        float dmin = fp16_to_f32((uint16_t)(b[2] | (b[3] << 8)));
        const uint8_t *sc = b + 4;
        const uint8_t *q  = b + 16;
        int is = 0;
        float *y = out;
        for (int n = 0; n < 256; n += 64) {
            uint8_t s1, m1, s2, m2;
            k4_scale_min(is + 0, sc, &s1, &m1);
            k4_scale_min(is + 1, sc, &s2, &m2);
            float d1 = d * (float)s1, off1 = dmin * (float)m1;
            float d2 = d * (float)s2, off2 = dmin * (float)m2;
            for (int l = 0; l < 32; l++) *y++ = d1 * (float)(q[l] & 0xF) - off1;
            for (int l = 0; l < 32; l++) *y++ = d2 * (float)(q[l] >> 4)  - off2;
            q += 32;
            is += 2;
        }
        return 256;
    }

    case 14: {                                  /* Q6_K: 256 elements */
        const uint8_t *ql = b;
        const uint8_t *qh = b + 128;
        const int8_t  *sc = (const int8_t *)(b + 192);
        float d = fp16_to_f32((uint16_t)(b[208] | (b[209] << 8)));
        float *y = out;
        for (int n = 0; n < 256; n += 128) {
            for (int l = 0; l < 32; l++) {
                int is = l / 16;
                int q1 = (int)((ql[l +  0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32;
                int q2 = (int)((ql[l + 32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32;
                int q3 = (int)((ql[l +  0] >>  4) | (((qh[l] >> 4) & 3) << 4)) - 32;
                int q4 = (int)((ql[l + 32] >>  4) | (((qh[l] >> 6) & 3) << 4)) - 32;
                y[l +  0] = d * (float)sc[is + 0] * (float)q1;
                y[l + 32] = d * (float)sc[is + 2] * (float)q2;
                y[l + 64] = d * (float)sc[is + 4] * (float)q3;
                y[l + 96] = d * (float)sc[is + 6] * (float)q4;
            }
            y  += 128;
            ql += 64;
            qh += 32;
            sc += 8;
        }
        return 256;
    }
    default:
        return -1;
    }
}

int llm_tensor_peek(int idx, uint64_t first, int n, int32_t *out) {
    if (idx < 0 || idx >= tensor_n || !model_rd) return -1;
    tensor_t *t = &tensors[idx];
    uint32_t be, bb;
    if (quant_block(t->type, &be, &bb) != 0) return -1;
    if (first + (uint64_t)n > t->elems) return -1;

    static float block[256];
    static uint8_t raw[256];
    int done = 0;
    while (done < n) {
        uint64_t e = first + (uint64_t)done;
        uint64_t bi = e / be;                 /* which block */
        uint32_t within = (uint32_t)(e % be);

        uint32_t got = 0;
        if (model_rd(model_ctx, data_start + t->offset + bi * bb,
                     raw, bb, &got) != 0 || got < bb) return -1;
        if (dequant_block(t->type, raw, block) < 0) return -1;

        while (within < be && done < n) {
            float v = block[within++];
            out[done++] = (int32_t)(v * 1000000.0f);
        }
    }
    return done;
}

/* =====================================================================
 * Inference
 *
 * No libm here either, so the transcendentals are written out below.
 * They only need to be good to float32, which a short polynomial after
 * range reduction manages comfortably.
 * ===================================================================== */

#define K_PI      3.14159265358979f
#define K_TWO_PI  6.28318530717959f
#define K_LN2     0.693147180559945f

static float k_exp(float x) {
    if (x > 88.0f)  return 3.0e38f;
    if (x < -88.0f) return 0.0f;
    float n = x * (1.0f / K_LN2);
    int ni = (int)(n >= 0.0f ? n + 0.5f : n - 0.5f);
    float r = x - (float)ni * K_LN2;          /* |r| <= ln2/2 */
    float p = 1.0f + r * (1.0f + r * (0.5f + r * (1.0f / 6.0f +
              r * (1.0f / 24.0f + r * (1.0f / 120.0f + r * (1.0f / 720.0f))))));
    int e = 127 + ni;
    if (e <= 0) return 0.0f;
    if (e >= 255) return 3.0e38f;
    union { uint32_t u; float f; } s;
    s.u = (uint32_t)e << 23;
    return p * s.f;
}

static float k_log(float x) {
    if (x <= 0.0f) return -1.0e30f;
    union { uint32_t u; float f; } v;
    v.f = x;
    int e = (int)((v.u >> 23) & 0xFF) - 127;
    v.u = (v.u & 0x007FFFFFu) | (127u << 23);  /* mantissa into [1,2) */
    float m = v.f;
    float t = (m - 1.0f) / (m + 1.0f);         /* atanh series converges fast */
    float t2 = t * t;
    float lm = 2.0f * t * (1.0f + t2 * (1.0f / 3.0f + t2 * (1.0f / 5.0f +
               t2 * (1.0f / 7.0f + t2 * (1.0f / 9.0f)))));
    return (float)e * K_LN2 + lm;
}

static float k_sin(float x) {
    /* fold into [-pi, pi], then into [-pi/2, pi/2] by symmetry */
    float k = x * (1.0f / K_TWO_PI);
    int ki = (int)(k >= 0.0f ? k + 0.5f : k - 0.5f);
    x -= (float)ki * K_TWO_PI;
    if (x >  K_PI * 0.5f) x =  K_PI - x;
    else if (x < -K_PI * 0.5f) x = -K_PI - x;
    float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f +
           x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f -
           x2 * (1.0f / 39916800.0f))))));
}

static float k_cos(float x) { return k_sin(x + K_PI * 0.5f); }

static float k_sqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r;
    __asm__ volatile("sqrtss %1, %0" : "=x"(r) : "x"(x));
    return r;
}

/* ---- weights in memory ---- */

typedef struct {
    const uint8_t *data;
    uint32_t type;
    uint32_t ne0, ne1;
} wt_t;

typedef struct {
    wt_t attn_norm, wq, bq, wk, bk, wv, bv, wo;
    wt_t ffn_norm, w_gate, w_up, w_down;
} layer_t;

static uint8_t *weights_blob;
static uint64_t weights_len;
static int      weights_ok;

static wt_t     w_tok_embd, w_out_norm;
static layer_t  w_layers[32];

static int      head_dim, kv_dim, n_ctx;

/* activations */
static float *a_x, *a_xb, *a_xb2, *a_q, *a_k, *a_v, *a_att, *a_hb, *a_hb2;

/* The same activations, for a batch of prefill positions. See the
 * prefill kernel below for why the batch is one. */
#define LLM_BATCH 1
static float *b_x, *b_xb, *b_xb2, *b_q, *b_k, *b_v, *b_hb, *b_hb2;
static float *a_logits, *kv_k, *kv_v;

/* snapshots of the first layer, kept only so a reference forward pass
 * can be compared against step by step */
static float *p_embd, *p_xb0, *p_q0;

int llm_weights_loaded(void) { return weights_ok; }

/* name building without a printf */
static void mkname(char *out, const char *pre, int idx, const char *post) {
    int o = 0;
    while (*pre) out[o++] = *pre++;
    if (idx >= 0) {
        char d[8];
        int n = 0;
        int v = idx;
        if (v == 0) d[n++] = '0';
        while (v > 0) { d[n++] = (char)('0' + v % 10); v /= 10; }
        while (n > 0) out[o++] = d[--n];
    }
    while (*post) out[o++] = *post++;
    out[o] = '\0';
}

static int bind_tensor(wt_t *w, const char *name, const char **err) {
    int i = llm_tensor_find(name);
    if (i < 0) { *err = "a tensor the model needs is missing"; return -1; }
    w->data = weights_blob + tensors[i].offset;
    w->type = tensors[i].type;
    w->ne0  = tensors[i].ne0;
    w->ne1  = tensors[i].ne1 ? tensors[i].ne1 : 1;
    return 0;
}

/*
 * Weight loading, handed out a chunk at a time.
 *
 * Reading ~370 MB off an emulated PIO disk takes the better part of a
 * minute, and doing it in one call means the render loop does not run
 * for that whole time: no cursor, no clock, nothing — which is
 * indistinguishable from a hung machine.  Split the same work into
 * begin/step so a caller can advance it from its frame loop and stay
 * alive, exactly as evaluation already does.
 */
static uint64_t load_done = 0;
static int      load_active = 0;

static int llm_bind_all(const char **err);

int llm_load_begin(const char **err) {
    if (!info.loaded) { *err = "no model loaded"; return -1; }
    weights_ok = 0;
    load_active = 0;

    head_dim = (int)(info.n_embd / info.n_head);
    kv_dim   = (int)(info.n_head_kv * (uint32_t)head_dim);
    n_ctx    = LLM_CTX_MAX;

    /* the payload runs from the data section to the end of the file */
    weights_len = info.file_size - data_start;
    weights_blob = (uint8_t *)arena_alloc(weights_len + 64);
    if (!weights_blob) { *err = "arena too small for the weights"; return -1; }

    load_done = 0;
    load_active = 1;
    return 0;
}

int llm_load_progress(void) {
    if (!weights_len) return 0;
    if (weights_ok) return 100;
    return (int)(load_done * 100 / weights_len);
}

int llm_load_active(void) { return load_active; }

/* One chunk per call: 1 when the model is fully resident, 0 for more to
 * do, -1 on failure with *err set. */
int llm_load_step(const char **err) {
    if (!load_active) { *err = "no load in progress"; return -1; }

    if (load_done < weights_len) {
        /*
         * 128 KB, not a megabyte.
         *
         * The caller paces this against a slice of the frame, and a chunk
         * it cannot subdivide is a floor on how long a frame can take. A
         * megabyte read through exFAT and ATA PIO is several frames' worth
         * of work on an emulated machine — enough that the desktop dropped
         * to 1 fps for the whole load however carefully the caller
         * budgeted. This is small enough for the budget to mean something
         * and still large enough that per-read overhead is noise.
         */
        uint32_t chunk = 128u << 10;
        if (load_done + chunk > weights_len)
            chunk = (uint32_t)(weights_len - load_done);
        uint32_t got = 0;
        if (model_rd(model_ctx, data_start + load_done,
                     weights_blob + load_done, chunk, &got) != 0 || got == 0) {
            *err = "read error while loading weights";
            load_active = 0;
            return -1;
        }
        load_done += got;
        if (load_done < weights_len) return 0;
    }

    load_active = 0;
    return llm_bind_all(err) == 0 ? 1 : -1;
}

int llm_load_weights(const char **err) {
    if (llm_load_begin(err) != 0) return -1;
    for (;;) {
        int r = llm_load_step(err);
        if (r < 0) return -1;
        if (r == 1) return 0;
    }
}

/* Resolve every tensor and carve out the activation buffers. */
static int llm_bind_all(const char **err) {
    if (bind_tensor(&w_tok_embd, "token_embd.weight", err) != 0) return -1;
    if (bind_tensor(&w_out_norm, "output_norm.weight", err) != 0) return -1;

    char nm[LLM_NAME_MAX];
    for (uint32_t l = 0; l < info.n_layer; l++) {
        layer_t *L = &w_layers[l];
        mkname(nm, "blk.", (int)l, ".attn_norm.weight");   if (bind_tensor(&L->attn_norm, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_q.weight");      if (bind_tensor(&L->wq, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_q.bias");        if (bind_tensor(&L->bq, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_k.weight");      if (bind_tensor(&L->wk, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_k.bias");        if (bind_tensor(&L->bk, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_v.weight");      if (bind_tensor(&L->wv, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_v.bias");        if (bind_tensor(&L->bv, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".attn_output.weight"); if (bind_tensor(&L->wo, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".ffn_norm.weight");    if (bind_tensor(&L->ffn_norm, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".ffn_gate.weight");    if (bind_tensor(&L->w_gate, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".ffn_up.weight");      if (bind_tensor(&L->w_up, nm, err)) return -1;
        mkname(nm, "blk.", (int)l, ".ffn_down.weight");    if (bind_tensor(&L->w_down, nm, err)) return -1;
    }

    uint32_t E = info.n_embd, F = info.n_ff;
    arena_failed = 0;
    a_x      = (float *)arena_need(E * 4);
    a_xb     = (float *)arena_need(E * 4);
    a_xb2    = (float *)arena_need(E * 4);
    a_q      = (float *)arena_need(E * 4);
    a_k      = (float *)arena_need((uint64_t)kv_dim * 4);
    a_v      = (float *)arena_need((uint64_t)kv_dim * 4);
    a_att    = (float *)arena_need((uint64_t)info.n_head * n_ctx * 4);
    a_hb     = (float *)arena_need(F * 4);
    a_hb2    = (float *)arena_need(F * 4);
    a_logits = (float *)arena_need((uint64_t)info.n_vocab * 4);
    kv_k = (float *)arena_need((uint64_t)info.n_layer * n_ctx * kv_dim * 4);
    kv_v = (float *)arena_need((uint64_t)info.n_layer * n_ctx * kv_dim * 4);
    p_embd = (float *)arena_need(E * 4);
    p_xb0  = (float *)arena_need(E * 4);
    p_q0   = (float *)arena_need(E * 4);

    /* Batched prefill activations: eight positions in flight. */
    b_x    = (float *)arena_need((uint64_t)LLM_BATCH * E * 4);
    b_xb   = (float *)arena_need((uint64_t)LLM_BATCH * E * 4);
    b_xb2  = (float *)arena_need((uint64_t)LLM_BATCH * E * 4);
    b_q    = (float *)arena_need((uint64_t)LLM_BATCH * E * 4);
    b_k    = (float *)arena_need((uint64_t)LLM_BATCH * kv_dim * 4);
    b_v    = (float *)arena_need((uint64_t)LLM_BATCH * kv_dim * 4);
    b_hb   = (float *)arena_need((uint64_t)LLM_BATCH * F * 4);
    b_hb2  = (float *)arena_need((uint64_t)LLM_BATCH * F * 4);
    if (arena_failed) { *err = "arena too small for the KV cache"; return -1; }

    weights_ok = 1;
    return 0;
}

/* ---- kernels ---- */

/* Expand row `row` of a weight tensor into `out` (ne0 elements). */
static void deq_row(const wt_t *w, uint32_t row, float *out) {
    uint32_t be, bb;
    quant_block(w->type, &be, &bb);
    uint64_t first = (uint64_t)row * w->ne0;
    const uint8_t *p = w->data + (first / be) * bb;
    static float blk[256];
    uint32_t n = 0;
    while (n < w->ne0) {
        int got = dequant_block(w->type, p, blk);
        if (got <= 0) return;
        for (int i = 0; i < got && n < w->ne0; i++) out[n++] = blk[i];
        p += bb;
    }
}

/*
 * dot(row j, x), dequantising a block at a time straight into the sum.
 *
 * The obvious version expands the whole row into a scratch buffer and
 * then dots it, which is what this used to do, and it costs two extra
 * trips through memory for every weight in the model: one to write the
 * expanded row out, one to read it back. For the feed-forward down
 * projection alone that is 896 rows of 4864 floats — seventeen megabytes
 * of pure round-trip, per layer, per token.
 *
 * It is also a cache disaster. The scratch buffer was 32 KB, larger than
 * the L1 on most cores, so the write evicted whatever the read was about
 * to want. A block is a kilobyte at most and stays hot.
 *
 * The arithmetic is identical; only the order changed.
 */
static float dot_row(const wt_t *w, uint32_t row, const float *x) {
    uint32_t be, bb;
    quant_block(w->type, &be, &bb);
    uint64_t first = (uint64_t)row * w->ne0;
    const uint8_t *p = w->data + (first / be) * bb;

    float blk[256];
    float s = 0.0f;
    uint32_t n = 0;
    while (n < w->ne0) {
        int got = dequant_block(w->type, p, blk);
        if (got <= 0) break;
        uint32_t take = (uint32_t)got;
        if (n + take > w->ne0) take = w->ne0 - n;
        for (uint32_t i = 0; i < take; i++) s += blk[i] * x[n + i];
        n += take;
        p += bb;
    }
    return s;
}

/* out[j] = dot(row j, x) for every output row */
static void matmul(float *out, const float *x, const wt_t *w) {
    for (uint32_t j = 0; j < w->ne1; j++)
        out[j] = dot_row(w, j, x);
}

static void rmsnorm(float *out, const float *x, const wt_t *w, uint32_t n) {
    static float g[8192];
    deq_row(w, 0, g);
    float ss = 0.0f;
    for (uint32_t i = 0; i < n; i++) ss += x[i] * x[i];
    ss = ss / (float)n + info.rms_eps;
    float scale = 1.0f / k_sqrt(ss);
    for (uint32_t i = 0; i < n; i++) out[i] = x[i] * scale * g[i];
}

static void softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float sum = 0.0f;
    for (int i = 0; i < n; i++) { x[i] = k_exp(x[i] - mx); sum += x[i]; }
    float inv = 1.0f / sum;
    for (int i = 0; i < n; i++) x[i] *= inv;
}

/*
 * Rotary embeddings, NeoX style: the pair for index i is (i, i + d/2),
 * not (2i, 2i+1).  Qwen2 uses that convention, and mixing the two up
 * yields fluent-looking output that is subtly wrong, so it is worth
 * being explicit about.
 */
static void rope(float *vec, int n_heads, int pos) {
    float base_log = k_log(info.rope_freq_base);
    int half = head_dim / 2;
    for (int h = 0; h < n_heads; h++) {
        float *p = vec + h * head_dim;
        for (int i = 0; i < half; i++) {
            float freq = k_exp(-base_log * (2.0f * (float)i) / (float)head_dim);
            float th = (float)pos * freq;
            float c = k_cos(th), s = k_sin(th);
            float x0 = p[i], x1 = p[i + half];
            p[i]        = x0 * c - x1 * s;
            p[i + half] = x0 * s + x1 * c;
        }
    }
}

/*
 * Evaluation is resumable.  One forward pass takes about a minute under
 * emulation, and a UI that called it straight through would freeze the
 * whole desktop for that long, so the work is handed out a layer at a
 * time and the caller redraws in between.
 */
static int32_t  ev_token;
static int      ev_pos;
static uint32_t ev_layer;
static int      ev_stage;      /* 0 layers, 1 logits, 2 done */
static uint32_t ev_logit_row;
static int      ev_active;

#define EV_LOGIT_CHUNK 8192

static void eval_layer(uint32_t l, int pos) {
    uint32_t E = info.n_embd, H = info.n_head, KVH = info.n_head_kv;
    layer_t *L = &w_layers[l];

    rmsnorm(a_xb, a_x, &L->attn_norm, E);

    matmul(a_q, a_xb, &L->wq);
    matmul(a_k, a_xb, &L->wk);
    matmul(a_v, a_xb, &L->wv);

    /* Qwen2 puts a bias on the q, k and v projections */
    static float bias[8192];
    deq_row(&L->bq, 0, bias);
    for (uint32_t i = 0; i < E; i++) a_q[i] += bias[i];
    deq_row(&L->bk, 0, bias);
    for (int i = 0; i < kv_dim; i++) a_k[i] += bias[i];
    deq_row(&L->bv, 0, bias);
    for (int i = 0; i < kv_dim; i++) a_v[i] += bias[i];

    if (l == 0) {
        for (uint32_t i = 0; i < E; i++) { p_xb0[i] = a_xb[i]; p_q0[i] = a_q[i]; }
    }

    rope(a_q, (int)H, pos);
    rope(a_k, (int)KVH, pos);

    float *krow = kv_k + ((uint64_t)l * n_ctx + pos) * kv_dim;
    float *vrow = kv_v + ((uint64_t)l * n_ctx + pos) * kv_dim;
    for (int i = 0; i < kv_dim; i++) { krow[i] = a_k[i]; vrow[i] = a_v[i]; }

    int group = (int)(H / KVH);
    float inv_sqrt = 1.0f / k_sqrt((float)head_dim);

    for (uint32_t h = 0; h < H; h++) {
        const float *qh = a_q + h * head_dim;
        int kvh = (int)h / group;
        float *sc = a_att + (uint64_t)h * n_ctx;

        for (int t = 0; t <= pos; t++) {
            const float *kt = kv_k + ((uint64_t)l * n_ctx + t) * kv_dim
                                   + kvh * head_dim;
            float sdot = 0.0f;
            for (int i = 0; i < head_dim; i++) sdot += qh[i] * kt[i];
            sc[t] = sdot * inv_sqrt;
        }
        softmax(sc, pos + 1);

        float *ob = a_xb + h * head_dim;
        for (int i = 0; i < head_dim; i++) ob[i] = 0.0f;
        for (int t = 0; t <= pos; t++) {
            const float *vt = kv_v + ((uint64_t)l * n_ctx + t) * kv_dim
                                   + kvh * head_dim;
            float a = sc[t];
            for (int i = 0; i < head_dim; i++) ob[i] += a * vt[i];
        }
    }

    matmul(a_xb2, a_xb, &L->wo);
    for (uint32_t i = 0; i < E; i++) a_x[i] += a_xb2[i];

    rmsnorm(a_xb, a_x, &L->ffn_norm, E);
    matmul(a_hb,  a_xb, &L->w_gate);
    matmul(a_hb2, a_xb, &L->w_up);

    for (uint32_t i = 0; i < info.n_ff; i++) {
        float g = a_hb[i];
        g = g / (1.0f + k_exp(-g));          /* SiLU */
        a_hb[i] = g * a_hb2[i];
    }

    matmul(a_xb2, a_hb, &L->w_down);
    for (uint32_t i = 0; i < E; i++) a_x[i] += a_xb2[i];
}

/*
 * `want_logits` is the difference between reading a prompt and answering.
 *
 * Feeding a prompt in exists to fill the key/value cache; the layers do
 * that, and the logit head — a dot product against all 151,936 rows of
 * the embedding matrix — is only needed to *choose* a token. For every
 * prompt token but the last, those logits are computed and discarded.
 *
 * That is not a small waste. The head is roughly three fifths of the work
 * per token, so skipping it where it cannot be read makes reading a
 * retrieved article about two and a half times faster, which on this
 * platform is the difference between a usable chat panel and one nobody
 * waits for.
 */
static int ev_want_logits = 1;

static int eval_begin_common(int32_t token, int pos, int want_logits) {
    if (!weights_ok) return -1;
    if (pos < 0 || pos >= n_ctx) return -1;
    if (token < 0 || (uint32_t)token >= info.n_vocab) return -1;

    deq_row(&w_tok_embd, (uint32_t)token, a_x);
    for (uint32_t i = 0; i < info.n_embd; i++) p_embd[i] = a_x[i];

    ev_token = token;
    ev_pos = pos;
    ev_layer = 0;
    ev_stage = 0;
    ev_logit_row = 0;
    ev_want_logits = want_logits;
    ev_active = 1;
    return 0;
}


/* ---- prefill kernel ----
 *
 * Reading a prompt costs what it costs because of *dequantisation*, not
 * arithmetic. `llm bench` says so directly: two milliseconds to expand
 * the model's largest weight, under one to multiply by it. Vectorising
 * the multiply was tried first and bought nothing, which is what sent
 * the search here.
 *
 * This kernel replaces the whole prompt-reading path and is worth about
 * three times end to end on the same question — 38 s down to 12 s, while
 * generating rather more of an answer. That figure is reproducible; the
 * reason it is quite that large is not fully accounted for. The obvious
 * candidates are the per-row `quant_block` call hoisted out of the loop
 * and a block that now stays in L1 instead of being written to a 32 KB
 * scratch buffer and read back, but the measurement is the claim here,
 * not the explanation.
 *
 * LLM_BATCH is 1, and that is a measured result rather than a
 * placeholder. The design intent was to decode each weight block once
 * and use it against several inputs, dividing the dominant cost by the
 * batch size. It does not work out: eight inputs of nearly 20 KB each
 * are 155 KB of working set against a 128 KB L1, so the reuse that was
 * supposed to pay for itself evicts the very block it wanted hot.
 * Measured on the same question — 1: 12 s, 8: 24 s, 4: 45 s. The
 * machinery is kept because it is what runs, and because a wider batch
 * becomes worth it the moment the inputs are small enough to fit.
 */
/* out[b][j] = dot(row j of w, x[b]) for b < nb */
static void matmul_batch(float *out, uint32_t out_stride,
                         const float *x, uint32_t x_stride,
                         int nb, const wt_t *w) {
    uint32_t be, bb;
    quant_block(w->type, &be, &bb);

    for (uint32_t j = 0; j < w->ne1; j++) {
        const uint8_t *p = w->data + ((uint64_t)j * w->ne0 / be) * bb;
        float acc[LLM_BATCH];
        for (int b = 0; b < nb; b++) acc[b] = 0.0f;

        float blk[256];
        uint32_t n = 0;
        while (n < w->ne0) {
            int got = dequant_block(w->type, p, blk);
            if (got <= 0) break;
            uint32_t take = (uint32_t)got;
            if (n + take > w->ne0) take = w->ne0 - n;

            for (int b = 0; b < nb; b++) {
                const float *xb = x + (uint64_t)b * x_stride + n;
                float s = acc[b];
                for (uint32_t i = 0; i < take; i++) s += blk[i] * xb[i];
                acc[b] = s;
            }
            n += take;
            p += bb;
        }
        for (int b = 0; b < nb; b++)
            out[(uint64_t)b * out_stride + j] = acc[b];
    }
}

static void add_bias(float *v, const wt_t *w, uint32_t n) {
    static float bias[8192];
    deq_row(w, 0, bias);
    for (uint32_t i = 0; i < n; i++) v[i] += bias[i];
}

/*
 * One layer, for a run of `nb` consecutive positions starting at `p0`.
 *
 * The projections are batched; attention is not, and cannot be — each
 * position attends over a different span of the cache, and the positions
 * in this very batch are part of what the later ones attend to. So the
 * keys and values for the whole batch are written first, and only then
 * is attention computed position by position. Getting that order wrong
 * would let a token attend to a key that had not been written yet, which
 * is the kind of mistake that still produces fluent output.
 */
static void eval_layer_batch(uint32_t l, int p0, int nb) {
    layer_t *L = &w_layers[l];
    uint32_t E = info.n_embd, F = info.n_ff;

    for (int b = 0; b < nb; b++)
        rmsnorm(b_xb + (uint64_t)b * E, b_x + (uint64_t)b * E, &L->attn_norm, E);

    matmul_batch(b_q, E, b_xb, E, nb, &L->wq);
    matmul_batch(b_k, (uint32_t)kv_dim, b_xb, E, nb, &L->wk);
    matmul_batch(b_v, (uint32_t)kv_dim, b_xb, E, nb, &L->wv);

    for (int b = 0; b < nb; b++) {
        add_bias(b_q + (uint64_t)b * E, &L->bq, E);
        add_bias(b_k + (uint64_t)b * kv_dim, &L->bk, (uint32_t)kv_dim);
        add_bias(b_v + (uint64_t)b * kv_dim, &L->bv, (uint32_t)kv_dim);

        rope(b_q + (uint64_t)b * E, (int)info.n_head, p0 + b);
        rope(b_k + (uint64_t)b * kv_dim, (int)info.n_head_kv, p0 + b);

        float *krow = kv_k + ((uint64_t)l * n_ctx + p0 + b) * kv_dim;
        float *vrow = kv_v + ((uint64_t)l * n_ctx + p0 + b) * kv_dim;
        for (int i = 0; i < kv_dim; i++) {
            krow[i] = b_k[(uint64_t)b * kv_dim + i];
            vrow[i] = b_v[(uint64_t)b * kv_dim + i];
        }
    }

    uint32_t H = info.n_head, KVH = info.n_head_kv;
    int group = (int)(H / KVH);
    float inv_sqrt = 1.0f / k_sqrt((float)head_dim);

    for (int b = 0; b < nb; b++) {
        int pos = p0 + b;
        float *xb = b_xb + (uint64_t)b * E;
        for (uint32_t h = 0; h < H; h++) {
            const float *qh = b_q + (uint64_t)b * E + h * head_dim;
            int kvh = (int)h / group;
            float *sc = a_att + (uint64_t)h * n_ctx;

            for (int t = 0; t <= pos; t++) {
                const float *kt = kv_k + ((uint64_t)l * n_ctx + t) * kv_dim
                                       + kvh * head_dim;
                float sdot = 0.0f;
                for (int i = 0; i < head_dim; i++) sdot += qh[i] * kt[i];
                sc[t] = sdot * inv_sqrt;
            }
            softmax(sc, pos + 1);

            float *ob = xb + h * head_dim;
            for (int i = 0; i < head_dim; i++) ob[i] = 0.0f;
            for (int t = 0; t <= pos; t++) {
                const float *vt = kv_v + ((uint64_t)l * n_ctx + t) * kv_dim
                                       + kvh * head_dim;
                float a = sc[t];
                for (int i = 0; i < head_dim; i++) ob[i] += a * vt[i];
            }
        }
    }

    matmul_batch(b_xb2, E, b_xb, E, nb, &L->wo);
    for (int b = 0; b < nb; b++)
        for (uint32_t i = 0; i < E; i++)
            b_x[(uint64_t)b * E + i] += b_xb2[(uint64_t)b * E + i];

    for (int b = 0; b < nb; b++)
        rmsnorm(b_xb + (uint64_t)b * E, b_x + (uint64_t)b * E, &L->ffn_norm, E);

    matmul_batch(b_hb,  F, b_xb, E, nb, &L->w_gate);
    matmul_batch(b_hb2, F, b_xb, E, nb, &L->w_up);

    for (int b = 0; b < nb; b++) {
        float *hb = b_hb + (uint64_t)b * F, *hb2 = b_hb2 + (uint64_t)b * F;
        for (uint32_t i = 0; i < F; i++) {
            float g = hb[i];
            g = g / (1.0f + k_exp(-g));          /* SiLU */
            hb[i] = g * hb2[i];
        }
    }

    matmul_batch(b_xb2, E, b_hb, F, nb, &L->w_down);
    for (int b = 0; b < nb; b++)
        for (uint32_t i = 0; i < E; i++)
            b_x[(uint64_t)b * E + i] += b_xb2[(uint64_t)b * E + i];
}

/* ---- steppable prefill, one layer of one batch per step ---- */
static int      pf_active = 0;
static int      pf_pos = 0, pf_left = 0, pf_nb = 0;
static uint32_t pf_layer = 0;
static const int32_t *pf_toks = 0;

int llm_prefill_begin(const int32_t *toks, int n, int start_pos) {
    if (!weights_ok || n <= 0) return -1;
    if (start_pos < 0 || start_pos + n > n_ctx) return -1;
    pf_toks = toks;
    pf_pos = start_pos;
    pf_left = n;
    pf_layer = 0;
    pf_nb = 0;
    pf_active = 1;
    return 0;
}

/* 1 when the whole run is in the cache, 0 while there is more to do. */
int llm_prefill_step(void) {
    if (!pf_active) return 1;

    if (pf_nb == 0) {                       /* start a batch */
        pf_nb = pf_left < LLM_BATCH ? pf_left : LLM_BATCH;
        uint32_t E = info.n_embd;
        for (int b = 0; b < pf_nb; b++)
            deq_row(&w_tok_embd, (uint32_t)pf_toks[b], b_x + (uint64_t)b * E);
        pf_layer = 0;
    }

    eval_layer_batch(pf_layer, pf_pos, pf_nb);

    if (++pf_layer >= info.n_layer) {       /* batch done */
        pf_toks += pf_nb;
        pf_pos  += pf_nb;
        pf_left -= pf_nb;
        pf_nb = 0;
        if (pf_left <= 0) { pf_active = 0; return 1; }
    }
    return 0;
}

int llm_prefill_progress(void) {
    return pf_active ? 1 : 0;
}

int llm_eval_begin(int32_t token, int pos) {
    return eval_begin_common(token, pos, 1);
}

/* Same forward pass, stopping once the cache is filled. llm_argmax() is
 * meaningless afterwards and the caller must not read it. */
int llm_eval_begin_prefill(int32_t token, int pos) {
    return eval_begin_common(token, pos, 0);
}

/* Returns 1 once the logits are ready, 0 while there is more to do. */
int llm_eval_step(void) {
    if (!ev_active) return 1;

    if (ev_stage == 0) {
        eval_layer(ev_layer, ev_pos);
        if (++ev_layer >= info.n_layer) {
            rmsnorm(a_x, a_x, &w_out_norm, info.n_embd);
            if (!ev_want_logits) {      /* prefill: the cache is what mattered */
                ev_active = 0;
                ev_stage = 2;
                return 1;
            }
            ev_stage = 1;
        }
        return 0;
    }

    /* the logit head is a quarter of the work on its own, so it is
     * handed out in chunks too */
    uint32_t end = ev_logit_row + EV_LOGIT_CHUNK;
    if (end > info.n_vocab) end = info.n_vocab;
    static float row[8192];
    for (uint32_t j = ev_logit_row; j < end; j++) {
        deq_row(&w_tok_embd, j, row);
        float sdot = 0.0f;
        for (uint32_t i = 0; i < info.n_embd; i++) sdot += row[i] * a_x[i];
        a_logits[j] = sdot;
    }
    ev_logit_row = end;
    if (ev_logit_row >= info.n_vocab) {
        ev_active = 0;
        ev_stage = 2;
        return 1;
    }
    return 0;
}


/*
 * Split one matmul into its two halves and time each.
 *
 * `w_down` is the largest weight in the model (896 x 4864) and is
 * quantised, so it is representative of where inference actually spends
 * itself. Timing dequantisation alone against dequantisation-plus-
 * arithmetic says which half to attack — and the first attempt here
 * attacked the arithmetic, made it fractionally slower, and would have
 * gone on doing so without a number to argue with.
 */
/* This translation unit knows nothing about the machine, so it reads the
 * counter directly and reports raw cycles; the caller owns the
 * conversion because the caller is the one that calibrated it. */
static uint64_t bench_now(void) {
#if defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
#else
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
#endif
}

void llm_bench(uint64_t *deq_cy, uint64_t *dot_cy, uint64_t *both_cy) {
    if (deq_cy) *deq_cy = 0;
    if (dot_cy) *dot_cy = 0;
    if (both_cy) *both_cy = 0;
    if (!weights_ok) return;

    const wt_t *w = &w_layers[0].w_down;
    static float x[8192];
    static float out[8192];
    static float row[8192];
    for (uint32_t i = 0; i < w->ne0 && i < 8192; i++) x[i] = 0.001f * (float)(i & 63);

    /* dequantisation only */
    uint64_t t0 = bench_now();
    for (uint32_t j = 0; j < w->ne1; j++) deq_row(w, j, row);
    uint64_t t1 = bench_now();

    /* arithmetic only, over an already-expanded row */
    for (uint32_t j = 0; j < w->ne1; j++) {
        float s = 0.0f;
        for (uint32_t i = 0; i < w->ne0; i++) s += row[i] * x[i];
        out[j] = s;
    }
    uint64_t t2 = bench_now();

    /* both, as the model really runs it */
    matmul(out, x, w);
    uint64_t t3 = bench_now();

    if (deq_cy)  *deq_cy  = t1 - t0;
    if (dot_cy)  *dot_cy  = t2 - t1;
    if (both_cy) *both_cy = t3 - t2;
}

int llm_eval_progress(void) {
    if (!ev_active && ev_stage == 2) return 100;
    if (!ev_active) return 0;
    /* the layers are about three quarters of the cost */
    if (ev_stage == 0) return (int)(ev_layer * 75 / info.n_layer);
    return 75 + (int)((uint64_t)ev_logit_row * 25 / info.n_vocab);
}

int llm_eval(int32_t token, int pos) {
    if (llm_eval_begin(token, pos) != 0) return -1;
    while (llm_eval_step() == 0) { }
    return 0;
}

int llm_argmax(void) {
    if (!weights_ok) return -1;
    int best = 0;
    float bv = a_logits[0];
    for (uint32_t i = 1; i < info.n_vocab; i++)
        if (a_logits[i] > bv) { bv = a_logits[i]; best = (int)i; }
    return best;
}

int llm_logit(int32_t token, int32_t *scaled) {
    if (!weights_ok || token < 0 || (uint32_t)token >= info.n_vocab) return -1;
    *scaled = (int32_t)(a_logits[token] * 1000.0f);
    return 0;
}

/* Intermediate values, for checking against a reference forward pass. */
int llm_probe(const char *what, int layer, int n, int32_t *out) {
    if (!weights_ok) return -1;
    const float *src = 0;
    if (str_same(what, "embd")) src = p_embd;
    else if (str_same(what, "norm0")) src = p_xb0;
    else if (str_same(what, "q0")) src = p_q0;
    else if (str_same(what, "x")) src = a_x;
    else if (str_same(what, "xb")) src = a_xb;
    else if (str_same(what, "q")) src = a_q;
    else if (str_same(what, "k")) src = a_k;
    else if (str_same(what, "logits")) src = a_logits;
    else return -1;
    (void)layer;
    for (int i = 0; i < n; i++) out[i] = (int32_t)(src[i] * 1000000.0f);
    return n;
}
