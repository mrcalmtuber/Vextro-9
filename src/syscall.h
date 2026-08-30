#ifndef SYSCALL_H
#define SYSCALL_H

/*
 * src/syscall.h — the boundary.
 *
 * There used to be nothing here worth the name. `int 0x80` went to a
 * gate at DPL 0, and SYSCALL went to a stub that ran on the caller's
 * stack and returned with `jmp *%rcx` — both perfectly correct for what
 * they had to do, which was let ring-0 code call ring-0 code through a
 * numbered table. Neither survives contact with an actual privilege
 * change: a ring-3 `int 0x80` into a DPL-0 gate raises #GP before the
 * handler is reached, and SYSCALL from ring 3 arrives with the *user's*
 * stack pointer still in RSP, which is the classic way to let a program
 * choose where the kernel writes.
 *
 * So both entries now do the same three things in the same order:
 * get onto a kernel stack, save every register into a frame, and hand
 * that frame to one C function. Nothing reaches the service routines
 * except through that frame, which is what makes "sanitise the incoming
 * registers" a property of the code rather than a rule handlers have to
 * remember.
 *
 * Interrupts stay off for the whole syscall. IA32_FMASK clears IF on
 * entry and nothing here turns it back on: every service below is
 * bounded and short — a string measured, a rectangle filled, a page
 * mapped — and leaving them off means the frame, the kernel stack
 * pointer and the current address space cannot change underneath a
 * handler that is halfway through validating a pointer.
 */

#include <stdint.h>
#include "kernel_shared.h"
#include "gdt.h"
#include "vmm.h"

/* ---- numbers ----
 *
 * 1-3 are what apps/vextro.h has always sent and cannot move. The rest
 * are new. 20 and up are the calls that stand in for symbols the loader
 * used to patch straight into an image as kernel addresses; see the
 * trampoline below for why they exist at all.
 */
#define SYS_PRINT            1
#define SYS_DRAW_PIXEL       2
#define SYS_GET_MOUSE        3
#define SYS_EXIT             4
#define SYS_SBRK             5
#define SYS_WRITE            6
#define SYS_YIELD            7
#define SYS_TICKS            8
#define SYS_CANVAS           9
#define SYS_TTF_TEXT_WIDTH  20
#define SYS_TTF_DRAW_STRING 21
#define SYS_GFX_RECT        22
#define SYS_FORK            23
#define SYS_MEMINFO         24
/*
 * Entropy, for ring 3.
 *
 * RDRAND is not a privileged instruction, so a program could execute it
 * itself -- but not every processor has it, and the ones that do can
 * legitimately fail it under load. Getting that right means a retry
 * loop and a fallback decision in every program that wants a random
 * number, which is how one of them ends up seeding from the tick count
 * and nobody notices.
 *
 * So the kernel answers instead, from the same source TLS uses, and a
 * short read is reported rather than padded. The alternative that was
 * *not* chosen is mapping a kernel entropy pool into user space: a
 * shared page that several processes read is a side channel between
 * them, and this door costs a syscall to avoid it.
 */
#define SYS_RANDOM          25

/*
 * A fast user-space mutex. See the service routine in src/desktop.h for
 * what the two operations mean and why the channel is a physical
 * address rather than the pointer the caller passed.
 *
 * The two operations, and the length of one park. These are the kernel's
 * copy; libc/include/sys/syscall.h carries the same three numbers on the
 * other side of the boundary, because a header shared across a privilege
 * boundary is a header a program could change.
 */
#define SYS_FUTEX           26
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1

/* How long a waiter sleeps before returning to its own retry loop. Long
 * enough that a contended lock costs one system call and not a stream of
 * them; short enough that a wake which was somehow missed costs a fifth
 * of a second rather than a hung program. */
#define FUTEX_PARK_MS       200

/*
 * ---- the three privileged doors ----
 *
 * Everything above this line is a program acting on itself: its own
 * pixels, its own memory, its own scheduling. These three act on the
 * machine, and they are the first calls in this system that do.
 *
 * They are the three ways a program can make a permanent change:
 * rewriting a file rewrites NTFS metadata, rewriting the registry
 * rewrites the configuration hive, and writing a block goes past both
 * straight to the disk. Each one goes through uac_guard first, which is
 * what makes "may this program do that" a question with an answer
 * rather than an assumption.
 */
#define SYS_FS_WRITE        27
#define SYS_REG_SET         28
#define SYS_BLK_WRITE       29

