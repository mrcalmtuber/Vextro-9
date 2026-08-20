/*
 * src/lwipglue.c — lwIP's port to this machine.
 *
 * Two halves, and they are the two halves every lwIP port has:
 *
 *   sys_arch   the operating system underneath it -- threads,
 *              semaphores, mutexes, mailboxes and a clock. lwIP calls
 *              these; it does not care what a thread is.
 *
 *   netif      the network interface above the driver -- one function
 *              that puts a frame on the wire and one thread that takes
 *              frames off it.
 *
 * Everything it reaches for is in src/vxport.h, which is twenty-odd
 * functions and no kernel header at all. That is what keeps lwIP's
 * headers -- which macro-define htons(), and declare a `struct
 * sockaddr` and an `fd_set` -- out of kernel.c, where src/netstack.h
 * has had its own htons() since long before any of this.
 *
 * ---- on blocking ----
 *
 * Every wait here parks the thread on a scheduler wait channel. None of
 * them spins. That distinction is the whole reason this is a port to a
 * real scheduler rather than a poll loop wearing one as a costume: a
 * socket read with nothing to read costs no processor at all, and eight
 * threads waiting on eight connections cost eight times nothing.
 */

#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/dhcp.h"
/* The DHCP_STATE_* constants are in the protocol header, not the API
 * one -- lwip/dhcp.h declares the functions and leaves the state machine
 * to this. */
#include "lwip/prot/dhcp.h"
#include "lwip/dns.h"
#include "lwip/etharp.h"
#include "lwip/timeouts.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/tcp.h"
/* The pcb lists are internal, and counting them is the one thing this
 * file does that reaches past the public API. It is done under the core
 * lock, below, for exactly that reason. */
#include "lwip/priv/tcp_priv.h"
#include "netif/ethernet.h"

#include "vxport.h"
#include "vxnet.h"

#include <stdarg.h>

/* ===========================================================
 * sys_arch: the clock
 * =========================================================== */

u32_t sys_now(void) { return vx_now_ms(); }
u32_t sys_jiffies(void) { return vx_now_ms(); }

void sys_init(void) { }

/* ===========================================================
 * sys_arch: lightweight protection
 *
 * SYS_LIGHTWEIGHT_PROT asks for a way to be atomic against everything,
 * briefly. On one processor that is interrupts off, and the nesting has
 * to work -- lwIP takes this inside code that already holds it -- which
 * is why the previous state travels in the return value rather than
 * being assumed to have been "on".
 * =========================================================== */

sys_prot_t sys_arch_protect(void)          { return (sys_prot_t)vx_irq_save(); }
void sys_arch_unprotect(sys_prot_t pval)   { vx_irq_restore((uint64_t)pval); }

/* ===========================================================
 * sys_arch: semaphores
 * =========================================================== */

err_t sys_sem_new(sys_sem_t *sem, u8_t count) {
    if (!sem) return ERR_ARG;
    sem->count = count;
    sem->valid = 1;
    return ERR_OK;
}

void sys_sem_free(sys_sem_t *sem) {
    if (!sem) return;
    sem->valid = 0;
    /* Anything still parked here would sleep forever otherwise. It will
     * find the semaphore invalid and return an error, which is the
     * right answer to "the thing you were waiting for was destroyed". */
    vx_wake(sem, 1);
}

void sys_sem_signal(sys_sem_t *sem) {
    if (!sem || !sem->valid) return;
    uint64_t f = vx_irq_save();
    sem->count++;
    vx_irq_restore(f);
    /* One waiter, not all. Waking every thread parked on a counting
     * semaphore so that one of them can take the single item is the
     * thundering herd, and it shows up as processor time that scales
     * with the number of idle connections. */
    vx_wake(sem, 0);
}

