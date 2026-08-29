# third_party/wpe-config/vextro-wpe.cmake
#
# How WPE WebKit is configured for Vextro.
#
# Passed to cmake with -C, which loads it as an *initial cache*: every
# variable below is set before WebKit's own CMake runs, so its feature
# detection sees these as the answers rather than overwriting them. The
# alternative -- a long line of -D arguments -- puts the same values in
# the same places and leaves nowhere to say why.
#
# ============================================================
#  1. THE JIT, AND WHY EVERY TIER OF IT IS OFF
# ============================================================
#
# JavaScriptCore has four execution tiers, and three of them work by
# writing machine code into memory and jumping to it. On Vextro that is
# not slow, or discouraged, or a security trade-off to be weighed. It is
# impossible, and it is impossible by construction:
#
#   Every page of every program in this system is mapped either writable
#   or executable and never both. The two-segment linker script splits
#   text from data, the loader maps each page with the protection its
#   segment asked for, and -- the part that matters here -- the kernel
#   *refuses* mmap and mprotect requests for PROT_WRITE|PROT_EXEC. The
#   refusal is in the system call, so there is no sequence of calls from
#   user space that arrives at a page which is both.
#
# The consequence is worth stating precisely, because it is the reason
# these flags are not merely a performance setting. A JIT does not fail
# at the mprotect it checked the return value of. It fails at the jump,
# some thousands of instructions later, with a fault at an address in the
# middle of a data buffer and nothing connecting it to the decision. So
# every tier is turned off at configure time, and the interpreter -- the
# one tier that executes bytecode rather than emitting instructions --
# is what runs.
#
# The four are named separately rather than relying on ENABLE_JIT=OFF to
# imply them, because JSC's CMake will happily enable a tier whose own
# flag is still ON.

set(ENABLE_JIT                    OFF CACHE BOOL "no writable-executable pages exist here" FORCE)
set(ENABLE_C_LOOP                 ON  CACHE BOOL "the bytecode interpreter, which is the whole engine now" FORCE)
set(ENABLE_DFG_JIT                OFF CACHE BOOL "" FORCE)
set(ENABLE_FTL_JIT                OFF CACHE BOOL "" FORCE)
set(ENABLE_WEBASSEMBLY            OFF CACHE BOOL "wasm has no interpreter-only path worth having" FORCE)
set(ENABLE_WEBASSEMBLY_B3JIT      OFF CACHE BOOL "" FORCE)
set(ENABLE_SAMPLING_PROFILER      OFF CACHE BOOL "needs a signal to sample on" FORCE)

# ENABLE_C_LOOP is the positive form of the same decision and is the one
# that actually matters: it selects the portable bytecode interpreter
# over the assembly-language LLInt, which is itself generated machine
# code and would need the same writable-executable page.
#
# The cost is real and is not hidden. The C loop is roughly an order of
# magnitude slower than the baseline JIT on arithmetic-heavy code. It is
# also the configuration WebKit ships for architectures it has no
# assembler backend for, so it is a supported path rather than a corner.

# ============================================================
#  2. NO GPU
# ============================================================
#
# There is no GL, EGL or Vulkan a ring-3 program on this system can
# reach. The kernel drives the integrated graphics -- src/igpu.h runs the
# blitter the compositor composites with -- but that is the kernel's, and
# nothing exports it across the system call boundary.
#
# So WebKit rasterises in software, and the finished image reaches the
# screen through third_party/wpe-port: a memory copy into the window's
# own pixels, which are already mapped into the process. See that
# directory's README for why the usual dmabuf-and-socket path has nothing
# to attach to.

set(USE_OPENGL_OR_ES              OFF CACHE BOOL "no GL driver reachable from ring 3" FORCE)
set(ENABLE_GRAPHICS_CONTEXT_GL    OFF CACHE BOOL "" FORCE)
set(ENABLE_WEBGL                  OFF CACHE BOOL "follows from the above" FORCE)
set(ENABLE_WEBGL2                 OFF CACHE BOOL "" FORCE)
set(USE_ANGLE_WEBGL               OFF CACHE BOOL "" FORCE)
set(ENABLE_ACCELERATED_2D_CANVAS  OFF CACHE BOOL "" FORCE)
set(USE_SKIA_GPU                  OFF CACHE BOOL "" FORCE)
set(ENABLE_GPU_PROCESS            OFF CACHE BOOL "there is one process" FORCE)

