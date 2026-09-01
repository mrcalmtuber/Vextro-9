#ifndef VEXTRO_VLS_H
#define VEXTRO_VLS_H

/*
 * include/vls.h — the Vextro Linux Subset: what a Linux binary is
 * allowed to ask for, and in what shape the answer comes back.
 *
 * The implementation is src/sched/vls_core.c, compiled as its own
 * object beside the scheduler, NTFS and the scanner. This file is the
 * seam: the numbers, the structures that cross the ring boundary, the
 * hook table the router calls back through, and nothing else.
 *
 * ---- what this is for ----
 *
 * WebKit is not one process. It is a UI process that forks a web
 * process and a network process, hands them descriptors, waits on them,
 * and expects a signal when one dies. None of that is expressible in
 * this kernel's own system calls, which grew from a single-program
 * machine outwards: there is a fork, and until now there was no exec, no
 * wait, no kill, and no way for a program to be told anything
 * asynchronously at all.
 *
 * So this is a translation layer and deliberately not an emulation
 * layer. Nothing here interprets Linux instructions or simulates a Linux
 * kernel; it takes a system call in Linux's numbering and argument
 * order, checks it, and either forwards it to the native service that
 * already does that job or implements the part Linux has and this system
 * did not. The subset is the useful half of the ABI and is named a
 * subset because pretending otherwise would be a promise that broke on
 * the first call outside it.
 *
 * ---- the honest scope, stated once ----
 *
 * `make webkit` stops at OptionsWPE.cmake:10 on JPEG, which is a
 * missing-library problem and not a process-model one. VLS does not move
 * that line and is not meant to: it is the rung *after* the libraries,
 * and it exists now because every one of the calls below is something
 * the WebKit process model needs and this machine could not do at all.
 *
 * ---- two doors into the same room ----
 *
 * A Linux program and a Vextro program both issue `syscall`, and their
 * numbers collide: 1 is SYS_PRINT here and `write` there. Something has
 * to say which numbering a given call is written in, and there are two
 * mechanisms because they answer two different situations.
 *
 *   The bias. A call number at or above VLS_CALL_BIAS is a Linux number
 *   with the bias added. Unambiguous, needs no state, and works from a
 *   program that is otherwise native — which is what makes the whole
 *   thing testable from a Vextro app that still wants to print.
 *
 *   The personality. VLS_PERSONALITY_LINUX on a process says every
 *   unbiased number from it is a Linux number. This is what an actual
 *   Linux ELF needs, because its calls were compiled long before it got
 *   here and cannot be biased. It is a one-way door on purpose: from
 *   the instant it is set, that process's native numbers are gone.
 *
 * The two are not redundant and the signal trampoline is the proof. It
 * lives on the shared trampoline page, is the same instructions in every
 * process, and has to work in both — so it issues the *biased* number
 * for sigreturn, which means exactly one stub serves a native process
 * that caught a signal and a Linux one that did.
 */

#include <stdint.h>
#include "kernel_shared.h"

/* ============================================================
 *  1. the two doors
 * ============================================================
 *
 * The bias is a bit rather than an offset so that the arithmetic is
 * exact in both directions and a Linux number can never be mistaken for
 * a native one by an off-by-one. Bit 30 is chosen because the whole
 * Linux table fits far below it and because a program that gets here
 * with a call number in the billions has already gone wrong in a way
 * worth reporting rather than silently masking.
 */
#define VLS_CALL_BIAS   0x40000000u

/* Highest Linux number the table may hold. Linux's x86-64 table ends
 * around 460; the bound is what keeps a stray number out of the search
 * and out of the report. */
#define VLS_NR_MAX      512

#define VLS_PERSONALITY_NATIVE 0
#define VLS_PERSONALITY_LINUX  1

/* ============================================================
 *  2. Linux x86_64 system call numbers
 * ============================================================
 *
 * Only the ones the table below actually has an entry for. A number
 * with no entry is not missing from this list by accident — the list is
 * the list of things that work, and a name here with no implementation
 * behind it would be the stub this file exists not to have.
 */
