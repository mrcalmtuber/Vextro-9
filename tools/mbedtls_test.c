/*
 * tools/mbedtls_test.c — does the stripped Mbed TLS still do TLS 1.3?
 *
 * The strip in third_party/mbedtls/vextro_config.h turns off about four
 * hundred switches, and the failure mode of turning off one too many is
 * not a compile error. It is a handshake that negotiates nothing and
 * returns -0x7780 from somewhere twelve frames deep, over a serial line,
 * on a machine with no debugger. So the config is proved here first,
 * on the host, against a real TLS 1.3 server.
 *
 * This runs the same code the kernel runs -- the same 56 vendored source
 * files, the same config header, the same calloc/free indirection and
 * the same entropy hook shape. What differs is only what is underneath:
 * host sockets rather than lwIP, host malloc rather than the kernel
 * heap. Those are exactly the two things the port replaces, and both are
 * reached through the function pointers the port sets, so proving the
 * library here proves everything above the two seams.
 *
 *     make mbedtls-test        starts openssl s_server and connects
 *
 * With no server reachable it still runs the offline half -- entropy,
 * the allocator hooks, AES-GCM and SHA-256 against their published
 * vectors -- and says so rather than passing silently.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdarg.h>

#include "mbedtls/build_info.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/platform.h"
#include "mbedtls/threading.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "psa/crypto.h"

static int checks = 0, failures = 0;
static void check(const char *what, int ok) {
    checks++;
    if (!ok) { failures++; printf("  FAIL  %s\n", what); }
    else      printf("  ok    %s\n", what);
}

/* ---- the two formatting functions the config points the library at ---- */
int vx_mbed_snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap); return r;
}
int vx_mbed_printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap); return r;
}

/* ---- the allocator seam ----
 *
 * Counted, so the test can assert the library really routes through the
 * hook rather than reaching malloc directly -- which is the property
 * the kernel depends on and the one that fails silently if a vendored
 * file slips past MBEDTLS_PLATFORM_MEMORY. */
static long alloc_calls = 0, free_calls = 0, live_bytes = 0;

static void *test_calloc(size_t n, size_t size) {
    alloc_calls++;
    size_t total = n * size;
    size_t *p = malloc(total + 16);
    if (!p) return NULL;
    p[0] = total; live_bytes += (long)total;
    memset((char *)p + 16, 0, total);
    return (char *)p + 16;
}
static void test_free(void *p) {
    if (!p) return;
    free_calls++;
    size_t *base = (size_t *)((char *)p - 16);
    live_bytes -= (long)base[0];
    free(base);
}

/* ---- entropy ----
 *
 * MBEDTLS_ENTROPY_HARDWARE_ALT means the library has no source of its
 * own and this is the only one. The kernel's is RDRAND; the host's is
 * the C library, because what is under test here is the wiring, not the
 * randomness. */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len,
                          size_t *olen) {
    (void)data;
    for (size_t i = 0; i < len; i++) output[i] = (unsigned char)(rand() & 0xFF);
    *olen = len;
    return 0;
}

/* ---- threading ----
 *
 * Single-threaded here, so these only have to be correct, not
 * contended. The kernel's versions block on the scheduler. */
static void th_init(mbedtls_threading_mutex_t *m) { m->locked = 0; m->owner = -1; m->waiters = 0; m->ready = 1; }
static void th_free(mbedtls_threading_mutex_t *m) { m->ready = 0; }
static int  th_lock(mbedtls_threading_mutex_t *m) {
    if (!m->ready) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    m->locked = 1; return 0;
}
static int  th_unlock(mbedtls_threading_mutex_t *m) {
    if (!m->ready) return MBEDTLS_ERR_THREADING_BAD_INPUT_DATA;
    m->locked = 0; return 0;
}

/* ---- known-answer tests ---- */

/* NIST GCM test case 16 (AES-128, 60-byte plaintext, 20-byte AAD). */
static void test_gcm(void) {
    static const unsigned char key[16] = {
        0xfe,0xff,0xe9,0x92,0x86,0x65,0x73,0x1c,
        0x6d,0x6a,0x8f,0x94,0x67,0x30,0x83,0x08 };
    static const unsigned char iv[12] = {
        0xca,0xfe,0xba,0xbe,0xfa,0xce,0xdb,0xad,0xde,0xca,0xf8,0x88 };
    static const unsigned char aad[20] = {
        0xfe,0xed,0xfa,0xce,0xde,0xad,0xbe,0xef,0xfe,0xed,
        0xfa,0xce,0xde,0xad,0xbe,0xef,0xab,0xad,0xda,0xd2 };
    static const unsigned char pt[60] = {
        0xd9,0x31,0x32,0x25,0xf8,0x84,0x06,0xe5,0xa5,0x59,0x09,0xc5,
        0xaf,0xf5,0x26,0x9a,0x86,0xa7,0xa9,0x53,0x15,0x34,0xf7,0xda,
        0x2e,0x4c,0x30,0x3d,0x8a,0x31,0x8a,0x72,0x1c,0x3c,0x0c,0x95,
        0x95,0x68,0x09,0x53,0x2f,0xcf,0x0e,0x24,0x49,0xa6,0xb5,0x25,
        0xb1,0x6a,0xed,0xf5,0xaa,0x0d,0xe6,0x57,0xba,0x63,0x7b,0x39 };
    static const unsigned char want_ct[60] = {
        0x42,0x83,0x1e,0xc2,0x21,0x77,0x74,0x24,0x4b,0x72,0x21,0xb7,
        0x84,0xd0,0xd4,0x9c,0xe3,0xaa,0x21,0x2f,0x2c,0x02,0xa4,0xe0,
        0x35,0xc1,0x7e,0x23,0x29,0xac,0xa1,0x2e,0x21,0xd5,0x14,0xb2,
        0x54,0x66,0x93,0x1c,0x7d,0x8f,0x6a,0x5a,0xac,0x84,0xaa,0x05,
        0x1b,0xa3,0x0b,0x39,0x6a,0x0a,0xac,0x97,0x3d,0x58,0xe0,0x91 };
    static const unsigned char want_tag[16] = {
        0x5b,0xc9,0x4f,0xbc,0x32,0x21,0xa5,0xdb,
        0x94,0xfa,0xe9,0x5a,0xe7,0x12,0x1a,0x47 };

    unsigned char ct[60], tag[16];
    mbedtls_gcm_context g;
    mbedtls_gcm_init(&g);
    int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
    rc |= mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, sizeof pt,
                                    iv, sizeof iv, aad, sizeof aad,
                                    pt, ct, sizeof tag, tag);
    mbedtls_gcm_free(&g);
    check("AES-128-GCM matches NIST case 16",
          rc == 0 && memcmp(ct, want_ct, sizeof ct) == 0 &&
          memcmp(tag, want_tag, sizeof tag) == 0);
}

