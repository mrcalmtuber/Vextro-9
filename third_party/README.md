# third_party

Two libraries, vendored rather than fetched at build time, so that a
clone of this repository builds the same system a year from now whether
or not the upstreams still exist.

## lwip/ — lwIP 2.2.1

Tag `STABLE-2_2_1_RELEASE`. BSD 3-clause; see `lwip/LICENSE`.

39 of the 124 source files: IPv4, ARP, ICMP, UDP, TCP, DHCP, DNS and the
sequential/socket API over Ethernet. Left out are IPv6, PPP, SLIP,
6LoWPAN, the bridge interface and the ZigBee encapsulation, none of
which this machine has any way to use. The `src/include` tree is
complete, because the headers include one another freely and pruning
them buys nothing.

## mbedtls/ — Mbed TLS 3.6.4 (LTS)

Tag `mbedtls-3.6.4`. Apache 2.0; see `mbedtls/LICENSE`.

56 of the 108 library files. The other 52 were not chosen by hand: they
are the ones that compile to an *empty object* under
`vextro_config.h`, because every feature in Mbed TLS is wrapped in
`#if defined(MBEDTLS_..._C)` and a feature that is off produces a file
with nothing in it. The strip is expressed in the config header and the
file list follows from it, which is the only way round that stays true
when the config changes.

Three exceptions are kept despite compiling to nothing by default --
`debug.c`, `error.c` and `ssl_debug_helpers_generated.c` -- because
`-DVX_MBEDTLS_DEBUG` switches them on.

## The port

Nothing above is modified. Everything this system adds lives beside it:

    lwip-port/lwipopts.h        lwIP's configuration
    lwip-port/arch/cc.h         compiler and diagnostics
    lwip-port/arch/sys_arch.h   the four types lwIP blocks on
    mbedtls/vextro_config.h     the strip
    mbedtls/threading_alt.h     the mutex type
    include/                    a freestanding libc for the vendored code
    vxport.c                    and its implementation

    ../src/lwipglue.c           sys_arch on the scheduler, netif on e1000
    ../src/tlsglue.c            the platform hooks and the eight-slot pool
    ../src/vxport.h             the whole seam with the kernel
    ../src/vxnet.h              what the rest of the system sees

Keeping the vendored trees unpatched is the point: upgrading lwIP means
replacing a directory, and any incompatibility appears as a compile
error in the port rather than as a merge conflict inside someone else's
source file.
