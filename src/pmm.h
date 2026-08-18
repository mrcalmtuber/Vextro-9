#ifndef PMM_H
#define PMM_H

/*
 * src/pmm.h — the physical frame allocator.
 *
 * Until now nothing in this kernel owned physical memory. Limine handed
 * over a map of it, the inference arena took the largest region whole,
 * the MMIO mapper carved page-table pages out of a 32-entry static pool,
 * and everything else lived in .bss. That works exactly as long as
 * nothing needs a page it did not declare at compile time — which is to
 * say, right up until a process needs its own address space.
 *
 * So: one owner, a bitmap over every usable frame the firmware reported.
 * A bit set means the frame is in use. The bitmap itself is placed in
 * memory the map says is usable rather than in .bss, because sizing it
 * statically means either wasting two megabytes on a machine with a
 * gigabyte of RAM or refusing to boot on one with more.
 *
 * Everything here takes and returns *physical* addresses. Turning one
 * into something the CPU can dereference is phys_to_virt()'s job, and it
 * only works because Limine maps all of RAM into the higher half.
 */

#include <stdint.h>
#include <stddef.h>
#include "limine.h"
#include "pci.h"          /* hal_hhdm_offset, phys_to_virt, serial_* */

#define PAGE_SIZE   4096ULL
#define PAGE_SHIFT  12
#define PAGE_MASK   (PAGE_SIZE - 1)

#define PAGE_ALIGN_DOWN(x) ((uint64_t)(x) & ~PAGE_MASK)
#define PAGE_ALIGN_UP(x)   PAGE_ALIGN_DOWN((uint64_t)(x) + PAGE_MASK)

/* ---- interrupt-safe spinlock ----
 *
 * The allocator is reachable from the page-fault handler and from the
 * scheduler's own bookkeeping, so a lock that merely spins would
 * deadlock the instant a timer tick landed inside one. Saving and
 * clearing IF around the critical section is what makes it safe to call
 * from a thread and from the interrupt that preempts it.
 *
 * There is one processor here, so the spin itself never actually spins;
 * it is written this way so that bringing up an AP later does not
 * require revisiting every caller.
 */
typedef struct {
    volatile uint32_t locked;
    const char       *name;      /* for the report when it never comes free */
} spinlock_t;

/*
 * A lock that gives up.
 *
 * A plain spinlock has exactly one failure mode and it is the worst one:
 * if the holder never releases -- because it faulted, or because two
 * locks were taken in opposite orders on two processors -- every other
 * processor spins forever and the machine is dead with nothing on the
 * wire. There is no way to tell that apart from a hang.
 *
 * So the spin is bounded. After a number of attempts far beyond any
 * legitimate hold time, the lock is *taken anyway* and the event is
 * reported. That is not correct in the sense of preserving mutual
 * exclusion -- nothing can preserve it once the holder is gone -- but it
 * converts a silent freeze into a live machine and a message naming the
 * lock, which is the difference between a bug that can be found and one
 * that cannot.
 */
#define SPIN_TIMEOUT 40000000u

static uint64_t spin_timeouts = 0;

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}
static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
}

static void spin_report_timeout(spinlock_t *l);

