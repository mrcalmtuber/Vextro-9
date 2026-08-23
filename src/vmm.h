#ifndef VMM_H
#define VMM_H

/*
 * src/vmm.h — address spaces.
 *
 * Every process gets its own PML4. The lower half of it, entries 0-255,
 * is that process's alone: its image, its stack, its heap, all marked
 * user-accessible. The upper half, entries 256-511, is the kernel's and
 * is the *same table pages* in every address space — not a copy, the
 * same physical frames — so a mapping the kernel makes after a process
 * exists is visible inside it, and so a syscall does not have to switch
 * CR3 to reach kernel memory.
 *
 * That sharing is why vmm_init() fills in every absent higher-half PML4
 * entry with an empty page directory pointer table up front. Once done,
 * the top-level array never changes again; everything the kernel maps
 * later lands one level down, inside a table every address space already
 * points at. Without it, an MMIO region mapped after a process was
 * created would be missing from that process's tables, and the driver
 * that touched it would fault — but only sometimes, and only depending
 * on which process happened to be current. That is the kind of bug that
 * takes a week.
 *
 * The kernel's own mappings come from Limine and are left exactly as
 * they are. There is no attempt here to rebuild the higher half: the
 * HHDM, the framebuffer, the kernel image and every device BAR mapped
 * through mmio_map() are already correct, and re-deriving them would be
 * a great deal of work whose only possible outcome is being wrong
 * somewhere.
 */

#include <stdint.h>
#include "pmm.h"

/* Page table entry bits beyond the ones pci.h already names. */
#define PTE_USER      (1ULL << 2)
#define PTE_ACCESSED  (1ULL << 5)
#define PTE_DIRTY     (1ULL << 6)
#define PTE_GLOBAL    (1ULL << 8)
#define PTE_NX        (1ULL << 63)

/*
 * Bits 9, 10 and 11 are the three the architecture leaves to software,
 * and all three are now spoken for.
 *
 *   SHARED  the frame belongs to somebody else — the app canvas, the
 *           syscall trampoline — so tearing an address space down unmaps
 *           it without handing the frame back to the allocator. Freeing
 *           a page the kernel is still using is silent corruption.
 *
 *   COW     the mapping is read-only *on purpose*, and a write to it is
 *           not an error but a request for a private copy. Without a bit
 *           saying so, the fault handler cannot tell a page shared for
 *           copy-on-write from one the program genuinely may not write.
 *
 *   GUARD   nothing is mapped and nothing ever will be. It marks the
 *           page below a stack, so that a fault there can be reported as
 *           an overflow rather than as an address out of nowhere.
 */
#define PTE_SHARED    (1ULL << 9)
#define PTE_COW       (1ULL << 10)
#define PTE_GUARD     (1ULL << 11)

/* ---- the user half ----
 *
 * Images keep the address they were linked at, which is 0x1000 for both
 * app.ld and vx.ld. That costs nothing and buys something real: an image
 * with an absolute reference in it still works, and the loader stops
 * being the only thing standing between a program and a wrong pointer.
 * Page zero stays unmapped, so a null dereference is a fault rather than
 * a read of whatever the image starts with.
 */
#define USER_MIN          0x0000000000001000ULL
#define USER_IMAGE_MAX    0x0000000010000000ULL   /* 256 MB of image space */
#define USER_TRAMP_VA     0x0000000020000000ULL   /* syscall trampolines   */
#define USER_CANVAS_VA    0x0000000030000000ULL   /* the window's pixels   */
#define USER_HEAP_BASE    0x0000000040000000ULL   /* brk starts here       */
#define USER_HEAP_MAX     0x0000100000000000ULL
#define USER_STACK_TOP    0x00007FFFFFFFF000ULL
#define USER_STACK_SIZE   (256 * 1024)
#define USER_SPACE_END          0x0000800000000000ULL

