#ifndef SWAP_H
#define SWAP_H

/*
 * src/swap.h — physical memory that outlives the physical memory.
 *
 * Until now every page this kernel handed out was backed by RAM for as
 * long as it was mapped, and pmm_alloc() returning zero was the end of
 * the conversation: vmm_alloc_range() stopped early, the loader failed,
 * the program did not start. That is an honest answer to "there is no
 * more memory" and it is the wrong one, because there was somewhere else
 * to put the page all along.
 *
 * So: a 256 MB file on the system volume, a reverse map from frame back
 * to the mapping that owns it, a Clock hand to choose what has not been
 * used lately, and a page-fault handler that treats a not-present fault
 * on a page it recognises as an instruction to go and fetch it.
 *
 * ---- what "suspend the thread" means here ----
 *
 * It means the thread is not running, which is already true: it took an
 * exception, and what is executing is the handler. The read happens
 * synchronously inside that handler and the IRETQ at the end of it
 * returns to the instruction that faulted, which re-executes against a
 * page that is now there.
 *
 * There is no asynchronous alternative available to write. Every block
 * driver underneath blk.h -- NVMe, AHCI, ATA, USB mass storage -- polls
 * its device to completion; not one of them raises a completion
 * interrupt, calls sched_sleep_ms() or looks at sched_ticks. There is
 * nothing to be woken *by*. A design that marked the thread T_SLEEPING
 * and returned to the scheduler would need an I/O completion path that
 * does not exist, and building one would mean rewriting four drivers.
 *
 * The cost is real and worth naming: interrupts are off for the duration
 * of the read, because the #PF gate is an interrupt gate. A page-in is
 * therefore a few milliseconds during which nothing else runs. On a
 * machine that is swapping, that is not the slowest thing happening.
 *
 * ---- what may be swapped, and what may never be ----
 *
 * Only a user page: present, PTE_USER, below USER_SPACE_END, not
 * PTE_SHARED, not part of a copy-on-write pair, in an address space that
 * is still live. Everything else is refused, and the refusals are not
 * conservatism -- each one is a way to corrupt the machine:
 *
 *   kernel pages     the handler itself lives there, and so does the
 *                    driver it is about to call.
 *   page tables      they have no rmap entry, so they are never
 *                    candidates; a swapped page table is a fault that
 *                    cannot be resolved, because resolving it requires
 *                    walking it.
 *   PTE_SHARED       the frame belongs to the kernel -- the app canvas,
 *                    the trampoline page -- and the kernel writes to it
 *                    through the direct map without faulting. Evicting
 *                    one would not fault; it would silently lose writes.
 *   refcount > 1     two address spaces point at the frame and this only
 *                    updates one of their entries.
 *
 * That list is also why the block layer can be called from the fault
 * handler at all. A driver reads into kernel memory, kernel memory is
 * never swapped, so a driver can never itself take the fault that would
 * re-enter it.
 *
 * ---- and what this is not ----
 *
 * It is not a working set policy. Clock approximates least-recently-used
 * over the accessed bit and that is the whole of the algorithm: no
 * ageing across scans, no per-process fairness, no readahead, no
 * swappiness. A page is chosen because nothing touched it since the hand
 * last passed, which is the property the name promises and the only one
 * claimed here.
 */

/* ---- the area ---- */

#define SWAP_MB           256u
#define SWAP_BYTES        ((uint64_t)SWAP_MB * 1024 * 1024)
#define SWAP_SLOTS        ((uint32_t)(SWAP_BYTES / PAGE_SIZE))   /* 65536 */
#define SWAP_SECTORS_PER  (PAGE_SIZE / EXF_SECTOR)               /* 8     */
#define SWAP_PATH         "/pagefile.sys"
#define SWAP_LEAF         "pagefile.sys"

/*
 * A swapped page's entry.
 *
 * The processor reads exactly one bit of a non-present entry -- bit 0,
 * to discover that it is non-present -- and ignores the other sixty
 * three completely. They are the natural place to record where the page
 * went, and doing so means a swapped page needs no side table at all:
 * the page table *is* the index, and it is already walked by address.
 *
 * Bit 10 is the marker, and it is the same bit PTE_COW uses. That is
 * deliberate rather than a collision. Every test of PTE_COW in vmm.h is
 * guarded by PTE_PRESENT first (vmm_resolve_cow checks it at vmm.h:605,
 * vmm_fork only ever sets it on a present entry), so the pair
 * (present, bit 10) has two meanings that the processor itself keeps
 * apart: present means copy-on-write, not-present means swapped. Bit 11
 * was the alternative and is worse -- PTE_GUARD is *also* tested on
 * non-present entries, and the two would genuinely alias.
 *
 * The permission bits are carried across untouched in their own
 * positions, so that a page comes back exactly as writable, as
 * user-accessible and as executable as it went out.
 */
#define PTE_SWAPPED       (1ULL << 10)
_Static_assert(PTE_SWAPPED == PTE_COW,
               "the swap marker is bit 10 on purpose; see the note above");

#define SWAP_SLOT_SHIFT   12
#define SWAP_SLOT_MASK    0xFFFFULL      /* 65536 slots needs sixteen bits */
#define SWAP_PERM_MASK    (PTE_WRITE | PTE_USER | PTE_NX)