#define LNX_read              0
#define LNX_write             1
#define LNX_open              2
#define LNX_close             3
#define LNX_stat              4
#define LNX_fstat             5
#define LNX_lstat             6
#define LNX_lseek             8
#define LNX_mmap              9
#define LNX_mprotect         10
#define LNX_munmap           11
#define LNX_brk              12
#define LNX_rt_sigaction     13
#define LNX_rt_sigprocmask   14
#define LNX_rt_sigreturn     15
#define LNX_ioctl            16
#define LNX_readv            19
#define LNX_writev           20
#define LNX_access           21
#define LNX_sched_yield      24
#define LNX_nanosleep        35
#define LNX_getpid           39
#define LNX_socket           41
#define LNX_socketpair       53
#define LNX_connect          42
#define LNX_sendto           44
#define LNX_recvfrom         45
#define LNX_shutdown         48
#define LNX_setsockopt       54
#define LNX_clone            56
#define LNX_fork             57
#define LNX_vfork            58
#define LNX_execve           59
#define LNX_exit             60
#define LNX_wait4            61
#define LNX_kill             62
#define LNX_uname            63
#define LNX_fcntl            72
#define LNX_fsync            74
#define LNX_ftruncate        77
#define LNX_getcwd           79
#define LNX_mkdir            83
#define LNX_unlink           87
#define LNX_readlink         89
#define LNX_gettimeofday     96
#define LNX_getuid          102
#define LNX_getgid          104
#define LNX_geteuid         107
#define LNX_getegid         108
#define LNX_getppid         110
#define LNX_dup              32
#define LNX_dup2             33
#define LNX_rmdir            84
#define LNX_sigaltstack     131
#define LNX_arch_prctl      158
#define LNX_gettid          186
#define LNX_tkill           200
#define LNX_time            201
#define LNX_futex           202
#define LNX_getdents64      217
#define LNX_set_tid_address 218
#define LNX_clock_gettime   228
#define LNX_exit_group      231
#define LNX_tgkill          234
#define LNX_openat          257
#define LNX_newfstatat      262
#define LNX_unlinkat        263
#define LNX_pipe             22
#define LNX_poll              7
#define LNX_pipe2           293
#define LNX_ppoll           271
#define LNX_getrandom       318

/* arch_prctl subfunctions. Only the two that touch FS, because that is
 * the only one of the four segment bases this machine gives a program
 * any say over — see SYS_SET_FSBASE. */
#define LNX_ARCH_SET_FS  0x1002
#define LNX_ARCH_GET_FS  0x1003

/* clone flags, and only the ones that change what is built. A flag not
 * listed here is not silently ignored: the router refuses a clone whose
 * flags it does not wholly understand, because a thread that shares
 * three things out of the four it was asked to share is worse than no
 * thread at all. */
#define LNX_CSIGNAL             0x000000ffu
#define LNX_CLONE_VM            0x00000100u
#define LNX_CLONE_FS            0x00000200u
#define LNX_CLONE_FILES         0x00000400u
#define LNX_CLONE_SIGHAND       0x00000800u
#define LNX_CLONE_THREAD        0x00010000u
#define LNX_CLONE_SYSVSEM       0x00040000u
#define LNX_CLONE_SETTLS        0x00080000u
#define LNX_CLONE_PARENT_SETTID 0x00100000u
#define LNX_CLONE_CHILD_CLEARTID 0x00200000u
#define LNX_CLONE_CHILD_SETTID  0x01000000u
#define LNX_CLONE_DETACHED      0x00400000u
#define LNX_CLONE_UNTRACED      0x00800000u
#define LNX_CLONE_IO            0x80000000u

/* wait4 options, and what a status word means. Encoded exactly as
 * Linux encodes it, because the macros a ported program uses to take it
 * apart are its own and were compiled against that encoding. */
#define LNX_WNOHANG     1
#define LNX_WUNTRACED   2

/* openat and friends take a directory descriptor. This system has one
 * working directory per machine rather than one per process, so the only
 * value that can be honoured is the one that means "the working
 * directory", and anything else is refused rather than quietly treated
 * as if it were. */
#define LNX_AT_FDCWD  (-100)

