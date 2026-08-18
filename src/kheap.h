#ifndef KHEAP_H
#define KHEAP_H

/*
 * src/kheap.h — the kernel heap.
 *
 * Before this there was no such thing. Every buffer in the system was a
 * static array sized for the worst case at compile time, which is why
 * the app arena is 256 KB whether the program needs 4 KB or 4 MB, and
 * why the MMIO mapper's page pool is thirty-two pages and a device with
 * a larger BAR than that simply does not get mapped.
 *
 * Two allocators, picked by size:
 *
 *   ≤ 2 KB   a slab. One page per slab, carved into equal objects of one
 *            size class, with the free ones threaded together through
 *            their own storage. Allocation and release are both a couple
 *            of pointer moves, and freeing needs no search because the
 *            page a pointer belongs to is the pointer with its low bits
 *            cleared.
 *
 *   > 2 KB   a run of physically contiguous frames straight from the
 *            page allocator, addressed through the direct map. No page
 *            tables are touched: every byte of RAM already has a kernel
 *            virtual address, and inventing a second one for it would
 *            buy nothing but TLB pressure.
 *
 * Both put a header at the start of the containing page, and the two
 * headers carry different magic numbers. That is what lets free() work
 * out which allocator a pointer came from without the caller saying, and
 * what lets it refuse a pointer that came from neither.
 */

#include <stdint.h>
#include <stddef.h>
#include "pmm.h"

#define KHEAP_SLAB_MAGIC   0x534C41425F565839ULL   /* "SLAB_VX9" */
#define KHEAP_LARGE_MAGIC  0x4C4152475F565839ULL   /* "LARG_VX9" */

#define KHEAP_MAX_SLAB     2048
#define KHEAP_HDR_BYTES    64

/* 16 is the smallest class because it is the smallest that can hold the
 * free-list pointer an object needs while it is free, with room left for
 * the alignment every caller assumes. */
static const uint32_t kheap_classes[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};
#define KHEAP_NCLASS ((int)(sizeof(kheap_classes) / sizeof(kheap_classes[0])))

typedef struct kslab {
    uint64_t      magic;
    struct kslab *next;
    struct kslab *prev;
    void         *freelist;
    uint32_t      obj_size;
    uint32_t      obj_total;
    uint32_t      obj_free;
    uint32_t      cls;
} kslab_t;

typedef struct {
    uint64_t magic;
    uint64_t frames;        /* how many pages the run holds */
    uint64_t bytes;         /* what the caller asked for */
    uint64_t pad;
} klarge_t;

static kslab_t   *kheap_partial[KHEAP_NCLASS];
static spinlock_t kheap_lock;
static uint64_t   kheap_live_bytes = 0;
static uint64_t   kheap_slab_pages = 0;
static uint64_t   kheap_large_pages = 0;

static int kheap_class_of(uint64_t n) {
    for (int i = 0; i < KHEAP_NCLASS; i++)
        if (n <= kheap_classes[i]) return i;
    return -1;
}

/* A new slab for a class, threaded so that the first object is the head
 * of the free list and each free object points at the next. */
static kslab_t *kheap_new_slab(int cls) {
    uint64_t phys = pmm_alloc();
    if (!phys) return 0;
    kslab_t *s = (kslab_t *)(uintptr_t)phys_to_virt(phys);

    uint32_t osz = kheap_classes[cls];
    uint32_t total = (uint32_t)((PAGE_SIZE - KHEAP_HDR_BYTES) / osz);

    s->magic     = KHEAP_SLAB_MAGIC;
    s->next      = 0;
    s->prev      = 0;
    s->obj_size  = osz;
    s->obj_total = total;
    s->obj_free  = total;
    s->cls       = (uint32_t)cls;

    uint8_t *base = (uint8_t *)s + KHEAP_HDR_BYTES;
    s->freelist = base;
    for (uint32_t i = 0; i + 1 < total; i++)
        *(void **)(base + (uint64_t)i * osz) = base + (uint64_t)(i + 1) * osz;
    *(void **)(base + (uint64_t)(total - 1) * osz) = 0;

    kheap_slab_pages++;
    return s;
}

static void kheap_unlink(kslab_t *s) {
    if (s->prev) s->prev->next = s->next;
    else         kheap_partial[s->cls] = s->next;
    if (s->next) s->next->prev = s->prev;
    s->next = s->prev = 0;
}

static void kheap_link(kslab_t *s) {
    s->prev = 0;
    s->next = kheap_partial[s->cls];
    if (s->next) s->next->prev = s;
    kheap_partial[s->cls] = s;
}