static inline int swap_pte_is_swapped(uint64_t e) {
    return !(e & PTE_PRESENT) && (e & PTE_SWAPPED);
}
static inline uint32_t swap_pte_slot(uint64_t e) {
    return (uint32_t)((e >> SWAP_SLOT_SHIFT) & SWAP_SLOT_MASK);
}
static inline uint64_t swap_pte_make(uint32_t slot, uint64_t old) {
    return ((uint64_t)slot << SWAP_SLOT_SHIFT) | PTE_SWAPPED |
           (old & SWAP_PERM_MASK);
}

/* ---- state ---- */

static int      swap_ready      = 0;
static uint64_t swap_base_lba   = 0;     /* absolute device LBA, not volume */
static uint32_t swap_slot_count = 0;
static uint32_t swap_slots_used = 0;
static uint32_t swap_hand       = 0;     /* the Clock hand, a frame index  */
static const char *swap_errstr  = "";

/* Statistics, because a pager that is thrashing and a pager that is idle
 * look identical from outside and the difference is the whole diagnosis. */
static uint64_t swap_outs = 0, swap_ins = 0, swap_fails = 0, swap_scans = 0;

/* One bit per slot: 65536 bits is eight kilobytes, small enough to be
 * static and simple enough to be obviously right. */
static uint64_t swap_slot_bm[SWAP_SLOTS / 64];

/*
 * The reverse map: frame -> the mapping that owns it.
 *
 * The frame bitmap in pmm.h answers "is this frame in use", which is all
 * an allocator needs and none of what an evictor does. To take a page
 * away from a process, its page table entry has to be found and cleared,
 * and a physical address on its own does not say which entry that is --
 * or even which address space. So it is recorded when the mapping is
 * made.
 *
 * Sixteen bytes per frame, from the frame allocator itself once it is
 * up: eight megabytes on a two-gigabyte machine, four times what the
 * reference table costs and the same order of magnitude.
 *
 * This table is a *hint* and is never trusted. See swap_victim_ok().
 */
typedef struct {
    addr_space_t *as;
    uint64_t      virt;
} swap_rmap_t;

static swap_rmap_t *swap_rmap = 0;

/*
 * Re-entrancy.
 *
 * Reclaim runs inside pmm_alloc(), and swapping a page out ends in a
 * disk write that allocates nothing -- but a future caller may not be so
 * careful, and the failure mode of getting this wrong is a stack that
 * recurses until it hits the guard page. So a second entry does not
 * recurse: it fails, and pmm_alloc() returns zero exactly as it did
 * before any of this existed.
 */
static int swap_busy = 0;

/* ===== 1. THE AREA ON DISK =====
 *
 * A contiguous file rather than a range of raw sectors, and that is
 * forced rather than chosen: tools/mkexfat.py builds a partition-less
 * "superfloppy" -- the volume boot record is at sector 0 and exFAT owns
 * every sector of the device. There is no unpartitioned space to take.
 * Writing raw sectors at a fixed offset would land inside the volume and
 * quietly overwrite whatever the allocator had put there, which on this
 * image is a 937 MB encyclopedia and two language models.
 *
 * So the file is allocated through the filesystem, once, and then used
 * as a raw extent: its cluster run is resolved to one absolute device
 * LBA at startup and every transfer afterwards is blk_*_raw() against
 * that range. Nothing in the swap path touches the filesystem layer
 * again -- which matters, because exf_secbuf and exf_dirbuf are single
 * shared static sectors and a page fault taken during a directory read
 * would corrupt whichever one was in flight.
 *
 * NoFatChain is what makes this legitimate rather than a trick. exFAT
 * has a flag meaning "this file's clusters are consecutive, do not
 * consult the FAT", so a contiguous file is a documented on-disk state
 * and not an assumption that happens to hold today.
 */

/* Find `count` consecutive free clusters and claim them. */
static uint32_t swap_alloc_contig_clusters(uint32_t count) {
    uint32_t run = 0, start = 0;
    for (uint32_t c = 2; c < exf_vol.cluster_count + 2; c++) {
        int used = 0;
        if (exf_bitmap_test(c, &used) != 0) return 0;
        if (used) { run = 0; continue; }
        if (run == 0) start = c;
        if (++run == count) {
            for (uint32_t k = start; k < start + count; k++)
                if (exf_bitmap_set(k, 1) != 0) return 0;
            return start;
        }
    }
    return 0;
}

/*
 * Adopt the pagefile if it is already there and usable, otherwise build
 * one. Called once, after the volume is mounted.
 *
 * "Usable" means contiguous and at least as large as asked for. A
 * fragmented one is not repaired and not used: the whole design rests on
 * the extent being one run, and a swapper that silently fell back to
 * walking a cluster chain per page would be doing filesystem I/O from
 * inside the page-fault handler, which is the one thing this file exists
 * to avoid.
 */