/* ============================================================
 *  2b. the native numbers this file forwards to
 * ============================================================
 *
 * A second copy of numbers that src/syscall.h already defines, and the
 * duplication is deliberate and checked rather than tolerated.
 *
 * src/syscall.h cannot be included here: it defines variables, emits the
 * two assembly entry stubs, and forward-declares a static function, so a
 * second translation unit that included it would not link. And a module
 * that reached into a kernel-proper header would be the exact violation
 * the Makefile's note beside KERN_MODULES warns about.
 *
 * What makes two lists safe is that they are proved equal where both are
 * visible: src/syscall.h carries a _Static_assert per line below, so a
 * number changed in one place and not the other is a compile error at
 * the point of the change rather than a call that quietly does something
 * else. That is the same trick vx_stat_t uses across the ring boundary,
 * applied across an object boundary.
 */
#define VXN_PRINT        1
#define VXN_EXIT         4
#define VXN_SBRK         5
#define VXN_WRITE        6
#define VXN_YIELD        7
#define VXN_TICKS        8
#define VXN_FORK        23
#define VXN_RANDOM      25
#define VXN_FUTEX       26
#define VXN_MMAP        30
#define VXN_MUNMAP      31
#define VXN_MPROTECT    32
#define VXN_THREAD_EXIT 34
#define VXN_GETTID      35
#define VXN_SET_FSBASE  36
#define VXN_CLOCK       37
#define VXN_EXIT_GROUP  38
#define VXN_NANOSLEEP   39
#define VXN_OPEN        40
#define VXN_READ        41
#define VXN_CLOSE       42
#define VXN_LSEEK       43
#define VXN_STAT        44
#define VXN_FSTAT       45
#define VXN_GETDENTS    46
#define VXN_UNLINK      47
#define VXN_MKDIR       48
#define VXN_FSYNC       49
#define VXN_FTRUNCATE   50
#define VXN_SOCKET      51
#define VXN_CONNECT     52
#define VXN_SEND        54
#define VXN_RECV        55
#define VXN_SOCKOPT     56
#define VXN_SHUTDOWN    57
#define VXN_WALLCLOCK   59
#define VXN_EXECVE      60
#define VXN_WAIT4       61
#define VXN_DUP         62
#define VXN_DUP2        63
#define VXN_PERSONALITY 64
#define VXN_PIPE2       65
#define VXN_POLL        66
#define VXN_SOCKETPAIR  67

/* The error numbers used here, which are Linux's, which are the ones
 * src/syscall.h calls VXE_* and libc/include/errno.h calls E*. Three
 * copies now; the reason is the one src/syscall.h gives at the point it
 * makes the second — a header shared across a privilege boundary is a
 * header a program could edit — and it applies again across the object
 * boundary. Only the codes something below can actually return are
 * listed. */
#define VLS_EPERM         1
#define VLS_ENOENT        2
#define VLS_ESRCH         3
#define VLS_EINTR         4
#define VLS_EIO           5
#define VLS_EBADF         9
#define VLS_ECHILD       10
#define VLS_EAGAIN       11
#define VLS_ENOMEM       12
#define VLS_EACCES       13
#define VLS_EFAULT       14
#define VLS_EBUSY        16
#define VLS_EEXIST       17
#define VLS_ENODEV       19
#define VLS_ENOTDIR      20
#define VLS_EISDIR       21
#define VLS_EINVAL       22
#define VLS_ENOTTY       25
#define VLS_ESPIPE       29
#define VLS_EROFS        30
#define VLS_ERANGE       34
#define VLS_ENAMETOOLONG 36
#define VLS_ENOSYS       38
#define VLS_EOVERFLOW    75
#define VLS_EOPNOTSUPP   95

/* Everything in [-4095, -1] is an error and everything else is a
 * result, which is the convention both sides of this already use. */
#define VLS_ERRNO_MAX  4095

/* ============================================================
 *  3. signals
 * ============================================================ */

#define VLS_SIGHUP     1
#define VLS_SIGINT     2
#define VLS_SIGQUIT    3
#define VLS_SIGILL     4
#define VLS_SIGTRAP    5
#define VLS_SIGABRT    6
#define VLS_SIGBUS     7
#define VLS_SIGFPE     8
#define VLS_SIGKILL    9
#define VLS_SIGUSR1   10
#define VLS_SIGSEGV   11
#define VLS_SIGUSR2   12
#define VLS_SIGPIPE   13
#define VLS_SIGALRM   14
#define VLS_SIGTERM   15
#define VLS_SIGCHLD   17
#define VLS_SIGCONT   18
#define VLS_SIGSTOP   19
#define VLS_SIGWINCH  28