/*
 * ===== 30-39: what a C library needs and this system had no way to say
 * =====
 *
 * Nine calls, added together because they are one thing: the smallest
 * set under which a program can have more than one thread and more
 * memory than its break.
 *
 * Everything above this line was reachable from a program with one
 * thread, one heap that only grows, and no notion of time finer than a
 * tick count. That was enough for the programs written for it — a
 * Mandelbrot set, a terminal, a game of life. It is not enough for a
 * library that was written against POSIX, and the gap is not a matter of
 * missing convenience functions: pthread_create has nowhere to put a
 * thread, malloc's large path has nothing to ask for a region, and a
 * thread-local variable has no register to be addressed from.
 *
 * ---- the three memory calls ----
 *
 * MMAP reserves address space; MUNMAP gives it back; MPROTECT changes
 * what may be done with it. Reserving is not mapping — see the long note
 * in src/vmm.h about why a promise is a record rather than a page table
 * entry — and that distinction is the only reason a program can ask for
 * four gigabytes on a machine with two.
 *
 * MPROTECT is also where W^X stops being a property of the loader and
 * becomes a property of the system. The loader has always mapped an
 * image's text without PTE_WRITE and its data with PTE_NX, but that was
 * a decision taken once, about a file, by code the program does not
 * control. A program that can change its own protections can undo it —
 * and a just-in-time compiler is precisely a program that wants to. The
 * refusal is in the service routine, so there is no combination of calls
 * that arrives at a page which is both.
 *
 * ---- the four thread calls ----
 *
 * CLONE is fork's opposite number and the difference is the whole point:
 * fork gives the new thread a copy of the address space, clone gives it
 * *the* address space. A pthread that could not see its creator's heap
 * would not be a pthread.
 *
 * THREAD_EXIT ends one thread; EXIT_GROUP ends all of them. SYS_EXIT
 * keeps the meaning it has always had, which is now THREAD_EXIT's — that
 * is not a redefinition, because until today no process had a second
 * thread for the two to differ about.
 *
 * SET_FSBASE is what makes `__thread` work at all. GETTID is what lets a
 * thread recognise itself, which a recursive mutex needs and cannot get
 * any other way.
 *
 * ---- and the clock ----
 *
 * SYS_TICKS has always answered, and answers in scheduler ticks since
 * boot, which is a unit only this kernel knows. CLOCK answers in
 * nanoseconds, because that is the unit every timeout in every library
 * ever written is expressed in, and converting in user space would mean
 * every program carrying its own copy of the tick rate.
 */
#define SYS_MMAP            30
#define SYS_MUNMAP          31
#define SYS_MPROTECT        32
#define SYS_CLONE           33
#define SYS_THREAD_EXIT     34
#define SYS_GETTID          35
#define SYS_SET_FSBASE      36
#define SYS_CLOCK           37
#define SYS_EXIT_GROUP      38
#define SYS_NANOSLEEP       39

/*
 * ===== 40-59: descriptors, and the two things they can name =====
 *
 * Until this block a ring-3 program on this system could not open
 * anything. It had one file call — SYS_FS_WRITE, which takes a path and
 * a buffer and replaces a whole file behind a prompt — and no way at all
 * to reach the network, notwithstanding that the kernel has carried a
 * complete TCP/IP stack with TLS since src/vxnet.h was written.
 *
 * That was not an oversight in either case; it was the absence of the
 * one abstraction both of them need. A file you read a window at a time
 * has a position, and a socket has a connection, and neither of those
 * can be expressed by a call that takes a path. What a descriptor is, is
 * a name for state the kernel holds on the program's behalf between two
 * calls — and once there is a table of those, `read` and `recv` are the
 * same operation applied to different things in it.
 *
 * ---- what a descriptor may be ----
 *
 * A console stream (0, 1 and 2, which every process starts with), an
 * open file, an open directory, a TCP connection, or a TLS session. The
 * table lives in src/vfs.h, hangs off the address space rather than the
 * thread — see the long note in include/kernel_shared.h for why that is
 * the semantics and not merely the storage — and is closed by the last
 * thread of the process to leave.
 *
 * ---- how these report failure, and why it is different ----
 *
 * Every call above this line answers (uint64_t)-1 and nothing else. That
 * is enough when there is one way to fail, and the calls above mostly
 * have one: a pointer that is not the caller's.
 *
 * These have many. A port that cannot tell ENOENT from EACCES from
 * EISDIR cannot decide whether to create the file, ask for a password,
 * or give up, and every one of those is a different program. So the
 * calls in this block return the *negated* error number on failure —
 * -ENOENT, -EACCES — and a non-negative value on success, which is the
 * convention Linux's system calls use and therefore the one the code
 * being ported already expects its libc to unpack. libc/file.c does the
 * unpacking; the numbers themselves are in libc/include/errno.h and are
 * Linux's, for the same reason.
 *
 * The range is what makes this unambiguous: a value in [-4095, -1] is an
 * error and everything else is a result. mmap can return an address with
 * the top bit set and could not use this convention; nothing here can.
 *
 * ---- and the one door that is not here ----
 *
 * There is no SYS_WRITE in this block because there already is one.
 * SYS_WRITE (6) has taken a descriptor since it was written and has
 * always answered for 1 and 2; what changes is that it now looks a
 * descriptor up in the table when it is given anything else. A separate
 * number would have meant every ported program calling the wrong one.
 */
