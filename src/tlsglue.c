/*
 * src/tlsglue.c — Mbed TLS's port, and the eight secure connections
 * above it.
 *
 * Three jobs:
 *
 *   the platform   allocation, entropy, threading and formatting --
 *                  everything the library needs from an operating
 *                  system, none of which exists here by default.
 *
 *   the pool       eight independent TLS 1.3 sessions, each with its
 *                  own record state, each drivable from its own thread.
 *
 *   the guard      FXSAVE and FXRSTOR around the handshake, as the
 *                  brief asked.
 *
 * ---- on that last one ----
 *
 * It is redundant, and it is here anyway. The scheduler already saves
 * 512 bytes of extended state on every context switch, and every
 * interrupt handler in this kernel is compiled general-regs-only so
 * that none of them can touch an XMM register at all. Between those
 * two there is no path by which a network interrupt reaches the
 * registers a handshake is using.
 *
 * What the explicit pair buys is that the guarantee becomes *local*.
 * The reasoning above depends on a compiler flag in a Makefile and an
 * attribute on a function in another file; someone adding an interrupt
 * handler next year has no reason to know that the TLS code was relying
 * on either. Sixty cycles against a handshake costing millions is a
 * price worth paying to not have to know.
 */

#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "mbedtls/threading.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#include "vxport.h"
#include "vxnet.h"

#include <stdarg.h>
#include <stddef.h>

/* ===========================================================
 * the platform: formatting
 *
 * The two functions vextro_config.h points the library at. Neither is
 * on any path that matters for throughput -- they are used by the OID
 * table and by certificate description strings -- so a small correct
 * formatter beats a fast one. It handles what Mbed TLS actually asks
 * for: %s, %d, %u, %x, %c, and the width/zero-pad forms of those.
 * =========================================================== */

static void vs_putc(char **p, char *end, char c) {
    if (*p < end) **p = c;
    (*p)++;
}

static void vs_num(char **p, char *end, unsigned long v, int base,
                   int width, int zero, int neg) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { int d = (int)(v % (unsigned)base);
                tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v /= (unsigned)base; }
    int len = n + (neg ? 1 : 0);
    if (!zero) for (int i = len; i < width; i++) vs_putc(p, end, ' ');
    if (neg) vs_putc(p, end, '-');
    if (zero) for (int i = len; i < width; i++) vs_putc(p, end, '0');
    while (n) vs_putc(p, end, tmp[--n]);
}

static int vx_vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    char *p = buf, *end = buf + (size ? size - 1 : 0);
    for (const char *f = fmt; *f; f++) {
        if (*f != '%') { vs_putc(&p, end, *f); continue; }
        f++;
        int zero = 0, width = 0, lng = 0;
        if (*f == '0') { zero = 1; f++; }
        while (*f >= '0' && *f <= '9') { width = width * 10 + (*f - '0'); f++; }
        while (*f == 'l') { lng = 1; f++; }
        switch (*f) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int n = 0; while (s[n]) n++;
            for (int i = n; i < width; i++) vs_putc(&p, end, ' ');
            while (*s) vs_putc(&p, end, *s++);
            break;
        }
        case 'c': vs_putc(&p, end, (char)va_arg(ap, int)); break;
        case 'd': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            int neg = v < 0;
            vs_num(&p, end, (unsigned long)(neg ? -v : v), 10, width, zero, neg);
            break;
        }
        case 'u': {
            unsigned long v = lng ? va_arg(ap, unsigned long)
                                  : (unsigned long)va_arg(ap, unsigned int);
            vs_num(&p, end, v, 10, width, zero, 0);
            break;
        }
        case 'x': case 'X': case 'p': {
            unsigned long v = (*f == 'p') ? (unsigned long)(uintptr_t)va_arg(ap, void *)
                            : lng ? va_arg(ap, unsigned long)
                                  : (unsigned long)va_arg(ap, unsigned int);
            vs_num(&p, end, v, 16, width, zero, 0);
            break;
        }
        case '%': vs_putc(&p, end, '%'); break;
        default:  vs_putc(&p, end, '%'); vs_putc(&p, end, *f); break;
        }
    }
    if (size) *(p < end ? p : end) = 0;
    return (int)(p - buf);
}