/* 1..64 inclusive, so the arrays are 65 long and index zero is never
 * used. Keeping the wasted slot is worth not writing `sig - 1` in
 * eleven places, which is where an off-by-one would eventually live. */
#define VLS_NSIG      65

#define VLS_SIG_DFL   0ull
#define VLS_SIG_IGN   1ull

/* sigaction flags. SA_RESTORER is the one that matters most here and is
 * the least obvious: Linux has no kernel-provided signal trampoline on
 * x86-64, so libc supplies one and sets this flag. This system does
 * provide one, on the shared trampoline page, and uses the program's
 * only when it asked for it. */
#define VLS_SA_NOCLDSTOP 0x00000001u
#define VLS_SA_NOCLDWAIT 0x00000002u
#define VLS_SA_SIGINFO   0x00000004u
#define VLS_SA_RESTART   0x10000000u
#define VLS_SA_NODEFER   0x40000000u
#define VLS_SA_RESETHAND 0x80000000u
#define VLS_SA_RESTORER  0x04000000u

/* rt_sigprocmask's `how`. */
#define VLS_SIG_BLOCK    0
#define VLS_SIG_UNBLOCK  1
#define VLS_SIG_SETMASK  2

/* si_code values for the two faults that actually reach a handler
 * here. A crash reporter reads this to say "address not mapped" rather
 * than "permissions", and both are things the page-fault error code
 * already told the trap handler. */
#define VLS_SEGV_MAPERR  1
#define VLS_SEGV_ACCERR  2
#define VLS_SI_USER      0
#define VLS_SI_KERNEL    0x80
#define VLS_CLD_EXITED   1
#define VLS_CLD_KILLED   2

/* A mask with just this signal in it. Signal 64 is the top, so the
 * shift never reaches the width of the type. */
#define VLS_SIGBIT(s)  (1ull << ((s) - 1))

/* SIGKILL and SIGSTOP cannot be caught, blocked or ignored, and this is
 * the one place that fact is written down. */
#define VLS_SIG_UNBLOCKABLE  (VLS_SIGBIT(VLS_SIGKILL) | VLS_SIGBIT(VLS_SIGSTOP))

/*
 * What a Linux program passes to rt_sigaction. Fixed layout, crossing a
 * privilege boundary, so it is asserted rather than assumed — the same
 * discipline vx_stat_t and vx_dirent_t are held to in src/syscall.h.
 */
typedef struct {
    uint64_t handler;     /* VLS_SIG_DFL, VLS_SIG_IGN, or a user address */
    uint64_t flags;       /* VLS_SA_*                                    */
    uint64_t restorer;    /* the program's own trampoline, or zero       */
    uint64_t mask;        /* blocked for the duration of the handler     */
} vls_sigaction_t;
_Static_assert(sizeof(vls_sigaction_t) == 32,
               "struct sigaction crosses the boundary");

/*
 * siginfo_t, to the extent anything reads it.
 *
 * Linux's is 128 bytes with a union in it whose members overlap at
 * well-known offsets. Only three of those offsets are ever read by the
 * code this system has to satisfy: si_signo, si_code, and — for a
 * SIGSEGV — si_addr, which is what a JIT's fault handler compares
 * against its own code buffer to decide whether the fault was one it
 * planted. So the three are placed exactly where Linux places them and
 * the rest is zero, which is a truthful siginfo rather than a partial
 * one: every field a program can read has the value it should have.
 */
typedef struct {
    int32_t  si_signo;        /*  0 */
    int32_t  si_errno;        /*  4 */
    int32_t  si_code;         /*  8 */
    int32_t  __pad0;          /* 12 */
    uint64_t si_addr;         /* 16 -- also si_pid/si_uid for a SIGCHLD */
    int32_t  si_status;       /* 24 */
    int32_t  __pad1;          /* 28 */
    uint8_t  __pad2[96];      /* 32..127 */
} vls_siginfo_t;
_Static_assert(sizeof(vls_siginfo_t) == 128, "siginfo_t is 128 bytes");

/*
 * The general registers as a signal handler sees them.
 *
 * The order is Linux's REG_* enumeration and not this kernel's, because
 * this array is the one structure here a *program* indexes: a handler
 * that wants the faulting instruction reads gregs[REG_RIP], and REG_RIP
 * is 16 on every x86-64 system there is.
 */
