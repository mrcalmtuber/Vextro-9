#ifndef _UCONTEXT_H
#define _UCONTEXT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ucontext.h — the register file a signal handler is handed, and is
 * allowed to change.
 *
 * ---- why this exists at all ----
 *
 * A three-argument handler's third parameter is a `void *` by POSIX, and
 * a program that only wants to know *that* a signal arrived never looks
 * at it. The programs that do look are the ones this system is being
 * built for: a just-in-time compiler catches SIGSEGV, decides whether
 * the fault was one it planted, and — this is the part that needs a
 * layout — **assigns `uc_mcontext.gregs[REG_RIP]` and returns**, so that
 * execution continues at a bail-out stub instead of re-running the
 * instruction that faulted forever.
 *
 * src/sched/vls_core.c honours that: rt_sigreturn restores from the
 * ucontext rather than from a private snapshot, precisely so the
 * assignment means something. A private snapshot would have been simpler
 * and would have silently ignored it.
 *
 * ---- the layout is not ours to choose ----
 *
 * Every offset below is glibc's, because this is a structure a *program*
 * indexes by a constant it was compiled against elsewhere. REG_RIP is 16
 * on every x86-64 system there is, and a handler that reads gregs[16]
 * has to find RIP there. The static assertions are the check; they are
 * the same discipline vx_stat_t is held to across the ring boundary,
 * applied to a structure that crosses no boundary at all and still must
 * not drift.
 *
 * ---- what is not filled in ----
 *
 * `fpregs` is null, and Linux does the same when there is no extended
 * state to record. Nothing is lost: a signal is delivered at a system
 * call or at a fault, and the thread's floating-point state is in
 * thread_t.fx either way — saved by the context switch and restored
 * underneath the handler and the resumed code alike. A handler that
 * clobbers XMM0 clobbers it for the interrupted code too, exactly as
 * calling any other function would.
 *
 * There is also no makecontext/swapcontext here. Those are a
 * coroutine facility that happens to share a structure name, they are
 * obsolescent in POSIX, and nothing that needs the register file needs
 * them. Absent rather than stubbed.
 */

#include <signal.h>
#include <stddef.h>

typedef long long greg_t;

#define REG_R8       0
#define REG_R9       1
#define REG_R10      2
#define REG_R11      3
#define REG_R12      4
#define REG_R13      5
#define REG_R14      6
#define REG_R15      7
#define REG_RDI      8
#define REG_RSI      9
#define REG_RBP     10
#define REG_RBX     11
#define REG_RDX     12
#define REG_RAX     13
#define REG_RCX     14
#define REG_RSP     15
#define REG_RIP     16
#define REG_EFL     17
#define REG_CSGSFS  18
#define REG_ERR     19
#define REG_TRAPNO  20
#define REG_OLDMASK 21
#define REG_CR2     22
#define NGREG       23

typedef greg_t gregset_t[NGREG];

typedef struct {
    gregset_t      gregs;          /*   0 .. 183 */
    void          *fpregs;         /* 184 -- null; see above  */
    unsigned long  __reserved[8];  /* 192 .. 255 */
} mcontext_t;

typedef struct {
    void         *ss_sp;
    int           ss_flags;
    size_t        ss_size;
} stack_t;

#define SS_ONSTACK 1
#define SS_DISABLE 2

typedef struct ucontext_t {
    unsigned long      uc_flags;      /*   0 */
    struct ucontext_t *uc_link;       /*   8 */
    stack_t            uc_stack;      /*  16 .. 39 */
    mcontext_t         uc_mcontext;   /*  40 .. 295 */
    unsigned long      uc_sigmask[16];/* 296 .. 423 -- 1024 bits, as Linux */
    unsigned char      __fpstate[512];/* 424 .. 935 */
    unsigned long      __reserved[4]; /* 936 .. 967 */
} ucontext_t;

_Static_assert(sizeof(mcontext_t) == 256, "mcontext_t is 256 bytes");
_Static_assert(sizeof(ucontext_t) == 968, "ucontext_t is 968 bytes");
_Static_assert(__builtin_offsetof(ucontext_t, uc_mcontext) == 40,
               "uc_mcontext must be at 40, where a handler looks for it");
_Static_assert(__builtin_offsetof(ucontext_t, uc_sigmask) == 296,
               "uc_sigmask must be at 296");

#ifdef __cplusplus
}
#endif

#endif /* _UCONTEXT_H */