/*
 * Wait, and report how long it took -- lwIP subtracts this from its own
 * timeouts, so returning a wrong figure makes every timer above it
 * drift.
 *
 * The check and the parking are one atomic step: see the note on the
 * lost-wakeup race in src/sched.h. Doing it the obvious way instead
 * hangs roughly one connection in a few thousand, at a point in the
 * handshake that looks like a network fault.
 */
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout) {
    if (!sem || !sem->valid) return SYS_ARCH_TIMEOUT;
    u32_t start = vx_now_ms();

    for (;;) {
        uint64_t f = vx_irq_save();
        if (sem->count > 0) {
            sem->count--;
            vx_irq_restore(f);
            return vx_now_ms() - start;
        }
        if (!sem->valid) { vx_irq_restore(f); return SYS_ARCH_TIMEOUT; }

        if (timeout) {
            u32_t elapsed = vx_now_ms() - start;
            if (elapsed >= timeout) { vx_irq_restore(f); return SYS_ARCH_TIMEOUT; }
            vx_block_locked(sem, timeout - elapsed, f);
        } else {
            vx_block_locked(sem, 0, f);
        }
    }
}

int  sys_sem_valid(sys_sem_t *sem)        { return sem && sem->valid; }
void sys_sem_set_invalid(sys_sem_t *sem)  { if (sem) sem->valid = 0; }

/* ===========================================================
 * sys_arch: mutexes
 *
 * Not recursive, and lwIP does not need them to be. `owner` is kept so
 * that a thread taking a mutex it already holds is a message on the
 * serial line rather than a machine that stops with no explanation --
 * which is what self-deadlock looks like from the outside.
 * =========================================================== */

err_t sys_mutex_new(sys_mutex_t *mutex) {
    if (!mutex) return ERR_ARG;
    mutex->locked = 0;
    mutex->owner  = 0;
    mutex->valid  = 1;
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t *mutex) {
    if (!mutex) return;
    mutex->valid = 0;
    vx_wake(mutex, 1);
}

void sys_mutex_lock(sys_mutex_t *mutex) {
    if (!mutex || !mutex->valid) return;
    int me = vx_thread_id();

    for (;;) {
        uint64_t f = vx_irq_save();
        if (!mutex->locked) {
            mutex->locked = 1;
            mutex->owner  = me;
            vx_irq_restore(f);
            return;
        }
        if (mutex->owner == me) {
            vx_irq_restore(f);
            vx_log("[lwip] mutex taken twice by one thread\n");
            return;
        }
        vx_block_locked(mutex, 0, f);
    }
}

void sys_mutex_unlock(sys_mutex_t *mutex) {
    if (!mutex || !mutex->valid) return;
    uint64_t f = vx_irq_save();
    mutex->locked = 0;
    mutex->owner  = 0;
    vx_irq_restore(f);
    vx_wake(mutex, 0);
}

int  sys_mutex_valid(sys_mutex_t *mutex)       { return mutex && mutex->valid; }
void sys_mutex_set_invalid(sys_mutex_t *mutex) { if (mutex) mutex->valid = 0; }

/* ===========================================================
 * sys_arch: mailboxes
 *
 * The channel between the tcpip thread and everything else. A packet
 * arriving, a socket call being made and a timer expiring all become a
 * message in one of these.
 *
 * Two wait channels per mailbox, not one: `mbox` for readers waiting on
 * something to arrive and `&mbox->cap` for writers waiting on room.
 * With a single channel a post wakes readers *and* writers, and the
 * writers immediately go back to sleep having done nothing -- which is
 * harmless until the mailbox is full, at which point the reader that
 * would have drained it is competing with sixteen woken writers for the
 * same slot.
 * =========================================================== */

err_t sys_mbox_new(sys_mbox_t *mbox, int size) {
    if (!mbox) return ERR_ARG;
    if (size <= 0) size = 16;
    mbox->slots = (void **)vx_alloc((uint64_t)size * sizeof(void *));
    if (!mbox->slots) return ERR_MEM;
    mbox->head = mbox->tail = 0;
    mbox->cap   = (uint32_t)size;
    mbox->valid = 1;
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t *mbox) {
    if (!mbox) return;
    mbox->valid = 0;
    vx_wake(mbox, 1);
    vx_wake(&mbox->cap, 1);
    if (mbox->slots) { vx_free(mbox->slots); mbox->slots = 0; }
}

