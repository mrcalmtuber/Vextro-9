# third_party/wpe-config/vextro-toolchain.cmake
#
# The cross-compilation description CMake needs before it runs a single
# compiler test.
#
# Passed as CMAKE_TOOLCHAIN_FILE rather than folded into vextro-wpe.cmake
# because CMake reads it earlier — before it tries to compile anything,
# which on a bare-metal target it must be told not to do in the usual
# way.

# CMAKE_SYSTEM_NAME is what makes this a cross build. "Generic" is
# CMake's name for a target with no operating system underneath, which
# is exactly right here and has one consequence worth knowing: it turns
# off every try_run() test, because there is nothing to run the result
# on. WebKit's CMake uses try_compile in a few places and try_run in
# none, so this is survivable rather than merely correct.
set(CMAKE_SYSTEM_NAME       Generic)
set(CMAKE_SYSTEM_PROCESSOR  x86_64)

# ---- and why UNIX is asserted on top of it ----
#
# Source/cmake/WebKitCommon.cmake decides what operating system it is
# building for from a closed list:
#
#     if (UNIX)
#         ... MACOS / LINUX / UNIX
#     elseif (CMAKE_SYSTEM_NAME MATCHES "Windows")
#     elseif (CMAKE_SYSTEM_NAME MATCHES "Fuchsia")
#     else ()
#         message(FATAL_ERROR "Unknown OS '${CMAKE_SYSTEM_NAME}'")
#
# There is no branch for a target without an operating system, so
# "Generic" — the honest description — is the one answer the list
# refuses, and it refuses it at include() time, before a single
# find_package runs. Configuring at all means landing on one of those
# three names.
#
# UNIX is the one to assert, and *not* CMAKE_SYSTEM_NAME Linux, for two
# reasons.
#
# The first is which branch it lands on. With UNIX true, APPLE false and
# the system name not matching "Linux", WebKit sets WTF_OS_UNIX — the
# generic-Unix path the BSDs use. Naming the system Linux would set
# WTF_OS_LINUX instead and select build files written for glibc, /proc
# and Linux-only system calls, none of which exist here. The weaker
# claim is the true one: this system has descriptors, sockets, threads,
# mmap and a fork that copies on write, and does not have /proc.
#
# The second is that CMAKE_SYSTEM_NAME Linux would also make CMake load
# Platform/Linux.cmake, which describes a machine with a dynamic loader
# — shared library rules, -rdynamic, CMAKE_DL_LIBS. Platform/Generic
# describes one without, which is this one. Setting UNIX by hand takes
# the branch without taking the platform description with it.
#
# So this is a claim about the target rather than a spoof of it, and the
# claim is defensible: what UNIX selects downstream is a process model,
# and by now this system has one.
#
# ---- and why it cannot simply be set here ----
#
# `set(UNIX 1)` in this file does not survive. project() recomputes
# UNIX, WIN32 and APPLE from CMAKE_SYSTEM_NAME after the toolchain file
# has been read, and overwrites whatever was there. Measured rather than
# assumed — a four-line probe prints
#
#     BEFORE project(): UNIX='1'      (inherited from the host)
#     AFTER  project(): UNIX=''       (recomputed for Generic)
#
# and WebKit's CMakeLists.txt calls project() on line 10 and
# include(WebKitCommon) on line 16, so the assignment is undone six
# lines before it is read.
#
# CMAKE_PROJECT_INCLUDE is the seam CMake provides for exactly this: a
# file run as the last step of every project() call, which is after the
# recompute and before line 16. It is the documented way to configure a
# project whose CMakeLists you do not own, and it leaves not one byte of
# WebKit changed.
set(CMAKE_PROJECT_INCLUDE
    "${CMAKE_CURRENT_LIST_DIR}/vextro-project-inject.cmake")