static int swap_init(void) {
    swap_ready = 0;
    swap_errstr = "";

    if (fs_kind != FS_EXFAT || !exf_vol.mounted) {
        swap_errstr = "no exFAT volume to put a pagefile on";
        return -1;
    }
    if (!pmm_ready) { swap_errstr = "the frame allocator is not up"; return -1; }

    /* The reverse map first: without it nothing can be evicted, and
     * finding that out after the file exists wastes 256 MB. */
    uint64_t bytes  = pmm_highest_frame * sizeof(swap_rmap_t);
    uint64_t frames = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    uint64_t phys   = pmm_alloc_contig(frames);
    if (!phys) {
        swap_errstr = "no room for the reverse map";
        return -1;
    }
    swap_rmap = (swap_rmap_t *)(uintptr_t)phys_to_virt(phys);
    for (uint64_t i = 0; i < pmm_highest_frame; i++) {
        swap_rmap[i].as = 0;
        swap_rmap[i].virt = 0;
    }

    const uint32_t cbytes  = exf_vol.cluster_bytes;
    const uint32_t need_cl = (uint32_t)((SWAP_BYTES + cbytes - 1) / cbytes);
    uint32_t first = 0;

    exf_dirent_t e;
    if (exf_lookup(SWAP_PATH, &e) && !(e.attr & EXF_ATTR_DIR)) {
        if (e.contiguous && e.size >= SWAP_BYTES) {
            first = e.first_clus;
            serial_puts("[swap] reusing the pagefile already on the volume\n");
        } else {
            /* Wrong shape. Take it away and build it again -- it holds
             * nothing that outlives a boot. */
            exf_delete(SWAP_PATH);
        }
    }

    if (!first) {
        if (exf_vol.free_clusters < need_cl) {
            swap_errstr = "not enough free space on the volume";
            return -1;
        }
        first = swap_alloc_contig_clusters(need_cl);
        if (!first) {
            swap_errstr = "no contiguous run large enough; swap stays off";
            return -1;
        }

        /* The directory entry, marked NoFatChain. No FAT entries are
         * written and that is the point of the flag. */
        exf_dirent_t parent;
        char leaf[EXF_NAME_MAX];
        if (!exf_split(SWAP_PATH, &parent, leaf)) {
            swap_errstr = "cannot reach the root directory";
            return -1;
        }
        uint64_t dlba; int didx;
        if (exf_find_slots(&parent, exf_entries_for(leaf), &dlba, &didx) != 0) {
            swap_errstr = "no room in the root directory";
            return -1;
        }
        if (exf_write_set(dlba, didx, leaf,
                          EXF_ATTR_HIDDEN | EXF_ATTR_RO,
                          first, SWAP_BYTES, 1) != 0) {
            swap_errstr = "could not write the directory entry";
            return -1;
        }
        serial_puts("[swap] created a new pagefile\n");
    }

    /*
     * Volume-relative to absolute, once. Everything after this is
     * blk_*_raw() against a device LBA, so this is the only line that
     * has to know the volume starts somewhere.
     */
    swap_base_lba   = exf_vol.part_lba + exf_cluster_lba(first);
    swap_slot_count = SWAP_SLOTS;
    swap_slots_used = 0;
    for (uint32_t i = 0; i < SWAP_SLOTS / 64; i++) swap_slot_bm[i] = 0;

    /*
     * The block cache holds sectors by device LBA and knows nothing
     * about who wrote them. Anything that read part of the pagefile
     * through the filesystem -- `type pagefile.sys`, a scan, the search
     * indexer -- left lines in it that the raw writes below will not
     * update, and a later cached read would hand back a page from
     * before it was swapped. Drop the whole extent now, and after every
     * raw write.
     */
    blk_cache_drop(swap_base_lba, (uint32_t)(SWAP_BYTES / EXF_SECTOR));

    swap_ready = 1;

    serial_puts("[swap] ");
    serial_put_dec(SWAP_MB);
    serial_puts(" MB pagefile, ");
    serial_put_dec(swap_slot_count);
    serial_puts(" slots at LBA ");
    serial_put_dec((uint32_t)swap_base_lba);
    serial_puts(", reverse map ");
    serial_put_dec((uint32_t)(bytes / 1024));
    serial_puts(" KB\n");
    return 0;
}

/* ---- slots ---- */

static uint32_t swap_slot_alloc(void) {
    for (uint32_t w = 0; w < SWAP_SLOTS / 64; w++) {
        if (swap_slot_bm[w] == ~0ULL) continue;
        uint32_t b = (uint32_t)__builtin_ctzll(~swap_slot_bm[w]);
        uint32_t slot = w * 64 + b;
        if (slot >= swap_slot_count) break;
        swap_slot_bm[w] |= 1ULL << b;
        swap_slots_used++;
        return slot + 1;                 /* 0 means "none" to the caller */
    }
    return 0;
}

static void swap_slot_free(uint32_t slot) {
    if (slot >= swap_slot_count) return;
    uint64_t bit = 1ULL << (slot & 63);
    if (swap_slot_bm[slot >> 6] & bit) {
        swap_slot_bm[slot >> 6] &= ~bit;
        swap_slots_used--;
    }
}

/* ---- the transfers ----
 *
 * Straight from the frame through the direct map: the page is already
 * contiguous physical memory and the driver takes a virtual address, so
 * a bounce buffer would be a second copy of four kilobytes to no end.
 */