# ============================================================
#  3. ONE PROCESS
# ============================================================
#
# WebKit's normal architecture is three processes -- UI, Web and Network
# -- talking over sockets, with the sandbox as the point. Vextro has no
# sockets in ring 3 and no way to pass a descriptor between processes,
# because it has no descriptors in ring 3 at all.
#
# The isolation that architecture buys is not lost so much as provided
# elsewhere: this browser runs in ring 3 in an address space of its own,
# with a restricted token, and cannot write a file, touch the registry or
# reach a disk block without a person answering a prompt. That is a
# different boundary in a different place, and it is the one this system
# already enforces on every program.

set(ENABLE_MULTIPROCESS           OFF CACHE BOOL "no sockets in ring 3 to talk over" FORCE)
set(ENABLE_NETWORK_PROCESS        OFF CACHE BOOL "" FORCE)
set(ENABLE_BUBBLEWRAP_SANDBOX     OFF CACHE BOOL "the sandbox here is the address space" FORCE)

# ============================================================
#  4. WHAT IS LEFT OUT, AND WHY EACH ONE
# ============================================================
#
# Not to make the build smaller -- though it does -- but because each of
# these needs a subsystem this machine does not have, and a WebKit
# configured to look for one fails at run time rather than at configure
# time.

set(ENABLE_VIDEO                  OFF CACHE BOOL "no GStreamer" FORCE)
set(ENABLE_WEB_AUDIO              OFF CACHE BOOL "no audio path from ring 3" FORCE)
set(ENABLE_MEDIA_STREAM           OFF CACHE BOOL "" FORCE)
set(ENABLE_MEDIA_SOURCE           OFF CACHE BOOL "" FORCE)
set(ENABLE_ENCRYPTED_MEDIA        OFF CACHE BOOL "" FORCE)
set(USE_GSTREAMER                 OFF CACHE BOOL "" FORCE)
set(ENABLE_SPEECH_SYNTHESIS       OFF CACHE BOOL "" FORCE)
set(ENABLE_GAMEPAD                OFF CACHE BOOL "no device, and libwpe's is left out" FORCE)
set(ENABLE_TOUCH_EVENTS           OFF CACHE BOOL "no touch device" FORCE)
set(ENABLE_GEOLOCATION            OFF CACHE BOOL "" FORCE)
set(ENABLE_NOTIFICATIONS          OFF CACHE BOOL "" FORCE)
set(ENABLE_REMOTE_INSPECTOR       OFF CACHE BOOL "needs a socket to be inspected over" FORCE)
set(ENABLE_INTROSPECTION          OFF CACHE BOOL "" FORCE)
set(ENABLE_SPELLCHECK             OFF CACHE BOOL "no enchant" FORCE)
set(ENABLE_XSLT                   OFF CACHE BOOL "no libxslt" FORCE)
set(ENABLE_SERVICE_WORKER         OFF CACHE BOOL "needs a storage process" FORCE)
set(ENABLE_WEB_RTC                OFF CACHE BOOL "" FORCE)
set(ENABLE_JOURNALD_LOG           OFF CACHE BOOL "" FORCE)

# ============================================================
#  5. THREADS
# ============================================================
#
# WebKit is thoroughly threaded and cannot be built otherwise. What it
# gets here is real -- SYS_CLONE gives a thread the caller's address
# space, and libc/pthread.c is a complete implementation over it -- with
# one property that has to be stated because it changes how the engine
# performs rather than whether it works.
#
# Every ring-3 thread in this system runs on processor zero. That is not
# a limitation of the threading library but of the system call boundary:
# src/syscall.h keeps the kernel stack for the next entry from user mode
# in a single global word, and two user threads entering it on two
# processors would overwrite each other. So threads here buy concurrency
# and structure, not parallelism, and a worker pool sized to the
# processor count should be told there is one.

set(USE_SYSTEM_MALLOC             ON  CACHE BOOL "bmalloc wants madvise and mmap semantics this system does not have" FORCE)
set(ENABLE_SAMPLING_PROFILER      OFF CACHE BOOL "" FORCE)

# ============================================================
#  6. THE TOOLCHAIN
# ============================================================
#
# Set by third_party/wpe-config/vextro-toolchain.cmake, which is passed
# separately as CMAKE_TOOLCHAIN_FILE because CMake reads it before this
# file and before any compiler test.

set(PORT                          "WPE" CACHE STRING "" FORCE)
set(CMAKE_BUILD_TYPE              "Release" CACHE STRING "" FORCE)
set(ENABLE_DEVELOPER_MODE         OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS             OFF CACHE BOOL "there is no dynamic loader" FORCE)
set(ENABLE_STATIC_JSC             ON  CACHE BOOL "" FORCE)
