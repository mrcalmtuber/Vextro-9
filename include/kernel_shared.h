#ifndef VEXTRO_KERNEL_SHARED_H
#define VEXTRO_KERNEL_SHARED_H

/*
 * include/kernel_shared.h — the seam between the kernel's translation
 * units.
 *
 * ---- why this file has to exist ----
 *
 * For most of this system's life the kernel was one translation unit:
 * src/kernel.c included ninety-odd headers, every one of them a body of
 * `static` functions and `static` state, and the compiler saw all
 * 69,000 lines at once. That is not an accident of laziness. `static`
 * is what let a driver keep its ring buffers, its MMIO map and its
 * device list private, with no risk of another driver's identically
 * named table colliding at link time.
 *
 * It also meant the whole kernel rebuilt whenever anything changed, and
 * that one file grew to hold the boot path, the login screen, the
 * compositor's frame loop and the panic handler together.
 *
 * So the kernel is now four objects rather than one. The composition
 * root — src/core/main.c — still includes the driver and desktop
 * headers, because they genuinely share static state and splitting them
 * would be a rewrite rather than a refactor. Three modules that had a
 * *narrow*, measurable interface came out into objects of their own:
 *
 *     src/sched/scheduler.c        28 exports, 14 imports
 *     src/fs/ntfs/ntfs_ops.c        8 exports,  9 imports
 *     src/security/anti_virus.c    14 exports,  5 imports
 *
 * What crosses those boundaries is declared here, and only here.
 *
 * ---- the invariant, and what breaks if it is broken ----
 *
 *   A header that owns mutable static state must appear in exactly one
 *   translation unit's include closure.
 *
 * This is the whole discipline of the split, and it is worth stating
 * the failure mode because it is silent. Suppose scheduler.c included
 * apic.h to get APIC_VEC_TIMER. apic.h defines `lapic_base` as static,
 * so scheduler.o would get its *own* copy, initialised to zero. main.o's
 * boot path would find the APIC and write main.o's copy. The timer ISR
 * — in scheduler.o — would then signal end-of-interrupt through a null
 * pointer. Everything compiles, everything links, and the machine takes
 * exactly one timer interrupt.
 *
 * Nothing in the compiler catches that, so the rule is mechanical
 * instead: the three module TUs include *only* this file, their own
 * declaration header, and <stdint.h>/<stddef.h>. They never include
 * anything from the legacy driver-header set. Where a module needs a macro or
 * a struct from one of those headers, the definition is hoisted here and
 * the original header includes this one — moved, never copied, because a
 * copy is a second definition waiting to drift.
 *
 * ---- the two kinds of thing declared below ----
 *
 * Stateless helpers — port I/O, string arithmetic, register reads — are
 * `static inline` here. They carry no state, so a copy per object file
 * is free and costs a call. These were *moved* out of their old homes;
 * leaving the original behind would be a redefinition error in main.o,
 * which is the intended safety net.
 *
 * Everything else is an extern prototype whose definition lost its
 * `static` where it stands. That direction matters: a de-static'd symbol
 * defined twice fails loudly at link, whereas a `static` one defined
 * twice succeeds and gives each object a private copy. The whole point
 * of this seam is to convert silent duplication into a link error.
 */

#include <stdint.h>
#include <stddef.h>

/* ===== 1. THE CONSOLE =====
 *
 * Defined in src/pci.h, which is where the serial port is brought up
 * early enough to report on everything after it. Every module logs, so
 * this is the one import all three share.
 */
void serial_putc(char c);
void serial_puts(const char *s);
void serial_put_hex32(uint32_t v);
void serial_put_dec(uint32_t val);

/* ===== 2. THE BLOCK LAYER =====
 *
 * Defined in src/blk.h. The filesystem's entire view of storage: four
 * functions, LBA-addressed, 512-byte sectors. blk_present() answers
 * whether there is a disk at all, which the NTFS mount asks before it
 * probes so that a diskless ISO boot stays quiet rather than reporting
 * a failed mount.
 */
int blk_present(void);
int blk_read(uint64_t lba, uint32_t count, void *buf);
int blk_write(uint64_t lba, uint32_t count, const void *buf);
int blk_flush(void);

