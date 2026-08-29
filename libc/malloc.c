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
 *
 * ============================================================
 *  1. THE LOCK, AND THE BUG IT FIXES
 * ============================================================
 *
 * There was not one, and there needed to be from the moment SYS_CLONE
 * existed.
 *
 * Everything below walks and rewrites one global list. Two threads in
 * malloc at the same time both find the same free block, both split it,
 * and both return a pointer to it — so two objects occupy one piece of
 * memory and each one's writes are the other one's corruption. Two
 * threads in free() coalescing adjacent blocks can splice `next` into a
 * cycle, and the next allocation walks it forever.
 *
 * That is not a hazard this code grew into. It was true the day threads
 * were added and it did not show, because the program that exercised
 * them allocated before starting its workers and not after — which is
 * exactly the shape of a latent bug: correct behaviour that depends on
 * something nobody wrote down.
 *
 * The lock is the raw futex mutex from <vxmutex.h> and *not*
 * pthread_mutex_t, which matters: pthread_mutex_lock has an error-check
 * and a recursive mode that record an owner, and the owner is a thread
 * identifier fetched through machinery that can itself allocate. A
 * mutex that calls malloc is a mutex that cannot protect malloc.
 *
 * ============================================================
 *  2. THE LARGE PATH
 * ============================================================
 *
 * Everything used to come from sbrk, and the break only grows. A program
 * that allocates a hundred megabytes for one image, frees it, and never
 * asks for that much again keeps the hundred megabytes: the block goes
 * back on the free list, the break stays where it was, and the pages
 * stay committed.
 *
 * That is fine for the programs this started with and wrong for the ones
 * it is being built for. So an allocation of a quarter megabyte or more
 * is a mapping of its own, taken with mmap and given back with munmap —
 * which returns the address space *and* the frames behind it the moment
 * the caller is done with it.
 *
 * It also happens to be what makes `operator new` in the C++ runtime an
 * mmap-backed allocation without there being two allocators to keep in
 * step, which is the arrangement libcxx/src/new.cpp relies on.
 *
 * A large block is recognised by a magic word in its header rather than
 * by its size, because a block's size can be *changed* by realloc and
 * the way it was obtained cannot.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vxmutex.h>

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

/*
 * At and above this, an allocation is its own mapping.
 *
 * A quarter of a megabyte: large enough that the two extra system calls
 * are lost in the noise of using something that big, and small enough
 * that the buffers a program really does want back — a decoded image, a
 * parsed document, a frame — are on the right side of it.
 */
#define BIG_MIN    (256u * 1024u)

/* In `pad`, which was named that because it had nothing in it. A large
 * block's `next` is null and it is never on the list at all. */
#define BIG_MAGIC  0x5642474Du           /* "VBGM" */

static block_t *heap_head = 0;
static block_t *heap_tail = 0;

/*
 * Statically initialised to the free state, and that is load-bearing
 * rather than tidy: there is no call before the first malloc in which to
 * initialise it, because the first malloc may be the one crt0 makes
 * before anything else in this library has run.
 */
static vx_mutex_t heap_lock = 0;

static void *sbrk_raw(long delta) {
    long r = __syscall1(SYS_SBRK, delta);
    if (r == -1) return (void *)-1;
    return (void *)(uintptr_t)r;
}

/* ---- the large path ---- */

static void *big_alloc(size_t want) {
    const size_t bytes = ALIGN_UP(want + HDR_SIZE);
    void *p = mmap(0, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return 0;

    block_t *b = (block_t *)p;
    b->size = bytes - HDR_SIZE;
    b->next = 0;
    b->free = 0;
    b->pad  = (int)BIG_MAGIC;
    return (uint8_t *)b + HDR_SIZE;
}

static int is_big(const block_t *b) {
    return (uint32_t)b->pad == BIG_MAGIC;
}

static void big_free(block_t *b) {
    munmap(b, b->size + HDR_SIZE);
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
    if (want < n) return 0;                    /* the round-up overflowed */

    /* Its own mapping, and no lock: mmap does not touch the list. */
    if (want >= BIG_MIN) return big_alloc(want);

    vx_mutex_lock(&heap_lock);

    for (block_t *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= want) {
            split(b, want);
            b->free = 0;
            vx_mutex_unlock(&heap_lock);
            return (uint8_t *)b + HDR_SIZE;
        }
    }

    block_t *b = heap_grow(want);
    if (!b) { vx_mutex_unlock(&heap_lock); return 0; }
    split(b, want);
    b->free = 0;
    vx_mutex_unlock(&heap_lock);
    return (uint8_t *)b + HDR_SIZE;
}

void free(void *p) {
    if (!p) return;

    /* An over-aligned allocation first, because for one of those the
     * bytes below `p` are padding rather than a block header, and
     * reading a header out of padding is how this used to put a
     * fabricated pointer on the free list. See aligned_alloc in
     * libc/stdlib2.c for the argument that the check cannot misfire. */
    {
        size_t off = 0;
        if (__vx_aligned_block(p, &off, 0)) {
            p = (uint8_t *)p - off;
        }
    }

    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);

    if (is_big(b)) { big_free(b); return; }

    vx_mutex_lock(&heap_lock);
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
    vx_mutex_unlock(&heap_lock);
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

    /* An over-aligned block is always moved: growing it in place would
     * have to preserve an alignment the new block need not have. The
     * usable size is recorded beside the pointer, which is the only
     * reason this can copy the right number of bytes. */
    {
        size_t off = 0, had = 0;
        if (__vx_aligned_block(p, &off, &had)) {
            void *q = malloc(n);
            if (!q) return 0;
            memcpy(q, p, had < n ? had : n);
            free(p);
            return q;
        }
    }

    block_t *b = (block_t *)((uint8_t *)p - HDR_SIZE);
    size_t want = ALIGN_UP(n);
    if (want < n) return 0;

    /*
     * A large block is always moved rather than grown in place.
     *
     * mmap here has no remap, so extending one would mean a second
     * mapping that happened to be adjacent, which nothing guarantees.
     * Shrinking one *is* handled in place, and matters: a parser that
     * reads a file into a generous buffer and then trims it to the
     * length it actually used should not pay a copy of the whole thing
     * to do so.
     */
    if (is_big(b)) {
        if (want <= b->size) return p;
        void *q = malloc(n);
        if (!q) return 0;
        memcpy(q, p, b->size < n ? b->size : n);
        big_free(b);
        return q;
    }

    vx_mutex_lock(&heap_lock);

    if (b->size >= want) {
        split(b, want);
        vx_mutex_unlock(&heap_lock);
        return p;
    }

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
        vx_mutex_unlock(&heap_lock);
        return p;
    }

    const size_t had = b->size;
    vx_mutex_unlock(&heap_lock);

    /* Outside the lock, because malloc and free take it themselves and
     * this one is not recursive. `had` was read under it and the block
     * is this caller's own, so nothing else may change it. */
    void *q = malloc(n);
    if (!q) return 0;
    memcpy(q, p, had < n ? had : n);
    free(p);
    return q;
}

/* ---- the rest of stdlib ---- */

/* exit() moved to libc/exit.c when C++ arrived: it has to run the
 * static destructors, and the table those are registered in cannot live
 * in crt0.o -- see the note at the head of that file. */

void abort(void) {
    /* Not exit(): abort is defined as *not* running the handlers, and
     * the usual reason a program aborts is that something is already
     * broken enough that running more of its code is a bad idea. */
    _exit(134);
}

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