static int mbox_try_put(sys_mbox_t *mbox, void *msg) {
    uint64_t f = vx_irq_save();
    if (mbox->head - mbox->tail >= mbox->cap) { vx_irq_restore(f); return 0; }
    mbox->slots[mbox->head % mbox->cap] = msg;
    mbox->head++;
    vx_irq_restore(f);
    vx_wake(mbox, 0);
    return 1;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !mbox->valid) return;
    while (!mbox_try_put(mbox, msg)) {
        if (!mbox->valid) return;
        /* Full. Wait for a reader to make room. Bounded so that a
         * mailbox whose reader has died is a stall someone can see in
         * the log rather than a thread that never returns. */
        uint64_t f = vx_irq_save();
        if (mbox->head - mbox->tail >= mbox->cap)
            vx_block_locked(&mbox->cap, 100, f);
        else
            vx_irq_restore(f);
    }
}

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg) {
    if (!mbox || !mbox->valid) return ERR_VAL;
    return mbox_try_put(mbox, msg) ? ERR_OK : ERR_MEM;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg) {
    return sys_mbox_trypost(mbox, msg);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout) {
    if (!mbox || !mbox->valid) return SYS_ARCH_TIMEOUT;
    u32_t start = vx_now_ms();

    for (;;) {
        uint64_t f = vx_irq_save();
        if (mbox->head != mbox->tail) {
            void *m = mbox->slots[mbox->tail % mbox->cap];
            mbox->tail++;
            vx_irq_restore(f);
            if (msg) *msg = m;
            vx_wake(&mbox->cap, 0);          /* a writer may be waiting */
            return vx_now_ms() - start;
        }
        if (!mbox->valid) { vx_irq_restore(f); return SYS_ARCH_TIMEOUT; }

        if (timeout) {
            u32_t elapsed = vx_now_ms() - start;
            if (elapsed >= timeout) { vx_irq_restore(f); return SYS_ARCH_TIMEOUT; }
            vx_block_locked(mbox, timeout - elapsed, f);
        } else {
            vx_block_locked(mbox, 0, f);
        }
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg) {
    if (!mbox || !mbox->valid) return SYS_MBOX_EMPTY;
    uint64_t f = vx_irq_save();
    if (mbox->head == mbox->tail) { vx_irq_restore(f); return SYS_MBOX_EMPTY; }
    void *m = mbox->slots[mbox->tail % mbox->cap];
    mbox->tail++;
    vx_irq_restore(f);
    if (msg) *msg = m;
    vx_wake(&mbox->cap, 0);
    return 0;
}

int  sys_mbox_valid(sys_mbox_t *mbox)       { return mbox && mbox->valid; }
void sys_mbox_set_invalid(sys_mbox_t *mbox) { if (mbox) mbox->valid = 0; }

/* ===========================================================
 * sys_arch: threads
 *
 * The stack size lwIP asks for is ignored, and saying so is more honest
 * than pretending to honour it: every kernel thread here gets the
 * scheduler's 32 KB with an unmapped guard page beneath it, so an
 * overflow is a page fault naming the thread rather than silent
 * corruption of whatever the allocator handed out next.
 * =========================================================== */

sys_thread_t sys_thread_new(const char *name, lwip_thread_fn function,
                            void *arg, int stacksize, int prio) {
    (void)stacksize;
    return vx_thread_start((void (*)(void *))function, arg, name, prio);
}

/* ===========================================================
 * cc.h's two upcalls
 * =========================================================== */

/*
 * Format arguments are deliberately dropped -- see arch/cc.h. Printing
 * the literal part is enough to locate what lwIP is complaining about,
 * and it keeps a vsnprintf out of the packet path.
 */
int vx_vsnprintf_pub(char *buf, size_t size, const char *fmt, va_list ap);

void vx_lwip_diag(const char *fmt, ...) {
    if (!fmt) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vx_vsnprintf_pub(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vx_log(buf);
}

uint32_t vx_lwip_rand(void) {
    uint32_t v = 0;
    if (vx_random((uint8_t *)&v, 4) == 4) return v;
    /* No RDRAND. Sequence numbers and DNS transaction IDs become
     * guessable, so this is said out loud rather than papered over --
     * but the alternative is no networking at all, which is worse. */
    static uint32_t s = 0x9E3779B9u;
    static int warned = 0;
    if (!warned) {
        warned = 1;
        vx_log("[lwip] no RDRAND: sequence numbers are not unpredictable\n");
    }
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s ^ vx_now_ms();
}

/* ===========================================================
 * netif: the interface on top of the e1000
 * =========================================================== */

static struct netif vx_netif;
static int          vx_netif_up = 0;
static uint8_t      vx_rx_frame[1600];

/*
 * One frame out.
 *
 * lwIP hands over a pbuf chain, which may be several buffers for one
 * frame -- a header pbuf pointing at a payload pbuf is the normal case
 * for TCP. The driver takes one contiguous buffer, so the chain is
 * flattened here.
 */
static err_t vx_linkoutput(struct netif *netif, struct pbuf *p) {
    (void)netif;
    static uint8_t out[1600];
    uint16_t off = 0;

    for (struct pbuf *q = p; q; q = q->next) {
        if (off + q->len > sizeof(out)) {
            LINK_STATS_INC(link.lenerr);
            return ERR_BUF;
        }
        for (u16_t i = 0; i < q->len; i++)
            out[off + i] = ((const uint8_t *)q->payload)[i];
        off += q->len;
    }

    /* A full transmit ring is back-pressure, not a lost frame: lwIP
     * will retransmit if ERR_IF comes back, and dropping it silently
     * turns a busy moment into a stalled connection. */
    if (vx_nic_send(out, off) != 0) {
        LINK_STATS_INC(link.drop);
        return ERR_IF;
    }
    LINK_STATS_INC(link.xmit);
    return ERR_OK;
}

static err_t vx_netif_init_fn(struct netif *netif) {
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->output     = etharp_output;
    netif->linkoutput = vx_linkoutput;
    netif->mtu        = (u16_t)vx_nic_mtu();
    netif->hwaddr_len = 6;
    vx_nic_mac(netif->hwaddr);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_LINK_UP | NETIF_FLAG_IGMP;
#if LWIP_NETIF_HOSTNAME
    netif->hostname = "vextro";
#endif
    return ERR_OK;
}

/*
 * The receive thread.
 *
 * The e1000 in this system is polled rather than interrupt-driven, so
 * something has to ask. It drains the ring completely before sleeping,
 * because under load frames arrive in bursts and returning to sleep
 * after each one turns a burst into a queue that never empties.
 *
 * A millisecond of sleep when the ring is empty is the whole idle cost.
 * The stack this replaces polled once per display frame -- sixteen
 * milliseconds -- so this is both cheaper when idle and sixteen times
 * more responsive when not.
 */
static void vx_rx_thread(void *arg) {
    (void)arg;
    for (;;) {
        int drained = 0;
        for (int i = 0; i < 32; i++) {
            uint16_t len = 0;
            if (!vx_nic_recv(vx_rx_frame, sizeof(vx_rx_frame), &len)) break;
            if (!len) continue;
            drained++;

            struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
            if (!p) { LINK_STATS_INC(link.memerr); continue; }
            pbuf_take(p, vx_rx_frame, len);
            LINK_STATS_INC(link.recv);

            /* tcpip_input rather than netif->input: this is not the
             * tcpip thread, and calling the stack directly from here
             * would race with every socket call in the system. It
             * queues the frame and lets the one thread that owns the
             * stack pick it up. */
            if (tcpip_input(p, &vx_netif) != ERR_OK) pbuf_free(p);
        }
        if (!drained) vx_sleep_ms(1);
    }
}

/* ===========================================================
 * bring-up
 * =========================================================== */

static volatile int vx_tcpip_ready = 0;
static void vx_tcpip_done(void *arg) { (void)arg; vx_tcpip_ready = 1; }

/*
 * Start the stack. Returns 1 if the interface came up.
 *
 * The address is taken by DHCP where there is a server and falls back to
 * the fixed 10.0.2.15 that QEMU's user-mode network hands out anyway --
 * so a machine whose DHCP is slow or absent still has an address rather
 * than no network at all.
 */
int vxnet_init(void) {
    if (!vx_nic_present()) {
        vx_log("[net] no network interface\n");
        return 0;
    }

    tcpip_init(vx_tcpip_done, 0);
    /* tcpip_init spawns the thread; wait for it to say it is running
     * before handing it a netif, because netif_add from here would
     * otherwise race the stack's own initialisation. */
    for (int i = 0; i < 2000 && !vx_tcpip_ready; i++) vx_sleep_ms(1);
    if (!vx_tcpip_ready) {
        vx_log("[net] the stack thread did not start\n");
        return 0;
    }

    /*
     * Everything from here to the unlock is lwIP's *core* API, and with
     * LWIP_TCPIP_CORE_LOCKING it may only be called by the thread that
     * holds the core lock.
     *
     * This is not a theoretical rule and breaking it does not crash.
     * netif_add and netif_set_up appear to work from any thread; what
     * silently does not is the timer registration underneath
     * dhcp_start, which ends up on a list the tcpip thread is
     * concurrently walking. The visible symptom is a DHCP client that
     * sends nothing and times out, on a network with a working server
     * -- which reads as a driver fault and is not one.
     */
    LOCK_TCPIP_CORE();

    ip4_addr_t any; IP4_ADDR(&any, 0, 0, 0, 0);
    if (!netif_add(&vx_netif, &any, &any, &any, 0,
                   vx_netif_init_fn, tcpip_input)) {
        UNLOCK_TCPIP_CORE();
        vx_log("[net] netif_add failed\n");
        return 0;
    }
    netif_set_default(&vx_netif);
    netif_set_up(&vx_netif);
    netif_set_link_up(&vx_netif);

    UNLOCK_TCPIP_CORE();

    /* Started before DHCP: the client cannot get an answer if nothing
     * is taking frames off the ring. */
    vx_thread_start(vx_rx_thread, 0, "net-rx", VXNET_PRIO_RX);

    LOCK_TCPIP_CORE();
    dhcp_start(&vx_netif);
    UNLOCK_TCPIP_CORE();

    /*
     * Waited on without the lock held, because the tcpip thread needs
     * it to do the work being waited for. Holding a lock across a sleep
     * that depends on the lock is the classic way to turn four seconds
     * into forever.
     *
     * The two-part budget is not padding. A plain four-second timeout
     * failed here against a server that was answering perfectly: the
     * exchange completes in about eighty milliseconds, and then lwIP
     * ARPs for the address it was just given to check nobody else is
     * using it. That check is slow by design -- it has to wait long
     * enough for a silent host to speak up -- and it runs *after* the
     * ACK, so a timeout sized for the handshake expires in the middle
     * of it and throws away an address that had already been granted.
     *
     * So: four seconds to hear from a server at all, and once one has
     * answered, twenty more to let the check finish. A machine with no
     * DHCP server still falls through in four.
     */
    int bound = 0;
    for (int i = 0; i < 2400; i++) {
        vx_sleep_ms(10);
        if (dhcp_supplied_address(&vx_netif)) { bound = 1; break; }

        if (i >= 400) {
            struct dhcp *d = netif_dhcp_data(&vx_netif);
            /* Still asking after four seconds, and nobody has replied:
             * there is no server here. */
            if (!d || d->state == DHCP_STATE_INIT ||
                      d->state == DHCP_STATE_SELECTING) break;
        }
    }
    (void)bound;

    if (!dhcp_supplied_address(&vx_netif)) {
        LOCK_TCPIP_CORE();
        dhcp_stop(&vx_netif);
        ip4_addr_t ip, mask, gw;
        IP4_ADDR(&ip,   10, 0, 2, 15);
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gw,   10, 0, 2, 2);
        netif_set_addr(&vx_netif, &ip, &mask, &gw);
        ip_addr_t dns; IP_ADDR4(&dns, 10, 0, 2, 3);
        dns_setserver(0, &dns);
        UNLOCK_TCPIP_CORE();
        /* Which half failed is the whole diagnosis, and the two look
         * identical from outside: frames sent and none received is a
         * network with no server on it, while nothing sent at all is a
         * fault on this machine. */
        vx_log("[net] no DHCP answer after 4s (sent ");
        vx_log_u32(lwip_stats.link.xmit);
        vx_log(", received ");
        vx_log_u32(lwip_stats.link.recv);
        vx_log("); using the fixed address\n");
    }

    vx_netif_up = 1;

    vx_log("[net] lwIP " LWIP_VERSION_STRING " up, address ");
    vxnet_log_ip(ip4_addr_get_u32(netif_ip4_addr(&vx_netif)));
    vx_log("\n");
    return 1;
}

int vxnet_up(void) { return vx_netif_up; }

void vxnet_addr(uint8_t ip[4], uint8_t mask[4], uint8_t gw[4]) {
    uint32_t a = ip4_addr_get_u32(netif_ip4_addr(&vx_netif));
    uint32_t m = ip4_addr_get_u32(netif_ip4_netmask(&vx_netif));
    uint32_t g = ip4_addr_get_u32(netif_ip4_gw(&vx_netif));
    for (int i = 0; i < 4; i++) {
        ip[i]   = (uint8_t)(a >> (i * 8));
        mask[i] = (uint8_t)(m >> (i * 8));
        gw[i]   = (uint8_t)(g >> (i * 8));
    }
}

void vxnet_log_ip(uint32_t addr_net_order) {
    for (int i = 0; i < 4; i++) {
        vx_log_u32((uint32_t)((addr_net_order >> (i * 8)) & 0xFF));
        if (i != 3) vx_log(".");
    }
}

/* ===========================================================
 * the native socket API
 *
 * A narrow, native-typed face on lwIP's sockets, so that kernel.c can
 * open a connection without ever seeing `struct sockaddr` or lwIP's
 * htons(). Everything above this line is the port; everything below is
 * the interface the rest of the system was given.
 * =========================================================== */

int vxnet_socket(void) {
    if (!vx_netif_up) return -1;
    return lwip_socket(AF_INET, SOCK_STREAM, 0);
}

int vxnet_connect(int s, const uint8_t ip[4], uint16_t port) {
    struct sockaddr_in sa;
    for (unsigned i = 0; i < sizeof(sa); i++) ((uint8_t *)&sa)[i] = 0;
    sa.sin_family = AF_INET;
    sa.sin_port   = lwip_htons(port);
    sa.sin_addr.s_addr = ((uint32_t)ip[0]) | ((uint32_t)ip[1] << 8) |
                         ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
    return lwip_connect(s, (struct sockaddr *)&sa, sizeof(sa));
}

int vxnet_send(int s, const void *buf, int len) {
    return (int)lwip_send(s, buf, (size_t)len, 0);
}

int vxnet_recv(int s, void *buf, int len) {
    return (int)lwip_recv(s, buf, (size_t)len, 0);
}

void vxnet_close(int s) { lwip_close(s); }

/*
 * Bound how long a read may block.
 *
 * Without this a socket whose peer has vanished without a FIN -- a
 * laptop closing its lid, a NAT dropping the entry -- holds the calling
 * thread forever. With eight of them, the whole secure pool leaks away
 * one connection at a time and nothing in the log says why.
 */
int vxnet_timeout(int s, uint32_t ms) {
    int v = (int)ms;
    if (lwip_setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &v, sizeof(v)) != 0)
        return -1;
    return lwip_setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &v, sizeof(v));
}