#define VLS_REG_R8       0
#define VLS_REG_R9       1
#define VLS_REG_R10      2
#define VLS_REG_R11      3
#define VLS_REG_R12      4
#define VLS_REG_R13      5
#define VLS_REG_R14      6
#define VLS_REG_R15      7
#define VLS_REG_RDI      8
#define VLS_REG_RSI      9
#define VLS_REG_RBP     10
#define VLS_REG_RBX     11
#define VLS_REG_RDX     12
#define VLS_REG_RAX     13
#define VLS_REG_RCX     14
#define VLS_REG_RSP     15
#define VLS_REG_RIP     16
#define VLS_REG_EFL     17
#define VLS_REG_CSGSFS  18
#define VLS_REG_ERR     19
#define VLS_REG_TRAPNO  20
#define VLS_REG_OLDMASK 21
#define VLS_REG_CR2     22
#define VLS_NGREG       23

typedef struct {
    uint64_t gregs[VLS_NGREG];   /*   0 .. 183 */
    uint64_t fpregs;             /* 184 -- null: see the note below     */
    uint64_t __reserved[8];      /* 192 .. 255 */
} vls_mcontext_t;
_Static_assert(sizeof(vls_mcontext_t) == 256, "mcontext_t is 256 bytes");

typedef struct {
    uint64_t       uc_flags;      /*   0 */
    uint64_t       uc_link;       /*   8 */
    uint64_t       ss_sp;         /*  16 */
    int32_t        ss_flags;      /*  24 */
    int32_t        __pad0;        /*  28 */
    uint64_t       ss_size;       /*  32 */
    vls_mcontext_t uc_mcontext;   /*  40 .. 295 */
    uint64_t       uc_sigmask[16];/* 296 .. 423 -- 1024 bits, as Linux  */
    uint8_t        __fpstate[512];/* 424 .. 935 */
    uint64_t       __reserved[4]; /* 936 .. 967 */
} vls_ucontext_t;
_Static_assert(sizeof(vls_ucontext_t) == 968, "ucontext_t is 968 bytes");
_Static_assert(__builtin_offsetof(vls_ucontext_t, uc_mcontext) == 40,
               "uc_mcontext must be at 40, where a handler looks for it");

/*
 * What actually goes on the user stack under a handler.
 *
 * Linux's layout, plus a magic word this kernel checks on the way back.
 * The magic is not paranoia about the program — a program may write
 * whatever it likes to its own stack — it is what turns "sigreturn with
 * a wild pointer" into a refusal that names itself instead of a restore
 * of twenty-three registers read out of arbitrary memory.
 *
 * The floating-point state is *not* saved here and the pointer in
 * mcontext is null, which is what Linux itself does when a process has
 * no FPU state to record. The reason it is safe is specific: a signal is
 * delivered only at a system-call boundary or at a fault, and in both
 * cases the thread's extended state is already in thread_t.fx, saved by
 * the context switch and restored underneath the handler and the
 * resumed code alike. A handler that clobbers XMM0 clobbers it for the
 * interrupted code too — the same as calling any function that does.
 */
#define VLS_SIGFRAME_MAGIC  0x564C5346524D4531ull  /* "VLSFRME1" */

typedef struct {
    uint64_t       magic;
    uint64_t       saved_mask;
    uint64_t       sig;
    uint64_t       __pad;
    vls_siginfo_t  info;
    vls_ucontext_t uc;
    /* Eight bytes of nothing, because glibc's ucontext_t is 968 bytes
     * and 968 is eight past a sixteen-byte boundary. Without this the
     * frame's *size* would be what breaks the alignment the placement
     * arithmetic in vls_build_frame is careful to establish — which is
     * the kind of bug that only shows up on the first handler that
     * happens to use a vector register. */
    uint64_t       __tail;
} vls_sigframe_t;
_Static_assert(sizeof(vls_sigframe_t) % 16 == 0,
               "the frame must not disturb the ABI's stack alignment");
_Static_assert(__builtin_offsetof(vls_sigframe_t, uc) % 16 == 0,
               "the ucontext must itself be sixteen-aligned");