/*
 * ---- kernel virtual space for guarded stacks ----
 *
 * Kernel stacks came from the heap, which lives in the direct map, and
 * the direct map has every page of RAM mapped by construction. There is
 * no way to put a hole in it, so a kernel thread that overran its stack
 * quietly wrote into whatever the allocator had handed out below it —
 * another thread's stack, a page table, anything — and the damage
 * surfaced somewhere else entirely.
 *
 * Stacks now live in a region of their own with an unmapped page below
 * each. An overflow is a page fault at a known address instead, which
 * the handler recognises and names.
 *
 * Index 320 of the top-level table: clear of the direct map at 256 and
 * of the kernel image at 511, and inside the range every address space
 * shares by reference.
 */
#define KSTACK_VA_BASE    0xFFFFA00000000000ULL
#define KSTACK_VA_SPAN    (256ULL * 1024 * 1024)

/* What a staged image's page is allowed to be used for. Shared by every
 * loader — ELF, .vx and PE all describe the same three things. */
#define APROT_READ  1
#define APROT_WRITE 2
#define APROT_EXEC  4

typedef struct {
    uint64_t  pml4_phys;
    uint64_t *pml4;          /* through the HHDM */
    uint64_t  brk;           /* next unallocated heap byte */
    uint64_t  brk_top;       /* how far the heap has actually been mapped */
    uint64_t  canvas_va;     /* where the window's pixels landed this run */
    uint64_t  tramp_va;      /* and the trampoline page - both randomised */
    int       live;
} addr_space_t;

static uint64_t *vmm_kernel_pml4      = 0;
static uint64_t  vmm_kernel_pml4_phys = 0;
static int       vmm_ready            = 0;

/*
 * Whose address space CR3 currently holds, or null for the kernel's own.
 * Every syscall that validates a user pointer needs to know which set of
 * page tables the pointer is supposed to make sense in, and this is it.
 */
static addr_space_t *vmm_current = 0;

/* The kernel's own half, described as an address space so the same
 * mapping code can serve it. */
static addr_space_t vmm_kernel_as;

/*
 * Anchor the higher half so it can be shared by reference.
 *
 * Called once, before any address space exists and after the last
 * mapping the *bootloader* made — but deliberately not after the last
 * mapping the kernel makes, which is the whole point.
 */
static void vmm_init(void) {
    vmm_kernel_pml4_phys = read_cr3() & PTE_ADDR_MASK;
    vmm_kernel_pml4 = (uint64_t *)(uintptr_t)phys_to_virt(vmm_kernel_pml4_phys);

    int added = 0;
    for (int i = 256; i < 512; i++) {
        if (vmm_kernel_pml4[i] & PTE_PRESENT) continue;
        uint64_t phys = pmm_alloc();
        if (!phys) break;
        /* No PTE_USER: the higher half is the kernel's, and a process
         * that can read it can read every other process. */
        vmm_kernel_pml4[i] = phys | PTE_PRESENT | PTE_WRITE;
        added++;
    }

    vmm_kernel_as.pml4      = vmm_kernel_pml4;
    vmm_kernel_as.pml4_phys = vmm_kernel_pml4_phys;
    vmm_kernel_as.live      = 1;

    vmm_ready = 1;
    pmm_ref_init();
    serial_puts("[vmm] kernel half anchored, ");
    serial_put_dec((uint32_t)added);
    serial_puts(" empty directories added\n");
}

/* Walk to the page table entry for `virt`, creating tables on the way if
 * asked to. Intermediate entries get USER and WRITE unconditionally;
 * long mode takes the most restrictive of the whole path, so the leaf is
 * what actually decides, and a shared intermediate must not veto a
 * mapping some sibling wants. */
