#ifndef LLM_H
#define LLM_H

#include <stdint.h>

/*
 * Local language model inference.
 *
 * This lives in its own translation unit for one reason: the rest of the
 * kernel is compiled -mno-sse -mno-80387 and is deliberately integer
 * only, but a transformer is float maths from end to end.  Only this
 * file is built with SSE2 enabled, so nothing else in the kernel can
 * quietly grow a floating-point dependency — and, importantly, no
 * interrupt handler can, since none of them live here.
 *
 * The kernel enables the CPU side of that (CR0.EM clear, CR0.MP,
 * CR4.OSFXSR, CR4.OSXMMEXCPT) at boot before any of this is called.
 *
 * The model is far larger than any static buffer this kernel could
 * carry, so weights go in an arena carved out of the largest usable
 * region Limine reports.  Reads come back through a callback because
 * the filesystem layer is static to the main translation unit.
 */

/* how the caller hands us bytes out of the model file */
typedef int (*llm_read_fn)(void *ctx, uint64_t off, void *buf,
                           uint32_t len, uint32_t *got);

/* ---- two models at once ----
 *
 * Both sets of weights live in the arena together and a slot is selected
 * by index, so a question can be put to each of them without either
 * being reloaded. Every call below acts on the selected slot.
 */
#define LLM_SLOTS 2

int llm_slot_select(int i);
int llm_slot_current(void);
int llm_slot_loaded(int i);

/* ---- arena ---- */
void        llm_arena_init(void *base, uint64_t size);
uint64_t    llm_arena_total(void);
uint64_t    llm_arena_used(void);

/* ---- the other processors ----
 *
 * A transformer's time goes almost entirely into one operation: for each
 * of several thousand output rows, the dot product of that row of a
 * quantised weight matrix with one activation vector. Every row is
 * independent of every other. They read weights that nothing writes and
 * each writes one element of an output array, so there is no ordering
 * between them, nothing to lock, and nothing to allocate.
 *
 * That is the only shape of work an application processor in this kernel
 * may be given — it holds no locks, owns no address space and cannot
 * take a system call — and it is a coincidence worth taking: the single
 * most expensive thing this machine does is also the one thing that is
 * unconditionally safe to hand out.
 *
 * The runner is injected rather than called directly because this
 * translation unit is compiled apart from the rest of the kernel and
 * cannot see src/smp.h. Null, and on a machine with one processor it
 * stays null, means "do it here" -- the same loop, one participant.
 */
typedef void (*llm_row_fn)(void *ctx, uint32_t first, uint32_t last);
typedef void (*llm_par_fn)(llm_row_fn fn, void *ctx, uint32_t rows,
                           uint32_t min_chunk);

void        llm_set_parallel(llm_par_fn run);
uint64_t    llm_parallel_calls(void);

/* ---- model ---- */
#define LLM_NAME_MAX 64

typedef struct {
    int      loaded;
    char     arch[32];
    char     name[LLM_NAME_MAX];
    uint32_t n_layer;
    uint32_t n_embd;
    uint32_t n_head;
    uint32_t n_head_kv;
    uint32_t n_ff;
    uint32_t n_vocab;
    uint32_t n_ctx_train;
    float    rope_freq_base;
    float    rms_eps;
    uint64_t n_tensors;
    uint64_t file_size;
    uint64_t weight_bytes;    /* total tensor payload */
    uint32_t quant_counts[32];
} llm_info_t;

/* Parse a GGUF file's header, metadata and tensor table.  Returns 0 on
 * success, or -1 with *err set. */
int          llm_load(llm_read_fn rd, void *ctx, uint64_t file_size,
                      const char **err);
const llm_info_t *llm_get_info(void);
const char  *llm_quant_name(uint32_t type);

/* ---- tensors ---- */
int          llm_tensor_count(void);
/* Find a tensor by name; returns its index or -1. */
int          llm_tensor_find(const char *name);
const char  *llm_tensor_name(int idx);
uint32_t     llm_tensor_type(int idx);
uint64_t     llm_tensor_elems(int idx);
/*
 * Dequantise `n` elements of a tensor starting at `first`, returning
 * them scaled by 1e6 as integers — the caller lives in the integer-only
 * translation unit and cannot hold a float.
 */
int          llm_tensor_peek(int idx, uint64_t first, int n, int32_t *out);

/* ---- tokenizer ---- */
#define LLM_TOK_MAX 4096          /* tokens per encode call */

int          llm_tok_ready(void);
uint32_t     llm_tok_count(void);
uint32_t     llm_merge_count(void);
/* Encode text into ids; returns how many were produced, or -1. */
int          llm_encode(const char *text, int32_t *out, int max_out);
/* Append token id's text to out (UTF-8), NUL-terminated. */
int          llm_decode(int32_t id, char *out, int max);
int          llm_token_id(const char *piece);   /* -1 if absent */
/* Greedy pick with the recently emitted tokens held back, so decoding
 * cannot fall into emitting one token forever. Modifies the logit buffer
 * in place, so it may be called only once per forward pass. */
int          llm_argmax_penalized(const int32_t *recent, int n_recent);

/* Proof that the floating-point unit is actually usable in the kernel.
 * The result comes back scaled by 10000 as an integer, because the
 * caller lives in the integer-only translation unit and cannot so much
 * as hold a float in a register. */
int          llm_fpu_selftest(uint32_t *scaled_by_10000);

#endif /* LLM_H */

/* ---- inference ---- */
#define LLM_CTX_MAX 1024

int      llm_weights_loaded(void);
/* Pull every tensor into the arena; slow, once per boot. */
int      llm_load_weights(const char **err);
/* The same, a chunk at a time, so a UI can keep drawing while it runs.
 * llm_load_step returns 1 when the model is resident, 0 for more to do. */
int      llm_load_begin(const char **err);
int      llm_load_step(const char **err);
int      llm_load_progress(void);      /* 0..100 */
int      llm_load_active(void);
/* Run one token through the model at position pos, start to finish. */
int      llm_eval(int32_t token, int pos);
/* The same, handed out a layer at a time so a UI can redraw between. */
int      llm_eval_begin(int32_t token, int pos);
int      llm_eval_begin_prefill(int32_t token, int pos);  /* no logit head */
/* Read a run of prompt tokens into the cache, several at a time. This is
 * where a long prompt's time goes, and batching divides its dominant
 * cost — dequantisation — by the batch size. */
int      llm_prefill_begin(const int32_t *toks, int n, int start_pos);
int      llm_prefill_step(void);      /* 1 when the run is cached */
int      llm_eval_step(void);        /* 1 when the logits are ready */
int      llm_eval_progress(void);    /* 0..100 */
/* Greedy pick from the last eval's logits. */
int      llm_argmax(void);
/* A logit, scaled by 1000, for inspection from the integer-only side. */
int      llm_logit(int32_t token, int32_t *scaled_by_1000);
/* Intermediate values for verification, also scaled by 1e6. */
int      llm_probe(const char *what, int layer, int n, int32_t *out);
/* Where the time actually goes: milliseconds for one full-size matmul,
 * split into dequantisation and arithmetic. Optimising the wrong half of
 * a kernel is easy and this is how to avoid it. */
void     llm_bench(uint64_t *deq_cy, uint64_t *dot_cy, uint64_t *both_cy);
