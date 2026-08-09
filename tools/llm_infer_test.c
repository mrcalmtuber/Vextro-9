/*
 * Run the kernel's own transformer on the host, for one prompt.
 *
 * src/llm.c depends on nothing but llm.h and a read callback, so the
 * whole forward pass -- metadata, tensor binding, dequantisation, RoPE,
 * grouped-query attention, the feed-forward, the norms and the logit
 * head -- can be exercised natively and compared against a reference
 * implementation of the same model. That is the only way to know a new
 * architecture is right without booting: a wrong RoPE base or a missing
 * bias still produces fluent-looking tokens, and the difference only
 * shows against something that is known to be correct.
 *
 *   cc -O2 -o build/llm_infer_test tools/llm_infer_test.c
 *   ./build/llm_infer_test model.gguf "Some prompt" 8
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/llm.c"

static FILE *g_fp;

static int host_read(void *ctx, uint64_t off, void *buf,
                     uint32_t len, uint32_t *got) {
    (void)ctx;
    if (fseeko(g_fp, (off_t)off, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, len, g_fp);
    *got = (uint32_t)n;
    return n == len ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <gguf> <prompt> [n_generate]\n", argv[0]);
        return 2;
    }
    const int want = argc > 3 ? atoi(argv[3]) : 8;

    g_fp = fopen(argv[1], "rb");
    if (!g_fp) { perror(argv[1]); return 2; }
    fseeko(g_fp, 0, SEEK_END);
    const uint64_t size = (uint64_t)ftello(g_fp);

    static uint8_t *arena;
    const uint64_t arena_bytes = 900ull << 20;
    arena = malloc(arena_bytes);
    if (!arena) { fprintf(stderr, "no memory for the arena\n"); return 1; }
    llm_arena_init(arena, arena_bytes);

    const char *err = "?";
    if (llm_load(host_read, 0, size, &err) != 0) {
        fprintf(stderr, "load failed: %s\n", err);
        return 1;
    }
    const llm_info_t *info = llm_get_info();
    fprintf(stderr, "arch %s  %u layers  %u embd  %u heads (%u kv)  ff %u\n",
            info->arch, info->n_layer, info->n_embd, info->n_head,
            info->n_head_kv, info->n_ff);
    fprintf(stderr, "rope_freq_base %.1f  rms_eps %.3g  vocab %u  ctx %u\n",
            info->rope_freq_base, info->rms_eps, info->n_vocab,
            info->n_ctx_train);

    if (llm_load_weights(&err) != 0) {
        fprintf(stderr, "weights failed: %s\n", err);
        return 1;
    }

    static int32_t toks[1024];
    const int n = llm_encode(argv[2], toks, 1024);
    if (n <= 0) { fprintf(stderr, "tokenizer refused the prompt\n"); return 1; }
    fprintf(stderr, "%d prompt tokens\n", n);

    /* Feed the prompt, then generate greedily -- the same order the
     * kernel uses, so a disagreement here is a disagreement there. */
    int pos = 0;
    for (int i = 0; i < n; i++) {
        if (llm_eval(toks[i], pos++) != 0) {
            fprintf(stderr, "eval failed at %d\n", i);
            return 1;
        }
    }

    for (int g = 0; g < want; g++) {
        /* The five most likely continuations, so a disagreement with a
         * reference implementation can be read as "a near tie the other
         * way" or "a different answer entirely". */
        {
            const uint32_t V = llm_get_info()->n_vocab;
            int best[5]; float bv[5];
            for (int k = 0; k < 5; k++) { best[k] = -1; bv[k] = -1e30f; }
            for (uint32_t i = 0; i < V; i++) {
                const float v = a_logits[i];
                for (int k = 0; k < 5; k++) {
                    if (v > bv[k]) {
                        for (int j = 4; j > k; j--) { bv[j] = bv[j-1]; best[j] = best[j-1]; }
                        bv[k] = v; best[k] = (int)i;
                        break;
                    }
                }
            }
            fprintf(stderr, "  step %d:", g);
            for (int k = 0; k < 5; k++) {
                char pc[64];
                llm_decode(best[k], pc, sizeof(pc));
                fprintf(stderr, "  %.3f %s", bv[k], pc[0] ? pc : "?");
            }
            fprintf(stderr, "\n");
        }
        const int next = llm_argmax();
        char piece[64];
        llm_decode(next, piece, sizeof(piece));
        printf("%s", piece);
        fflush(stdout);
        if (llm_eval(next, pos++) != 0) break;
    }
    printf("\n");
    return 0;
}