static int swap_write_slot(uint32_t slot, uint64_t phys) {
    uint64_t lba = swap_base_lba + (uint64_t)slot * SWAP_SECTORS_PER;
    void *src = (void *)(uintptr_t)phys_to_virt(phys);
    if (blk_write_raw(lba, SWAP_SECTORS_PER, src) != 0) return -1;
    blk_cache_drop(lba, SWAP_SECTORS_PER);
    return 0;
}

static int swap_read_slot(uint32_t slot, uint64_t phys) {
    uint64_t lba = swap_base_lba + (uint64_t)slot * SWAP_SECTORS_PER;
    void *dst = (void *)(uintptr_t)phys_to_virt(phys);
    return blk_read_raw(lba, SWAP_SECTORS_PER, dst) == 0 ? 0 : -1;
}

/* ===== 2. THE REVERSE MAP AND THE CLOCK ===== */

/*
 * Record that `phys` is now mapped at `virt` in `as`. Called from
 * vmm_map() for every mapping it makes; the filter for what is
 * swappable is applied there rather than here, so that a frame which can
 * never be a candidate never occupies an entry.
 */
static void swap_rmap_record(uint64_t phys, addr_space_t *as, uint64_t virt) {
    if (!swap_rmap) return;
    uint64_t f = phys >> PAGE_SHIFT;
    if (f >= pmm_highest_frame) return;
    swap_rmap[f].as   = as;
    swap_rmap[f].virt = virt;
}

static void swap_rmap_forget(uint64_t phys) {
    if (!swap_rmap) return;
    uint64_t f = phys >> PAGE_SHIFT;
    if (f >= pmm_highest_frame) return;
    swap_rmap[f].as   = 0;
    swap_rmap[f].virt = 0;
}

/*
 * Is this frame really evictable, and where from?
 *
 * The reverse map is a hint and this is where that is made safe. Every
 * fact it offers is checked against the page tables before anything is
 * written or cleared, because the entry can be stale in ways nothing
 * notices at the time it goes stale:
 *
 *   - the address space exited, and the frame has since been handed to
 *     somebody else entirely;
 *   - a copy-on-write fault moved the mapping to a fresh frame and freed
 *     this one;
 *   - the frame was recycled into a page table, which has no rmap entry
 *     of its own and so never overwrote the old one.
 *
 * In each case the recorded entry no longer points back at this frame,
 * and the mismatch is the signal. Eager invalidation everywhere a frame
 * changes hands would be more code in more places and would still need
 * this check to be correct; this way there is one rule, in one function,
 * and a stale entry costs a skipped candidate rather than a wrong page.
 */
/*
 * A note on walking somebody else's address space, because it looks
 * wrong and is not.
 *
 * Reclaim is reached from pmm_alloc(), and pmm_alloc() is called from
 * everywhere -- including vmm_walk(create=1) part way through building a
 * page table, and including vmm_resolve_cow() inside the fault handler.
 * So the victim chosen below routinely belongs to an address space that
 * is not the one in CR3, and the walk of it happens while some other
 * walk is halfway done. Three things make that safe, and all three are
 * properties of code in vmm.h rather than promises made here:
 *
 *   the walk never uses CR3. vmm_walk() starts at as->pml4, which is a
 *   direct-map pointer, and every level after it is phys_to_virt() of
 *   the entry above. It reads any address space from any address space.
 *
 *   it never allocates. create is 0 on every walk in this file, so a
 *   reclaim cannot re-enter the allocator that called it. swap_busy
 *   guards the case this reasoning misses.
 *
 *   it never frees a page table. The frame released at the end of
 *   swap_out_one() is a leaf, and a leaf that swap_victim_ok() has
 *   already confirmed is a user data page -- page tables have no reverse
 *   map entry, so they are not candidates and cannot become one. An
 *   interrupted vmm_walk's local pointer into a table therefore stays
 *   valid across a reclaim that happens underneath it.
 */
static int swap_victim_ok(uint64_t frame, addr_space_t **out_as,
                          uint64_t *out_va, uint64_t **out_pte) {
    if (!swap_rmap) return 0;
    addr_space_t *as = swap_rmap[frame].as;
    uint64_t va = swap_rmap[frame].virt;
    if (!as || !as->live || !as->pml4) return 0;
    if (va < USER_MIN || va >= USER_SPACE_END) return 0;

    uint64_t *pte = vmm_walk(as, va, 0);
    if (!pte) { swap_rmap[frame].as = 0; return 0; }

    uint64_t e = *pte;
    if (!(e & PTE_PRESENT))            { swap_rmap[frame].as = 0; return 0; }
    if ((e & PTE_ADDR_MASK) >> PAGE_SHIFT != frame) {
        swap_rmap[frame].as = 0;       /* it moved; this entry is a ghost */
        return 0;
    }
    if (!(e & PTE_USER))  return 0;    /* kernel side: never */
    if (e & PTE_SHARED)   return 0;    /* the kernel writes it directly */
    if (e & PTE_COW)      return 0;    /* present + bit 10: shared, not swapped */
    if (pmm_ref_count(e & PTE_ADDR_MASK) > 1) return 0;

    *out_as = as; *out_va = va; *out_pte = pte;
    return 1;
}