int vxnet_nodelay(int s, int on) {
    return lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/*
 * Resolve a name. Blocking, and that is the point: it runs on the
 * caller's thread, so eight threads can be resolving eight different
 * names at once. The stack this replaced had one query in flight for
 * the whole system.
 */
int vxnet_resolve(const char *host, uint8_t out[4]) {
    if (!vx_netif_up || !host) return 0;

    ip_addr_t addr;
    /* A literal address should not become a DNS question. */
    if (ip4addr_aton(host, ip_2_ip4(&addr))) {
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(&addr));
        for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v >> (i * 8));
        return 1;
    }

    struct addrinfo hints, *res = 0;
    for (unsigned i = 0; i < sizeof(hints); i++) ((uint8_t *)&hints)[i] = 0;
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (lwip_getaddrinfo(host, 0, &hints, &res) != 0 || !res) return 0;
    struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
    uint32_t v = sa->sin_addr.s_addr;
    for (int i = 0; i < 4; i++) out[i] = (uint8_t)(v >> (i * 8));
    lwip_freeaddrinfo(res);
    return 1;
}

/* ===========================================================
 * the self-test
 *
 * `make iso EXTRA=-DNET_SELFTEST` and boot with a server on the host.
 * It proves the four claims that matter and cannot be seen in a
 * screenshot: that names resolve, that plain TCP carries a request,
 * that TLS 1.3 completes a handshake and moves data, and -- the one
 * this whole exercise exists for -- that eight connections can be open
 * at the same moment rather than one after another.
 *
 * The last is measured rather than asserted. Eight threads each open a
 * connection and then wait for all eight to be up before any of them
 * closes; if the stack were serialising them the count would never
 * reach eight, and the test would fail rather than pass slowly.
 * =========================================================== */