/* ===== 3. THE PARTITION TABLE =====
 *
 * Defined in src/part.h, which parses both MBR and GPT into this one
 * list. The type lives here rather than there because ntfs_ops.o needs
 * the layout to walk the table, and src/part.h includes this file to
 * pick the definition back up.
 *
 * NTFS reads two things from it — how many entries there are, and where
 * each starts — but the table is exported whole rather than through an
 * accessor, because a partition list is data, and narrowing it to the
 * current caller's needs would only have to be widened again.
 */
#define PART_MAX      64
#define PART_NAME_LEN 40

typedef struct {
    uint64_t start;              /* first LBA                       */
    uint64_t sectors;
    uint8_t  mbr_type;           /* 0 when the entry came from GPT  */
    uint8_t  from_gpt;
    uint8_t  bootable;
    char     name[PART_NAME_LEN];
    uint8_t  type_guid[16];
} partition_t;

extern partition_t part_table[PART_MAX];
extern int         part_count;

/* ===== 4. TIME =====
 *
 * Defined in src/sched/scheduler.c. The 1 kHz tick, which NTFS uses to
 * timestamp records and which several waiters compare against.
 *
 * `volatile` is not decoration and must match the definition exactly.
 * It is written by the timer interrupt and read from ordinary code, so
 * without it the compiler is entitled to assume a loop that only reads
 * this can never make progress — and to fold the difference between two
 * reads to zero. That has already happened once here, to sched_switches
 * below, where it turned a context-switch selftest into one that passed
 * by measuring nothing.
 */
extern volatile uint64_t sched_ticks;

/* ===== 5. STRING ARITHMETIC =====
 *
 * These were `static` in src/gfx.h, where they had ended up for no
 * better reason than that the first caller drew text. They are pure
 * functions over caller-supplied buffers — no state, no allocation, no
 * dependency on anything above — so they are `static inline` here and
 * each object gets its own copy, which costs nothing and saves a call.
 *
 * They were *moved*, not copied: src/gfx.h no longer defines them. A
 * copy left behind would be a second definition free to drift from this
 * one, and the two would only ever be compared by a bug.
 *
 * The truncating semantics are the originals and are relied on
 * throughout: `max` is the size of the destination *including* the
 * terminator, and both copy and append always terminate.
 */

static inline int str_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

static inline int str_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == *b;
}

static inline int str_starts_with(const char *str, const char *prefix) {
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++; prefix++;
    }
    return 1;
}

static inline void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static inline void str_append(char *dst, const char *src, int max) {
    int len = str_len(dst);
    int i = 0;
    while (src[i] && len < max - 1) { dst[len++] = src[i++]; }
    dst[len] = '\0';
}

static inline void uint_to_str(uint32_t val, char *out) {
    if (val == 0) { out[0] = '0'; out[1] = '\0'; return; }
    char tmp[12];
    int i = 0;
    while (val > 0) { tmp[i++] = (char)('0' + val % 10); val /= 10; }
    int j = 0;
    while (i > 0) out[j++] = tmp[--i];
    out[j] = '\0';
}

/* ===== 6. THE FILESYSTEM =====
 *
 * Defined in src/desktop.h, which is where the volume is mounted and
 * where the path-to-driver dispatch lives. Only the three calls the
 * policy file needs are exported; anti_virus.o reads and writes
 * /etc/policy.cfg through them and knows nothing about which filesystem
 * is underneath.
 *
 * fs_read_file returns a pointer into a driver-owned buffer that stays
 * valid only until the next call — the caller copies what it needs.
 */
const void *fs_read_file(const char *filename, uint64_t *out_size);
int         fs_write_file(const char *path, const void *data, uint32_t len);
int         fs_mkdir(const char *path);

/* ===== 7. WHAT THE SCHEDULER NEEDS =====
 *
 * src/sched/scheduler.c is the largest of the three module objects and
 * the one with the widest seam: fifteen symbols in, twenty-four out.
 * Everything it imports is below, grouped by what it is for, because
 * the grouping is the argument for why the split is safe — a context
 * switch touches the descriptor table, the page tables, the interrupt
 * controller and the heap, and those four are the whole of it.
 */

/* ---- 7a. instructions the compiler has no operator for ----
 *
 * Moved from src/idt.h and src/pmm.h. Pure inline assembly with no
 * state behind it, so `static inline` here and a copy per object costs
 * nothing.
 *
 * irq_save/irq_restore are a pair and must stay one: restore only
 * re-enables interrupts if they were enabled when saved, which is what
 * makes them nest correctly inside a region that had already disabled
 * them.
 *
 * ---- why this section is guarded and the rest is not ----
 *
 * Everything above compiles anywhere: prototypes, plain structs and
 * string arithmetic. From here down it is x86-64 instructions, and the
 * host test suite builds two of the module sources natively — on an
 * arm64 Mac, among others — to check them against reference
 * implementations. Those tests need the seam's declarations and never
 * execute a privileged instruction, so the machine-specific half is
 * fenced off rather than the whole file being made kernel-only.
 *
 * The alternative was a second copy of the declarations for the host,
 * which is the arrangement this refactor exists to get rid of.
 */
