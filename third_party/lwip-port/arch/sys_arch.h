#ifndef VEXTRO_LWIP_ARCH_SYS_ARCH_H
#define VEXTRO_LWIP_ARCH_SYS_ARCH_H

/*
 * third_party/lwip-port/arch/sys_arch.h — the four types lwIP blocks on.
 *
 * lwIP asks the port for a semaphore, a mutex, a mailbox and a thread
 * handle, and it does not care what they are as long as the functions
 * in src/lwipglue.c can work with them. These are the smallest things
 * that will do the job on this scheduler.
 *
 * The one design decision worth stating: none of them holds a queue of
 * waiting threads. Blocking here is done against a *wait channel* --
 * the address of the object itself -- and the scheduler finds the
 * sleepers by walking its own thread table when someone posts. That
 * costs a pass over sixty-four pointers per wakeup and saves
 * maintaining a linked list from inside interrupt context, which is the
 * kind of list that is correct until the day it is not.
 *
 * `valid` is not decoration. lwIP checks sys_sem_valid() before using a
 * semaphore and will hand back one that was never created if the check
 * is wrong, so the flag has to distinguish "zeroed struct" from "made".
 */

#include <stdint.h>

typedef struct {
    volatile int  count;
    volatile int  valid;
} sys_sem_t;

typedef struct {
    volatile int  locked;
    volatile int  owner;    /* thread id, 0 when free */
    volatile int  valid;
} sys_mutex_t;

/*
 * A ring of void*, allocated at the size lwIP asks for.
 *
 * head and tail are free-running counts rather than wrapped indices, so
 * "how many are queued" is a subtraction and the empty and full cases
 * cannot be confused -- the classic ring bug where head == tail means
 * both.
 */
typedef struct {
    void            **slots;
    volatile uint32_t head;
    volatile uint32_t tail;
    uint32_t          cap;
    volatile int      valid;
} sys_mbox_t;

typedef int sys_thread_t;

#define SYS_MBOX_NULL   ((sys_mbox_t *)0)
#define SYS_SEM_NULL    ((sys_sem_t *)0)

/* lwIP's protection level for SYS_LIGHTWEIGHT_PROT: interrupts off,
 * nested, with the previous flags carried in the value. */
typedef uint64_t sys_prot_t;

/* There is no sys/time.h, so lwIP is told to define struct timeval
 * itself (LWIP_TIMEVAL_PRIVATE defaults to 1, which is what we want). */

#endif /* VEXTRO_LWIP_ARCH_SYS_ARCH_H */
