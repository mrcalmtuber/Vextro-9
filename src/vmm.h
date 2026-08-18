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
 * Bit 9 is one of three the architecture leaves to software. It marks a
 * mapping whose frame belongs to somebody else — the app canvas, the
 * syscall trampoline — so that tearing an address space down unmaps it
 * without handing the frame back to the allocator. Freeing a page the
 * kernel is still using is silent corruption; this is the one bit that
 * prevents it.
 */
#define PTE_SHARED    (1ULL << 9)

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

typedef struct {
    uint64_t  pml4_phys;
    uint64_t *pml4;          /* through the HHDM */
    uint64_t  brk;           /* next unallocated heap byte */
    uint64_t  brk_top;       /* how far the heap has actually been mapped */
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

    vmm_ready = 1;
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

static int vmm_map(addr_space_t *as, uint64_t virt, uint64_t phys,
                   uint64_t flags) {
    uint64_t *pte = vmm_walk(as, virt, 1);
    if (!pte) return -1;
    *pte = (phys & PTE_ADDR_MASK) | flags | PTE_PRESENT;
    if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys) flush_tlb_page(virt);
    return 0;
}

static uint64_t vmm_resolve(addr_space_t *as, uint64_t virt) {
    uint64_t *pte = vmm_walk(as, virt, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;
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
                    if (!(pt[l] & PTE_PRESENT)) continue;
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

#endif /* VMM_H */
