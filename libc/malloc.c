/*
 * libc/malloc.c — the user-space heap.
 *
 * A first-fit free list over memory obtained from the kernel with sbrk,
 * with splitting on allocation and coalescing on release. That is the
 * textbook allocator and it is the right one here: the programs this
 * serves allocate a handful of buffers and keep them, and a
 * size-classed allocator would be more code to reach the same answer.
 *
 * Every block carries a header immediately before the pointer handed
 * out, which is what makes free(p) possible without the caller saying
 * how large p was. The header is padded so that the payload comes back
 * sixteen-byte aligned — the alignment SSE loads and stores require, and
 * therefore the alignment the compiler assumes malloc returns now that
 * user programs are built with floating point available.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>

typedef struct block {
    size_t        size;      /* payload bytes, not counting this header */
    struct block *next;      /* the next block by address, free or not  */
    int           free;
    int           pad;
} block_t;

#define HDR_SIZE   ((size_t)32)          /* sizeof(block_t), rounded to 16 */
#define ALIGN_UP(n) (((n) + 15u) & ~(size_t)15u)
#define MIN_SPLIT  32                    /* below this, do not bother      */
#define GROW_MIN   (64 * 1024)           /* ask the kernel in useful sizes */

static block_t *heap_head = 0;
static block_t *heap_tail = 0;

static void *sbrk_raw(long delta) {
    long r = __syscall1(SYS_SBRK, delta);
    if (r == -1) return (void *)-1;
    return (void *)(uintptr_t)r;
}

/* Take more address space and turn it into one free block at the end of
 * the list. The request is rounded up because a syscall per malloc would
 * cost more than the memory it saves. */
static block_t *heap_grow(size_t need) {
    size_t want = ALIGN_UP(need + HDR_SIZE);
    if (want < GROW_MIN) want = GROW_MIN;

    void *base = sbrk_raw((long)want);
    if (base == (void *)-1) return 0;

    block_t *b = (block_t *)base;
    b->size = want - HDR_SIZE;
    b->next = 0;
    b->free = 1;
    b->pad  = 0;

    if (heap_tail) {
        heap_tail->next = b;
        /* If the previous end of the heap was free, the two are now one
         * contiguous run and there is no reason to keep them apart. */
        if (heap_tail->free &&
            (uint8_t *)heap_tail + HDR_SIZE + heap_tail->size == (uint8_t *)b) {
            heap_tail->size += HDR_SIZE + b->size;
            heap_tail->next = 0;
            return heap_tail;
        }
    } else {
        heap_head = b;
    }
    heap_tail = b;
    return b;
}

static void split(block_t *b, size_t want) {
    if (b->size < want + HDR_SIZE + MIN_SPLIT) return;
    block_t *rest = (block_t *)((uint8_t *)b + HDR_SIZE + want);
    rest->size = b->size - want - HDR_SIZE;
    rest->next = b->next;
    rest->free = 1;
    rest->pad  = 0;
    b->size = want;
    b->next = rest;
    if (heap_tail == b) heap_tail = rest;
}

void *malloc(size_t n) {
    if (n == 0) return 0;
    size_t want = ALIGN_UP(n);

    for (block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            return (uint8_t *)b + HDR_SIZE;
        }
    }

    block_t *b = heap_grow(want);
    if (!b) return 0;
    split(b, want);
    b->free = 0;
    return (uint8_t *)b + HDR_SIZE;
}

void free(void *p) {
    if (!p) return;
    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    b->free = 1;

    /* Merge forward, then find the predecessor and merge that too. The
     * walk is linear, which is the price of not keeping a back pointer
     * in every header; on a heap of tens of blocks it is nothing. */
    if (b->next && b->next->free &&
        (uint8_t *)b + HDR_SIZE + b->size == (uint8_t *)b->next) {
        if (heap_tail == b->next) heap_tail = b;
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
    }
    for (block_t *q = heap_head; q && q != b; q = q->next) {
        if (q->next == b && q->free &&
            (uint8_t *)q + HDR_SIZE + q->size == (uint8_t *)b) {
            if (heap_tail == b) heap_tail = q;
            q->size += HDR_SIZE + b->size;
            q->next  = b->next;
            break;
        }
    }
}

void *calloc(size_t count, size_t size) {
    size_t n = count * size;
    if (count && n / count != size) return 0;      /* overflowed */
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}

void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }

    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    size_t want = ALIGN_UP(n);
    if (b->size >= want) { split(b, want); return p; }

    /* Growing into a free neighbour costs nothing and saves the copy,
     * which is the case a loop that appends one element at a time hits
     * on every iteration. */
    if (b->next && b->next->free &&
        (uint8_t *)b + HDR_SIZE + b->size == (uint8_t *)b->next &&
        b->size + HDR_SIZE + b->next->size >= want) {
        if (heap_tail == b->next) heap_tail = b;
        b->size += HDR_SIZE + b->next->size;
        b->next  = b->next->next;
        split(b, want);
        return p;
    }

    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, b->size < n ? b->size : n);
    free(p);
    return q;
}

/* ---- the rest of stdlib ---- */

void exit(int status) {
    __syscall1(SYS_EXIT, (long)status);
    for (;;) { }                 /* the kernel does not return from that */
}

void abort(void) { exit(134); }

long strtol(const char *s, char **end, int base) {
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;

    int neg = 0;
    if (*p == '-') { neg = 1; p++; }
    else if (*p == '+') p++;

    if ((base == 0 || base == 16) && p[0] == '0' &&
        (p[1] == 'x' || p[1] == 'X')) { p += 2; base = 16; }
    else if (base == 0 && p[0] == '0') { base = 8; }
    else if (base == 0) base = 10;

    long v = 0;
    for (;; p++) {
        int d;
        if      (*p >= '0' && *p <= '9') d = *p - '0';
        else if (*p >= 'a' && *p <= 'z') d = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'Z') d = *p - 'A' + 10;
        else break;
        if (d >= base) break;
        v = v * base + d;
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}

int  atoi(const char *s) { return (int)strtol(s, 0, 10); }
long atol(const char *s) { return strtol(s, 0, 10); }

int  abs(int v)   { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }

/* Numerical Recipes' constants: a full-period generator modulo 2^32,
 * with the top bit dropped so the result is non-negative. */
static unsigned long rand_state = 1;

void srand(unsigned int seed) { rand_state = seed; }

int rand(void) {
    rand_state = rand_state * 1664525u + 1013904223u;
    return (int)((rand_state >> 1) & 0x7FFFFFFF);
}