/*
 * Clock.
 *
 * The hand sweeps physical frames. A candidate whose accessed bit is set
 * has been touched since the hand last came round, so the bit is cleared
 * and it gets another turn; one whose bit is already clear has not, and
 * is the victim. That is the entire algorithm, and its cost is what
 * makes it the one worth having: no list to keep ordered on every
 * access, one bit the processor sets for free.
 *
 * Two sweeps at most. The first clears accessed bits; anything the first
 * pass demoted is available to the second, so a scan can only fail if
 * genuinely nothing is evictable.
 */
static uint64_t swap_find_victim(addr_space_t **out_as, uint64_t *out_va,
                                 uint64_t **out_pte) {
    if (!swap_rmap || pmm_highest_frame == 0) return 0;

    for (int sweep = 0; sweep < 2; sweep++) {
        for (uint64_t n = 0; n < pmm_highest_frame; n++) {
            uint64_t f = swap_hand;
            swap_hand++;
            if (swap_hand >= pmm_highest_frame) swap_hand = 0;
            swap_scans++;

            addr_space_t *as; uint64_t va; uint64_t *pte;
            if (!swap_victim_ok(f, &as, &va, &pte)) continue;

            if (*pte & PTE_ACCESSED) {
                *pte &= ~PTE_ACCESSED;
                /* Only the current address space needs the shootdown.
                 * Another one's stale accessed bit costs this page one
                 * extra turn round the clock and nothing else, and the
                 * CR3 write on the next switch flushes it anyway. */
                if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys)
                    flush_tlb_page(va);
                continue;
            }

            *out_as = as; *out_va = va; *out_pte = pte;
            return f;
        }
    }
    return 0;
}

/* ===== 3. OUT AND BACK ===== */

/*
 * Evict one page and return its frame to the allocator.
 *
 * Returns 1 if a frame was freed. The order inside is the part that
 * matters: the data reaches the disk *before* the entry stops pointing
 * at it, so a failed write leaves the page mapped and untouched rather
 * than leaving a process with an entry pointing at a slot that was never
 * filled.
 */
static int swap_out_one(void) {
    addr_space_t *as = 0; uint64_t va = 0; uint64_t *pte = 0;

    uint64_t frame = swap_find_victim(&as, &va, &pte);
    if (!frame) { swap_errstr = "nothing evictable"; return 0; }

    uint32_t slot1 = swap_slot_alloc();
    if (!slot1) { swap_errstr = "the pagefile is full"; swap_fails++; return 0; }
    uint32_t slot = slot1 - 1;

    uint64_t phys = *pte & PTE_ADDR_MASK;
    if (swap_write_slot(slot, phys) != 0) {
        swap_slot_free(slot);
        swap_errstr = "write to the pagefile failed";
        swap_fails++;
        return 0;
    }

    *pte = swap_pte_make(slot, *pte);
    if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys)
        flush_tlb_page(va);

    swap_rmap_forget(phys);
    pmm_free(phys);
    swap_outs++;
    return 1;
}

/*
 * Reclaim, for pmm_alloc() to call when it has nothing left.
 *
 * No lock is held across the disk write, and it must stay that way: the
 * caller drops pmm_lock before asking, because a spinlock held across
 * several milliseconds of polled I/O is a spinlock the timer will find
 * held on every tick.
 */
static int swap_reclaim(void) {
    if (!swap_ready || swap_busy) return 0;
    swap_busy = 1;
    int got = swap_out_one();
    swap_busy = 0;
    return got;
}

/*
 * Bring a page back.
 *
 * Returns 1 if the entry was a swapped one and is now present. Zero
 * means it was not ours -- an ordinary unmapped address, which is still
 * a fault and still the caller's to report.
 */
static int swap_in_page(addr_space_t *as, uint64_t va) {
    if (!swap_ready || !as || !as->live) return 0;

    va = PAGE_ALIGN_DOWN(va);
    uint64_t *pte = vmm_walk(as, va, 0);
    if (!pte || !swap_pte_is_swapped(*pte)) return 0;

    uint32_t slot = swap_pte_slot(*pte);
    if (slot >= swap_slot_count) return 0;

    /*
     * A frame for it. This may itself have to evict something, and that
     * is allowed to happen: the page being read in is not mapped yet, so
     * it has no reverse-map entry and cannot be chosen as its own
     * victim.
     */
    uint64_t phys = pmm_alloc();
    if (!phys) {
        swap_errstr = "no frame to page into";
        swap_fails++;
        return 0;
    }

    if (swap_read_slot(slot, phys) != 0) {
        pmm_free(phys);
        swap_errstr = "read from the pagefile failed";
        swap_fails++;
        return 0;
    }

    *pte = (phys & PTE_ADDR_MASK) | (*pte & SWAP_PERM_MASK) | PTE_PRESENT;
    flush_tlb_page(va);

    swap_slot_free(slot);
    swap_rmap_record(phys, as, va);
    swap_ins++;
    return 1;
}

/*
 * Release a swapped page without reading it back.
 *
 * For teardown. vmm_destroy() walks only present entries, so without
 * this every page a process had swapped out would hold its slot for the
 * rest of the boot -- a leak that is invisible until the pagefile is
 * full and nothing can be evicted any more.
 */