static uint64_t *vmm_walk(addr_space_t *as, uint64_t virt, int create) {
    uint64_t *table = as->pml4;
    int user = virt < USER_SPACE_END;

    for (int level = 39; level > 12; level -= 9) {
        uint64_t idx = (virt >> level) & 0x1FF;
        if (!(table[idx] & PTE_PRESENT)) {
            if (!create) return 0;
            uint64_t phys = pmm_alloc();
            if (!phys) return 0;
            table[idx] = phys | PTE_PRESENT | PTE_WRITE |
                         (user ? PTE_USER : 0ULL);
        } else if (user && !(table[idx] & PTE_USER)) {
            table[idx] |= PTE_USER;
        }
        if (table[idx] & PTE_HUGE) return 0;   /* not ours to subdivide */
        table = (uint64_t *)(uintptr_t)phys_to_virt(table[idx] & PTE_ADDR_MASK);
    }
    return &table[(virt >> 12) & 0x1FF];
}

/*
 * The pager, defined in swap.h and used from four places in this file.
 *
 * Declared rather than included because swap.h needs a block device and
 * a mounted volume, and this file is compiled long before either
 * exists. Every one of these is inert until swap_init() runs.
 *
 *   rmap_record   remembers which mapping owns a frame, which is the one
 *                 thing the frame bitmap cannot say and the one thing an
 *                 evictor has to know.
 *   in_page       fetches a page back; 1 if it did.
 *   is_swapped    reads a non-present entry's marker bit.
 *   discard_pte   releases a slot without reading it back, for teardown.
 */
static void swap_rmap_record(uint64_t phys, addr_space_t *as, uint64_t virt);
static int  swap_in_page(addr_space_t *as, uint64_t va);
static inline int swap_pte_is_swapped(uint64_t e);
static void swap_discard_pte(uint64_t e);

static int vmm_map(addr_space_t *as, uint64_t virt, uint64_t phys,
                   uint64_t flags) {
    uint64_t *pte = vmm_walk(as, virt, 1);
    if (!pte) return -1;
    *pte = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;

    /*
     * Only a user page is ever a candidate, so only a user page earns an
     * entry. Filtering here rather than in the evictor means a frame
     * that can never be chosen never occupies a slot in the table, and
     * -- more usefully -- that a kernel or shared mapping laid over a
     * frame that used to be a user page erases the old entry instead of
     * leaving it to be caught later by revalidation.
     */
    if ((flags & PTE_USER) && !(flags & PTE_SHARED) && virt < USER_SPACE_END)
        swap_rmap_record(phys, as, virt);
    else
        swap_rmap_record(phys, 0, 0);
    /* Kernel-half entries live in tables every address space shares, so
     * the mapping is visible immediately whatever CR3 holds -- but this
     * processor's TLB may be caching the absence of it. */
    if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys || virt >= USER_SPACE_END)
        flush_tlb_page(virt);
    return 0;
}

/*
 * Physical address for a virtual one, or zero.
 *
 * The swap case is not an optimisation, it is a correctness fix. When
 * the *kernel* reaches into a process's memory -- as_write() staging an
 * image, a syscall copying a buffer out -- it does not go through the
 * MMU and so it does not fault. It calls this. Without the branch below
 * a swapped page simply reads as "not mapped", and the write fails or
 * the read returns zeros, for a page that is perfectly intact and one
 * disk read away. The program never gets a chance to fault it back in,
 * because nothing ever executed an instruction against it.
 */
static uint64_t vmm_resolve(addr_space_t *as, uint64_t virt) {
    uint64_t *pte = vmm_walk(as, virt, 0);
    if (!pte) return 0;
    if (!(*pte & PTE_PRESENT)) {
        if (!swap_pte_is_swapped(*pte)) return 0;
        if (!swap_in_page(as, virt)) return 0;
        pte = vmm_walk(as, virt, 0);
        if (!pte || !(*pte & PTE_PRESENT)) return 0;
    }
    return (*pte & PTE_ADDR_MASK) | (virt & PAGE_MASK);
}

/* Fresh anonymous memory: allocate a frame per page and map it. Returns
 * the number of bytes actually mapped, which is less than asked for only
 * when physical memory ran out. */
