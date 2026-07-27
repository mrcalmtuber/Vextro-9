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

/* ---- arena ---- */
void        llm_arena_init(void *base, uint64_t size);
uint64_t    llm_arena_total(void);
uint64_t    llm_arena_used(void);

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

/* Proof that the floating-point unit is actually usable in the kernel.
 * The result comes back scaled by 10000 as an integer, because the
 * caller lives in the integer-only translation unit and cannot so much
 * as hold a float in a register. */
int          llm_fpu_selftest(uint32_t *scaled_by_10000);

#endif /* LLM_H */