static void swap_discard_pte(uint64_t e) {
    if (!swap_ready || !swap_pte_is_swapped(e)) return;
    uint32_t slot = swap_pte_slot(e);
    if (slot < swap_slot_count) swap_slot_free(slot);
}

/*
 * Force one named page out, whatever the clock thinks.
 *
 * The pager itself never calls this -- reclaim always goes through the
 * clock, because choosing a victim *is* the policy. It exists for the
 * self-test, which has to make an eviction happen on demand: the
 * alternative is exhausting two gigabytes of RAM to provoke one, which
 * puts every other subsystem under pressure at the same moment and so
 * cannot say which of them broke.
 *
 * It takes the same swap_victim_ok() path as the clock does rather than
 * trusting its caller, so a test cannot ask for an eviction the pager
 * would refuse and conclude from the result that the refusal works.
 */
__attribute__((unused))
static int swap_out_specific(addr_space_t *as, uint64_t va) {
    if (!swap_ready || !as || !as->live) return 0;
    va = PAGE_ALIGN_DOWN(va);

    uint64_t *pte = vmm_walk(as, va, 0);
    if (!pte || !(*pte & PTE_PRESENT)) return 0;

    uint64_t frame = (*pte & PTE_ADDR_MASK) >> PAGE_SHIFT;
    addr_space_t *ck_as; uint64_t ck_va; uint64_t *ck_pte;
    if (!swap_victim_ok(frame, &ck_as, &ck_va, &ck_pte)) return 0;
    if (ck_pte != pte) return 0;         /* the rmap names a different va */

    uint32_t slot1 = swap_slot_alloc();
    if (!slot1) return 0;
    uint32_t slot = slot1 - 1;

    uint64_t phys = *pte & PTE_ADDR_MASK;
    if (swap_write_slot(slot, phys) != 0) { swap_slot_free(slot); return 0; }

    *pte = swap_pte_make(slot, *pte);
    if ((read_cr3() & PTE_ADDR_MASK) == as->pml4_phys) flush_tlb_page(va);

    swap_rmap_forget(phys);
    pmm_free(phys);
    swap_outs++;
    return 1;
}

/*
 * The page-fault half, as trap.h calls it.
 *
 * Deliberately narrow: a not-present fault, on a user address, in a live
 * address space, on an entry carrying the marker. Everything else --
 * including a not-present fault on an address that was never mapped --
 * returns 0 and is reported as the fault it is.
 */
static int swap_handle_fault(uint64_t cr2, uint64_t error) {
    if (!swap_ready) return 0;
    if (error & 1) return 0;                    /* a protection fault      */
    if (cr2 >= USER_SPACE_END) return 0;        /* the kernel's own half   */
    if (!vmm_current) return 0;
    return swap_in_page(vmm_current, cr2);
}

/* Is this the pagefile? Asked by the filesystem layer before it deletes
 * or truncates anything -- see the note at fs_write_file. */
static int swap_owns_path(const char *abs) {
    if (!swap_ready) return 0;
    return str_eq(abs, SWAP_PATH);
}

/* ===== the self-test =====
 *
 * `make iso EXTRA=-DSWAP_SELFTEST`, then watch the serial line.
 *
 * Eviction is forced on a scratch address space rather than provoked by
 * exhausting memory, and that is the only way this is worth running: a
 * two-gigabyte machine takes a long time to fill, filling it puts every
 * other subsystem under pressure at the same moment, and a test that
 * has to destabilise the whole system to reach its subject cannot say
 * which of the two broke.
 *
 * What is checked is the property that matters and the one an eye
 * cannot verify: that a page which went to disk and came back holds the
 * same bytes. A pager that loses a page is indistinguishable from a
 * working one until something reads what it lost.
 */
#ifdef SWAP_SELFTEST

static int swap_st_checks = 0, swap_st_fail = 0;

static void swap_st_ok(const char *what, int cond) {
    swap_st_checks++;
    if (!cond) swap_st_fail++;
    serial_puts(cond ? "  ok    " : "  FAIL  ");
    serial_puts(what);
    serial_putc('\n');
}

