/*
 * Two models, resident at once, answering the same question.
 *
 * Checks the thing that is easy to get wrong about holding two models in
 * one arena: that loading the second does not disturb the first. So it
 * loads both, then asks *slot 0 again* after slot 1 is resident, and the
 * answer has to be the same as it was before slot 1 existed. A shared
 * bump allocator makes that a real question rather than a formality.
 *
 *   cc -O2 -o build/llm_pair_test tools/llm_pair_test.c
 *   ./build/llm_pair_test a.gguf b.gguf "prompt" 12
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/llm.c"

static FILE *g_fp[LLM_SLOTS];

static int host_read(void *ctx, uint64_t off, void *buf,
                     uint32_t len, uint32_t *got) {
    FILE *f = (FILE *)ctx;
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return -1;
    size_t n = fread(buf, 1, len, f);
    *got = (uint32_t)n;
    return n == len ? 0 : -1;
}

static int load_into(int slot, const char *path) {
    llm_slot_select(slot);
    g_fp[slot] = fopen(path, "rb");
    if (!g_fp[slot]) { perror(path); return -1; }
    fseeko(g_fp[slot], 0, SEEK_END);
    const uint64_t size = (uint64_t)ftello(g_fp[slot]);
    const char *err = "?";
    if (llm_load(host_read, g_fp[slot], size, &err) != 0 ||
        llm_load_weights(&err) != 0) {
        fprintf(stderr, "slot %d: %s\n", slot, err);
        return -1;
    }
    fprintf(stderr, "slot %d: %s  %u layers, %u embd, vocab %u\n",
            slot, llm_get_info()->arch, llm_get_info()->n_layer,
            llm_get_info()->n_embd, llm_get_info()->n_vocab);
    return 0;
}

static void answer(int slot, const char *prompt, int want, char *out, int max) {
    llm_slot_select(slot);
    static int32_t toks[1024];
    const int n = llm_encode(prompt, toks, 1024);
    int pos = 0, w = 0;
    out[0] = '\0';
    if (n <= 0) return;
    for (int i = 0; i < n; i++) llm_eval(toks[i], pos++);
    for (int g = 0; g < want; g++) {
        const int next = llm_argmax();
        char piece[64];
        llm_decode(next, piece, sizeof(piece));
        for (int i = 0; piece[i] && w < max - 1; i++) out[w++] = piece[i];
        out[w] = '\0';
        if (llm_eval(next, pos++) != 0) break;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <a.gguf> <b.gguf> <prompt> [n]\n", argv[0]);
        return 2;
    }
    const int want = argc > 4 ? atoi(argv[4]) : 12;

    static uint8_t *arena;
    const uint64_t bytes = 3ull << 30;
    arena = malloc(bytes);
    if (!arena) { fprintf(stderr, "no arena\n"); return 1; }
    llm_arena_init(arena, bytes);

    if (load_into(0, argv[1]) != 0) return 1;

    char first[512];
    answer(0, argv[3], want, first, sizeof(first));

    if (load_into(1, argv[2]) != 0) return 1;

    char again[512], second[512];
    answer(0, argv[3], want, again, sizeof(again));
    answer(1, argv[3], want, second, sizeof(second));

    printf("slot 0 before : %s\n", first);
    printf("slot 0 after  : %s\n", again);
    printf("slot 1        : %s\n", second);
    printf("\nslot 0 survived loading slot 1: %s\n",
           strcmp(first, again) == 0 ? "yes" : "NO -- the arena was clobbered");
    return strcmp(first, again) == 0 ? 0 : 1;
}