#if defined(__x86_64__)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr" :: "c"(msr),
                     "a"((uint32_t)val), "d"((uint32_t)(val >> 32)) : "memory");
}

static inline uint64_t irq_save(void) {
    uint64_t flags;
    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & 0x200ULL) __asm__ volatile("sti" ::: "memory");
}

/* ---- 7a2. the spinlock ----
 *
 * Moved here from src/pmm.h, unchanged, for the reason the comment there
 * always anticipated: it was written the way it is so that bringing up a
 * second processor would not mean revisiting every caller. That day has
 * arrived, and the callers are now in more than one translation unit —
 * the scheduler's per-core queues take these, and scheduler.o cannot see
 * src/pmm.h.
 *
 * The exchange was already atomic and already correct under real
 * contention; what changes with an AP awake is only that the spin can
 * now genuinely spin. Saving and clearing IF around the critical section
 * is what makes a lock safe to take from a thread and from the interrupt
 * that preempts it, and that is unchanged too.
 *
 * The bounded spin stays as it was: after a hold time far beyond any
 * legitimate one the lock is taken anyway and the event reported, which
 * converts a silent freeze into a live machine and a message naming the
 * lock. spin_report_timeout keeps its counter in src/pmm.h — it is
 * diagnostic state and belongs to one object, so what crosses the seam
 * is the call and not the variable.
 */
typedef struct {
    volatile uint32_t locked;
    const char       *name;      /* for the report when it never comes free */
} spinlock_t;

#define SPIN_TIMEOUT 40000000u

void spin_report_timeout(spinlock_t *l);

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

/* The same lock without touching the interrupt flag, for the one place
 * that needs it: an application processor's idle loop, which must keep
 * interrupts exactly as it found them across a queue pop. */
static inline void spin_lock(spinlock_t *l) {
    uint32_t spins = 0;
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        if (++spins >= SPIN_TIMEOUT) {
            spin_report_timeout(l);
            __atomic_store_n(&l->locked, 1u, __ATOMIC_RELEASE);
            break;
        }
        __asm__ volatile("pause" ::: "memory");
    }
}

static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
}

/* ---- 7b. segment selectors and the task-state segment ----
 *
 * Moved from src/gdt.h. Long mode kept the TSS for one field: RSP0, the
 * stack the processor switches to when a ring-3 thread traps. The
 * scheduler rewrites it on every context switch and reads it once when
 * adopting the boot stack, so the structure is exported whole rather
 * than through accessors — `tss.rsp0 = ...` stays a single store inside
 * sched_on_tick, which is hand-tuned and where an added call is not
 * worth the risk for no gain.
 */
#define GDT_NULL     0x00
#define GDT_KCODE    0x08
#define GDT_KDATA    0x10
#define GDT_UCODE32  0x18        /* SYSRET base — never loaded directly */
#define GDT_UDATA    0x20
#define GDT_UCODE    0x28
#define GDT_TSS      0x30        /* 16 bytes: occupies 0x30 and 0x38 */

#define GDT_ENTRIES  8

/* Selectors as a user-mode thread sees them, RPL 3 included. */
#define SEL_UCODE    (GDT_UCODE | 3)
#define SEL_UDATA    (GDT_UDATA | 3)

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed)) tss_t;

extern tss_t tss;

/* ---- 7c. the local APIC ----
 *
 * Moved from src/apic.h. lapic_eoi stays `always_inline` and
 * general-regs-only exactly as it was: it is called from the timer
 * interrupt stub, which is compiled with the same restriction so that
 * it cannot disturb the extended state the switch is in the middle of
 * moving. Turning it into a cross-object call would have been the
 * simpler split and would have put a function call inside that window.
 *
 * An interrupt that returns without acknowledging blocks every vector
 * at or below its priority permanently, which presents as the machine
 * freezing some seconds after boot with nothing on the wire — so this
 * one really is worth keeping identical.
 */
