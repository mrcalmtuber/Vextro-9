/*
 * third_party/libgcrypt-port/config.h — libgcrypt's build configuration,
 * decided here instead of by ./configure.
 *
 * The same arrangement FreeType, libjpeg-turbo, libepoxy and
 * libgpg-error are under, and for the reason spelled out beside each:
 * configure's probes work by linking and running a program, this target
 * has nowhere to run one, and a configure pass on the build machine
 * would faithfully describe macOS on arm64.
 *
 * ---- the decision that shapes the whole file ----
 *
 * **No assembly, anywhere.** Every HAVE_GCC_INLINE_ASM_*, every
 * ENABLE_*_SUPPORT and every HAVE_CPU_ARCH_* is left undefined, which
 * puts the portable C implementation on every path — AES, the hashes,
 * the bignums, all of it.
 *
 * That is not caution. libgcrypt's assembly is selected at *run time*
 * from CPUID through its hwfeatures layer, and the .S files are
 * assembled by upstream's own build with its own defines; both of those
 * are reachable from a configure run and neither is reachable from here.
 * With none of them defined, `_gcry_get_hw_features()` returns zero and
 * every cipher takes the branch it would take on a processor with no
 * extensions — which is a supported configuration upstream tests, not a
 * corner. What it costs is speed and nothing else: apps/gcrypttest.c
 * checks the output against published vectors, and the answers are the
 * same answers.
 *
 * ---- with one exception, which is not an exception to that ----
 *
 * HAVE_CPU_ARCH_X86 *is* defined, and it is not a CPU extension. It says
 * "this is an x86 processor", which is true, and what it selects in
 * mpi/ec-inline.h is a handful of plain `addq`/`adcq` sequences — base
 * instructions every x86-64 has had since the architecture existed.
 *
 * It is defined because leaving it out is not the conservative choice,
 * it is a wrong one. With neither HAVE_CPU_ARCH_X86 nor
 * HAVE_CPU_ARCH_ARM set, ec-inline.h has no 64-bit definition of
 * ADD3_LIMB64 and falls through to a *32-bit* one that packs each limb
 * into a .hi/.lo pair — and then feeds those halves to longlong.h's
 * `addq`, which is 64-bit. The assembler says
 *
 *     ec-nist.c:260: Error: operand type mismatch for `add'
 *
 * and the cause is four files away. The fallback is written for machines
 * with 32-bit limbs and this is not one.
 *
 * It also turns on run-time CPU feature detection (src/hwf-x86.c, which
 * is compiled), so _gcry_get_hw_features() reports what the processor
 * actually has. Nothing acts on it: every accelerated implementation is
 * selected by a USE_* macro that is off, so the detection is
 * informational here and `gcry_get_config` reports it honestly.
 *
 * ---- and the one that decides where randomness comes from ----
 *
 * USE_RNDGETENTROPY, which is the getentropy() backend. The alternatives
 * are rndoldlinux (opens /dev/random and reads it), rndunix (runs
 * programs like `ps` and hashes their output), rndegd (talks to an
 * entropy daemon over a socket) and rndjent (times its own instruction
 * stream). This system has getentropy in libc over SYS_RANDOM, which
 * goes to RDSEED/RDRAND — so the direct one is both available and the
 * best of the five, and the others are left out rather than compiled
 * and never chosen.
 */

#ifndef VX_GCRYPT_CONFIG_H
#define VX_GCRYPT_CONFIG_H

/*
 * The name the library checks for, and it is not decoration.
 * src/types.h refuses to compile without it —
 *
 *     #ifndef _GCRYPT_CONFIG_H_INCLUDED
 *     # error config.h must be included before types.h
 *     #endif
 *
 * — which is upstream's guard against a source file that reached types.h
 * through some path that had not established the integer widths. The
 * guard is right and the name is arbitrary; it is simply the one the
 * library agreed with itself on, and a hand-written config.h has to
 * honour it.
 */
#define _GCRYPT_CONFIG_H_INCLUDED 1