/* The same formatter, under the name third_party/vxport.c exposes as
 * snprintf and vsnprintf. One implementation, three spellings, so that
 * a fix to the width handling cannot reach one caller and miss another. */
int vx_vsnprintf_pub(char *buf, size_t size, const char *fmt, va_list ap) {
    return vx_vsnprintf(buf, size, fmt, ap);
}

int vx_mbed_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vx_vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vx_mbed_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    int r = vx_vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vx_log(buf);
    return r;
}

/* ===========================================================
 * the platform: allocation
 *
 * The hook the brief named. Every allocation the library makes -- key
 * material, certificate chains, record buffers, PSA's key store -- goes
 * through here into the kernel's slab allocator, so a TLS session shows
 * up in the same `free` figure as a window title and there is no second
 * heap to size, tune or run out of separately.
 *
 * Counted, because a leak inside a TLS library is a leak that scales
 * with how much the user browses.
 * =========================================================== */

static volatile uint64_t tls_live_bytes = 0;
static volatile uint32_t tls_live_blocks = 0;

static void *tls_calloc(size_t n, size_t size) {
    void *p = vx_calloc((uint64_t)n, (uint64_t)size);
    if (p) {
        uint64_t f = vx_irq_save();
        tls_live_bytes += vx_alloc_size(p);
        tls_live_blocks++;
        vx_irq_restore(f);
    }
    return p;
}

static void tls_free(void *p) {
    if (!p) return;
    uint64_t f = vx_irq_save();
    uint64_t sz = vx_alloc_size(p);
    if (tls_live_bytes >= sz) tls_live_bytes -= sz;
    if (tls_live_blocks) tls_live_blocks--;
    vx_irq_restore(f);
    vx_free(p);
}

uint32_t vxsec_heap_kb(void) { return (uint32_t)(tls_live_bytes / 1024); }

/* ===========================================================
 * the platform: entropy
 *
 * MBEDTLS_ENTROPY_HARDWARE_ALT means this is the library's only source.
 * There is no /dev/urandom and no system-wide pool to fall back on, so
 * a failure here has to be a failure -- not a zeroed buffer, which
 * would seed the CSPRNG identically on every boot and produce a working
 * handshake with a key an observer can derive.
 * =========================================================== */