static void *kmalloc(uint64_t n) {
    if (n == 0) return 0;
    uint64_t flags = spin_lock_irq(&kheap_lock);
    void *out = 0;

    int cls = n <= KHEAP_MAX_SLAB ? kheap_class_of(n) : -1;
    if (cls >= 0) {
        kslab_t *s = kheap_partial[cls];
        if (!s) {
            s = kheap_new_slab(cls);
            if (s) kheap_link(s);
        }
        if (s) {
            out = s->freelist;
            s->freelist = *(void **)out;
            s->obj_free--;
            /* A slab with nothing left is taken off the partial list; it
             * comes back the moment one of its objects is freed. */
            if (s->obj_free == 0) kheap_unlink(s);
            kheap_live_bytes += s->obj_size;
        }
    } else {
        uint64_t need   = KHEAP_HDR_BYTES + n;
        uint64_t frames = PAGE_ALIGN_UP(need) / PAGE_SIZE;
        uint64_t phys   = pmm_alloc_contig(frames);
        if (phys) {
            klarge_t *h = (klarge_t *)(uintptr_t)phys_to_virt(phys);
            h->magic  = KHEAP_LARGE_MAGIC;
            h->frames = frames;
            h->bytes  = n;
            h->pad    = 0;
            out = (uint8_t *)h + KHEAP_HDR_BYTES;
            kheap_large_pages += frames;
            kheap_live_bytes  += n;
        }
    }

    spin_unlock_irq(&kheap_lock, flags);
    return out;
}

static void kfree(void *p) {
    if (!p) return;
    uint64_t flags = spin_lock_irq(&kheap_lock);

    uint64_t page = (uint64_t)(uintptr_t)p & ~PAGE_MASK;
    uint64_t magic = *(uint64_t *)(uintptr_t)page;

    if (magic == KHEAP_SLAB_MAGIC) {
        kslab_t *s = (kslab_t *)(uintptr_t)page;
        int was_full = (s->obj_free == 0);
        *(void **)p = s->freelist;
        s->freelist = p;
        s->obj_free++;
        kheap_live_bytes -= s->obj_size;
        if (was_full) kheap_link(s);
        /* An entirely free slab goes back to the page allocator. Keeping
         * one around per class would avoid a little churn; giving it back
         * means a burst of small allocations does not permanently cost
         * the machine the pages it briefly needed. */
        if (s->obj_free == s->obj_total) {
            kheap_unlink(s);
            s->magic = 0;
            kheap_slab_pages--;
            uint64_t phys = kern_virt_to_phys((void *)(uintptr_t)page);
            spin_unlock_irq(&kheap_lock, flags);
            pmm_free(phys);
            return;
        }
    } else if (magic == KHEAP_LARGE_MAGIC) {
        klarge_t *h = (klarge_t *)(uintptr_t)page;
        uint64_t frames = h->frames;
        h->magic = 0;
        kheap_large_pages -= frames;
        kheap_live_bytes  -= h->bytes;
        uint64_t phys = kern_virt_to_phys((void *)(uintptr_t)page);
        spin_unlock_irq(&kheap_lock, flags);
        pmm_free_contig(phys, frames);
        return;
    } else {
        /* Not ours. Silently ignoring it would turn a double free into a
         * corruption later and somewhere else. */
        spin_unlock_irq(&kheap_lock, flags);
        serial_puts("[kheap] free of a pointer this heap did not hand out\n");
        return;
    }

    spin_unlock_irq(&kheap_lock, flags);
}

/* How big is the block behind this pointer, really? Needed by realloc,
 * and the answer differs by allocator. */
static uint64_t kheap_usable(void *p) {
    if (!p) return 0;
    uint64_t page = (uint64_t)(uintptr_t)p & ~PAGE_MASK;
    uint64_t magic = *(uint64_t *)(uintptr_t)page;
    if (magic == KHEAP_SLAB_MAGIC)  return ((kslab_t *)(uintptr_t)page)->obj_size;
    if (magic == KHEAP_LARGE_MAGIC) return ((klarge_t *)(uintptr_t)page)->bytes;
    return 0;
}

static void *kcalloc(uint64_t count, uint64_t size) {
    uint64_t n = count * size;
    if (count && n / count != size) return 0;      /* overflowed */
    void *p = kmalloc(n);
    if (p) for (uint64_t i = 0; i < n; i++) ((uint8_t *)p)[i] = 0;
    return p;
}

static void *krealloc(void *p, uint64_t n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return 0; }
    uint64_t have = kheap_usable(p);
    if (have >= n) return p;
    void *q = kmalloc(n);
    if (!q) return 0;
    for (uint64_t i = 0; i < have; i++) ((uint8_t *)q)[i] = ((uint8_t *)p)[i];
    kfree(p);
    return q;
}

/* Page-aligned, for the callers that need it — DMA rings, page tables
 * handed to a device, anything the hardware indexes by frame. */
static void *kmalloc_pages(uint64_t frames, uint64_t *out_phys) {
    uint64_t phys = pmm_alloc_contig(frames);
    if (!phys) { if (out_phys) *out_phys = 0; return 0; }
    if (out_phys) *out_phys = phys;
    kheap_large_pages += frames;
    return (void *)(uintptr_t)phys_to_virt(phys);
}

static void kfree_pages(void *p, uint64_t frames) {
    if (!p) return;
    kheap_large_pages -= frames;
    pmm_free_contig(kern_virt_to_phys(p), frames);
}

static uint64_t kheap_in_use_kb(void) { return kheap_live_bytes / 1024; }

#endif /* KHEAP_H */