#define SYS_OPEN            40
#define SYS_READ            41
#define SYS_CLOSE           42
#define SYS_LSEEK           43
#define SYS_STAT            44
#define SYS_FSTAT           45
#define SYS_GETDENTS        46
#define SYS_UNLINK          47
#define SYS_MKDIR           48
#define SYS_FSYNC           49
#define SYS_FTRUNCATE       50

/*
 * ---- and the one that is deliberately absent ----
 *
 * There is no dup. On Unix a descriptor is a *name* for an open file
 * description, and dup makes a second name for one — which is why two
 * duplicated descriptors share a file offset, and why a fork shares one
 * with its child rather than copying it.
 *
 * Here the descriptor *is* the description. There is no second layer for
 * two names to point at, so dup could only ever be a copy: two entries
 * with two independent offsets, two write-back images of one file, and
 * two closes of one socket. Every one of those is silently wrong in a
 * way a program would discover as data loss rather than as an error.
 *
 * So it is not provided, and libc does not declare it. A port that
 * wants it fails to link against a name that does not exist, which is
 * the failure that can be read, rather than calling one that lies. The
 * same reasoning is why a fork does not inherit sockets — see
 * vfs_clone_table in src/vfs.h.
 */

#define SYS_SOCKET          51
#define SYS_CONNECT         52
#define SYS_CONNECT_HOST    53
#define SYS_SEND            54
#define SYS_RECV            55
#define SYS_SOCKOPT         56
#define SYS_SHUTDOWN        57
#define SYS_RESOLVE         58

/*
 * ===== 59: the calendar =====
 *
 * Seconds since 1970-01-01, read from the CMOS clock.
 *
 * Its own call rather than a clock id on SYS_CLOCK, because it is a
 * different quantity from the one that call answers. SYS_CLOCK returns
 * the scheduler tick: monotonic, starting at zero when the machine
 * boots, and the right thing to measure an interval with. This returns
 * a point on a calendar, which can be earlier than the last one it
 * returned if somebody sets the clock, and which is only as accurate as
 * the firmware that set it. Two quantities with different guarantees
 * should not share a system call number and be told apart by an
 * argument.
 *
 * Free of any permission check, which is worth saying out loud: the
 * time of day is not a secret, every process can already see it in the
 * taskbar, and a call that fails would only push programs back onto the
 * monotonic count they were using before.
 */
#define SYS_WALLCLOCK       59

/*
 * ---- open flags ----
 *
 * Linux's numbers, and octal as Linux writes them, because ported code
 * does reach for the constants directly and a port that assembled
 * O_WRONLY|O_CREAT|O_TRUNC out of a different set would open the wrong
 * thing rather than fail to compile.
 */
#define VX_O_RDONLY     0
#define VX_O_WRONLY     1
#define VX_O_RDWR       2
#define VX_O_ACCMODE    3
#define VX_O_CREAT      0100
#define VX_O_EXCL       0200
#define VX_O_TRUNC      01000
#define VX_O_APPEND     02000
#define VX_O_DIRECTORY  0200000
#define VX_O_CLOEXEC    02000000

#define VX_SEEK_SET  0
#define VX_SEEK_CUR  1
#define VX_SEEK_END  2

/* What SYS_STAT and SYS_FSTAT fill in. Fixed layout, checked by a
 * static assertion on both sides of the boundary, because a structure
 * that crosses a privilege boundary and is described twice is a
 * structure that will one day be described differently twice.
 *
 * `mtime_ns` is zero and says so here rather than inventing a number:
 * ntfs_lookup answers size and kind, the timestamps in
 * $STANDARD_INFORMATION are not on that path, and a modification time
 * that is always the epoch is at least honestly wrong rather than
 * plausibly wrong. A make(1) built against it would rebuild everything,
 * which is the safe direction. */
typedef struct {
    uint64_t size;
    uint64_t ino;          /* the MFT record on NTFS; 0 elsewhere */
    uint32_t mode;         /* VX_S_IF*                            */
    uint32_t nlink;
    uint64_t mtime_ns;     /* always 0 -- see above               */
} vx_stat_t;
_Static_assert(sizeof(vx_stat_t) == 32, "vx_stat_t crosses the boundary");

#define VX_S_IFMT   0170000
#define VX_S_IFREG  0100000
#define VX_S_IFDIR  0040000
#define VX_S_IFCHR  0020000     /* the console streams */
#define VX_S_IFSOCK 0140000

/* One entry from SYS_GETDENTS. Fixed-length rather than the packed,
 * self-describing records Linux uses: a variable record means the
 * kernel writes a length that user space then trusts to step through a
 * buffer, and a fixed one means it cannot. 272 bytes each is a
 * directory listing that costs a page per fifteen names, which is
 * nothing against the read that produced it. */
#define VX_NAME_MAX 255
typedef struct {
    uint64_t size;
    uint32_t type;          /* VX_DT_* */
    uint32_t namelen;
    char     name[256];
} vx_dirent_t;
_Static_assert(sizeof(vx_dirent_t) == 272, "vx_dirent_t crosses the boundary");

#define VX_DT_UNKNOWN 0
#define VX_DT_REG     1
#define VX_DT_DIR     2

