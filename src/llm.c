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
                    for (uint64_t j = 0; j < slen; j++) {
                        uint8_t c = g_u8(&g);
                        if (used < cap - 1) tok_blob[used++] = (char)c;
                    }
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
                    uint32_t o = 0;
                    for (uint64_t j = 0; j < slen; j++) {
                        uint8_t c = g_u8(&g);
                        if (o < sizeof(line) - 1) line[o++] = (char)c;
                    }
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
                for (uint64_t k = 0; k < n && !g.failed; k++) g_skip_one(&g, et);
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