/* ============================================================
 *  4. per-process signal state
 * ============================================================
 *
 * Hung off addr_space_t, not off thread_t, and allocated on first use.
 * That placement is a decision rather than a convenience: in this kernel
 * the address space *is* the process (include/kernel_shared.h says so at
 * the point the descriptor table lands), and POSIX puts dispositions on
 * the process. Two threads of one program share their handlers because
 * they are one program.
 *
 * The blocked mask is here too, and that one is a documented departure:
 * POSIX makes the mask per-thread. Making it per-process here costs a
 * multi-threaded program the ability to give one worker a different mask
 * from another, and buys the property that a signal sent to a process
 * cannot be delivered to a thread that has masked it while a sibling has
 * not — which is the failure a single-threaded port would actually hit.
 * Threads are the newer thing on this machine; the honest per-thread
 * mask is the next rung and is written down here rather than discovered.
 */
#define VLS_MAX_CHILDREN 16

typedef struct {
    uint32_t pid;
    int32_t  status;      /* Linux's encoding: see vls_encode_status */
    uint8_t  used;
} vls_child_t;

struct vls_sig {
    vls_sigaction_t act[VLS_NSIG];
    uint64_t        blocked;
    uint64_t        pending;
    /* Where a fault's address goes between the trap handler noticing it
     * and the frame being built. One word rather than a queue, because
     * a synchronous fault is delivered on the way out of the fault that
     * raised it and never outlives it. */
    uint64_t        fault_addr;
    uint32_t        fault_code;
    /* Children that have ended and not yet been waited for. A fixed
     * array rather than a list: sixteen is more simultaneous unreaped
     * children than a machine with SCHED_MAX_THREADS threads can have,
     * and a fixed array cannot fail to allocate inside a reap. */
    vls_child_t     child[VLS_MAX_CHILDREN];
    /*
     * Which signal ended this process, or zero if it exited.
     *
     * Recorded here rather than folded into thread_t.exit_code because
     * a status word has to be able to say "killed by 9" and "exited
     * with 9", and an exit code has no room left to carry the
     * difference. The reaper reads it out of the dying process's own
     * state, in the window sched_reap leaves open before the address
     * space is destroyed, and hands it to the parent's record.
     */
    uint8_t         killed_by;
    /* Set by set_tid_address, cleared and woken at thread exit, because
     * that is the word a pthread implementation joins on. */
    uint64_t        clear_child_tid;
};
typedef struct vls_sig vls_sig_t;

/* ============================================================
 *  5. a register snapshot, in this kernel's own order
 * ============================================================
 *
 * The first fifteen words are deliberately laid out exactly as
 * syscall_frame_t and exc_frame_t lay them out, so marshalling from
 * either is a copy of fifteen words rather than fifteen assignments
 * that could be mis-ordered. The three after are the ones the two
 * frames keep in different places, which is precisely why they are
 * copied by name.
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rsp, rflags;
    /* Only meaningful when the snapshot came from a fault. */
    uint64_t fault_addr;
    uint64_t trapno, err;
} vls_regs_t;

/* What vls_signal_dispatch decided. */
#define VLS_DELIVER_NONE     0   /* nothing pending; carry on            */
#define VLS_DELIVER_HANDLER  1   /* regs rewritten; return to ring 3     */
#define VLS_DELIVER_FATAL   -1   /* the caller must end this process     */

/* ============================================================
 *  6. the hook table
 * ============================================================
 *
 * Everything below is implemented in src/desktop.h and reached from
 * here through a function pointer, for the reason the Makefile gives
 * beside KERN_MODULES: a module that included a driver header directly
 * would compile, link, and get a private copy of that driver's state.
 * The kernel proper is one translation unit and its service routines
 * are static within it; a pointer set once at boot is the narrowest
 * thing that can cross that line.
 *
 * It is the same shape as sched_reap_hook and for the same reason —
 * the dependency has to point from the composition root into the
 * module, never the other way.
 */