/*
 * ---- sockets ----
 *
 * AF_INET and SOCK_STREAM, and nothing else, because underneath is
 * lwIP as this system configures it: TCP over IPv4. A datagram socket
 * is refused rather than accepted and made to fail later, for the same
 * reason mmap refuses a non-anonymous mapping.
 *
 * VX_IPPROTO_TLS is this system's own and is not a protocol number
 * anybody else uses. It selects the Mbed TLS path in src/tlsglue.c
 * instead of the plain one, so that a program gets an encrypted stream
 * through the same four calls rather than through a second API.
 *
 * It comes with a caveat that is repeated everywhere it is mentioned
 * because it is the kind of thing that gets discovered rather than
 * read: there is no certificate authority store on this volume, so the
 * chain is parsed and the handshake signature checked against the key
 * in the leaf, and nothing establishes that the leaf belongs to the
 * host that was asked for. vxsec_verifies_certificates() returns 0.
 * That stops somebody listening and does not stop somebody in the
 * middle.
 */
#define VX_AF_INET       2
#define VX_SOCK_STREAM   1
#define VX_SOCK_DGRAM    2
#define VX_IPPROTO_TCP   6
#define VX_IPPROTO_TLS   256

/* Socket options, flattened. The BSD interface names an option by a
 * (level, name) pair whose numbering differs between every two systems
 * that implement it; what actually crosses this boundary is one of
 * three things, so it is one of three numbers and libc/socket.c does
 * the mapping from whatever the caller wrote. */
#define VX_OPT_RCVTIMEO  1      /* milliseconds */
#define VX_OPT_SNDTIMEO  2      /* milliseconds */
#define VX_OPT_NODELAY   3      /* boolean      */

#define VX_SHUT_RD    0
#define VX_SHUT_WR    1
#define VX_SHUT_RDWR  2

/*
 * ---- the error numbers the kernel returns ----
 *
 * The kernel's copy, exactly as FUTEX_WAIT and FUTEX_WAKE are the
 * kernel's copy of numbers that also appear in libc/include/sys/syscall.h.
 * The reason is the one given there and it has not changed: a header
 * shared across a privilege boundary is a header a program could edit,
 * and the kernel must not read its constants out of anything a program
 * can reach.
 *
 * They are Linux's numbers, and libc/include/errno.h has the same ones
 * on the other side. That is not deference; it is that the code being
 * ported was written against them, and the only thing worse than
 * inventing a numbering here would be inventing one that overlapped.
 *
 * Only the codes something below actually returns are listed. A code
 * that no service routine can produce would be a promise nothing keeps.
 */
#define VXE_PERM         1
#define VXE_NOENT        2
#define VXE_IO           5
#define VXE_BADF         9
#define VXE_AGAIN       11
#define VXE_NOMEM       12
#define VXE_ACCES       13
#define VXE_FAULT       14
#define VXE_BUSY        16
#define VXE_EXIST       17
#define VXE_NOTDIR      20
#define VXE_ISDIR       21
#define VXE_INVAL       22
#define VXE_MFILE       24
#define VXE_NOSPC       28
#define VXE_SPIPE       29
#define VXE_ROFS        30
#define VXE_NAMETOOLONG 36
#define VXE_NOSYS       38
#define VXE_NOTEMPTY    39
#define VXE_OVERFLOW    75
#define VXE_NOTSOCK     88
#define VXE_OPNOTSUPP   95
#define VXE_AFNOSUPPORT 97
#define VXE_NETDOWN    100
#define VXE_ISCONN     106
#define VXE_NOTCONN    107
#define VXE_TIMEDOUT   110
#define VXE_CONNREFUSED 111
#define VXE_HOSTUNREACH 113

/* The window the convention above reserves. A service routine returns
 * -VXE_* and everything outside [-4095, -1] is a result; this is the
 * one place that constant is written down. */
#define VX_ERRNO_MAX  4095

/* mmap protection, as the caller states it. Deliberately the same three
 * numbers POSIX uses, because the code being ported says PROT_READ. */
#define VX_PROT_NONE   0
#define VX_PROT_READ   1
#define VX_PROT_WRITE  2
#define VX_PROT_EXEC   4

/* mmap flags. MAP_ANONYMOUS is the only kind of mapping this system can
 * make -- there is no file to map, because ring 3 has no way to open one
 * -- and it is required rather than assumed so that a port which passes
 * a descriptor is refused loudly instead of silently given blank pages
 * where it expected a file. */
#define VX_MAP_PRIVATE    0x02
#define VX_MAP_ANONYMOUS  0x20
#define VX_MAP_FIXED      0x10
#define VX_MAP_NORESERVE  0x4000

/*
 * Every register a user thread had, in the order the entry stubs push
 * them, is syscall_frame_t in include/kernel_shared.h. It moved there
 * because sched_fork_thread copies a parent's registers out of it and
 * scheduler.o is compiled separately now.
 *
 * The offsets are load-bearing — the assembly below writes them by
 * position, the C reads them by name, and nothing checks that the two
 * agree except that the machine stops working if they do not. That is
 * an argument for one definition, not two.
 */