static uint64_t vmm_alloc_range(addr_space_t *as, uint64_t virt,
                                uint64_t bytes, uint64_t flags) {
    uint64_t base = PAGE_ALIGN_DOWN(virt);
    uint64_t end  = PAGE_ALIGN_UP(virt + bytes);
    uint64_t done = 0;
    for (uint64_t v = base; v < end; v += PAGE_SIZE) {
        uint64_t phys = pmm_alloc();
        if (!phys) break;
        if (vmm_map(as, v, phys, flags) != 0) { pmm_free(phys); break; }
        done += PAGE_SIZE;
    }
    return done;
}

/* Map memory the kernel owns into a process, without giving up ownership
 * of the frames. Used for the app canvas and the trampoline page. */
static int vmm_map_shared(addr_space_t *as, uint64_t virt,
                          const void *kernel_addr, uint64_t bytes,
                          uint64_t flags) {
    uint64_t src = PAGE_ALIGN_DOWN((uint64_t)(uintptr_t)kernel_addr);
    uint64_t end = PAGE_ALIGN_UP((uint64_t)(uintptr_t)kernel_addr + bytes);
    uint64_t v = PAGE_ALIGN_DOWN(virt);
    for (; src < end; src += PAGE_SIZE, v += PAGE_SIZE) {
        uint64_t phys = kern_virt_to_phys((void *)(uintptr_t)src);
        if (!phys) return -1;
        if (vmm_map(as, v, phys, flags | PTE_SHARED) != 0) return -1;
    }
    return 0;
}

/*
 * A new address space.
 *
 * The higher half is copied entry for entry from the kernel's PML4.
 * Copying 256 eight-byte words is not a copy of the kernel — each of
 * those words is the physical address of a table the kernel still owns,
 * so what the process actually receives is a reference.
 */
static int vmm_create(addr_space_t *as) {
    if (!vmm_ready) return -1;
    uint64_t phys = 0;
    uint64_t *pml4 = (uint64_t *)pmm_alloc_page_virt(&phys);
    if (!pml4) return -1;

    for (int i = 0; i < 256; i++) pml4[i] = 0;
    for (int i = 256; i < 512; i++) pml4[i] = vmm_kernel_pml4[i];

    as->pml4      = pml4;
    as->pml4_phys = phys;
    as->brk       = USER_HEAP_BASE;
    as->brk_top   = USER_HEAP_BASE;
    as->live      = 1;
    return 0;
}

/*
 * Give it all back.
 *
 * Only the lower half is walked, which is the difference between
 * releasing a process and unmapping the kernel out from under every
 * other one. Frames flagged PTE_SHARED are unmapped but not freed; they
 * were never this process's to release.
 */
static void vmm_destroy(addr_space_t *as) {
    if (!as->live) return;
    for (int i = 0; i < 256; i++) {
        if (!(as->pml4[i] & PTE_PRESENT)) continue;
        uint64_t pdpt_phys = as->pml4[i] & PTE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)phys_to_virt(pdpt_phys);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) continue;
            uint64_t pd_phys = pdpt[j] & PTE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)(uintptr_t)phys_to_virt(pd_phys);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) continue;
                uint64_t pt_phys = pd[k] & PTE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)(uintptr_t)phys_to_virt(pt_phys);
                for (int l = 0; l < 512; l++) {
                    /* A page this process had swapped out owns a slot in
                     * the pagefile and no frame at all. Walking only
                     * present entries would free the frame it does not
                     * have and leak the slot it does -- invisible until
                     * the pagefile fills and nothing can be evicted. */
                    if (!(pt[l] & PTE_PRESENT)) {
                        swap_discard_pte(pt[l]);
                        continue;
                    }
                    if (pt[l] & PTE_SHARED) continue;
                    pmm_free(pt[l] & PTE_ADDR_MASK);
                }
                pmm_free(pt_phys);
            }
            pmm_free(pd_phys);
        }
        pmm_free(pdpt_phys);
        as->pml4[i] = 0;
    }
    pmm_free(as->pml4_phys);
    as->pml4 = 0;
    as->pml4_phys = 0;
    as->live = 0;
}

