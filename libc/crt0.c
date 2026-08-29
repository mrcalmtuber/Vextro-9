/*
 * libc/crt0.c — what runs before main.
 *
 * Vextro applications have never had a crt0. A program defines _start,
 * the loader jumps to it, and falling off the end lands on an exit stub
 * the loader put on the stack — which is a complete and perfectly good
 * startup contract for a program that needs nothing set up.
 *
 * Two things now need setting up before the first line of a program can
 * run, and neither can be done lazily:
 *
 * Thread-local storage. A `__thread` or `thread_local` variable compiles
 * to a load through the FS segment. The compiler emits that instruction
 * directly, so there is no library call to intercept and no way to
 * initialise on first use — the segment base has to be right before the
 * access, not at it. A program that uses one without this runs until it
 * touches the variable and then faults on a wild address.
 *
 * Static constructors. C++ requires every namespace-scope object to be
 * constructed before main, and the compiler records them in .init_array
 * for something to walk. Nothing walked it here, because there was no
 * C++ and nothing to walk it with, so a program with a global object
 * would enter main holding uninitialised memory that looked initialised.
 *
 * Programs that need neither keep their own _start and are unaffected;
 * every application written for this system before threads does exactly
 * that and none of them links against this file.
 */

#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>

extern int main(int argc, char **argv, char **envp);

/*
 * The constructor and destructor arrays, as the linker script lays them
 * out. Both are empty in a C program and the loops below do nothing;
 * the symbols exist unconditionally so that this file compiles whether
 * or not anything registered.
 */
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);

/*
 * ---- what used to be here ----
 *
 * The atexit table and the C++ ABI's __cxa_atexit moved out, and it is
 * worth saying where and why rather than leaving a gap.
 *
 * The atexit machinery went to libc/exit.c, because exit() has to be
 * able to run it and exit() is in the archive: a C++ program calls
 * exit() from wherever it likes rather than by returning from main, and
 * static destructors have to run when it does.
 *
 * __cxa_atexit, __dso_handle, __cxa_pure_virtual and
 * __cxa_deleted_virtual went to libcxx/src/cxa.cpp, which is where they
 * belong now that there is a C++ runtime to put them in. They were here
 * as placeholders for the day one existed. That day has arrived, and a C
 * program that links crt0 no longer carries four symbols it can never
 * reference.
 */

void __libc_run_exit_handlers(void);

/*
 * The entry point.
 *
 * There is no argument vector: nothing on this system passes one, and
 * the loader's contract is a jump to _start with an exit stub on the
 * stack as the return address. main gets a program name and a null
 * terminator, which is what a program that inspects argv[0] expects to
 * find and is more useful than an empty vector.
 */
static char  argv0[] = "vx";
static char *argv[]  = { argv0, 0 };
static char *envp[]  = { 0 };

void _start(void) {
    /* Before anything else, and before any constructor: a constructor is
     * ordinary code and may touch a thread-local or set errno. */
    __libc_init_tls();

    for (void (**p)(void) = __init_array_start; p < __init_array_end; p++)
        (*p)();

    int rc = main(1, argv, envp);

    /* exit() runs the handlers itself now, so returning from main and
     * calling exit() take the same path -- which is what the standard
     * says and what a C++ program depends on. */
    exit(rc);
}