/*
 * Where the next entry from user mode should put its stack pointer. The
 * scheduler rewrites this and tss.rsp0 together on every switch, because
 * they answer the same question for the two different ways into the
 * kernel.
 *
 * Both are external rather than static because the entry stub below
 * names them, and a name the assembler has to resolve is a name that has
 * to survive into the object file.
 */
uint64_t syscall_kstack = 0;
uint64_t syscall_user_rsp_slot = 0;

/* Implemented in desktop.h, where the things a syscall can ask for
 * actually live. Returns the value the caller finds in RAX. */
static uint64_t syscall_service(uint64_t num, uint64_t a0, uint64_t a1,
                                uint64_t a2, uint64_t a3, uint64_t a4,
                                uint64_t a5);

/*
 * The frame the call in progress arrived on, and which door it came
 * through.
 *
 * Only fork needs these, and it needs them absolutely: a child has to
 * resume at the same instruction as its parent, and the only record of
 * where that is lives in the frame. Safe as a global because interrupts
 * are masked for the whole of a syscall, so there is never more than one
 * in progress.
 *
 * The door matters because the two do not carry the same information.
 * SYSCALL puts the return address in RCX and the flags in R11 by
 * architecture; `int 0x80` puts them in the interrupt frame the
 * processor pushed, above what this structure covers, and leaves RCX and
 * R11 holding whatever the program had in them.
 */
/* Not static: sched_fork_thread copies the parent's registers out of
 * whichever frame it entered the kernel through. */
syscall_frame_t *syscall_cur_frame = 0;
int              syscall_via_fast  = 0;

__attribute__((used))
void syscall_dispatch_frame(syscall_frame_t *f);

void syscall_dispatch_frame(syscall_frame_t *f) {
    syscall_cur_frame = f;
    syscall_via_fast  = 1;
    f->rax = syscall_service(f->rax, f->rdi, f->rsi, f->rdx,
                             f->r10, f->r8, f->r9);
    syscall_cur_frame = 0;
}

/*
 * The same work, with the result thrown away — and that is not an
 * oversight, it is the older door's contract.
 *
 * apps/vextro.h has said since the beginning that `int 0x80` preserves
 * every general-purpose register, and GCC believes it. Two calls in a
 * row to os_print compile to one `mov eax, 1` and two interrupts,
 * because the compiler can see that nothing between them changes EAX.
 * Return a value in RAX and the second call is made with whatever the
 * first one returned as its number: it does nothing, silently, and the
 * program looks like it skipped a line.
 *
 * That was found exactly that way. So the legacy gate keeps its promise,
 * to the letter and to binaries already installed on somebody's disk,
 * and anything that needs an answer back uses SYSCALL — which has no
 * such history and returns in RAX like every other calling convention on
 * this machine.
 */
__attribute__((used))
void syscall_dispatch_legacy(syscall_frame_t *f);

void syscall_dispatch_legacy(syscall_frame_t *f) {
    syscall_cur_frame = f;
    syscall_via_fast  = 0;
    (void)syscall_service(f->rax, f->rdi, f->rsi, f->rdx,
                          f->r10, f->r8, f->r9);
    syscall_cur_frame = 0;
}

/*
 * SYSCALL entry.
 *
 * On arrival: CPL is already 0, RCX holds the return address, R11 holds
 * the caller's RFLAGS, IF is clear because FMASK says so — and RSP is
 * still whatever the user program had. The first instruction therefore
 * cannot be a push. It parks the user stack pointer in a global, which
 * is safe precisely because interrupts are masked: nothing can arrive
 * between writing it and reading it back.
 *
 * The exit is SYSRETQ, which needs RCX canonical and R11 sane; both come
 * straight back out of the frame the CPU itself filled in.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl syscall_entry\n"
    ".type syscall_entry, @function\n"
    "syscall_entry:\n"
    "  movq %rsp, syscall_user_rsp_slot(%rip)\n"
    "  movq syscall_kstack(%rip), %rsp\n"
    "  pushq syscall_user_rsp_slot(%rip)\n"
    "  pushq %rax\n"
    "  pushq %rbx\n"
    "  pushq %rcx\n"
    "  pushq %rdx\n"
    "  pushq %rsi\n"
    "  pushq %rdi\n"
    "  pushq %rbp\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  pushq %r10\n"
    "  pushq %r11\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  cld\n"
    "  movq %rsp, %rdi\n"
    /* The ABI wants RSP sixteen-aligned at the call, and how it arrives
     * here depends on how many words the processor pushed, which differs
     * between the two entry paths. Align it explicitly and put it back
     * from RBP, whose own value is already safe in the frame. */
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"
    "  call syscall_dispatch_frame\n"
    "  movq %rbp, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %r11\n"
    "  popq %r10\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rbp\n"
    "  popq %rdi\n"
    "  popq %rsi\n"
    "  popq %rdx\n"
    "  popq %rcx\n"
    "  popq %rbx\n"
    "  popq %rax\n"
    "  popq %rsp\n"
    "  sysretq\n"
    ".popsection\n"
);
extern void syscall_entry(void);