static inline void vmm_switch(addr_space_t *as) {
    uint64_t want = as && as->live ? as->pml4_phys : vmm_kernel_pml4_phys;
    if ((read_cr3() & PTE_ADDR_MASK) != want)
        __asm__ volatile("mov %0, %%cr3" :: "r"(want) : "memory");
}

static inline void vmm_switch_kernel(void) {
    if ((read_cr3() & PTE_ADDR_MASK) != vmm_kernel_pml4_phys)
        __asm__ volatile("mov %0, %%cr3"
                         :: "r"(vmm_kernel_pml4_phys) : "memory");
}

/*
 * Is this pointer one the calling process is allowed to have handed us?
 *
 * Every syscall that takes an address asks this before touching it. The
 * check is deliberately about the *address*, not about whether it
 * happens to be mapped: a user pointer into the higher half is a request
 * for the kernel to read or write kernel memory on the caller's behalf,
 * and no amount of it being mapped makes that acceptable.
 */
static inline int user_range_ok(uint64_t addr, uint64_t len) {
    if (len == 0) return 1;
    if (addr < USER_MIN) return 0;
    if (addr >= USER_SPACE_END) return 0;
    if (addr + len < addr) return 0;          /* wrapped */
    if (addr + len > USER_SPACE_END) return 0;
    return 1;
}

/* Every page of the range present in this address space? A syscall that
 * walks a user buffer without asking turns a program's bad pointer into
 * a kernel page fault. */
static int user_range_mapped(addr_space_t *as, uint64_t addr, uint64_t len,
                             int need_write) {
    if (!user_range_ok(addr, len)) return 0;
    if (!as || !as->live) return 0;
    uint64_t v = PAGE_ALIGN_DOWN(addr);
    uint64_t end = PAGE_ALIGN_UP(addr + len);
    for (; v < end; v += PAGE_SIZE) {
        uint64_t *pte = vmm_walk(as, v, 0);
        if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_USER)) return 0;
        if (need_write && !(*pte & PTE_WRITE)) return 0;
    }
    return 1;
}

/* A user string, copied in and bounded. Returns the length, or -1 if the
 * string is unterminated inside the limit or leaves mapped memory. */
static int user_strncpy_in(addr_space_t *as, char *dst, uint64_t uptr,
                           int max) {
    if (max <= 0) return -1;
    for (int i = 0; i < max - 1; i++) {
        uint64_t a = uptr + (uint64_t)i;
        if (!user_range_mapped(as, a, 1, 0)) { dst[i] = '\0'; return -1; }
        char c = *(const char *)(uintptr_t)a;
        dst[i] = c;
        if (!c) return i;
    }
    dst[max - 1] = '\0';
    return max - 1;
}

/* ===== GUARDED KERNEL STACKS ===== */

static uint64_t   kstack_va_next = KSTACK_VA_BASE;
static spinlock_t kstack_lock = { 0, "kstack" };
static uint64_t   kstack_live = 0;

/*
 * A kernel stack with nothing mapped under it.
 *
 * The virtual range is bump-allocated and never reused, which sounds
 * wasteful and is not: a quarter of a gigabyte of address space is
 * eight thousand stacks, and address space is the one resource this
 * machine has in genuine abundance. Not reusing it means a stale pointer
 * into a freed stack faults instead of landing in somebody else's.
 */
