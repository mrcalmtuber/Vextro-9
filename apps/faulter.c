/*
 * faulter — a program that does something it is not allowed to do.
 *
 * Every other application here is written to work. This one exists to
 * fail, because the interesting property of a protected system is not
 * that correct programs run, it is that incorrect ones are contained:
 * the thread dies, the fault is reported with the address that caused
 * it, and the machine keeps drawing.
 *
 * That path had never been executed before this file was written. It is
 * short, it is on the disk, and running it is the only way to know.
 */

#include "vextro.h"

void _start(void) {
    os_print("faulter: about to write through a null pointer\n");

    /*
     * Page zero is deliberately left unmapped by the loader, which is
     * why this is a fault and not a silent write into whatever the image
     * happens to start with. `volatile` so the compiler cannot decide a
     * store nobody reads is a store it need not make.
     */
    volatile unsigned int *nowhere = (volatile unsigned int *)0;
    *nowhere = 0xDEADBEEFu;

    os_print("faulter: still running, which should be impossible\n");
}