/*
 * The int 0x80 gate.
 *
 * Slower than SYSCALL and kept because it is what every existing app
 * binary emits, down to the ones already installed on somebody's disk.
 * The processor has done the stack switch by the time this runs — that
 * is what a DPL-3 interrupt gate and a TSS are for — so the only work is
 * to reach up into the frame it pushed for the user's RSP and lay the
 * registers out the same way the SYSCALL path does.
 *
 * The gate is also reachable from ring 0, where no stack switch happens
 * and 24(%rsp) is the kernel's own stack pointer. That is harmless: the
 * value is copied into the frame, never loaded back.
 */
__asm__(
    ".pushsection .text, \"ax\", @progbits\n"
    ".align 16\n"
    ".globl int80_stub\n"
    ".type int80_stub, @function\n"
    "int80_stub:\n"
    "  pushq 24(%rsp)\n"          /* user RSP out of the interrupt frame */
    "  pushq %rax\n"
    "  pushq %rbx\n"
    "  pushq %rcx\n"
    "  pushq %rdx\n"
    "  pushq %rsi\n"
    "  pushq %rdi\n"
    "  pushq %rbp\n"
    "  pushq %r8\n"
    "  pushq %r9\n"
    "  pushq %r10\n"
    "  pushq %r11\n"
    "  pushq %r12\n"
    "  pushq %r13\n"
    "  pushq %r14\n"
    "  pushq %r15\n"
    "  cld\n"
    "  movq %rsp, %rdi\n"
    "  movq %rsp, %rbp\n"
    "  andq $-16, %rsp\n"
    "  call syscall_dispatch_legacy\n"
    "  movq %rbp, %rsp\n"
    "  popq %r15\n"
    "  popq %r14\n"
    "  popq %r13\n"
    "  popq %r12\n"
    "  popq %r11\n"
    "  popq %r10\n"
    "  popq %r9\n"
    "  popq %r8\n"
    "  popq %rbp\n"
    "  popq %rdi\n"
    "  popq %rsi\n"
    "  popq %rdx\n"
    "  popq %rcx\n"
    "  popq %rbx\n"
    "  popq %rax\n"
    "  addq $8, %rsp\n"           /* drop the copied RSP */
    "  iretq\n"
    ".popsection\n"
);
extern void int80_stub(void);

/*
 * ===== the user trampoline page =====
 *
 * A .vx image carries an import table: names the loader fills in with
 * addresses before the image runs. Those addresses used to be kernel
 * function pointers, which worked because applications ran in ring 0 and
 * stops working the moment they do not.
 *
 * The names survive, and so does every image that uses them; what
 * changes is what they resolve to. Each now points at one of the stubs
 * below, which lives on a page mapped into the process's own address
 * space and does nothing but turn a C call into a syscall. The stubs are
 * written by hand because they must be genuinely position independent —
 * no data references at all — so that the same physical page can be
 * mapped at whatever address each process is given.
 *
 * The two eight-argument entries have more parameters than any calling
 * convention passes in registers, so they rebuild their arguments as a
 * contiguous block on the caller's stack and pass a pointer to it. The
 * kernel side validates that pointer like any other.
 *
 * They enter through SYSCALL rather than `int 0x80` because one of them
 * has to return a value, and the legacy gate is bound by an older
 * promise to leave RAX exactly as it found it. SYSCALL clobbers RCX and
 * R11, which is why every argument that arrived in RCX is pushed before
 * the instruction is issued.
 */