typedef struct {
    /* The native system call switch, entered *below* the routing
     * wrapper so that a translated call cannot be routed a second time
     * and read as a Linux number. */
    uint64_t (*native)(uint64_t num, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5);

    /* Is this range inside user space at all, and is it actually
     * mapped and writable? The second faults a reservation in, which is
     * what makes writing a signal frame onto a freshly mmap'd stack
     * work. */
    int (*range_ok)(uint64_t va, uint64_t len);
    int (*range_mapped)(uint64_t va, uint64_t len, int write);

    /* Bounded copies across the boundary, in both directions. */
    int (*copy_in)(uint64_t uptr, void *dst, uint64_t len);
    int (*copy_out)(uint64_t uptr, const void *src, uint64_t len);
    int (*copy_string)(uint64_t uptr, char *dst, int cap);

    /* Replace this process's image. Returns 0 having rewritten the
     * caller's frame, or a negated error number having changed nothing.
     */
    int64_t (*execve)(uint64_t path, uint64_t argv, uint64_t envp);

    /* Where the signal trampoline is in *this* process. The page is
     * shared but the address is per-process, because the layout is
     * displaced at spawn. */
    uint64_t (*sigreturn_va)(void);

    /* A refusal a person can read, on the same channel every other
     * refusal in this kernel uses. */
    void (*refuse)(const char *what);

    /* This process's identity and its parent's. Zero when there is no
     * process — a boot self-test calling from ring 0. */
    uint32_t (*pid)(void);
    uint32_t (*ppid)(void);
    uint32_t (*uid)(void);
} vls_host_t;

extern vls_host_t vls_host;

/* ============================================================
 *  7. what the module exports
 * ============================================================ */

/* The router. `nr` is a Linux number with the bias already removed. */
uint64_t vls_syscall(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                     uint64_t a3, uint64_t a4, uint64_t a5);

/* Signal state, found or made, for an address space. Returns null only
 * if the allocation failed. */
vls_sig_t *vls_sig_of(addr_space_t *as, int create);
void       vls_sig_free(addr_space_t *as);
int        vls_sig_clone(addr_space_t *child, addr_space_t *parent);
void       vls_sig_reset_for_exec(addr_space_t *as);

/* Post a signal to a process, from a program (kill) or from the kernel
 * (a fault). Returns 0, or a negated error number. */
int  vls_signal_post(addr_space_t *as, int sig, int from_kernel);

/* The synchronous ones. Called by the trap handler with the fault
 * already diagnosed; records the address so the frame can carry it. */
int  vls_signal_fault(addr_space_t *as, int sig, uint64_t addr, uint32_t code,
                      uint64_t trapno, uint64_t err);

/*
 * Deliver, if there is anything to deliver.
 *
 * Called at exactly two points and no others: on the way out of a system
 * call, and on the way out of a ring-3 fault. Both are places where the
 * thread is about to return to user mode with a complete register set in
 * a frame that can be rewritten. The timer interrupt is deliberately not
 * one of them — src/sched/scheduler.c says sched_on_tick is hand-tuned
 * and compiled general-regs-only, and putting a signal check in it would
 * be a new instruction sequence in the most delicate function here.
 *
 * The cost of that choice, stated rather than discovered: a thread
 * parked in a kernel sleep sees a caught signal when it next wakes, not
 * at the moment it is sent, and no system call returns EINTR.
 */
int  vls_signal_dispatch(addr_space_t *as, vls_regs_t *r);

/*
 * Record that a process has ended, so its parent can be told and can
 * wait for it — once, however many of the four ways out it took.
 *
 * Called from every one of them: the two exit calls, a fatal signal, and
 * the reaper as a backstop for a thread that died in a fault and made no
 * call at all. addr_space_t.exit_reported is what makes calling it four
 * times safe, and the note beside that field says why the reaper cannot
 * simply be the only caller.
 */
void vls_report_exit(addr_space_t *as, int exit_code, int killed);

/* The raw recorder underneath it, which does no deduplication. */
void vls_child_exited(uint32_t pid, uint32_t ppid, int exit_code, int killed);

/* wait4, which is native call 61 as well as Linux call 61 — a program
 * that forks natively has to be able to wait natively. */
int64_t vls_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                  uint64_t rusage_ptr);

/* Linux's status encoding, in the one place it is written down. */
static inline int32_t vls_encode_status(int exit_code, int killed) {
    return killed ? (int32_t)(exit_code & 0x7f)
                  : (int32_t)((exit_code & 0xff) << 8);
}

/* Called once from the composition root, after the hooks are set. */
void vls_init(void);

/* For the process panel and the report: how many calls have been
 * translated, and how many were refused for having no translation. */
uint64_t vls_calls_translated(void);
uint64_t vls_calls_refused(void);

#endif /* VEXTRO_VLS_H */
