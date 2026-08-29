/*
 * libc/exit.c — ending a program, and everything that has to happen
 * first.
 *
 * ---- why this is not in crt0.c any more ----
 *
 * It was, and that was correct for exactly as long as there was only one
 * way for a program to end: falling off the end of main, which returns
 * into crt0's _start, which ran the handlers and then stopped.
 *
 * C++ makes it wrong. `exit()` is required to run static destructors,
 * and a C++ program calls exit() from wherever it likes — main is not
 * involved, so crt0 never gets the chance. With the table in crt0.o and
 * exit() in the archive, the only way to fix that would be for a
 * function in the archive to call a symbol that exists only in an object
 * outside it, which is a link failure for every program that does not
 * link crt0 — and threadtest deliberately does not.
 *
 * So the table lives here, in the archive, where exit() can reach it.
 * crt0 keeps what genuinely belongs to it: setting up thread-local
 * storage, walking .init_array, and calling main.
 *
 * ---- the order, and why it is that order ----
 *
 *   1. the atexit handlers, in reverse
 *   2. .fini_array, in reverse
 *   3. the process ends
 *
 * Reverse in both cases, and it is not arbitrary: a handler registered
 * later may depend on something an earlier one set up, so unwinding in
 * registration order would tear down a dependency before its dependent.
 * The C++ runtime registers *one* handler — see libcxx/src/cxa.cpp —
 * which then runs every static destructor in its own reverse order, so
 * the same rule holds one level down.
 */

#include <stdlib.h>
#include <unistd.h>

#define ATEXIT_MAX 32

static void (*atexit_fns[ATEXIT_MAX])(void);
static int    atexit_count = 0;

/* Set while the handlers are running. exit() called from inside one must
 * not run the table a second time -- which is what the standard means by
 * "the behaviour is undefined" and what every implementation turns into
 * "ends the process without recursing". */
static int exiting = 0;

int atexit(void (*fn)(void)) {
    if (!fn || atexit_count >= ATEXIT_MAX) return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

/* Supplied by the linker script; empty in a program with no static
 * destructors, in which case the two are equal and the loop does
 * nothing. */
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);

void __libc_run_exit_handlers(void) {
    if (exiting) return;
    exiting = 1;

    for (int i = atexit_count - 1; i >= 0; i--)
        if (atexit_fns[i]) atexit_fns[i]();

    for (void (**p)(void) = __fini_array_end; p > __fini_array_start; )
        (*--p)();
}

void exit(int status) {
    __libc_run_exit_handlers();
    _exit(status);
}