#define APIC_REG_EOI      0x0B0
#define APIC_VEC_SPURIOUS 0xFF
#define APIC_VEC_TIMER    0x40

extern volatile uint8_t *lapic_base;
extern int               lapic_present;

static inline void lapic_write_reg(uint32_t reg, uint32_t val) {
    *(volatile uint32_t *)(lapic_base + reg) = val;
}

__attribute__((always_inline, target("general-regs-only")))
static inline void lapic_eoi(void) {
    if (lapic_present) lapic_write_reg(APIC_REG_EOI, 0);
}

/* ---- 7d. address spaces ----
 *
 * Moved from src/vmm.h. The scheduler loads CR3 from a thread's address
 * space on every switch and destroys it when the last thread using it
 * exits, so it needs the layout rather than an opaque handle.
 */
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

/*
 * The window surface a process draws into, and the token it draws with.
 *
 * Both are per-process rather than per-thread, which is why they are
 * here and not in thread_t: a fork shares the parent's pixels and
 * inherits the parent's privileges, and both of those follow the address
 * space. src/desktop.h owns the definition of struct app_surface and is
 * the only thing that ever dereferences the pointer; scheduler.o carries
 * it across a fork and a reap without needing to know what it is, which
 * is exactly what an incomplete type is for.
 */
struct app_surface;

/*
 * A reservation made by mmap, and why the address space carries a list of
 * them rather than page table entries.
 *
 * The obvious implementation of mmap is vmm_alloc_range: walk the range,
 * take a frame per page, write a present entry. That is what sbrk does
 * and it is right for sbrk, because a program grows its heap in sixty-
 * four kilobyte steps. It is wrong for a reservation, and a browser is
 * exactly the program that makes it wrong — a JavaScript heap asks for
 * gigabytes of address space and touches a few megabytes of it.
 *
 * Backing it eagerly costs the physical memory immediately, and costs it
 * inside a system call, which runs with interrupts masked. src/syscall.h
 * promises every service is "bounded and short — a string measured, a
 * rectangle filled, a page mapped". A million frames allocated under that
 * promise is how a machine stops answering its own timer.
 *
 * Marking the page tables instead would be cheaper in memory and no
 * cheaper in time: a marker still has to be *written*, one entry per
 * page, and the page tables to hold it still have to be built — eight
 * megabytes of them for a four gigabyte reserve.
 *
 * So a reservation is a record, not a mapping. The list is consulted by
 * the page fault handler, which is the only code that ever needs to know
 * the difference between "never asked for" and "asked for and not yet
 * touched". Sixty-four of them is a fixed cost of two kilobytes per
 * address space and more distinct mappings than any program here makes.
 */
struct vmm_area;

/*
 * ---- the open descriptors, and where a process actually is ----
 *
 * There is no `struct pcb` in this kernel and there never was. The unit
 * of scheduling is thread_t; the unit of *isolation* is this structure,
 * which is what a process is here — an address space, a window, a
 * security token, and now a table of open descriptors. src/sched/sched.h
 * says as much at the top, and it is worth repeating at the point the
 * table lands, because the placement decides the semantics rather than
 * merely recording them:
 *
 *   Threads share descriptors. SYS_CLONE gives the new thread *the*
 *   address space, so it gets the same table by construction — which is
 *   what POSIX means by a thread. A table hung off thread_t would give
 *   every worker its own fd 3 and no way to hand one to another.
 *
 *   Forks copy them. SYS_FORK makes a new address space, so the child
 *   gets a duplicate: the same files open, offsets independent from the
 *   split onwards, exactly as fork(2) has behaved since the seventh
 *   edition.
 *
 *   They close when the last thread of the process ends, not the first.
 *   `refs` below is what makes that expressible at all.
 *
 * Opaque here for the same reason struct app_surface is: src/vfs.h owns
 * the definition, scheduler.o carries the pointer across a fork and a
 * reap without ever dereferencing it, and an incomplete type is exactly
 * what that calls for.
 */
struct proc_files;

/* And what it has asked to be told about. include/vls.h owns this one;
 * it is here for the same reason struct proc_files is, and it is
 * separate from that header because a process that never catches a
 * signal never allocates it. */
struct vls_sig;

/* ---- the token ----
 *
 * UAC_TOKEN_RESTRICTED is what every process gets, including one started
 * by an administrator. The elevated form is granted for the life of one
 * process by an explicit answer at the keyboard and never inherited: a
 * child of an elevated process starts restricted like anything else,
 * because an elevation is an answer about one program and not a property
 * a program can pass on.
 */