static inline uint64_t spin_lock_irq(spinlock_t *l) {
    uint64_t flags = irq_save();
    uint32_t spins = 0;
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        if (++spins >= SPIN_TIMEOUT) {
            spin_report_timeout(l);
            __atomic_store_n(&l->locked, 1u, __ATOMIC_RELEASE);
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    return flags;
}
static inline void spin_unlock_irq(spinlock_t *l, uint64_t flags) {
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    irq_restore(flags);
}

/* ---- state ---- */

static uint64_t  *pmm_bitmap      = 0;    /* HHDM pointer, one bit per frame */
static uint64_t   pmm_bitmap_phys = 0;
static uint64_t   pmm_bitmap_words = 0;
static uint64_t   pmm_total_frames = 0;   /* frames the map called usable   */
static uint64_t   pmm_free_frames  = 0;
static uint64_t   pmm_highest_frame = 0;  /* one past the last tracked bit  */
static uint64_t   pmm_next_hint    = 0;   /* where the last search stopped  */
static spinlock_t pmm_lock = { 0, "pmm" };
static int        pmm_ready = 0;

static void spin_report_timeout(spinlock_t *l) {
    spin_timeouts++;
    serial_puts("[lock] timed out waiting for ");
    serial_puts(l->name ? l->name : "an unnamed lock");
    serial_puts(" - taking it anyway; the holder is not coming back\n");
}

static inline int pmm_test(uint64_t frame) {
    return (pmm_bitmap[frame >> 6] >> (frame & 63)) & 1u;
}
static inline void pmm_set(uint64_t frame) {
    pmm_bitmap[frame >> 6] |= 1ULL << (frame & 63);
}
static inline void pmm_clear(uint64_t frame) {
    pmm_bitmap[frame >> 6] &= ~(1ULL << (frame & 63));
}

/*
 * Build the bitmap.
 *
 * Two passes over the map, and the order matters. The first only
 * measures — how high physical memory goes, and which usable region is
 * big enough to hold the bitmap describing it. The bitmap cannot be
 * allocated before it exists, so it is placed by hand and then marked
 * used along with everything else that is not free.
 *
 * The map is trusted about what is usable and about nothing else:
 * regions are clipped to page boundaries inward, so a region that starts
 * mid-page never yields a frame that overlaps whatever owns the rest of
 * that page.
 */
static void pmm_init(struct limine_memmap_response *mm, uint64_t hhdm) {
    hal_hhdm_offset = hhdm;

    uint64_t highest = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t end = e->base + e->length;
        if (end > highest) highest = end;
    }
    pmm_highest_frame = highest >> PAGE_SHIFT;
    pmm_bitmap_words  = (pmm_highest_frame + 63) / 64;
    uint64_t bitmap_bytes = pmm_bitmap_words * 8;

    /* Somewhere to put it. The first usable region that can hold the
     * whole thing wins; a region is never split for this, because a
     * bitmap in two pieces would need an index to find either half. */
    for (uint64_t i = 0; i < mm->entry_count && !pmm_bitmap_phys; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t base = PAGE_ALIGN_UP(e->base);
        uint64_t end  = PAGE_ALIGN_DOWN(e->base + e->length);
        if (end > base && end - base >= bitmap_bytes)
            pmm_bitmap_phys = base;
    }
    if (!pmm_bitmap_phys) {
        serial_puts("[pmm] no region large enough for the frame bitmap\n");
        return;
    }
    pmm_bitmap = (uint64_t *)(uintptr_t)phys_to_virt(pmm_bitmap_phys);

    /* Everything is in use until something says otherwise. Reserved,
     * ACPI and bootloader regions therefore need no handling at all —
     * not being named as usable is enough to keep them out. */
    for (uint64_t w = 0; w < pmm_bitmap_words; w++) pmm_bitmap[w] = ~0ULL;

    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t f0 = PAGE_ALIGN_UP(e->base)              >> PAGE_SHIFT;
        uint64_t f1 = PAGE_ALIGN_DOWN(e->base + e->length) >> PAGE_SHIFT;
        for (uint64_t f = f0; f < f1; f++) {
            if (pmm_test(f)) { pmm_clear(f); pmm_free_frames++; }
            pmm_total_frames++;
        }
    }

    /* And now the bitmap's own pages, which the loop above just freed. */
    uint64_t bf0 = pmm_bitmap_phys >> PAGE_SHIFT;
    uint64_t bf1 = PAGE_ALIGN_UP(pmm_bitmap_phys + bitmap_bytes) >> PAGE_SHIFT;
    for (uint64_t f = bf0; f < bf1; f++)
        if (!pmm_test(f)) { pmm_set(f); pmm_free_frames--; }

    /*
     * The first megabyte is never handed out. Some of it is genuinely
     * usable and the map says so, but the real-mode IVT, the BIOS data
     * area and the VGA window all live there, and a driver that pokes
     * one of them expects to find it rather than a page table.
     */
    for (uint64_t f = 0; f < (0x100000ULL >> PAGE_SHIFT) &&
                         f < pmm_highest_frame; f++)
        if (!pmm_test(f)) { pmm_set(f); pmm_free_frames--; }

    pmm_next_hint = 0;
    pmm_ready = 1;

    serial_puts("[pmm] ");
    serial_put_dec((uint32_t)(pmm_free_frames * 4 / 1024));
    serial_puts(" MB free in ");
    serial_put_dec((uint32_t)pmm_free_frames);
    serial_puts(" frames, bitmap ");
    serial_put_dec((uint32_t)(bitmap_bytes / 1024));
    serial_puts(" KB at ");
    serial_put_hex32((uint32_t)pmm_bitmap_phys);
    serial_puts("\n");
}