set(CMAKE_C_COMPILER        x86_64-elf-gcc)
set(CMAKE_CXX_COMPILER      x86_64-elf-g++)
set(CMAKE_AR                x86_64-elf-ar)
set(CMAKE_RANLIB            x86_64-elf-ranlib)
set(CMAKE_LINKER            x86_64-elf-ld)

# A compiler that cannot link a hosted program cannot pass CMake's
# default compiler check, which builds and links a complete executable.
# STATIC_LIBRARY tells it to check by producing an archive instead,
# which is the only thing this toolchain can produce on its own.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# The same flags every other user-space object in this repository is
# built with, and each one for the same reason:
#
#   -ffreestanding      there is no hosted environment; the C library is
#                       libc/, and it does not implement everything the
#                       standard promises a hosted one does
#   -mno-red-zone       the trampoline stubs the loader maps into every
#                       process push onto the caller's stack, so the 128
#                       bytes below RSP the ABI promises a leaf function
#                       are not actually untouched here
#   -ftls-model=initial-exec
#                       without it GCC emits the general-dynamic TLS
#                       sequence, which is a call to __tls_get_addr
#                       through a module table that exists to support
#                       shared libraries. There are none, and nothing
#                       resolves that call.
#   -fno-exceptions     see the note in the README about libsupc++
#   -fno-rtti           the same
#
# ---- and the two defines, which are the other half of the OS answer ----
#
# set(UNIX 1) above tells WebKit's *build system* what this target is.
# It does not tell the compiler, and the two find out separately:
# Source/WTF/wtf/PlatformOS.h derives OS() entirely from predefined
# macros, never from the CMake variables. Its OS(UNIX) test is
#
#     #if OS(AIX) || OS(DARWIN) || ... || OS(LINUX) || OS(NETBSD)
#         || OS(OPENBSD) || defined(unix) || defined(__unix)
#         || defined(__unix__)
#
# and x86_64-elf-gcc, being a bare-metal compiler, predefines none of
# them. So a build configured past line 152 would still compile every
# WTF source with no operating system selected at all, which is not a
# state that header has a branch for either.
#
# -D__unix__ is the smallest true thing to say. It claims a process
# model — descriptors, sockets, threads, mmap, fork — and by now this
# system has one; it does not claim Linux, so nothing reaches for
# /proc, epoll or a glibc-shaped syscall wrapper on the strength of it.
#
# __vextro__ claims nothing and is here to be asked. Ported code that
# needs to know which Unix this is has no other way to find out, and
# `#ifdef __vextro__` in a port file is honest in a way that
# `#ifdef __unix__` doing double duty would not be.
#
# Both are set here and only here. The C library, libcxx and the three
# library ports are all built by the main Makefile without them, and
# they stay that way — the nine suites that pass on the machine were
# compiled the way they were compiled.
set(VX_COMMON_FLAGS
    "-O2 -ffreestanding -fno-stack-protector -fno-stack-check -mno-red-zone \
-fPIC -msse -msse2 -mfpmath=sse -fno-math-errno -ftls-model=initial-exec \
-D__unix__=1 -D__vextro__=1")