__asm__(
    ".pushsection .utext, \"ax\", @progbits\n"
    ".globl utramp_start\n"
    "utramp_start:\n"

    ".globl utramp_ttf_text_width\n"
    "utramp_ttf_text_width:\n"
    "  movl $20, %eax\n"
    "  syscall\n"
    "  ret\n"

    ".align 16\n"
    ".globl utramp_ttf_draw_string\n"
    "utramp_ttf_draw_string:\n"
    "  movq 16(%rsp), %rax\n"     /* arg8: size  */
    "  pushq %rax\n"
    "  movq 16(%rsp), %rax\n"     /* arg7: color */
    "  pushq %rax\n"
    "  pushq %r9\n"               /* arg6: s     */
    "  pushq %r8\n"               /* arg5: topY  */
    "  pushq %rcx\n"              /* arg4: topX  */
    "  pushq %rdx\n"              /* arg3: bh    */
    "  pushq %rsi\n"              /* arg2: bw    */
    "  pushq %rdi\n"              /* arg1: buf   */
    "  movq %rsp, %rdi\n"
    "  movl $21, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  ret\n"

    ".align 16\n"
    ".globl utramp_gfx_rect\n"
    "utramp_gfx_rect:\n"
    "  movq 16(%rsp), %rax\n"     /* arg8: color */
    "  pushq %rax\n"
    "  movq 16(%rsp), %rax\n"     /* arg7: h     */
    "  pushq %rax\n"
    "  pushq %r9\n"               /* arg6: w     */
    "  pushq %r8\n"               /* arg5: y     */
    "  pushq %rcx\n"              /* arg4: x     */
    "  pushq %rdx\n"              /* arg3: bh    */
    "  pushq %rsi\n"              /* arg2: bw    */
    "  pushq %rdi\n"              /* arg1: buf   */
    "  movq %rsp, %rdi\n"
    "  movl $22, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  ret\n"

    /*
     * Where a program lands when it returns from _start.
     *
     * Applications here are written as ordinary functions that end by
     * returning, which was fine while the kernel was the thing that
     * called them. Nothing calls them now. The loader writes the address
     * of this stub onto the new stack as the return address, so falling
     * off the end of main is an orderly exit rather than a jump to
     * whatever happened to be on the stack.
     */
    ".align 16\n"
    ".globl utramp_exit\n"
    "utramp_exit:\n"
    /* Zero, not EAX. `_start` returns void, so nothing has put a status
     * there — and the legacy gate preserves RAX, so what is actually in
     * it is the number of the last syscall the program made. Passing
     * that on made every clean exit report a failure. */
    "  xorl %edi, %edi\n"
    "  movl $4, %eax\n"
    "  int $0x80\n"
    "1:\n"
    "  jmp 1b\n"

    /*
     * ---- the Microsoft calling convention ----
     *
     * A PE image is compiled for a different ABI: its first four
     * arguments arrive in RCX, RDX, R8 and R9, where everything else in
     * this system uses RDI, RSI, RDX and RCX. The stubs below are the
     * translation, and they are why a Windows executable can call into
     * this kernel at all.
     *
     * The order of the moves is load-bearing: RSI must be filled from
     * RDX before RDX is overwritten from R8, or the second argument
     * becomes the third.
     */
    /*
     * ---- and the registers Windows expects back ----
     *
     * The two conventions disagree about who owns the vector registers.
     * System V says every XMM register is the caller's problem and may
     * be destroyed by any call. Microsoft says XMM6 through XMM15
     * belong to the *caller* and a callee must put them back.
     *
     * This kernel is System V throughout, so a syscall destroys all
     * sixteen -- which is correct for everything else here and silently
     * wrong for a PE image. GCC targeting Windows keeps loop constants
     * in XMM6 upwards precisely because it has been promised they
     * survive calls, so the first call inside a loop corrupts the
     * arithmetic of every iteration after it.
     *
     * That is not a theoretical hazard: it is what the first PE this
     * ever ran did, and the fault it produced was a write through an
     * address that had been a floating-point constant.
     *
     * 168 bytes rather than 160 because the stack arrives eight past a
     * sixteen-byte boundary and MOVAPS faults on anything else.
     */
    /* RDI and RSI are callee-saved under Microsoft's convention and
     * argument registers under System V, so the shuffle above destroys
     * two registers the caller expects back. That is what a Windows
     * compiler relies on when it keeps a pointer in RSI across a call --
     * and it is what made the first working PE fault writing through a
     * canvas pointer that had become a fragment of the previous call's
     * arguments. Saved here, before anything touches them. */
    ".macro MSABI_ENTER\n"
    "  subq $184, %rsp\n"
    "  movq %rdi, 160(%rsp)\n"
    "  movq %rsi, 168(%rsp)\n"
    "  movaps %xmm6,    0(%rsp)\n"
    "  movaps %xmm7,   16(%rsp)\n"
    "  movaps %xmm8,   32(%rsp)\n"
    "  movaps %xmm9,   48(%rsp)\n"
    "  movaps %xmm10,  64(%rsp)\n"
    "  movaps %xmm11,  80(%rsp)\n"
    "  movaps %xmm12,  96(%rsp)\n"
    "  movaps %xmm13, 112(%rsp)\n"
    "  movaps %xmm14, 128(%rsp)\n"
    "  movaps %xmm15, 144(%rsp)\n"
    ".endm\n"

    ".macro MSABI_LEAVE\n"
    "  movq 160(%rsp), %rdi\n"
    "  movq 168(%rsp), %rsi\n"
    "  movaps   0(%rsp), %xmm6\n"
    "  movaps  16(%rsp), %xmm7\n"
    "  movaps  32(%rsp), %xmm8\n"
    "  movaps  48(%rsp), %xmm9\n"
    "  movaps  64(%rsp), %xmm10\n"
    "  movaps  80(%rsp), %xmm11\n"
    "  movaps  96(%rsp), %xmm12\n"
    "  movaps 112(%rsp), %xmm13\n"
    "  movaps 128(%rsp), %xmm14\n"
    "  movaps 144(%rsp), %xmm15\n"
    "  addq $184, %rsp\n"
    ".endm\n"
    ".macro MSABI_SHUFFLE\n"
    "  movq %rcx, %rdi\n"
    "  movq %rdx, %rsi\n"
    "  movq %r8,  %rdx\n"
    "  movq %r9,  %r10\n"
    ".endm\n"


    ".align 16\n"
    ".globl petramp_print\n"
    "petramp_print:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $1, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_exit\n"
    "petramp_exit:\n"
    "  MSABI_SHUFFLE\n"
    "  movl $4, %eax\n"
    "  syscall\n"
    "1:\n"
    "  jmp 1b\n"

    ".align 16\n"
    ".globl petramp_pixel\n"
    "petramp_pixel:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $2, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_mouse\n"
    "petramp_mouse:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $3, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_yield\n"
    "petramp_yield:\n"
    "  MSABI_ENTER\n"
    "  movl $7, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_canvas\n"
    "petramp_canvas:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $9, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_ticks\n"
    "petramp_ticks:\n"
    "  MSABI_ENTER\n"
    "  movl $8, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_textw\n"
    "petramp_textw:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $20, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_sbrk\n"
    "petramp_sbrk:\n"
    "  MSABI_ENTER\n"
    "  MSABI_SHUFFLE\n"
    "  movl $5, %eax\n"
    "  syscall\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    /*
     * The two eight-argument calls, in the Microsoft convention: four
     * in registers and the rest on the stack at [rsp+40] onwards, past
     * the thirty-two bytes of shadow space the caller must reserve.
     */
    ".align 16\n"
    ".globl petramp_drawstr\n"
    "petramp_drawstr:\n"
    "  MSABI_ENTER\n"
    "  movq 248(%rsp), %rax\n"
    "  pushq %rax\n"                 /* arg8 */
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"                 /* arg7 */
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"                 /* arg6 */
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"                 /* arg5 */
    "  pushq %r9\n"
    "  pushq %r8\n"
    "  pushq %rdx\n"
    "  pushq %rcx\n"
    "  movq %rsp, %rdi\n"
        "  movl $21, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".align 16\n"
    ".globl petramp_fillrect\n"
    "petramp_fillrect:\n"
    "  MSABI_ENTER\n"
    "  movq 248(%rsp), %rax\n"
    "  pushq %rax\n"
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"
    "  movq 64(%rsp), %rax\n"
    "  pushq %rax\n"
    "  pushq %r9\n"
    "  pushq %r8\n"
    "  pushq %rdx\n"
    "  pushq %rcx\n"
    "  movq %rsp, %rdi\n"
        "  movl $22, %eax\n"
    "  syscall\n"
    "  addq $64, %rsp\n"
    "  MSABI_LEAVE\n"
    "  ret\n"

    ".globl utramp_end\n"
    "utramp_end:\n"
    ".popsection\n"
);

