#ifndef _SETJMP_H
#define _SETJMP_H

/* C++ reaches these now.
 *
 * libcxx/ compiles against this same library, and a C++ compiler mangles
 * every name it sees unless told not to -- so without this the C++ side
 * would fail to link against `malloc` and find `_Z6mallocm` missing.
 * Placed immediately after the include guard rather than after the
 * #includes below it, which is safe here because everything this header
 * includes is either one of the compiler's own type-only headers or one
 * of ours, and both want the same treatment. */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * setjmp.h — a non-local jump.
 *
 * Needed because a great deal of ported C uses it for error handling —
 * libjpeg and libpng both do, and so does every decompressor written
 * before exceptions were portable.
 *
 * What is saved is the callee-saved set and nothing else: RBX, RBP, R12
 * through R15, the stack pointer and the return address. That is
 * sufficient and it is also the whole contract — the standard is
 * explicit that a local variable modified between setjmp and longjmp
 * has an indeterminate value unless it is volatile, and the reason is
 * exactly this: a variable the compiler kept in a caller-saved register
 * is not in the buffer and comes back as whatever it was at the setjmp.
 *
 * The floating-point control word and the vector registers are not
 * saved. Under System V every XMM register is caller-saved, so there is
 * nothing there a longjmp could be expected to restore.
 */

typedef unsigned long jmp_buf[8];
typedef unsigned long sigjmp_buf[8];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

/* There are no signals in this system, so the mask argument has nothing
 * to save and these are the same two functions under other names. */
int  sigsetjmp(sigjmp_buf env, int savemask);
void siglongjmp(sigjmp_buf env, int val) __attribute__((noreturn));


#ifdef __cplusplus
}
#endif

#endif /* _SETJMP_H */