/* Zero a frame through the HHDM. Every page this hands out is clean,
 * because the two things that most want frames — page tables and fresh
 * user memory — are both wrong if they are not. */
static void pmm_zero_frame(uint64_t phys) {
    uint64_t *p = (uint64_t *)(uintptr_t)phys_to_virt(phys);
    for (uint64_t i = 0; i < PAGE_SIZE / 8; i++) p[i] = 0;
}

static uint64_t pmm_alloc(void) {
    if (!pmm_ready) return 0;
    uint64_t flags = spin_lock_irq(&pmm_lock);

    uint64_t frame = 0;
    /* Two sweeps: from the hint to the end, then from the start to the
     * hint. A single sweep from zero is O(RAM) on every allocation once
     * low memory fills, which is what makes loading a large image feel
     * like a hang rather than a load. */
    for (int pass = 0; pass < 2 && !frame; pass++) {
        uint64_t w0 = pass == 0 ? pmm_next_hint : 0;
        uint64_t w1 = pass == 0 ? pmm_bitmap_words : pmm_next_hint;
        for (uint64_t w = w0; w < w1; w++) {
            if (pmm_bitmap[w] == ~0ULL) continue;
            uint64_t bit = (uint64_t)__builtin_ctzll(~pmm_bitmap[w]);
            uint64_t f = w * 64 + bit;
            if (f >= pmm_highest_frame) break;
            pmm_bitmap[w] |= 1ULL << bit;
            pmm_free_frames--;
            pmm_next_hint = w;
            frame = f;
            break;
        }
    }
    spin_unlock_irq(&pmm_lock, flags);
    if (!frame) return 0;

    uint64_t phys = frame << PAGE_SHIFT;
    pmm_zero_frame(phys);
    return phys;
}

/*
 * ---- reference counts ----
 *
 * A frame used to have exactly one owner, so freeing it was
 * unconditional. Copy-on-write breaks that: two address spaces point at
 * one frame and neither knows about the other, so the first to release
 * it must not take it away from the second.
 *
 * One counter per frame, sixteen bits, allocated from the allocator
 * itself once the bitmap exists. Zero means "one owner" — the ordinary
 * case — so nothing has to be initialised and no allocation path pays
 * for the feature. Sharing raises it; freeing lowers it and only
 * actually releases the frame at zero.
 *
 * Saturation is deliberate. A frame shared 65535 ways is never freed
 * again, which wastes four kilobytes; wrapping would free it while
 * thousands of mappings still pointed at it.
 */
static uint16_t *pmm_ref = 0;

static uint64_t pmm_alloc_contig(uint64_t frames);

static void pmm_ref_init(void) {
    if (!pmm_ready || pmm_ref) return;
    uint64_t bytes  = pmm_highest_frame * sizeof(uint16_t);
    uint64_t frames = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    uint64_t phys   = pmm_alloc_contig(frames);
    if (!phys) {
        serial_puts("[pmm] no room for the reference table; "
                    "copy-on-write unavailable\n");
        return;
    }
    pmm_ref = (uint16_t *)(uintptr_t)phys_to_virt(phys);
    for (uint64_t i = 0; i < pmm_highest_frame; i++) pmm_ref[i] = 0;
    serial_puts("[pmm] reference table ");
    serial_put_dec((uint32_t)(bytes / 1024));
    serial_puts(" KB\n");
}

static void pmm_ref_inc(uint64_t phys) {
    if (!pmm_ref) return;
    uint64_t f = phys >> PAGE_SHIFT;
    if (f >= pmm_highest_frame) return;
    uint64_t flags = spin_lock_irq(&pmm_lock);
    if (pmm_ref[f] < 0xFFFF) pmm_ref[f]++;
    spin_unlock_irq(&pmm_lock, flags);
}

static int pmm_ref_count(uint64_t phys) {
    if (!pmm_ref) return 1;
    uint64_t f = phys >> PAGE_SHIFT;
    if (f >= pmm_highest_frame) return 1;
    return pmm_ref[f] + 1;
}

