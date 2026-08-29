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
set(VX_COMMON_FLAGS
    "-O2 -ffreestanding -fno-stack-protector -fno-stack-check -mno-red-zone \
-fPIC -msse -msse2 -mfpmath=sse -fno-math-errno -ftls-model=initial-exec")

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