int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen) {
    (void)data;
    uint32_t got = vx_random((uint8_t *)output, (uint32_t)len);
    if (got != len) {
        *olen = 0;
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    *olen = len;
    return 0;
}

/* ===========================================================
 * the platform: threading
 *
 * Eight connections on eight threads share one PSA key store and a
 * certain amount of library state behind it. MBEDTLS_THREADING_ALT is
 * how the library asks to be told about that, and these four functions
 * are the answer -- backed by the same scheduler wait channels
 * everything else in the port blocks on.
 * =========================================================== */

static void tls_mutex_init(mbedtls_threading_mutex_t *m) {
    if (!m) return;
    m->locked = 0; m->owner = -1; m->waiters = 0; m->ready = 1;
}

static void tls_mutex_free(mbedtls_threading_mutex_t *m) {
    if (!m) return;
    m->ready = 0;
    vx_wake(m, 1);
}

static int tls_mutex_lock(mbedtls_threading_mutex_t *m) {
    if (!m || !m->ready) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    int me = vx_thread_id();
    for (;;) {
        uint64_t f = vx_irq_save();
        if (!m->locked) {
            m->locked = 1; m->owner = me;
            vx_irq_restore(f);
            return 0;
        }
        if (m->owner == me) {
            /* Mbed TLS does not take a mutex recursively, so this is a
             * bug in the port rather than a case to support. Naming it
             * beats deadlocking in a library with no debugger. */
            vx_irq_restore(f);
            vx_log("[tls] mutex taken twice by one thread\n");
            return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
        }
        m->waiters++;
        vx_block_locked(m, 1000, f);
        uint64_t g = vx_irq_save();
        if (m->waiters) m->waiters--;
        vx_irq_restore(g);
    }
}

static int tls_mutex_unlock(mbedtls_threading_mutex_t *m) {
    if (!m || !m->ready) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    uint64_t f = vx_irq_save();
    m->locked = 0; m->owner = -1;
    vx_irq_restore(f);
    vx_wake(m, 0);
    return 0;
}

/* ===========================================================
 * the pool
 * =========================================================== */

typedef struct {
    int                       used;
    int                       sock;
    int                       err;
    char                      host[128];
    mbedtls_ssl_context       ssl;
    mbedtls_ssl_config        conf;
    mbedtls_ctr_drbg_context  drbg;
    mbedtls_entropy_context   entropy;
    /* The 512 bytes the guard writes through. Sixteen-aligned because
     * FXSAVE64 faults otherwise, and per-slot rather than shared
     * because two threads may be in a handshake at the same moment. */
    uint8_t                   fx[512] __attribute__((aligned(16)));
} vxsec_slot_t;

static vxsec_slot_t vxsec_slots[VXSEC_MAX];
static int          vxsec_started = 0;

/*
 * The certificate authority store, parsed once at startup.
 *
 * One chain shared by every session: the certificates are read-only
 * once parsed, Mbed TLS treats the CA chain as const during a
 * handshake, and a per-connection copy would be a megabyte of X.509
 * structures duplicated eight times for no benefit.
 */
static mbedtls_x509_crt vxsec_ca;
static int              vxsec_ca_loaded = 0;
static int              vxsec_ca_count = 0;
static mbedtls_threading_mutex_t vxsec_pool_lock;

/*
 * Whether a connection opened now would be authenticated.
 *
 * This used to return a constant zero and was correct to. It is a
 * question about the volume rather than about the code: with a bundle
 * on disk the answer is yes, without one it is no, and everything that
 * shows a padlock asks here rather than assuming.
 */
/*
 * Load the certificate authority store, once, as late as possible.
 *
 * Not from vxsec_init(): TLS starts with the network, which is long
 * before the volume is mounted, so there is no filesystem to read a
 * bundle from at that point. Doing it there recorded "no CA store" on a
 * machine that had one, and every connection after went unverified for
 * a reason that had nothing to do with certificates.
 *
 * So it happens on the first connection instead, by which time the disk
 * is up. Parsing is still once rather than per connection -- a few
 * hundred roots is milliseconds of X.509 decoding, which on every
 * request would be the most expensive part of the handshake.
 *
 * mbedtls_x509_crt_parse returns the number of certificates it could
 * *not* parse, so a positive result is a partial success: real bundles
 * carry the occasional expired or malformed entry and the rest of the
 * store is still good. Only a negative result, or an empty chain, means
 * there is nothing to verify against.
 */
static int vxsec_ca_attempted = 0;

static void vxsec_load_ca(void) {
    const uint8_t *pem = 0;
    uint32_t len;

    if (vxsec_ca_attempted) return;

    len = vx_ca_bundle(&pem);
    if (len == 0) {
        /* Not latched: the volume may simply not be mounted yet, and
         * vx_ca_bundle() distinguishes that from a volume with no
         * bundle on it. Trying again next time costs one failed open. */
        return;
    }

    vxsec_ca_attempted = 1;

    {
        int bad = mbedtls_x509_crt_parse(&vxsec_ca, pem, len);
        if (bad < 0 || vxsec_ca.version == 0) {
            vx_log("[tls] the CA bundle would not parse: certificates are "
                   "NOT verified\n");
            mbedtls_x509_crt_free(&vxsec_ca);
            mbedtls_x509_crt_init(&vxsec_ca);
            return;
        }
        vxsec_ca_loaded = 1;
        vxsec_ca_count = 0;
        for (mbedtls_x509_crt *p = &vxsec_ca; p; p = p->next)
            vxsec_ca_count++;
        vx_log("[tls] CA store loaded, ");
        vx_log_u32((uint32_t)vxsec_ca_count);
        vx_log(" roots; certificates ARE verified\n");
        if (bad > 0) {
            vx_log("[tls]   (");
            vx_log_u32((uint32_t)bad);
            vx_log(" entries in the bundle were unreadable)\n");
        }
    }
}

int vxsec_verifies_certificates(void) {
    vxsec_load_ca();
    return vxsec_ca_loaded;
}
int vxsec_ca_roots(void) { return vxsec_ca_count; }
int vxsec_ready(void) { return vxsec_started; }

/*
 * Bring the library up. Once, before any connection.
 *
 * psa_crypto_init() is not optional and its absence is not obvious:
 * TLS 1.3 in Mbed TLS 3.x routes its whole key schedule through PSA, so
 * without this every handshake fails deep inside the key derivation
 * with an error that names neither PSA nor initialisation.
 */
int vxsec_init(void) {
    if (vxsec_started) return 1;

    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    mbedtls_threading_set_alt(tls_mutex_init, tls_mutex_free,
                              tls_mutex_lock, tls_mutex_unlock);
    tls_mutex_init(&vxsec_pool_lock);

    /* Prove the entropy source before anything depends on it. A machine
     * with no RDRAND must say so here rather than produce eight
     * identical session keys. */
    uint8_t probe[32];
    if (vx_random(probe, sizeof probe) != sizeof probe) {
        vx_log("[tls] no hardware random source; TLS is disabled\n");
        return 0;
    }

    int rc = psa_crypto_init();
    if (rc != PSA_SUCCESS) {
        vx_log("[tls] psa_crypto_init failed\n");
        return 0;
    }

    mbedtls_x509_crt_init(&vxsec_ca);

    for (int i = 0; i < VXSEC_MAX; i++) vxsec_slots[i].used = 0;
    vxsec_started = 1;

    vx_log("[tls] Mbed TLS " MBEDTLS_VERSION_STRING
           ", TLS 1.3 only, ");
    vx_log_u32(VXSEC_MAX);
    vx_log(" parallel connections\n");
    /* Whether certificates get verified is not known yet: the CA store
     * lives on a volume that is not mounted at this point in the boot.
     * The answer is logged by vxsec_load_ca() once the disk is up,
     * rather than guessed here -- this line used to assert the
     * unfavourable answer unconditionally and was wrong as soon as a
     * bundle shipped. */
    return 1;
}

static int slot_take(void) {
    tls_mutex_lock(&vxsec_pool_lock);
    for (int i = 0; i < VXSEC_MAX; i++) {
        if (!vxsec_slots[i].used) {
            vxsec_slots[i].used = 1;
            tls_mutex_unlock(&vxsec_pool_lock);
            return i;
        }
    }
    tls_mutex_unlock(&vxsec_pool_lock);
    return -1;
}

static void slot_release(int i) {
    tls_mutex_lock(&vxsec_pool_lock);
    vxsec_slots[i].used = 0;
    tls_mutex_unlock(&vxsec_pool_lock);
}

int vxsec_active(void) {
    int n = 0;
    for (int i = 0; i < VXSEC_MAX; i++) if (vxsec_slots[i].used) n++;
    return n;
}

int vxsec_slot_used(int slot) {
    return slot >= 0 && slot < VXSEC_MAX && vxsec_slots[slot].used;
}

int vxsec_last_error(int slot) {
    return (slot >= 0 && slot < VXSEC_MAX) ? vxsec_slots[slot].err : 0;
}

const char *vxsec_peer_name(int slot) {
    return (slot >= 0 && slot < VXSEC_MAX) ? vxsec_slots[slot].host : "";
}

const char *vxsec_cipher(int slot) {
    if (slot < 0 || slot >= VXSEC_MAX || !vxsec_slots[slot].used) return "";
    const char *s = mbedtls_ssl_get_ciphersuite(&vxsec_slots[slot].ssl);
    return s ? s : "";
}

const char *vxsec_version(int slot) {
    if (slot < 0 || slot >= VXSEC_MAX || !vxsec_slots[slot].used) return "";
    const char *s = mbedtls_ssl_get_version(&vxsec_slots[slot].ssl);
    return s ? s : "";
}

/* ---- the BIO ----
 *
 * Where Mbed TLS meets lwIP. Everything above this is TLS and
 * everything below is TCP, and the two only ever touch through these
 * two functions.
 *
 * A timeout must come back as WANT_READ rather than an error: to the
 * library those mean "no progress yet, ask again" and "the connection
 * is gone", and confusing them turns a slow server into a failed
 * handshake. */
static int bio_send(void *ctx, const unsigned char *buf, size_t len) {
    int s = *(int *)ctx;
    int n = vxnet_send(s, buf, (int)len);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

static int bio_recv(void *ctx, unsigned char *buf, size_t len) {
    int s = *(int *)ctx;
    int n = vxnet_recv(s, buf, (int)len);
    if (n == 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if (n < 0)  return MBEDTLS_ERR_SSL_WANT_READ;
    return n;
}

/*
 * Open a TLS 1.3 connection to a host.
 *
 * Resolve, connect, hand the socket to the library, handshake. The FPU
 * guard brackets the handshake because that is where the arithmetic is
 * -- the X25519 ladder and the signature check -- and because it is the
 * one part that runs for long enough to be interrupted many times.
 */
int vxsec_open(const char *host, uint16_t port) {
    if (!vxsec_started || !host) return -1;

    uint8_t ip[4];
    if (!vxnet_resolve(host, ip)) {
        vx_log("[tls] cannot resolve ");
        vx_log(host);
        vx_log("\n");
        return -1;
    }

    int slot = slot_take();
    if (slot < 0) {
        vx_log("[tls] all eight connections are in use\n");
        return -1;
    }
    vxsec_slot_t *c = &vxsec_slots[slot];
    c->err = 0;

    int n = 0;
    while (host[n] && n < (int)sizeof(c->host) - 1) { c->host[n] = host[n]; n++; }
    c->host[n] = 0;

    c->sock = vxnet_socket();
    if (c->sock < 0) { slot_release(slot); return -1; }
    vxnet_timeout(c->sock, 15000);
    vxnet_nodelay(c->sock, 1);

    if (vxnet_connect(c->sock, ip, port) != 0) {
        vxnet_close(c->sock);
        slot_release(slot);
        vx_log("[tls] connect refused\n");
        return -1;
    }

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_ctr_drbg_init(&c->drbg);
    mbedtls_entropy_init(&c->entropy);

    /* A personalisation string per slot, so two connections opened in
     * the same millisecond do not draw the same stream. */
    unsigned char pers[24];
    for (int i = 0; i < 16; i++) pers[i] = (unsigned char)"vextro-tls-slot0"[i];
    pers[15] = (unsigned char)('0' + slot);
    uint32_t t = vx_now_ms();
    for (int i = 0; i < 4; i++) pers[16 + i] = (unsigned char)(t >> (i * 8));
    for (int i = 20; i < 24; i++) pers[i] = (unsigned char)vx_thread_id();

    int rc = mbedtls_ctr_drbg_seed(&c->drbg, mbedtls_entropy_func,
                                   &c->entropy, pers, sizeof pers);
    if (rc) goto fail;

    rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) goto fail;

    /*
     * Verify the chain when there is something to verify it against.
     *
     * With a bundle loaded this is VERIFY_REQUIRED: the handshake fails
     * if the server's chain does not lead to a root in the store, or if
     * the leaf's names do not include the host that was asked for. That
     * is the difference between a connection that is private and one
     * that is merely encrypted -- without it, anything able to answer
     * on the far end is indistinguishable from the site.
     *
     * With no bundle the behaviour is what it always was, and every
     * layer above says so rather than showing a padlock it has not
     * earned.
     */
    vxsec_load_ca();
    if (vxsec_ca_loaded) {
        mbedtls_ssl_conf_ca_chain(&c->conf, &vxsec_ca, 0);
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    } else {
        mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->drbg);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);

    rc = mbedtls_ssl_setup(&c->ssl, &c->conf);
    if (rc) goto fail;

    /* Server Name Indication. Without it a server behind a shared
     * address has no way to know which certificate to present, and
     * answers with the wrong one or with a handshake failure. */
    mbedtls_ssl_set_hostname(&c->ssl, host);
    mbedtls_ssl_set_bio(&c->ssl, &c->sock, bio_send, bio_recv, 0);

    /* ---- the guard ---- */
    vx_fpu_save(c->fx);

    uint32_t began = vx_now_ms();
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
        if (vx_now_ms() - began > 20000) { rc = MBEDTLS_ERR_SSL_TIMEOUT; break; }
        vx_yield();
    }

    vx_fpu_restore(c->fx);
    /* ---- end of guard ---- */

    if (rc) goto fail;

    vx_log("[tls] ");
    vx_log(host);
    vx_log(" on slot ");
    vx_log_u32((uint32_t)slot);
    vx_log(": ");
    vx_log(vxsec_version(slot));
    vx_log(" ");
    vx_log(vxsec_cipher(slot));
    vx_log("\n");
    return slot;

fail:
    c->err = rc;
    /* vx_log_u32 prints decimal, so the "-0x" this used to carry was a
     * lie that made every error code unsearchable: -0x9984 was really
     * -9984, which is -0x2700, which is the certificate verify failure
     * this line existed to report. */
    vx_log("[tls] handshake with ");
    vx_log(host);
    vx_log(" failed, mbedtls -");
    vx_log_u32((uint32_t)(-rc));
    vx_log("\n");

    /*
     * If it was the certificate, say which part of it. "Verification
     * failed" covers an expired leaf, a name that does not match, and a
     * chain that leads nowhere -- three problems with three different
     * answers, and the flags distinguish them.
     */
    if (rc == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        uint32_t f = mbedtls_ssl_get_verify_result(&c->ssl);
        vx_log("[tls]   certificate rejected:");
        if (f & MBEDTLS_X509_BADCERT_EXPIRED)   vx_log(" expired");
        if (f & MBEDTLS_X509_BADCERT_REVOKED)   vx_log(" revoked");
        if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH) vx_log(" wrong-host");
        if (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
            vx_log(" chain-does-not-reach-a-known-root");
        if (f & MBEDTLS_X509_BADCERT_FUTURE)    vx_log(" not-yet-valid");
        if (f & MBEDTLS_X509_BADCERT_BAD_MD)    vx_log(" unsupported-digest");
        if (f & MBEDTLS_X509_BADCERT_BAD_PK)    vx_log(" unsupported-key");
        if (f & MBEDTLS_X509_BADCERT_BAD_KEY)   vx_log(" bad-key");
        if (!f) vx_log(" (no flags: the chain was never checked)");
        vx_log("\n");
    }
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    vxnet_close(c->sock);
    slot_release(slot);
    return -1;
}