static void *kstack_alloc(uint64_t bytes) {
    if (!vmm_ready) return 0;
    uint64_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;

    uint64_t flags = spin_lock_irq(&kstack_lock);
    /* One unmapped page below and one above; the gap is what turns an
     * overflow into a fault at a known address. */
    uint64_t base = kstack_va_next + PAGE_SIZE;
    kstack_va_next = base + (pages + 1) * PAGE_SIZE;
    int out_of_space = kstack_va_next >= KSTACK_VA_BASE + KSTACK_VA_SPAN;
    spin_unlock_irq(&kstack_lock, flags);
    if (out_of_space) return 0;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc();
        if (!phys) return 0;
        if (vmm_map(&vmm_kernel_as, base + i * PAGE_SIZE, phys,
                    PTE_WRITE | PTE_NX) != 0) {
            pmm_free(phys);
            return 0;
        }
    }
    /* The guard below, recorded so a fault there can be named. It is a
     * present-bit-clear entry carrying a software bit, which the
     * hardware ignores and the handler reads. */
    uint64_t *g = vmm_walk(&vmm_kernel_as, base - PAGE_SIZE, 1);
    if (g) *g = PTE_GUARD;

    kstack_live += pages;
    return (void *)(uintptr_t)base;
}

static void kstack_free(void *p, uint64_t bytes) {
    if (!p) return;
    uint64_t base = (uint64_t)(uintptr_t)p;
    uint64_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = base + i * PAGE_SIZE;
        uint64_t *pte = vmm_walk(&vmm_kernel_as, va, 0);
        if (pte && (*pte & PTE_PRESENT)) {
            pmm_free(*pte & PTE_ADDR_MASK);
            *pte = 0;
            flush_tlb_page(va);
        }
    }
    kstack_live -= pages;
}

/* Was this fault a stack running off its end? */
static int vmm_is_guard(uint64_t va) {
    if (va < KSTACK_VA_BASE || va >= KSTACK_VA_BASE + KSTACK_VA_SPAN) return 0;
    uint64_t *pte = vmm_walk(&vmm_kernel_as, PAGE_ALIGN_DOWN(va), 0);
    return pte && (*pte & PTE_GUARD) && !(*pte & PTE_PRESENT);
}

/* ===== LARGE PAGES =====
 *
 * One entry covering two megabytes instead of five hundred and twelve
 * covering four kilobytes each. It costs one translation-lookaside
 * entry rather than up to five hundred, which for a region walked
 * linearly -- the framebuffer, the inference arena, the direct map -- is
 * the difference between hitting the TLB and missing it on every page.
 *
 * The cost is granularity: nothing inside a large page can have its own
 * protection, so it is only ever right for a region that is uniform.
 */
static int vmm_map_huge(addr_space_t *as, uint64_t virt, uint64_t phys,
                        uint64_t flags, int gigabyte) {
    int user = virt < USER_SPACE_END;
    uint64_t *table = as->pml4;
    int stop = gigabyte ? 30 : 21;

    for (int level = 39; level > stop; level -= 9) {
        uint64_t idx = (virt >> level) & 0x1FF;
        if (!(table[idx] & PTE_PRESENT)) {
            uint64_t p = pmm_alloc();
            if (!p) return -1;
            table[idx] = p | PTE_PRESENT | PTE_WRITE | (user ? PTE_USER : 0ULL);
        }
        if (table[idx] & PTE_HUGE) return -1;
        table = (uint64_t *)(uintptr_t)phys_to_virt(table[idx] & PTE_ADDR_MASK);
    }
    uint64_t idx = (virt >> stop) & 0x1FF;
    table[idx] = (phys & ~((1ULL << stop) - 1)) | flags | PTE_PRESENT | PTE_HUGE;
    if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys || !user)
        flush_tlb_page(virt);
    return 0;
}