static void pmm_free(uint64_t phys) {
    if (!pmm_ready || !phys) return;
    uint64_t frame = phys >> PAGE_SHIFT;
    if (frame >= pmm_highest_frame) return;
    uint64_t flags = spin_lock_irq(&pmm_lock);

    /* Still shared: this owner is done with it, the others are not. */
    if (pmm_ref && pmm_ref[frame]) {
        pmm_ref[frame]--;
        spin_unlock_irq(&pmm_lock, flags);
        return;
    }

    if (pmm_test(frame)) {
        pmm_clear(frame);
        pmm_free_frames++;
        if (frame / 64 < pmm_next_hint) pmm_next_hint = frame / 64;
    }
    spin_unlock_irq(&pmm_lock, flags);
}

/*
 * A physically contiguous run.
 *
 * Wanted by two callers with very different appetites: a DMA descriptor
 * table needs a handful of pages the device can walk, and the inference
 * arena needs most of the machine. Both are first-fit over the bitmap,
 * which is linear in the size of the map and is only ever done at setup.
 *
 * Pages are *not* zeroed here — a caller asking for four hundred
 * megabytes does not want them touched twice, and every current caller
 * either overwrites the run immediately or clears what it uses.
 */
static uint64_t pmm_alloc_contig(uint64_t frames) {
    if (!pmm_ready || frames == 0) return 0;
    uint64_t flags = spin_lock_irq(&pmm_lock);

    uint64_t found = 0;
    uint64_t run = 0, start = 0;
    for (uint64_t f = (0x100000ULL >> PAGE_SHIFT); f < pmm_highest_frame; f++) {
        /* Skip a full word at a time when it is entirely allocated. */
        if ((f & 63) == 0 && pmm_bitmap[f >> 6] == ~0ULL) {
            run = 0;
            f += 63;
            continue;
        }
        if (pmm_test(f)) { run = 0; continue; }
        if (run == 0) start = f;
        if (++run == frames) { found = start; break; }
    }
    if (found) {
        for (uint64_t f = found; f < found + frames; f++) pmm_set(f);
        pmm_free_frames -= frames;
    }
    spin_unlock_irq(&pmm_lock, flags);
    return found << PAGE_SHIFT;
}

static void pmm_free_contig(uint64_t phys, uint64_t frames) {
    for (uint64_t i = 0; i < frames; i++)
        pmm_free(phys + i * PAGE_SIZE);
}

/*
 * The largest run this machine can still give, down to a floor.
 *
 * The inference arena used to be handed the biggest region in the
 * firmware map outright, which is a fine answer when nothing else wants
 * physical memory and the wrong one now that page tables, kernel heap
 * and per-process address spaces all come from the same pool. So it asks
 * for what is left over instead: everything except `keep` frames, halved
 * on each refusal until it fits or falls below `floor`.
 */
static uint64_t pmm_alloc_largest(uint64_t keep, uint64_t floor,
                                  uint64_t *out_frames) {
    if (!pmm_ready || pmm_free_frames <= keep + floor) return 0;
    uint64_t want = pmm_free_frames - keep;
    while (want >= floor) {
        uint64_t p = pmm_alloc_contig(want);
        if (p) { *out_frames = want; return p; }
        want /= 2;
    }
    return 0;
}

/* A zeroed page as something the kernel can write to directly. Page
 * tables are built through this; every other caller wants the physical
 * address and calls pmm_alloc() instead. */
static void *pmm_alloc_page_virt(uint64_t *out_phys) {
    uint64_t p = pmm_alloc();
    if (!p) { if (out_phys) *out_phys = 0; return 0; }
    if (out_phys) *out_phys = p;
    return (void *)(uintptr_t)phys_to_virt(p);
}

static uint64_t pmm_free_kb(void)  { return pmm_free_frames * 4; }
static uint64_t pmm_total_kb(void) { return pmm_total_frames * 4; }

/* Let the MMIO mapper in pci.h take its page-table pages from here
 * rather than from its thirty-two-page static pool. */
static void pmm_install_mmio_hook(void) { mmio_frame_alloc = pmm_alloc; }

#endif /* PMM_H */