#define PACKAGE           "libgcrypt"
#define PACKAGE_NAME      "libgcrypt"
#define PACKAGE_TARNAME   "libgcrypt"
#define PACKAGE_VERSION   "1.10.3"
#define PACKAGE_STRING    "libgcrypt 1.10.3"
#define PACKAGE_BUGREPORT "https://bugs.gnupg.org"
#define PACKAGE_URL       ""
#define VERSION           "1.10.3"
#define BUILD_REVISION    "vextro"
#define BUILD_TIMESTAMP   "1970-01-01T00:00:00"
#define PRINTABLE_OS_NAME "Vextro"
#define LT_OBJDIR         ".libs/"

/* Which error source libgcrypt stamps into the gpg_error_t values it
 * returns, so that a caller can tell an error from this library from one
 * raised by something else sharing the same numbering. */
#define GPG_ERR_SOURCE_DEFAULT GPG_ERR_SOURCE_GCRYPT

/* ---- integer widths ---- */
#define SIZEOF_UNSIGNED_SHORT     2
#define SIZEOF_UNSIGNED_INT       4
#define SIZEOF_UNSIGNED_LONG      8
#define SIZEOF_UNSIGNED_LONG_LONG 8
#define SIZEOF_UINT64_T           8
#define SIZEOF_VOID_P             8
#define HAVE_UINTPTR_T 1
#define HAVE_U16 1
#define HAVE_U32 1
#define HAVE_U64 1
#define HAVE_BYTE 1
#define HAVE_USHORT 1
#define HAVE_VLA 1

/* ---- headers and functions this C library actually has ----
 *
 * Checkable with nm(1) against build/libvextro.a or by looking in
 * libc/include, which is why they are answered by hand: a probe that
 * cannot fail would have said yes to every one of them, including the
 * dozen below that are absent. */
#define STDC_HEADERS 1
#define HAVE_STDIO_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_UNISTD_H 1
#define HAVE_WCHAR_H 1
#define HAVE_DLFCN_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_MMAN_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_RANDOM_H 1
#define HAVE_MEMMOVE 1
#define HAVE_STRERROR 1
#define HAVE_STRTOUL 1
#define HAVE_STPCPY 1
#define HAVE_STRCASECMP 1
#define HAVE_ATEXIT 1
#define HAVE_RAISE 1
#define HAVE_VPRINTF 1
#define HAVE_MMAP 1
#define HAVE_GETPAGESIZE 1
#define HAVE_SYSCONF 1
#define HAVE_GETPID 1
#define HAVE_CLOCK 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_FTRUNCATE 1
#define HAVE_WAITPID 1
#define HAVE_WAIT4 1
#define HAVE_PTHREAD 1
#define HAVE_GETENTROPY 1

/* GCC's, and this is GCC. The bswap and count-leading-zero builtins are
 * what the portable C paths use in place of the assembly that is off. */
#define HAVE_BUILTIN_BSWAP32 1
#define HAVE_BUILTIN_BSWAP64 1
#define HAVE_BUILTIN_CLZ 1
#define HAVE_BUILTIN_CLZL 1
#define HAVE_BUILTIN_CTZ 1
#define HAVE_BUILTIN_CTZL 1
#define HAVE_SYNC_SYNCHRONIZE 1
#define HAVE_GCC_ATTRIBUTE_ALIGNED 1
#define HAVE_GCC_ATTRIBUTE_PACKED 1
#define HAVE_GCC_ATTRIBUTE_MAY_ALIAS 1
#define HAVE_GCC_DEFAULT_ABI_IS_SYSV_ABI 1

/* This is an x86 processor. See the long note at the top for why this
 * one architecture flag is set while every extension flag is not, and
 * what goes wrong four files away when it is left out. */
#define HAVE_CPU_ARCH_X86 1

/* ---- and what it does not have ----
 *
 * Each of these would have come back true from a probe that never links:
 *
 *   mlock         no memory may be pinned; see the secure-memory note
 *   syslog        there is one serial line and no daemon behind it
 *   getrusage,    no per-process accounting; wait4 refuses a rusage
 *   getauxval,    no auxiliary vector — the loader hands a program a
 *   sys/auxv.h    stack and three registers, not an auxv
 *   fcntl, flockfile, doprnt, rand, stricmp, explicit_bzero,
 *   gethrtime, sys/capability.h, spawn.h, /dev/random by name
 */