#define UAC_TOKEN_RESTRICTED 0
#define UAC_TOKEN_ELEVATED   1

/*
 * Whether this process has been allowed off the machine.
 *
 * Three states rather than two, because "asked and refused" has to be
 * distinguishable from "not asked yet". A program that has been told no
 * makes its next connection attempt a millisecond later; without the
 * third state that is a second prompt, and a stream of prompts is how a
 * person is trained to click yes.
 *
 * Loopback never consults this — see the note in src/vfs.h. A connection
 * to 127.0.0.1 does not leave the machine, so there is nothing to ask
 * about.
 */
#define NET_UNASKED   0
#define NET_ALLOWED   1
#define NET_REFUSED   2

typedef struct {
    uint64_t  pml4_phys;
    uint64_t *pml4;          /* through the HHDM */
    uint64_t  brk;           /* next unallocated heap byte */
    uint64_t  brk_top;       /* how far the heap has actually been mapped */
    uint64_t  canvas_va;     /* where the window's pixels landed this run */
    uint64_t  tramp_va;      /* and the trampoline page - both randomised */
    int       live;

    /*
     * ---- how many threads are standing in here ----
     *
     * One, for the whole of this system's life until now: a process was a
     * thread that happened to carry an address space, and sched_reap
     * destroyed the space when the thread that carried it ended. That is
     * correct exactly as long as no two threads ever carry the same one.
     *
     * SYS_CLONE makes two, which is the point of it — a pthread is a
     * thread in the caller's own address space, sharing its heap and its
     * globals, because a library that ran each thread in a copy of the
     * program's memory would not be threading. Without a count the first
     * of them to finish tears down the page tables the others are running
     * on, and the failure is a triple fault with no thread left to blame.
     *
     * Incremented by clone, decremented by the reaper, and the space is
     * released by whichever thread takes it to zero. A fork does not touch
     * it: fork makes a *new* address space, which starts at one like every
     * other.
     */
    uint32_t  refs;

    struct app_surface *surface;  /* private pixels, or null for the
                                   * shared fallback canvas             */

    /* ---- the security token ---- */
    uint32_t  sid;           /* which account owns this process         */
    uint8_t   sid_admin;     /* that account holds the administrator
                              * flag -- which is permission to be
                              * *asked*, never permission itself        */
    uint8_t   token;         /* UAC_TOKEN_*, this run only              */

    /*
     * ---- and whether this program may leave the machine ----
     *
     * Deliberately *not* the token above, and this is the whole reason
     * it is a separate byte.
     *
     * uac_guard grants by setting `token` to UAC_TOKEN_ELEVATED, which
     * is a single answer covering all three privileged doors: writing a
     * file, changing the registry, and writing raw sectors. Asking the
     * same question about the network and recording the answer there
     * would mean that saying yes to "may this program fetch a page"
     * silently said yes to "may this program overwrite the partition
     * table". Those are not the same question and must not share a bit.
     *
     * NET_* below, and it records a refusal as well as a grant, so a
     * person who has said no once is not asked again by the same
     * program.
     */
    uint8_t   net_ok;

    /* ---- the open descriptors ----
     *
     * Null on a process that never opens anything, which is every
     * program written before this and costs them nothing: the table is
     * allocated by the first open, not at spawn. See src/vfs.h.
     */
    struct proc_files *files;

    /* ---- the anonymous mappings ----
     *
     * Where the next mmap without a fixed address lands, and the list of
     * what has been reserved so far. Both null and zero on a process that
     * never calls mmap, which is every program written before this and
     * costs them nothing: the array is allocated on the first reservation,
     * not at spawn.
     */
    uint64_t  mmap_next;
    struct vmm_area *vmas;
    uint32_t  vma_count;

    /*
     * ---- who this process is ----
     *
     * New, and it is worth saying what changed rather than only what
     * these are. Until now nothing in this kernel had a process
     * identity: thread_t.pid was the only number anywhere and it names a
     * *thread*, so two threads of one program had two of them and
     * neither was the program's. That was sufficient while nothing could
     * ask about another process, and it stops being sufficient the
     * moment kill and wait4 exist — both of which take the number of a
     * process and must not be able to name one of its workers by
     * accident.
     *
     * So `pid` here is Linux's tgid: the thread id of whichever thread
     * created this address space, fixed for the life of the space and
     * unchanged by exec. thread_t.pid remains the tid. The two are
     * equal for a single-threaded program, which is why the distinction
     * went unnoticed for so long.
     *
     * `ppid` is the pid of the process that forked this one, or zero for
     * one the desktop launched. It is recorded at fork and never
     * updated: a child whose parent has ended keeps the number, and
     * wait4 finding no such process is exactly the answer POSIX gives.
     */
    uint32_t  pid;
    uint32_t  ppid;

    /*
     * Whether this process's ending has already been told to its parent.
     *
     * A process can stop in four different places — exit, exit_group, a
     * fatal signal, a fault — and only some of them are system calls.
     * The reaper is the one point every one of them passes through, so
     * it is the backstop; but the reaper runs on the compositor thread,
     * and during the boot self-tests the compositor thread is the one
     * blocked waiting for the program to finish. A wait4 that could only
     * be satisfied by a reap that cannot happen until the waiter returns
     * is a deadlock, and it is the exact shape of one.
     *
     * So the status is recorded at whichever of the four happens first
     * and this says it has been. One bit rather than a search of the
     * parent's records, because a second record for a pid the parent has
     * already collected would be handed out by the next wait4 as if a
     * second child had ended.
     */
    uint8_t   exit_reported;

    /*
     * ---- and which numbering its system calls are written in ----
     *
     * VLS_PERSONALITY_NATIVE or VLS_PERSONALITY_LINUX; include/vls.h
     * explains the two doors and why both exist. Inherited across fork,
     * reset by exec, and one-way within a process — a program that
     * flips it has given up the native numbers, because 1 cannot mean
     * both SYS_PRINT and `write`.
     */
    uint8_t   personality;

    /*
     * ---- what it has asked to be told about ----
     *
     * Null on a process that never installs a handler, which is every
     * program written before today and costs them nothing: the table is
     * allocated by the first rt_sigaction or the first signal posted,
     * not at spawn. Opaque here for the same reason struct proc_files
     * is — include/vls.h owns the definition and this header only has to
     * carry the pointer.
     */
    struct vls_sig *sig;
} addr_space_t;