# ---- where the libraries actually are ----
#
# This file is three directories down from the root, and the two include
# trees are at the top of it. Derived rather than written out, so that a
# clone in a different place still builds.
get_filename_component(VX_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

set(VX_LIBC_INCLUDE   "${VX_ROOT}/libc/include")
set(VX_LIBCXX_INCLUDE "${VX_ROOT}/libcxx/include")

if(NOT EXISTS "${VX_LIBC_INCLUDE}/stdio.h")
    message(FATAL_ERROR
        "libc/include is not where this toolchain file expects it "
        "(${VX_LIBC_INCLUDE}). Building against the host's headers would "
        "compile and would not run.")
endif()
if(NOT EXISTS "${VX_LIBCXX_INCLUDE}/vector")
    message(FATAL_ERROR
        "libcxx/include is not where this toolchain file expects it "
        "(${VX_LIBCXX_INCLUDE}).")
endif()

# ---- and why -nostdinc++ is not optional ----
#
# This file predates libcxx/. When it was written there was no C++
# standard library for this target at all, and the flags below said
# nothing about where to find one — which was honest then and is a trap
# now: without -nostdinc++ the cross compiler happily finds the *host's*
# /usr/include/c++, compiles WebKit against a library built for a
# different operating system, and links. The failure would arrive at run
# time, as a std::string whose layout disagrees with the one in libcxx.
#
# -nostdinc is deliberately *not* used. The compiler's own freestanding
# headers — stddef.h, stdint.h, stdarg.h — are the ones libc/ is written
# against and are correct for this target; it is only the C++ half that
# has to come from here.
# ---- and the paths are quoted, which is not decoration ----
#
# These are compiler *flag strings*, split on whitespace by whoever
# consumes them. A checkout in a directory whose name contains a space —
# which is where this one lives — otherwise produces
#
#     x86_64-elf-gcc: error: Custom: linker input file not found
#
# from CMake's very first compiler test, with nothing to connect it to a
# missing pair of quotes. Escaped quotes here survive into the command
# line as real ones.
set(VX_CXX_INCLUDES
    "-nostdinc++ -I\"${VX_LIBCXX_INCLUDE}\" -I\"${VX_LIBC_INCLUDE}\"")

set(CMAKE_C_FLAGS_INIT   "${VX_COMMON_FLAGS} -I\"${VX_LIBC_INCLUDE}\"")
set(CMAKE_CXX_FLAGS_INIT
    "${VX_COMMON_FLAGS} -fno-exceptions -fno-rtti ${VX_CXX_INCLUDES}")

# The archives a link would need, named here so that a target which gets
# as far as linking finds them rather than reporting several thousand
# undefined symbols beginning with _Zn.
set(VX_LIBC_ARCHIVE   "${VX_ROOT}/build/libvextro.a")
set(VX_LIBCXX_ARCHIVE "${VX_ROOT}/build/libvextrocxx.a")
set(CMAKE_CXX_STANDARD_LIBRARIES
    "\"${VX_LIBCXX_ARCHIVE}\" \"${VX_LIBC_ARCHIVE}\"" CACHE STRING "" FORCE)

# Look for headers and libraries in the sysroot only, never on the host.
# Without this CMake finds /usr/include/zlib.h and concludes zlib is
# available, and the failure appears at link time as a hundred undefined
# symbols rather than at configure time as "zlib not found".
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# ---- and the sysroot those three ONLYs refer to ----
#
# `make webkit-sysroot` stages the four libraries this system actually
# has into build/webkit-sysroot, laid out the way a Unix installation
# lays them out, because that is the only shape the find modules know
# how to look in: pkg-config first (there is none here) and
# find_path/find_library second.
#
#     include/harfbuzz/hb*.h        lib/libharfbuzz.a    8.5.0
#     include/freetype2/            lib/libfreetype.a    2.13.2
#     include/sqlite3.h             lib/libsqlite3.a     3.45.1
#     include/wpe/wpe.h             lib/libwpe-1.0.a     1.16.2
#
# Every file in it is copied from something already compiled by the main
# Makefile and already green in ring 3 -- 32, 35 and 32 assertions on
# the machine for the first three. Nothing is staged that was not built,
# and nothing is built here.
#
# The directory may not exist yet; find_ commands treat a missing root
# as no root and report the libraries as not found, which is the correct
# answer in that case rather than an error to guard against.
set(CMAKE_FIND_ROOT_PATH "${VX_ROOT}/build/webkit-sysroot")

# There are no shared objects on this target -- no runtime linker, no
# dlopen, nothing to resolve one. Saying so here means find_library
# looks for libfoo.a and stops, rather than looking for a .so first and
# finding one from the host if any path ever leaks in.
set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