/* Map a whole region with the largest pages that fit it. */
static uint64_t vmm_map_region(addr_space_t *as, uint64_t virt, uint64_t phys,
                               uint64_t bytes, uint64_t flags) {
    uint64_t done = 0;
    while (done < bytes) {
        uint64_t left = bytes - done;
        uint64_t v = virt + done, p = phys + done;
        if ((v & 0x3FFFFFFF) == 0 && (p & 0x3FFFFFFF) == 0 &&
            left >= (1ULL << 30)) {
            if (vmm_map_huge(as, v, p, flags, 1) != 0) break;
            done += 1ULL << 30;
        } else if ((v & 0x1FFFFF) == 0 && (p & 0x1FFFFF) == 0 &&
                   left >= (1ULL << 21)) {
            if (vmm_map_huge(as, v, p, flags, 0) != 0) break;
            done += 1ULL << 21;
        } else {
            if (vmm_map(as, v, p, flags) != 0) break;
            done += PAGE_SIZE;
        }
    }
    return done;
}

/* ===== ADDRESS SPACE RANDOMISATION =====
 *
 * An exploit that overflows a buffer has to know where to jump. Every
 * process here used to be laid out identically -- stack top, heap base
 * and mapped pages at the same address in every one of them -- so an
 * address discovered once was correct forever.
 *
 * What can move, moves. The stack and the heap get a random offset in
 * their own region, and so do the trampoline and canvas mappings. What
 * cannot move yet is the image itself: neither loader processes
 * relocations, so an image has to land at the address it was linked for.
 * That is the honest limit of this, and it is where the PE loader's
 * relocation support will eventually take it further.
 */
static uint64_t aslr_state = 0;

static void aslr_seed(void) {
    aslr_state = cycle_now() * 6364136223846793005ULL + 1442695040888963407ULL;
}

static uint64_t aslr_next(void) {
    aslr_state = aslr_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return aslr_state >> 17;
}

/* A page-aligned displacement of at most `pages` pages. */
static uint64_t aslr_offset(uint32_t pages) {
    if (!pages) return 0;
    return (aslr_next() % pages) * PAGE_SIZE;
}

/* ===== COPY ON WRITE ===== */

/*
 * Duplicate an address space without duplicating its memory.
 *
 * Every private, writable page in the parent is made read-only in both
 * copies and marked COW, and the frame's reference count goes up. The
 * first write from either side faults, and the handler gives that side a
 * private copy. Two processes running the same program therefore share
 * every page neither of them writes -- which for a program's text is all
 * of it.
 *
 * Read-only pages are shared outright with no COW bit: nothing will ever
 * write them, so there is nothing to resolve. Pages already flagged
 * SHARED belong to the kernel and are copied as they stand.
 */
static int vmm_fork(addr_space_t *dst, addr_space_t *src) {
    if (vmm_create(dst) != 0) return -1;
    dst->brk     = src->brk;
    dst->brk_top = src->brk_top;

    for (int i = 0; i < 256; i++) {
        if (!(src->pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)
            phys_to_virt(src->pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) continue;
            uint64_t *pd = (uint64_t *)(uintptr_t)
                phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) continue;
                uint64_t *pt = (uint64_t *)(uintptr_t)
                    phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    uint64_t e = pt[l];

                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    /*
                     * A swapped page has to come back before it can be
                     * shared, and there is no third option.
                     *
                     * Copying the entry across as it stands would give
                     * parent and child one pagefile slot between them:
                     * whichever faulted first would read it, free it,
                     * and leave the other pointing at a slot that has
                     * since been handed to somebody else. Skipping it
                     * would be quieter and no better -- the child would
                     * simply not have the page, and would discover that
                     * by faulting on an address its parent can read.
                     *
                     * So it is read in here, and then shared by exactly
                     * the same rules as any other present page. The
                     * clock is free to send it straight back out again.
                     */
                    if (!(e & PTE_PRESENT)) {
                        if (!swap_pte_is_swapped(e)) continue;
                        if (!swap_in_page(src, va)) return -1;
                        e = pt[l];
                        if (!(e & PTE_PRESENT)) continue;
                    }

                    uint64_t phys = e & PTE_ADDR_MASK;

                    if (!(e & PTE_SHARED) && (e & PTE_WRITE)) {
                        e &= ~PTE_WRITE;
                        e |= PTE_COW;
                        pt[l] = e;                 /* the parent too */
                        flush_tlb_page(va);
                    }
                    if (!(e & PTE_SHARED)) pmm_ref_inc(phys);

                    uint64_t *d = vmm_walk(dst, va, 1);
                    if (!d) return -1;
                    *d = e;
                }
            }
        }
    }
    return 0;
}