/* Whose address space CR3 currently holds, or null for the kernel's
 * own. Every syscall that validates a user pointer needs to know which
 * set of page tables the pointer is supposed to make sense in, and the
 * context switch is what sets it. */
extern addr_space_t *vmm_current;

void vmm_destroy(addr_space_t *as);

/* Kernel stacks come from their own virtual range with an unmapped page
 * below each, so an overflow is a page fault at a known address rather
 * than silent corruption of whatever was underneath. */
void *kstack_alloc(uint64_t bytes);
void  kstack_free(void *p, uint64_t bytes);

/* ---- 7e. the kernel heap ----
 *
 * kmalloc stays inline — it is a one-line dispatch to the real
 * allocator and every caller in the kernel goes through it — so what
 * crosses the seam is kmalloc_pool underneath it.
 *
 * The pool choice is not cosmetic: NONPAGED memory is never a candidate
 * for eviction, which is what anything an interrupt handler can touch
 * requires. A thread control block is exactly that.
 */
#define KPOOL_NONPAGED 0
#define KPOOL_PAGED    1
#define KPOOL_COUNT    2

void *kmalloc_pool(uint64_t n, int pool);
void  kfree(void *p);

static inline void *kmalloc(uint64_t n) {
    return kmalloc_pool(n, KPOOL_NONPAGED);
}

/* ---- 7f. the interrupt table ----
 *
 * The scheduler installs its own timer and spurious vectors, which is
 * the one thing it needs from src/idt.h beyond the MSR helpers.
 */
void idt_set_gate_ex(int vec, void *fn, uint16_t sel, uint8_t type_attr,
                     uint8_t ist);

/* ---- 7g. the system-call frame ----
 *
 * Moved from src/syscall.h. Forking a thread copies the parent's
 * register state out of whichever frame the parent entered the kernel
 * through, so the scheduler needs both the layout and the two variables
 * that say where the live one is.
 *
 * syscall_kstack was already external before the split: the assembly
 * entry stub names it, and a name the assembler has to resolve is one
 * that has to survive into the object file.
 */
typedef struct syscall_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t user_rsp;
} syscall_frame_t;

extern syscall_frame_t *syscall_cur_frame;
extern int              syscall_via_fast;
extern uint64_t         syscall_kstack;

#endif /* __x86_64__ */

#endif /* VEXTRO_KERNEL_SHARED_H */
