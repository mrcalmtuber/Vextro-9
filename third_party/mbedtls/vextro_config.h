#ifndef VEXTRO_MBEDTLS_CONFIG_H
#define VEXTRO_MBEDTLS_CONFIG_H

/*
 * third_party/mbedtls/vextro_config.h — Mbed TLS 3.6.4, stripped to what
 * this system actually speaks.
 *
 * Mbed TLS ships about four hundred configuration switches and expects
 * you to turn off the ones you do not want. That is the whole strip:
 * there is no subsetting of the source tree by hand, because every
 * feature is already wrapped in `#if defined(MBEDTLS_..._C)` and a file
 * whose feature is off compiles to an empty object. What is vendored is
 * the library as published; what is *built* is this file's answer to it.
 *
 * The brief was "TLS 1.3, SHA-256 and AES-GCM only". That is the record
 * layer exactly, and it is also not a handshake. A TLS 1.3 client cannot
 * reach the first encrypted byte without:
 *
 *   - an ephemeral key agreement, because 1.3 deleted static RSA key
 *     transport; X25519 and P-256 are the two groups every server
 *     offers, so both are here.
 *   - signature verification for CertificateVerify, which means ECDSA
 *     and RSA-PSS, which means bignum arithmetic.
 *   - HKDF, which the key schedule is built out of.
 *
 * Those are TLS 1.3's own minimum, not scope that crept. Everything
 * genuinely optional is off: no TLS 1.2, no DTLS, no session tickets,
 * no renegotiation, no CBC, no ChaCha20 (the record layer is AES-GCM,
 * as asked), no DHM, no server side.
 *
 * ---- what a bare-metal target changes ----
 *
 * MBEDTLS_PLATFORM_MEMORY is the hook the brief asked for: it makes
 * every allocation inside the library go through a function pointer
 * pair, which src/mtls.h points at the kernel heap. Nothing here calls
 * malloc, because there is no malloc.
 *
 * MBEDTLS_NO_PLATFORM_ENTROPY says there is no /dev/urandom and no
 * CryptGenRandom. MBEDTLS_ENTROPY_HARDWARE_ALT then requires us to
 * supply mbedtls_hardware_poll ourselves; ours is RDRAND, with the
 * retry loop Intel documents and a hard failure if the instruction is
 * missing, because silently seeding a CSPRNG with a constant is the
 * one failure mode here that looks exactly like success.
 *
 * MBEDTLS_THREADING_ALT is what makes eight simultaneous connections
 * safe. PSA keeps a global key store and the library has shared state
 * behind it; without a mutex, two threads in a handshake at once
 * corrupt it. The mutex is the scheduler's.
 *
 * No MBEDTLS_FS_IO (no filesystem calls from inside the library -- the
 * kernel reads files and hands over buffers), no MBEDTLS_NET_C (lwIP
 * sockets are wired in through the BIO callbacks instead), no
 * MBEDTLS_HAVE_TIME (there is a clock, but not one with libc's shape).
 */

/*
 * The two functions the library formats through, declared here because
 * this file is what build_info.h includes and therefore the one header
 * every translation unit has already seen. stddef.h is the compiler's
 * own, not a system one -- it comes from GCC's include directory and
 * exists under -ffreestanding, which is the whole distinction the "no
 * standard headers" rule is drawing.
 */
#include <stddef.h>
#include <stdarg.h>
int vx_mbed_snprintf(char *buf, size_t n, const char *fmt, ...);
int vx_mbed_printf(const char *fmt, ...);

/* ===== platform ===== */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO   vx_mbed_snprintf
#define MBEDTLS_PLATFORM_PRINTF_MACRO     vx_mbed_printf
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#define MBEDTLS_HAVE_ASM

/* Eight connections, each on its own thread, sharing one PSA key store. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT

/* ===== symmetric: AES-GCM and nothing else ===== */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_C