/*
 * ---- secure memory, and the one place this differs from a hosted build
 * ----
 *
 * HAVE_MLOCK is not defined, so libgcrypt's secure-memory pool is
 * ordinary memory rather than pinned memory. That is worth stating
 * plainly rather than leaving in a list, because "secure memory" is a
 * name that promises something:
 *
 * What mlock buys on a hosted system is that a page holding a key is
 * never written to swap. This system *does* have a pager — src/swap.h —
 * so the property genuinely is not obtained, and a key in gcry_malloc_secure
 * can reach the pagefile. What is still obtained is the other half of
 * what the pool does: the memory is wiped on release rather than merely
 * freed.
 *
 * HAVE_BROKEN_MLOCK is deliberately also not defined. That flag is for
 * systems where mlock exists and lies; here it does not exist at all,
 * which libgcrypt handles by simply not attempting it.
 */

/* ---- what is compiled in ----
 *
 * The default set, minus nothing. Curating it would have saved some
 * kilobytes and cost the ability to say that this is upstream's own
 * configuration: WebKit's WebCrypto reaches for AES, SHA-1 through
 * SHA-512, HMAC, PBKDF2, RSA, ECC and their OIDs, and a port that had
 * quietly dropped Camellia would be one nobody could compare against a
 * distribution build.
 */
#define LIBGCRYPT_CIPHERS "arcfour:blowfish:cast5:des:aes:twofish:serpent:rfc2268:seed:camellia:idea:salsa20:gost28147:chacha20:sm4"
#define LIBGCRYPT_PUBKEY_CIPHERS "dsa:elgamal:rsa:ecc"
#define LIBGCRYPT_DIGESTS "crc:gostr3411-94:md4:md5:rmd160:sha1:sha256:sha512:sha3:tiger:whirlpool:stribog:blake2:sm3"
#define LIBGCRYPT_KDFS "s2k:pkdf2:scrypt"

#define USE_AES 1
#define USE_ARCFOUR 1
#define USE_BLOWFISH 1
#define USE_CAST5 1
#define USE_CAMELLIA 1
#define USE_CHACHA20 1
#define USE_DES 1
#define USE_GOST28147 1
#define USE_IDEA 1
#define USE_RFC2268 1
#define USE_SALSA20 1
#define USE_SEED 1
#define USE_SERPENT 1
#define USE_SM4 1
#define USE_TWOFISH 1

#define USE_BLAKE2 1
#define USE_CRC 1
#define USE_GOST_R_3411_94 1
#define USE_GOST_R_3411_12 1
#define USE_MD4 1
#define USE_MD5 1
#define USE_RMD160 1
#define USE_SHA1 1
#define USE_SHA256 1
#define USE_SHA512 1
#define USE_SHA3 1
#define USE_SM3 1
#define USE_TIGER 1
#define USE_WHIRLPOOL 1

#define USE_DSA 1
#define USE_ELGAMAL 1
#define USE_ECC 1
#define USE_RSA 1

#define USE_SCRYPT 1

/* The random backend: getentropy, which is in libc over SYS_RANDOM. See
 * the note at the top for why the other four are not compiled. */
#define USE_RNDGETENTROPY 1

/*
 * Symbol visibility, off, for the reason it is off in every other port
 * here: there are no shared objects on this target, so the attribute
 * would decorate an archive with a property nothing reads.
 */
/* #undef GCRY_USE_VISIBILITY */

/* No underscore prefix on this ABI. */
/* #undef WITH_SYMBOL_UNDERSCORE */

/* Not a development snapshot, and not FIPS. The binary-check machinery
 * verifies an HMAC over the .so at load time, and there is no .so. */
/* #undef IS_DEVELOPMENT_VERSION */
/* #undef ENABLE_HMAC_BINARY_CHECK */

#endif /* VX_GCRYPT_CONFIG_H */