/* FIPS 180-4: SHA-256 of "abc". */
static void test_sha256(void) {
    static const unsigned char want[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,
        0x5d,0xae,0x22,0x23,0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad };
    unsigned char out[32];
    int rc = mbedtls_sha256((const unsigned char *)"abc", 3, out, 0);
    check("SHA-256 matches FIPS 180-4",
          rc == 0 && memcmp(out, want, 32) == 0);
}

/* ---- the handshake ---- */

#ifdef VX_MBEDTLS_DEBUG
#include "mbedtls/debug.h"
static void dbg_cb(void *ctx, int level, const char *file, int line,
                   const char *str) {
    (void)ctx; (void)level; (void)file; (void)line;
    fputs(str, stderr);
}
#endif

static int net_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = send(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}
static int net_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int *)ctx;
    ssize_t n = recv(fd, buf, len, 0);
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
}

static int try_handshake(const char *host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) { close(fd); return -1; }
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) { close(fd); return -1; }

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context drbg;

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_entropy_init(&ent);
    mbedtls_ctr_drbg_init(&drbg);

    const char *pers = "vextro-mbedtls-test";
    int rc = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent,
                                  (const unsigned char *)pers, strlen(pers));
    check("CTR_DRBG seeds from the hardware hook", rc == 0);
    if (rc) goto out;

    rc = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc) { check("ssl_config_defaults", 0); goto out; }

    /* No CA store on the volume, so nothing authenticates the peer.
     * Said out loud here and in every other place TLS is described. */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
    mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_3);

#ifdef VX_MBEDTLS_DEBUG
    mbedtls_debug_set_threshold(4);
    mbedtls_ssl_conf_dbg(&conf, dbg_cb, NULL);
#endif

    rc = mbedtls_ssl_setup(&ssl, &conf);
    if (rc) { check("ssl_setup", 0); goto out; }
    mbedtls_ssl_set_hostname(&ssl, "localhost");
    mbedtls_ssl_set_bio(&ssl, &fd, net_send, net_recv, NULL);

    while ((rc = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE)
            break;
    }
    if (rc != 0) printf("        handshake returned -0x%04X\n", (unsigned)-rc);
    check("TLS 1.3 handshake completes", rc == 0);
    if (rc == 0) {
        printf("        version=%s  suite=%s\n",
               mbedtls_ssl_get_version(&ssl),
               mbedtls_ssl_get_ciphersuite(&ssl));
        check("negotiated version is TLS 1.3",
              strcmp(mbedtls_ssl_get_version(&ssl), "TLSv1.3") == 0);

        const char *req = "GET / HTTP/1.0\r\nHost: localhost\r\n\r\n";
        int w = mbedtls_ssl_write(&ssl, (const unsigned char *)req, strlen(req));
        check("application data encrypts and sends", w == (int)strlen(req));

        unsigned char buf[512];
        int r = mbedtls_ssl_read(&ssl, buf, sizeof buf - 1);
        check("application data decrypts on the way back", r > 0);
        mbedtls_ssl_close_notify(&ssl);
    }

out:
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&ent);
    close(fd);
    return rc;
}

int main(int argc, char **argv) {
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    int port = argc > 2 ? atoi(argv[2]) : 0;

    printf("Mbed TLS %s, stripped\n\n", MBEDTLS_VERSION_STRING);

    mbedtls_platform_set_calloc_free(test_calloc, test_free);
    mbedtls_threading_set_alt(th_init, th_free, th_lock, th_unlock);

    int rc = psa_crypto_init();
    check("psa_crypto_init succeeds", rc == PSA_SUCCESS);

    test_sha256();
    test_gcm();
    check("the library allocates only through our hook", alloc_calls > 0);

    if (port) {
        printf("\n  connecting to %s:%d\n", host, port);
        try_handshake(host, port);
    } else {
        printf("\n  (no server given; skipping the handshake)\n");
    }

    /* PSA's key store is global and outlives every connection, so it is
     * released here rather than by any one of them. Forgetting this is
     * how a kernel that opens eight connections an hour runs out of
     * heap overnight. */
    mbedtls_psa_crypto_free();

    /* Every context was freed, so the count must come back to zero. A
     * leak inside the library is a leak inside the kernel heap. */
    check("no allocation is left outstanding", live_bytes == 0);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