extern uint8_t petramp_print[], petramp_exit[], petramp_pixel[],
               petramp_mouse[], petramp_yield[], petramp_canvas[],
               petramp_ticks[], petramp_textw[], petramp_sbrk[],
               petramp_drawstr[], petramp_fillrect[];

extern uint8_t utramp_start[], utramp_end[];
extern uint8_t utramp_ttf_text_width[], utramp_ttf_draw_string[],
               utramp_gfx_rect[], utramp_exit[];

/* Where a stub ends up once the page is mapped into a process. */
static inline uint64_t utramp_user_addr(const uint8_t *stub) {
    return USER_TRAMP_VA + (uint64_t)(stub - utramp_start);
}

/*
 * Program the MSRs.
 *
 * STAR's two halves are read by different instructions and mean
 * different things: [47:32] is the code selector SYSCALL loads, and
 * [63:48] is the base SYSRET does arithmetic on. Getting the second one
 * wrong is not diagnosable from the fault it produces, which is a #GP at
 * an address in user space with no obvious cause.
 *
 * FMASK clears more than the interrupt flag. DF matters because the C
 * ABI says string operations start forward and a user program can leave
 * it set; TF because a single-step flag surviving into ring 0 turns
 * every kernel instruction into a debug exception; AC because alignment
 * checking enabled by a user is not something kernel code is written to
 * survive. NT and IOPL follow for the same reason.
 */
static void syscall_init(void) {
    /* SCE enables the SYSCALL instruction. NXE enables bit 63 of a page
     * table entry to mean "no execute"; without it that bit is reserved
     * and setting it makes every access to the page a reserved-bit page
     * fault — so the no-execute mappings the loader creates depend on
     * this line as much as on the loader. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | (1ULL << 0) | (1ULL << 11));
    wrmsr(MSR_STAR, (STAR_SYSRET_BASE << 48) | (STAR_SYSCALL_CS << 32));
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200 | 0x400 | 0x100 | 0x40000 | 0x4000 | 0x3000);

    syscall_kstack = tss.rsp0;

    /* And the legacy door, at DPL 3 so ring 3 can actually knock. */
    idt_set_gate_ex(0x80, (void *)(uintptr_t)int80_stub, GDT_KCODE, 0, 3);

    serial_puts("[syscall] SYSCALL/SYSRET armed, int 0x80 gate open to ring 3\n");
}

#endif /* SYSCALL_H */