static void swap_selftest(void) {
    serial_puts("\n[swaptest] the pager\n");

    if (!swap_ready) {
        serial_puts("  SKIP  swap is off: ");
        serial_puts(swap_errstr);
        serial_puts("\n");
        return;
    }

    /* Slot bookkeeping, before anything touches a disk. */
    {
        uint32_t before = swap_slots_used;
        uint32_t a = swap_slot_alloc(), b = swap_slot_alloc();
        swap_st_ok("a slot can be allocated", a != 0 && b != 0);
        swap_st_ok("  and two are different", a != b);
        swap_st_ok("  and the count follows", swap_slots_used == before + 2);
        swap_slot_free(a - 1);
        swap_slot_free(b - 1);
        swap_st_ok("  and freeing gives them back", swap_slots_used == before);
    }

    /* The encoding: a slot has to survive the round trip through a page
     * table entry, and the permissions with it. */
    {
        uint64_t old = PTE_PRESENT | PTE_WRITE | PTE_USER | PTE_NX | 0x123000ULL;
        uint64_t e = swap_pte_make(4242, old);
        swap_st_ok("a swapped entry is not present", !(e & PTE_PRESENT));
        swap_st_ok("  it carries the marker", swap_pte_is_swapped(e));
        swap_st_ok("  the slot survives", swap_pte_slot(e) == 4242);
        swap_st_ok("  and so do the permissions",
                   (e & PTE_WRITE) && (e & PTE_USER) && (e & PTE_NX));
        swap_st_ok("a present entry is never read as swapped",
                   !swap_pte_is_swapped(old));
        swap_st_ok("the top slot round-trips",
                   swap_pte_slot(swap_pte_make(SWAP_SLOTS - 1, 0)) ==
                   SWAP_SLOTS - 1);
    }

    /* And now a real page, through a real address space. */
    addr_space_t as;
    if (vmm_create(&as) != 0) {
        serial_puts("  FAIL  could not create a scratch address space\n");
        swap_st_fail++;
        return;
    }

    const uint64_t va = 0x0000000041000000ULL;      /* inside the heap span */
    const uint64_t flags = PTE_USER | PTE_WRITE | PTE_NX;

    uint64_t bytes = vmm_alloc_range(&as, va, PAGE_SIZE, flags);
    swap_st_ok("a scratch page is mapped", bytes == PAGE_SIZE);

    uint64_t phys0 = vmm_resolve(&as, va);
    swap_st_ok("  and resolves", phys0 != 0);

    /* A pattern that would survive a partial write looking correct:
     * every byte depends on its own offset. */
    uint8_t *p = (uint8_t *)(uintptr_t)phys_to_virt(phys0 & ~PAGE_MASK);
    for (int i = 0; i < 4096; i++) p[i] = (uint8_t)((i * 31 + 7) & 0xFF);

    uint64_t outs_before = swap_outs, ins_before = swap_ins;
    uint32_t used_before = swap_slots_used;

    swap_st_ok("the page is evicted", swap_out_specific(&as, va) == 1);
    swap_st_ok("  the counter moved", swap_outs == outs_before + 1);
    swap_st_ok("  a slot is held", swap_slots_used == used_before + 1);

    {
        uint64_t *pte = vmm_walk(&as, va, 0);
        swap_st_ok("  the entry says swapped", pte && swap_pte_is_swapped(*pte));
        swap_st_ok("  the frame is gone from it",
                   pte && (*pte & PTE_ADDR_MASK) != (phys0 & ~PAGE_MASK));
    }

    /*
     * Back in through vmm_resolve, which is the kernel-side path -- the
     * one that used to return zero for a page that was merely elsewhere.
     */
    uint64_t phys1 = vmm_resolve(&as, va);
    swap_st_ok("the page comes back", phys1 != 0);
    swap_st_ok("  the counter moved", swap_ins == ins_before + 1);
    swap_st_ok("  the slot is released", swap_slots_used == used_before);

    int same = 1;
    if (phys1) {
        const uint8_t *q = (const uint8_t *)(uintptr_t)
                           phys_to_virt(phys1 & ~PAGE_MASK);
        for (int i = 0; i < 4096; i++)
            if (q[i] != (uint8_t)((i * 31 + 7) & 0xFF)) { same = 0; break; }
    } else same = 0;
    swap_st_ok("  and every byte of it is what went out", same);

    {
        uint64_t *pte = vmm_walk(&as, va, 0);
        swap_st_ok("  the entry is present again", pte && (*pte & PTE_PRESENT));
        swap_st_ok("  with its permissions intact",
                   pte && (*pte & PTE_WRITE) && (*pte & PTE_USER) &&
                          (*pte & PTE_NX));
    }

    /*
     * The fault path, entered exactly as trap.h enters it.
     *
     * This is as close to a real #PF as a boot-time test can get. What
     * it does not exercise is the IRETQ that resumes the faulting
     * instruction -- that needs a ring-3 thread, and there is not one
     * yet at this point in the boot. It does exercise every line of this
     * file that a fault reaches, including the filtering, with the same
     * arguments exception_handle passes.
     */
    {
        swap_st_ok("evicted again for the fault path",
                   swap_out_specific(&as, va) == 1);

        addr_space_t *saved = vmm_current;
        vmm_current = &as;

        swap_st_ok("a protection fault is not ours",
                   swap_handle_fault(va, 1) == 0);
        swap_st_ok("  nor is a kernel address",
                   swap_handle_fault(KSTACK_VA_BASE, 0) == 0);
        swap_st_ok("  nor an address that was never mapped",
                   swap_handle_fault(va + 0x10000, 0) == 0);

        /* And the one that is. */
        swap_st_ok("a not-present fault on a swapped page is handled",
                   swap_handle_fault(va + 0x40, 0) == 1);

        uint64_t *pte = vmm_walk(&as, va, 0);
        swap_st_ok("  the page is present after it",
                   pte && (*pte & PTE_PRESENT));

        int intact = 0;
        if (pte && (*pte & PTE_PRESENT)) {
            const uint8_t *q = (const uint8_t *)(uintptr_t)
                               phys_to_virt(*pte & PTE_ADDR_MASK);
            intact = 1;
            for (int i = 0; i < 4096; i++)
                if (q[i] != (uint8_t)((i * 31 + 7) & 0xFF)) { intact = 0; break; }
        }
        swap_st_ok("  with its contents", intact);

        vmm_current = saved;
    }

    /*
     * Reclaim: what pmm_alloc() calls when the bitmap is empty.
     *
     * Driven directly rather than by exhausting memory, for the reason
     * given at swap_out_specific. What it proves is that the clock finds
     * a victim on its own -- nothing here names the page -- and that a
     * frame genuinely comes back to the allocator.
     */
    {
        uint64_t outs = swap_outs;
        uint64_t free_before = pmm_free_frames;
        int got = swap_reclaim();
        swap_st_ok("reclaim finds a victim by itself", got == 1);
        swap_st_ok("  it went out", swap_outs == outs + 1);
        swap_st_ok("  and a frame came back to the allocator",
                   pmm_free_frames == free_before + 1);
    }

    /*
     * And now the path that actually matters: pmm_alloc() on a machine
     * with no free frames at all.
     *
     * Calling swap_reclaim() directly, as the block above does, proves
     * the mechanism and not the trigger -- the bitmap was never empty,
     * so pmm_alloc()'s own exhaustion branch never ran. This drains the
     * allocator for real and then asks it for one page.
     *
     * Draining does not mean touching two gigabytes. pmm_alloc_largest()
     * takes the biggest contiguous run that is left, so a handful of
     * calls parks essentially all of memory in a few pointers, and the
     * singles left over by fragmentation are swept up after it. Nothing
     * is written to any of it; it is held and handed straight back.
     *
     * preempt_disable() for the duration because the scheduler is
     * already running by this point in the boot -- sched_start() is four
     * hundred lines earlier -- and a thread that woke up inside this
     * window would find a machine with no memory. Nothing else here
     * allocates from an interrupt, so blocking the switch is enough.
     */
    {
#define SWAP_ST_PARK 128
        static uint64_t park_p[SWAP_ST_PARK];
        static uint64_t park_f[SWAP_ST_PARK];
        int nr = 0;

        /* The page has to be resident to be a candidate; the reclaim
         * above has probably just taken it. */
        (void)vmm_resolve(&as, va);

        preempt_disable();

        while (nr < SWAP_ST_PARK) {
            uint64_t frames = 0;
            uint64_t p = pmm_alloc_largest(0, 1, &frames);
            if (!p) break;
            park_p[nr] = p; park_f[nr] = frames; nr++;
        }
        while (nr < SWAP_ST_PARK) {          /* stragglers, one at a time */
            uint64_t p = pmm_alloc_once();
            if (!p) break;
            park_p[nr] = p; park_f[nr] = 1; nr++;
        }

        int drained = (nr < SWAP_ST_PARK);
        swap_st_ok("the allocator can be drained", drained);

        uint64_t outs = swap_outs;
        uint64_t probe = pmm_alloc_once();
        swap_st_ok("  the bitmap really is empty", probe == 0);
        if (probe) pmm_free(probe);

        /* The whole point. */
        uint64_t page = pmm_alloc();
        swap_st_ok("pmm_alloc still returns a frame when memory is full",
                   page != 0);
        swap_st_ok("  because a page was written out to get it",
                   swap_outs == outs + 1);

        if (page) pmm_free(page);
        for (int i = 0; i < nr; i++) pmm_free_contig(park_p[i], park_f[i]);

        preempt_enable();

        swap_st_ok("  and the parked memory is all back",
                   pmm_free_frames > 0);
#undef SWAP_ST_PARK
    }

    /*
     * A page that is out at teardown must not take its slot with it.
     *
     * Written so the reclaim above may or may not have already chosen
     * this page -- it is the only swappable one on the machine at this
     * point, so usually it has. What is asserted is the state, not the
     * history: the page is out, and destroying the address space that
     * owned it returns exactly one slot.
     */
    {
        uint64_t *pte = vmm_walk(&as, va, 0);
        if (!(pte && swap_pte_is_swapped(*pte))) {
            swap_st_ok("evicted for the teardown check",
                       swap_out_specific(&as, va) == 1);
            pte = vmm_walk(&as, va, 0);
        }
        swap_st_ok("the page is out at teardown",
                   pte && swap_pte_is_swapped(*pte));

        uint32_t held = swap_slots_used;
        vmm_destroy(&as);
        swap_st_ok("  and teardown returns its slot",
                   swap_slots_used == held - 1);
    }

    /* The pagefile cannot be deleted while it is being written to. */
    swap_st_ok("the pagefile is protected from delete",
               fs_delete(SWAP_PATH) != 0);
    swap_st_ok("  and from being overwritten",
               fs_write_file(SWAP_PATH, "x", 1) != 0);
    swap_st_ok("  and is still there afterwards",
               fs_stat(SWAP_PATH, 0, 0) == 1);

    serial_puts("\n[swaptest] ");
    serial_put_dec((uint32_t)swap_st_checks);
    serial_puts(" checks, ");
    serial_put_dec((uint32_t)swap_st_fail);
    serial_puts(" failures\n\n");
}
#endif /* SWAP_SELFTEST */

#endif /* SWAP_H */
