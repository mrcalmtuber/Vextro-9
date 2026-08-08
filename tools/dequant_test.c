/*
 * Dequantisation check, run on the host against the real model file.
 *
 * This compiles src/llm.c itself rather than a copy of it, so what is
 * tested is the code the kernel runs. llm.c depends on nothing but
 * llm.h and a read callback, which is what makes that possible.
 *
 * It prints values; tools/dequant_ref.py decodes the same bytes
 * straight from the GGUF file using an independent implementation of
 * the block formats and compares. Two implementations of a published
 * format agreeing on real weights is the check that matters -- a
 * decoder tested only against its own encoder can be wrong in both
 * directions at once and still pass.
 *
 *   cc -O2 -o build/dequant_test tools/dequant_test.c
 *   ./build/dequant_test assets/qwen2.gguf <tensor-name> <first> <count>
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
    if (argc < 5) {
        fprintf(stderr, "usage: %s <gguf> <tensor> <first> <count>\n", argv[0]);
        return 2;
    }
    g_fp = fopen(argv[1], "rb");
    if (!g_fp) { perror(argv[1]); return 2; }
    fseeko(g_fp, 0, SEEK_END);
    uint64_t size = (uint64_t)ftello(g_fp);

    /* llm.c wants an arena; the tensor table is all this test loads. */
    static uint8_t arena[64u << 20];
    llm_arena_init(arena, sizeof(arena));

    const char *err = "?";
    if (llm_load(host_read, 0, size, &err) != 0) {
        fprintf(stderr, "load failed: %s\n", err);
        return 1;
    }

    int idx = llm_tensor_find(argv[2]);
    if (idx < 0) { fprintf(stderr, "no tensor %s\n", argv[2]); return 1; }

    uint64_t first = strtoull(argv[3], 0, 0);
    int count = atoi(argv[4]);
    if (count <= 0 || count > 4096) { fprintf(stderr, "bad count\n"); return 2; }

    int32_t *vals = malloc((size_t)count * sizeof(int32_t));
    if (!vals) return 1;
    int n = llm_tensor_peek(idx, first, count, vals);
    if (n != count) {
        fprintf(stderr, "peek failed on %s (type %u): got %d\n",
                argv[2], llm_tensor_type(idx), n);
        return 1;
    }

    printf("# %s type=%u (%s) elems=%llu\n", argv[2], llm_tensor_type(idx),
           llm_quant_name(llm_tensor_type(idx)),
           (unsigned long long)llm_tensor_elems(idx));
    for (int i = 0; i < n; i++) printf("%d\n", vals[i]);
    return 0;
}