/*
 * Resolve a write to a copy-on-write page. Returns 1 if it was one and
 * the fault is now repaired, 0 if the fault was something else and the
 * caller should carry on treating it as an error.
 *
 * The last reference does not copy. If nobody else holds the frame there
 * is nothing to protect it from, so the mapping simply becomes writable
 * again — which is what makes a program that forks and then writes
 * everything cost the same as one that never forked.
 */
static int vmm_resolve_cow(addr_space_t *as, uint64_t va) {
    if (!as || !as->live) return 0;
    uint64_t *pte = vmm_walk(as, PAGE_ALIGN_DOWN(va), 0);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW)) return 0;

    uint64_t old = *pte & PTE_ADDR_MASK;
    if (pmm_ref_count(old) <= 1) {
        *pte = (*pte & ~PTE_COW) | PTE_WRITE;
        flush_tlb_page(PAGE_ALIGN_DOWN(va));
        return 1;
    }

    uint64_t fresh = pmm_alloc();
    if (!fresh) return 0;
    const uint8_t *src = (const uint8_t *)(uintptr_t)phys_to_virt(old);
    uint8_t *dst = (uint8_t *)(uintptr_t)phys_to_virt(fresh);
    for (int i = 0; i < 4096; i++) dst[i] = src[i];

    *pte = fresh | (*pte & ~(PTE_ADDR_MASK | PTE_COW)) | PTE_WRITE;
    flush_tlb_page(PAGE_ALIGN_DOWN(va));
    pmm_free(old);                      /* drops one reference */
    return 1;
}

/* ===== AGE AND RECLAIM =====
 *
 * The processor sets the accessed bit on a page it translates and never
 * clears it. Sweeping the bits and clearing them turns that into an
 * approximation of least-recently-used: a page whose bit is set was
 * touched since the last sweep, and one whose bit has been clear for
 * several sweeps has not been touched for a while.
 *
 * That is what a reclaimer needs to know. It is deliberately an
 * approximation — an exact LRU needs a data structure updated on every
 * memory reference, which is to say hardware nobody builds.
 */
typedef struct {
    uint64_t scanned;
    uint64_t active;      /* touched since the last sweep      */
    uint64_t idle;        /* not touched for several sweeps    */
    uint64_t dirty;       /* written, so worth writing back    */
} vmm_age_t;

static vmm_age_t vmm_age_last;

static void vmm_age_pass(addr_space_t *as) {
    if (!as || !as->live) return;
    vmm_age_t a = { 0, 0, 0, 0 };

    for (int i = 0; i < 256; i++) {
        if (!(as->pml4[i] & PTE_PRESENT)) continue;
        uint64_t *pdpt = (uint64_t *)(uintptr_t)
            phys_to_virt(as->pml4[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & PTE_PRESENT) || (pdpt[j] & PTE_HUGE)) continue;
            uint64_t *pd = (uint64_t *)(uintptr_t)
                phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & PTE_PRESENT) || (pd[k] & PTE_HUGE)) continue;
                uint64_t *pt = (uint64_t *)(uintptr_t)
                    phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    uint64_t e = pt[l];
                    if (!(e & PTE_PRESENT)) continue;
                    a.scanned++;
                    if (e & PTE_DIRTY) a.dirty++;
                    if (e & PTE_ACCESSED) {
                        a.active++;
                        pt[l] = e & ~PTE_ACCESSED;
                        uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                      ((uint64_t)k << 21) | ((uint64_t)l << 12);
                        flush_tlb_page(va);
                    } else {
                        a.idle++;
                    }
                }
            }
        }
    }
    vmm_age_last = a;
}

#endif /* VMM_H */