int vxsec_write(int slot, const void *buf, int len) {
    if (slot < 0 || slot >= VXSEC_MAX || !vxsec_slots[slot].used) return -1;
    vxsec_slot_t *c = &vxsec_slots[slot];

    vx_fpu_save(c->fx);
    int sent = 0;
    while (sent < len) {
        int rc = mbedtls_ssl_write(&c->ssl, (const unsigned char *)buf + sent,
                                   (size_t)(len - sent));
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vx_yield();
            continue;
        }
        if (rc <= 0) { c->err = rc; break; }
        sent += rc;
    }
    vx_fpu_restore(c->fx);
    return sent ? sent : -1;
}

int vxsec_read(int slot, void *buf, int len) {
    if (slot < 0 || slot >= VXSEC_MAX || !vxsec_slots[slot].used) return -1;
    vxsec_slot_t *c = &vxsec_slots[slot];

    vx_fpu_save(c->fx);
    int rc;
    for (;;) {
        rc = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, (size_t)len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            vx_yield();
            continue;
        }
        break;
    }
    vx_fpu_restore(c->fx);

    /* Both of these mean "the conversation ended tidily", which is zero
     * bytes rather than an error -- an HTTP/1.0 body is delimited by
     * exactly this. */
    if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_SSL_CONN_EOF)
        return 0;
    if (rc < 0) c->err = rc;
    return rc;
}

