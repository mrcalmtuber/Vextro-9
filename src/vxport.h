#ifndef VXPORT_H
#define VXPORT_H

/*
 * src/vxport.h — the whole of what the vendored code may reach.
 *
 * lwIP and Mbed TLS are compiled as their own translation units, the
 * way src/llm.c already is. They have to be: the kernel is one enormous
 * unit of `static` functions, and `static` is exactly the property that
 * makes it unreachable from anywhere else. Nothing in third_party/ can
 * call kmalloc, or the scheduler, or the e1000 driver, because from
 * outside kernel.c those names do not exist.
 *
 * So this file is the contract, and it is deliberately small. Twenty-odd
 * functions, native types, no lwIP header and no Mbed TLS header
 * anywhere in it -- which is what lets kernel.c include it without
 * inheriting `struct sockaddr`, `fd_set`, or lwIP's habit of
 * macro-defining htons() over the top of the one in netstack.h.
 *
 * Read in the other direction it is also the audit: this is every way a
 * quarter of a million lines of foreign code can affect this machine.
 * It cannot map a page, it cannot touch the framebuffer, it cannot open
 * a file, and it cannot see a process. It can allocate, block, sleep,
 * read the clock, and put a frame on the wire.
 *
 * The definitions live in src/vxport_impl.h, which kernel.c includes
 * once, where all of those static internals are in scope.
 */

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== memory =====
 *
 * Both libraries allocate. Mbed TLS does it through the calloc/free
 * pair the brief asked us to override; lwIP does it through MEM_LIBC_MALLOC.
 * Both land here, and here is the kernel's own slab allocator, so a
 * TLS session and a window title come out of the same heap and show up
 * in the same accounting.
 */
void    *vx_alloc(uint64_t bytes);
void    *vx_calloc(uint64_t n, uint64_t size);
void     vx_free(void *p);
uint64_t vx_alloc_size(void *p);

/* ===== threads ===== */

/* lwIP's sys_thread_new, in native terms. Returns 0 on failure.
 * `prio` is one of the PRIO_* levels; the glue passes PRIO_NORMAL for
 * worker threads and one step above it for the tcpip thread, which must
 * not be starved by the workers feeding it. */
int      vx_thread_start(void (*fn)(void *), void *arg,
                         const char *name, int prio);
/*
 * A thread that reaches the end of its function has nowhere to return
 * to -- the frame it was started on was written by the scheduler and
 * has no return address in it. Every thread must finish here instead.
 */
void     vx_thread_exit(void);
void     vx_yield(void);
void     vx_sleep_ms(uint32_t ms);
int      vx_thread_id(void);

/* ===== blocking =====
 *
 * The primitive underneath every semaphore, mutex and mailbox in the
 * port. `chan` is any address at all -- it is compared, never read.
 * vx_block returns 1 if it was woken, 0 if `timeout_ms` expired; zero
 * means wait forever.
 */
int      vx_block(void *chan, uint32_t timeout_ms);
void     vx_wake(void *chan, int all);

/*
 * The same, for a caller that already holds interrupts off and needs
 * the test of its condition and the decision to sleep to be one step.
 * Takes ownership of `flags` and restores them. See the note on the
 * lost-wakeup race in sched.h -- every semaphore in the port goes
 * through this one rather than vx_block.
 */
int      vx_block_locked(void *chan, uint32_t timeout_ms, uint64_t flags);

/* Raise and lower interrupts around the few places the port has to be
 * atomic against a handler. Nested, and returns the previous state, so
 * two of these inside one another do not enable interrupts early. */
uint64_t vx_irq_save(void);
void     vx_irq_restore(uint64_t flags);

/* ===== time =====
 *
 * Milliseconds since boot. lwIP's whole timer wheel is driven off this
 * one function, so it must be monotonic and it must not wrap inside a
 * session -- 32 bits of milliseconds is 49 days, which is what lwIP
 * itself assumes and handles.
 */
uint32_t vx_now_ms(void);

/* ===== the wire =====
 *
 * One frame in, one frame out. Deliberately not a ring: lwIP owns its
 * own queueing, and a second layer of buffering underneath it only adds
 * latency and a place for frames to be lost without anyone counting.
 */
int      vx_nic_present(void);
void     vx_nic_mac(uint8_t out[6]);
int      vx_nic_send(const uint8_t *frame, uint16_t len);
int      vx_nic_recv(uint8_t *out, uint16_t max, uint16_t *got);
uint32_t vx_nic_mtu(void);

/* ===== entropy =====
 *
 * RDRAND, and nothing else. Returns the number of bytes actually
 * produced, which is zero on a processor without the instruction --
 * and the caller must treat zero as fatal, because a CSPRNG seeded
 * with a buffer that was never written looks exactly like one that
 * works.
 */
uint32_t vx_random(uint8_t *out, uint32_t len);

/* ===== diagnostics ===== */
void     vx_log(const char *s);
void     vx_log_u32(uint32_t v);

/* ===== floating point =====
 *
 * The brief asks for FXSAVE and FXRSTOR around the handshake, and this
 * is that pair. `area` must be 512 bytes and sixteen-byte aligned.
 *
 * Strictly this is belt and braces. The scheduler already saves 512
 * bytes of extended state on every context switch, and every interrupt
 * handler in this kernel is compiled general-regs-only so that none of
 * them can touch an XMM register in the first place. Between those two
 * there is no path by which a network interrupt can reach the registers
 * a handshake is using.
 *
 * It is here anyway, because it costs about sixty cycles against a
 * handshake that costs millions, and because the guarantee it gives is
 * local: it holds whatever anyone later does to the interrupt handlers,
 * including adding one that is not compiled the careful way.
 */
void     vx_fpu_save(uint8_t *area);
void     vx_fpu_restore(const uint8_t *area);

#ifdef __cplusplus
}
#endif

#endif /* VXPORT_H */
