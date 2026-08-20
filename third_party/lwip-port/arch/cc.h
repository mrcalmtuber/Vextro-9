#ifndef VEXTRO_LWIP_ARCH_CC_H
#define VEXTRO_LWIP_ARCH_CC_H

/*
 * third_party/lwip-port/arch/cc.h — what lwIP is allowed to assume about
 * the compiler and the machine underneath it.
 *
 * The distinction this file keeps drawing is between headers the
 * *compiler* provides and headers an *operating system* provides.
 * stdint.h, stddef.h, stdarg.h and limits.h come out of GCC's own
 * include directory and exist under -ffreestanding, on a machine with no
 * C library at all -- they are part of the language. inttypes.h,
 * ctype.h and errno.h are not; they belong to a hosted implementation,
 * and lwIP is told below to do without each of them.
 */

#include <stdint.h>
#include <stddef.h>

/* lwIP would otherwise pull inttypes.h in for PRIu32 and friends. */
#define LWIP_NO_INTTYPES_H  1
#define X8_F   "02x"
#define U16_F  "u"
#define S16_F  "d"
#define X16_F  "x"
#define U32_F  "u"
#define S32_F  "d"
#define X32_F  "x"
#define SZT_F  "lu"

/* And ctype.h for isdigit/isspace, which its DNS and address parsers
 * use. lwIP defines its own when told the header is absent. */
#define LWIP_NO_CTYPE_H     1

/* errno comes from lwIP itself (LWIP_PROVIDE_ERRNO in lwipopts.h), so
 * there is no errno.h to find and no thread-local to invent. */

/* x86-64 is little-endian, and this is the only machine this tree
 * targets -- the aarch64 port is frozen. */
#define BYTE_ORDER          LITTLE_ENDIAN

/* GCC's packing is what the protocol headers need; the begin/end
 * variants are for compilers that spell it as a pragma. */
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT  __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x
/* Deliberately not PACK_STRUCT_USE_INCLUDES: that is for compilers whose
 * packing is a pragma and which therefore need a bpstruct.h/epstruct.h
 * pair around every protocol header. GCC says it with an attribute, so
 * the macros above are the whole mechanism. */

#define LWIP_CONST_CAST(target_type, val) ((target_type)((uintptr_t)val))

/*
 * ---- diagnostics ----
 *
 * Routed at the serial port through the port layer rather than at
 * printf, because there is no printf in the kernel's translation unit
 * and formatting one for lwIP's benefit would mean a vsnprintf in the
 * hot path of a packet handler.
 *
 * It formats properly. The first version dropped the arguments and
 * printed only the literal part of each message, on the reasoning that
 * a vsnprintf has no business in a packet handler -- and that was true
 * right up to the first thing that went wrong, where "dhcp_recv:
 * expected xid" without the two numbers is a sentence that names the
 * problem and withholds the answer. The formatter is only reached when
 * LWIP_DEBUG is on, which is off in the build that ships.
 */
void vx_log(const char *s);
void vx_log_u32(uint32_t v);

#define LWIP_PLATFORM_DIAG(x)   do { vx_lwip_diag x; } while (0)
void vx_lwip_diag(const char *fmt, ...);

/*
 * An assertion failure must not stop the machine.
 *
 * lwIP's default is an abort. On a desktop operating system that means
 * a malformed packet from anywhere on the network can take down the
 * compositor -- the failure is reachable by a stranger, which makes a
 * crash-on-assert a denial of service with a one-line exploit. Naming
 * it and carrying on is the only behaviour that is safe here; the
 * caller's own error path then handles the packet as invalid, which is
 * what it is.
 */
#define LWIP_PLATFORM_ASSERT(x) \
    do { vx_log("[lwip] assertion failed: "); vx_log(x); vx_log("\n"); } while (0)

/* ---- randomness ----
 *
 * Used for initial sequence numbers, DNS transaction IDs and ephemeral
 * port selection. Every one of those is a security property: a
 * predictable ISN lets an off-path attacker inject into a connection,
 * and a predictable DNS transaction ID lets one forge an answer. So
 * this is RDRAND, not a counter.
 */
uint32_t vx_lwip_rand(void);
#define LWIP_RAND()  vx_lwip_rand()

#endif /* VEXTRO_LWIP_ARCH_CC_H */