#ifdef NET_SELFTEST

static volatile int st_open_count = 0;
static volatile int st_done_count = 0;
static volatile int st_peak = 0;
static volatile int st_fail = 0;
static uint16_t     st_port = 14433;

static void st_worker(void *arg) {
    int id = (int)(uintptr_t)arg;

    int slot = vxsec_open("10.0.2.2", st_port);
    if (slot < 0) {
        st_fail++;
        st_done_count++;
        vx_log("[selftest]   thread ");
        vx_log_u32((uint32_t)id);
        vx_log(" could not open\n");
        vx_thread_exit();
        return;
    }

    uint64_t f = vx_irq_save();
    st_open_count++;
    if (st_open_count > st_peak) st_peak = st_open_count;
    vx_irq_restore(f);

    /* Hold the connection until every thread has one. This is the
     * measurement: if the pool were handing out one slot at a time, the
     * peak could never exceed one and this would deadlock instead of
     * passing. */
    for (int i = 0; i < 300 && st_open_count < VXSEC_MAX; i++)
        vx_sleep_ms(10);

    const char *req = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n";
    int n = 0; while (req[n]) n++;
    if (vxsec_write(slot, req, n) != n) st_fail++;

    uint8_t buf[512];
    int got = vxsec_read(slot, buf, sizeof buf);
    if (got <= 0) st_fail++;

    f = vx_irq_save();
    st_open_count--;
    vx_irq_restore(f);

    vxsec_close(slot);
    st_done_count++;
    vx_thread_exit();
}