void vxsec_close(int slot) {
    if (slot < 0 || slot >= VXSEC_MAX || !vxsec_slots[slot].used) return;
    vxsec_slot_t *c = &vxsec_slots[slot];

    /* A close_notify is what tells the peer this was a deliberate end
     * and not a connection cut, which is the difference between a
     * clean shutdown and a truncation attack the peer must assume. */
    mbedtls_ssl_close_notify(&c->ssl);

    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->drbg);
    mbedtls_entropy_free(&c->entropy);
    vxnet_close(c->sock);
    c->sock = -1;
    slot_release(slot);
}

/* ===========================================================
 * one HTTPS request
 * =========================================================== */

static int str_put(uint8_t *dst, int off, int max, const char *s) {
    while (*s && off < max) dst[off++] = (uint8_t)*s++;
    return off;
}

/*
 * GET a URL over TLS and return the body.
 *
 * HTTP/1.0 with `Connection: close`, because the alternative is parsing
 * chunked transfer encoding to know where the body ends -- and a server
 * closing the connection is an unambiguous end that needs no parsing at
 * all. The cost is one connection per request, which is what the
 * eight-slot pool is for.
 */
int vxsec_https_get(const char *host, const char *path, uint8_t *out, int max) {
    if (!vxsec_started) return -1;

    int slot = vxsec_open(host, 443);
    if (slot < 0) return -1;

    uint8_t req[512];
    int n = 0;
    n = str_put(req, n, sizeof req, "GET ");
    n = str_put(req, n, sizeof req, path && *path ? path : "/");
    n = str_put(req, n, sizeof req, " HTTP/1.0\r\nHost: ");
    n = str_put(req, n, sizeof req, host);
    n = str_put(req, n, sizeof req,
                "\r\nUser-Agent: Vextro/9\r\nAccept: */*\r\n"
                "Connection: close\r\n\r\n");

    if (vxsec_write(slot, req, n) != n) { vxsec_close(slot); return -1; }

    int total = 0, header_end = -1;
    while (total < max) {
        int r = vxsec_read(slot, out + total, max - total);
        if (r <= 0) break;
        total += r;

        if (header_end < 0) {
            for (int i = 3; i < total; i++) {
                if (out[i - 3] == '\r' && out[i - 2] == '\n' &&
                    out[i - 1] == '\r' && out[i] == '\n') {
                    header_end = i + 1;
                    break;
                }
            }
        }
    }
    vxsec_close(slot);

    if (header_end < 0) return total;   /* no header found; give it all back */

    int body = total - header_end;
    for (int i = 0; i < body; i++) out[i] = out[header_end + i];
    return body;
}