/* ===== hash: SHA-256 ===== */
/* SHA-224 rides in the same file and costs nothing to leave enabled;
 * PSA's hash dispatch expects the pair. */
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_MD_C
#define MBEDTLS_HKDF_C

/* ===== random ===== */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* ===== asymmetric: the part TLS 1.3 cannot do without ===== */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECDSA_DETERMINISTIC
#define MBEDTLS_HMAC_DRBG_C
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

/*
 * Not optional either, and it is the switch whose absence looks least
 * like its cause. TLS 1.3 removed PKCS#1 v1.5 signatures: an RSA
 * certificate must be verified with RSA-PSS, so this is the only RSA
 * signature scheme a 1.3 handshake can use. Without it Mbed TLS still
 * builds, still offers RSA in the ClientHello -- as rsa_pkcs1_sha256,
 * which 1.3 forbids -- and every RSA server on the internet answers
 * with a handshake_failure alert. Which is most of them.
 */
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C

#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C

/* ===== certificates =====
 *
 * Parsed, and deliberately not trusted. There is no certificate
 * authority store on the volume, so mbedtls_ssl_conf_authmode() is set
 * to MBEDTLS_SSL_VERIFY_NONE in src/mtls.h: the chain is decoded, the
 * server's signature over the handshake transcript is checked against
 * the key in the leaf, and nothing establishes that the leaf belongs to
 * the host in the address bar. That is encryption without
 * authentication, and it stops an eavesdropper but not a machine in the
 * middle. It is stated here, in the README, and by
 * tls_verifies_certificates() returning 0, because a system that
 * implies otherwise is worse than one with no TLS at all.
 */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_PEM_PARSE_C

/* ===== PSA =====
 *
 * Not optional. TLS 1.3 in Mbed TLS 3.x is written against the PSA
 * Crypto API throughout -- the key schedule, the record protection and
 * the key shares all go through psa_* calls -- so MBEDTLS_SSL_PROTO_TLS1_3
 * without MBEDTLS_PSA_CRYPTO_C does not compile, and psa_crypto_init()
 * must run before the first handshake or every one of them fails.
 */
#define MBEDTLS_PSA_CRYPTO_C

/* ===== TLS ===== */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_3
#define MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_EPHEMERAL_ENABLED
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE

/* One record's worth of buffer per direction per connection. Eight
 * connections at the 16 KB maximum would be a quarter of a megabyte of
 * permanently resident heap; 4 KB in and 8 KB out is enough for a
 * certificate chain and an HTTP request, and servers honour the
 * max_fragment_length we advertise. */
#define MBEDTLS_SSL_MAX_CONTENT_LEN     8192
#define MBEDTLS_SSL_IN_CONTENT_LEN      8192
#define MBEDTLS_SSL_OUT_CONTENT_LEN     4096

/* ===== deliberately absent =====
 *
 * MBEDTLS_FS_IO            no filesystem inside the library
 * MBEDTLS_NET_C            lwIP sockets are wired to the BIO callbacks
 * MBEDTLS_TIMING_C         no gettimeofday
 * MBEDTLS_HAVE_TIME        no time_t
 * MBEDTLS_SSL_PROTO_TLS1_2 1.3 only, as asked
 * MBEDTLS_SSL_PROTO_DTLS   no datagram TLS
 * MBEDTLS_SSL_SRV_C        client only
 * MBEDTLS_CHACHAPOLY_C     the record layer is AES-GCM
 * MBEDTLS_CIPHER_MODE_CBC  1.3 removed CBC
 * MBEDTLS_DHM_C            1.3 removed finite-field DH from practice
 * MBEDTLS_SELF_TEST        checked from tools/mbedtls_test.c instead
 * MBEDTLS_DEBUG_C          off by default; -DVX_MBEDTLS_DEBUG turns it on
 */
#ifdef VX_MBEDTLS_DEBUG
#define MBEDTLS_DEBUG_C
#define MBEDTLS_ERROR_C
#endif

#endif /* VEXTRO_MBEDTLS_CONFIG_H */