void vxnet_selftest(void) {
    vx_log("\n[selftest] network\n");
    int checks = 0, bad = 0;

    /* ---- 1. name resolution ---- */
    uint8_t ip[4];
    checks++;
    if (vxnet_resolve("example.com", ip)) {
        vx_log("[selftest]   ok   example.com resolves to ");
        vx_log_u32(ip[0]); vx_log("."); vx_log_u32(ip[1]); vx_log(".");
        vx_log_u32(ip[2]); vx_log("."); vx_log_u32(ip[3]); vx_log("\n");
    } else {
        bad++;
        vx_log("[selftest]   FAIL example.com does not resolve\n");
    }

    /* ---- 2. plain TCP ---- */
    checks++;
    {
        uint8_t host[4] = { 10, 0, 2, 2 };
        int s = vxnet_socket();
        int good = 0;
        if (s >= 0) {
            vxnet_timeout(s, 5000);
            if (vxnet_connect(s, host, 8000) == 0) {
                const char *req =
                    "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\nConnection: close\r\n\r\n";
                int n = 0; while (req[n]) n++;
                if (vxnet_send(s, req, n) == n) {
                    uint8_t buf[256];
                    if (vxnet_recv(s, buf, sizeof buf) > 0) good = 1;
                }
            }
            vxnet_close(s);
        }
        if (good) vx_log("[selftest]   ok   plain HTTP to 10.0.2.2:8000\n");
        else { bad++; vx_log("[selftest]   FAIL plain HTTP to 10.0.2.2:8000\n"); }
    }

    /* ---- 3. one TLS 1.3 connection ---- */
    checks++;
    {
        int slot = vxsec_open("10.0.2.2", st_port);
        if (slot >= 0) {
            vx_log("[selftest]   ok   TLS handshake: ");
            vx_log(vxsec_version(slot));
            vx_log(" ");
            vx_log(vxsec_cipher(slot));
            vx_log("\n");
            vxsec_close(slot);
        } else {
            bad++;
            vx_log("[selftest]   FAIL TLS handshake to 10.0.2.2\n");
        }
    }

    /* ---- 4. eight at once ---- */
    checks++;
    st_open_count = st_done_count = st_peak = st_fail = 0;
    for (int i = 0; i < VXSEC_MAX; i++)
        vx_thread_start(st_worker, (void *)(uintptr_t)i, "tlstest", 4);

    for (int i = 0; i < 3000 && st_done_count < VXSEC_MAX; i++)
        vx_sleep_ms(10);

    vx_log("[selftest]   ");
    if (st_peak == VXSEC_MAX && st_fail == 0) vx_log("ok   ");
    else { bad++; vx_log("FAIL "); }
    vx_log_u32((uint32_t)st_peak);
    vx_log(" of 8 secure connections open simultaneously, ");
    vx_log_u32((uint32_t)st_fail);
    vx_log(" failures\n");

    vx_log("[selftest] ");
    vx_log_u32((uint32_t)checks);
    vx_log(" checks, ");
    vx_log_u32((uint32_t)bad);
    vx_log(" failures\n\n");
}
#endif /* NET_SELFTEST */

/* ---- statistics, for the terminal's `net` command ---- */
void vxnet_stats(vxnet_stats_t *out) {
    if (!out) return;
    out->rx_frames = lwip_stats.link.recv;
    out->tx_frames = lwip_stats.link.xmit;
    out->rx_drop   = lwip_stats.link.drop;

    out->tcp_active = out->tcp_listen = out->tcp_tw = 0;
    if (!vx_netif_up) return;

    /* Walking a list the tcpip thread is free to modify would be a
     * use-after-free the moment a connection closed mid-count. The core
     * lock is what the stack itself takes to touch these. */
    LOCK_TCPIP_CORE();
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) out->tcp_active++;
    for (struct tcp_pcb_listen *p = tcp_listen_pcbs.listen_pcbs; p; p = p->next)
        out->tcp_listen++;
    for (struct tcp_pcb *p = tcp_tw_pcbs; p; p = p->next) out->tcp_tw++;
    UNLOCK_TCPIP_CORE();
}
