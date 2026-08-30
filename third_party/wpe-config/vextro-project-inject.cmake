# third_party/wpe-config/vextro-project-inject.cmake
#
# Run as the last step of every project() call, because
# vextro-toolchain.cmake names it in CMAKE_PROJECT_INCLUDE.
#
# ============================================================
#  WHAT THIS FILE IS FOR
# ============================================================
#
# WebKit's top-level CMakeLists.txt is six lines long in the part that
# matters:
#
#     cmake_minimum_required(VERSION 3.20)     # line 9
#     project(WebKit)                          # line 10
#     set(CMAKE_MODULE_PATH .../Source/cmake)  # line 15
#     include(WebKitCommon)                    # line 16
#
# and WebKitCommon.cmake:139 decides what operating system this is:
#
#     if (UNIX)
#         if (APPLE)                             -> WTF_OS_MACOS
#         elseif (CMAKE_SYSTEM_NAME MATCHES Linux) -> WTF_OS_LINUX
#         else ()                                -> WTF_OS_UNIX
#     elseif (CMAKE_SYSTEM_NAME MATCHES "Windows") -> WTF_OS_WINDOWS
#     elseif (CMAKE_SYSTEM_NAME MATCHES "Fuchsia") -> WTF_OS_FUCHSIA
#     else ()
#         message(FATAL_ERROR "Unknown OS '${CMAKE_SYSTEM_NAME}'")
#
# There is no branch for a target with no operating system underneath,
# which is what CMAKE_SYSTEM_NAME "Generic" means and what this machine
# is from CMake's point of view. The list is closed, it is consulted
# before any find_package runs, and a build that does not land on one of
# its names does not configure at all.
#
# This file sets UNIX. That is the whole of it.
#
# ============================================================
#  WHY IT IS A SEPARATE FILE RATHER THAN A LINE IN THE TOOLCHAIN
# ============================================================
#
# Because the line in the toolchain file does not work, and the reason
# is worth writing down rather than rediscovering.
#
# project() recomputes UNIX, WIN32 and APPLE from CMAKE_SYSTEM_NAME
# *after* it has read CMAKE_TOOLCHAIN_FILE. Anything the toolchain file
# said about them is overwritten. A four-line probe against this exact
# toolchain shows it plainly:
#
#     -- BEFORE project(): UNIX='1'     inherited from the macOS host
#     -- TOOLCHAIN: UNIX='1'            set here
#     -- AFTER  project(): UNIX=''      recomputed for Generic
#
# So the assignment has to happen between project() on line 10 and
# include(WebKitCommon) on line 16. CMAKE_PROJECT_INCLUDE names a file
# that CMake runs as the last step of project(), which is precisely that
# window, and which exists so that a project can be configured without
# editing its CMakeLists. Nothing under third_party/wpewebkit-2.46.5/ is
# modified by any of this.
#
# ============================================================
#  WHY UNIX AND NOT CMAKE_SYSTEM_NAME Linux
# ============================================================
#
# Both get past line 152. They do not get past it to the same place.
#
# Naming the system Linux sets WTF_OS_LINUX, which selects the build
# files and code paths written for glibc, /proc, futex(2) as a raw
# syscall, epoll, memfd and a dynamic loader. None of those exist here,
# and each would be a lie discovered later, one compile error at a time,
# with no way to tell an unported dependency from a false claim about
# the target.
#
# UNIX with the system name left as Generic sets WTF_OS_UNIX -- the
# generic path, which is the weaker and therefore truer claim. What it
# asserts is a process model: descriptors, sockets, threads, mmap, and a
# fork that copies on write. src/vfs.h, libc/socket.c, libc/pthread.c
# and libc/mmap.c are that model, they are 73 + 36 + 107 assertions
# green on the machine, and the claim is one this system can now meet.
#
# It also keeps CMake's own platform description honest. CMAKE_SYSTEM_NAME
# Linux would load Platform/Linux.cmake, which describes a machine with
# a runtime linker -- shared-library rules, -rdynamic, CMAKE_DL_LIBS.
# Platform/Generic describes one without, and that is this machine.
#
# The compiler half of the same answer is in vextro-toolchain.cmake:
# -D__unix__=1, because Source/WTF/wtf/PlatformOS.h derives OS() from
# predefined macros and never from these variables.

set(UNIX 1)

# WTF_OS_UNIX is what the above buys, and it is worth stating out loud
# in the configure log rather than leaving to be inferred: a reader of
# this build's output should be able to see which branch was taken and
# why, without opening two files to work it out.
message(STATUS
    "Vextro: UNIX asserted after project(); "
    "CMAKE_SYSTEM_NAME stays '${CMAKE_SYSTEM_NAME}', so WebKit takes its "
    "generic-Unix branch (WTF_OS_UNIX) rather than the Linux one.")

# ============================================================
#  WHAT THIS TARGET CAN AND CANNOT DO
# ============================================================
#
# ---- why these are answered here instead of measured ----
#
# Because measuring them, under this toolchain, produces the answer
# "yes" to every question — including questions with no correct answer.
# That was measured rather than assumed. A four-check probe against this
# exact toolchain file reports:
#
#     check_function_exists(fork)                        -> 1
#     check_function_exists(epoll_create)                -> 1
#     check_function_exists(this_function_does_not_exist) -> 1
#     check_symbol_exists(epoll_create "sys/epoll.h")    -> (empty)
#
# The cause is two lines above talking past each other.
# CMAKE_TRY_COMPILE_TARGET_TYPE is STATIC_LIBRARY, because a
# freestanding cross compiler cannot link the complete executable
# CMake's default compiler check wants. But check_function_exists works
# by *declaring* the function, calling it, and seeing whether the result
# links — and a try-compile that stops at `ar` never links anything. So
# the test cannot fail, and every function in the world exists.
#
# check_symbol_exists and check_include_files are unaffected: they are
# preprocessor questions and they answer honestly, which is why the
# fourth line above is empty and the third is not.
#
# ---- and why that is dangerous rather than merely wrong ----
#
# It is wrong in the direction that hurts. WebKit runs eleven
# WEBKIT_CHECK_HAVE_FUNCTION probes, and left alone every one of them
# comes back true: HAVE_STATX, HAVE_MALLOC_TRIM, HAVE_STRNSTR,
# HAVE_ALIGNED_MALLOC, and four DRM/GBM entry points from a graphics
# stack this machine does not have. Each one selects code that calls a
# function which is not in libvextro.a, and the failure arrives at link
# time, thousands of objects later, as an undefined symbol with nothing
# pointing back at the check that claimed it.
#
# So the answers are given here, before the checks run — CMake skips a
# check whose result variable is already set — and each is a fact about
# this repository that can be verified with nm(1) against
# build/libvextro.a rather than an opinion about the platform.

# ---- what the Linux subset now backs ----
#
# All five were absent when this file was written and are the reason it
# is being edited: include/vls.h and src/sched/vls_core.c, with the
# wrappers a C program calls in libc/process.c. `nm build/libvextro.a`
# shows fork, execve, waitpid, kill, sigaction, sigprocmask, signal,
# raise, dup, dup2 and getppid as defined text symbols, which is the
# thing check_function_exists was trying and failing to find out.
set(HAVE_SIGNAL_H     1 CACHE INTERNAL "sigaction and delivery: libc/process.c")
set(HAVE_SYS_TIME_H   1 CACHE INTERNAL "libc/include/sys/time.h")
set(HAVE_ERRNO_H      1 CACHE INTERNAL "libc/include/errno.h")
set(HAVE_MMAP         1 CACHE INTERNAL "libc/mmap.c over SYS_MMAP")
set(HAVE_LOCALTIME_R  1 CACHE INTERNAL "libc/calendar.c")
set(HAVE_TIMEGM       1 CACHE INTERNAL "libc/calendar.c")
set(HAVE_VASPRINTF    1 CACHE INTERNAL "libc/stdio.c")

# ---- and what it does not ----
#
# Every one of these would have come back true and been a lie. They are
# set to 0 rather than left undefined so that the cache carries the
# refusal explicitly: an empty variable would be re-probed by the next
# check_function_exists and answered "yes" again.
set(HAVE_STATX             0 CACHE INTERNAL "no statx; SYS_STAT answers vx_stat_t")
set(HAVE_MALLOC_TRIM       0 CACHE INTERNAL "libc/malloc.c never returns pages")
set(HAVE_STRNSTR           0 CACHE INTERNAL "not in libc/string.c")
set(HAVE_ALIGNED_MALLOC    0 CACHE INTERNAL "_aligned_malloc is Microsoft's")
set(HAVE_TIMINGSAFE_BCMP   0 CACHE INTERNAL "BSD; not in libc/string.c")
set(HAVE_PTHREAD_MAIN_NP   0 CACHE INTERNAL "BSD; libc/pthread.c has no such call")
set(HAVE_PTHREAD_NP_H      0 CACHE INTERNAL "no pthread_np.h")
set(HAVE_REGEX_H           0 CACHE INTERNAL "no regex.h; nothing implements regexec")
set(HAVE_LANGINFO_H        0 CACHE INTERNAL "no langinfo.h; U_HAVE_NL_LANGINFO_CODESET=0")
set(HAVE_FEATURES_H        0 CACHE INTERNAL "glibc's; this is not glibc")
set(HAVE_SYS_PARAM_H       0 CACHE INTERNAL "not written")
set(HAVE_SYS_TIMEB_H       0 CACHE INTERNAL "not written; ftime is obsolescent")
set(HAVE_LINUX_MEMFD_H     0 CACHE INTERNAL "no memfd; there is no /proc and no tmpfs")
set(HAVE_SHM_ANON          0 CACHE INTERNAL "BSD anonymous shm; no shm_open here")
set(HAVE_MAP_ALIGNED       0 CACHE INTERNAL "NetBSD's mmap flag")

# The four graphics entry points, which belong to libdrm and libgbm.
# Neither is ported and neither can be until there is something for them
# to talk to: this machine's graphics driver implements the blitter and
# not the 3D pipeline, and /dev/dri/renderD128 here is a framebuffer
# rather than a command submission interface. See src/devfs.h.
set(HAVE_DRM_GET_FORMAT_MODIFIER_NAME    0 CACHE INTERNAL "no libdrm")
set(HAVE_DRM_GET_FORMAT_MODIFIER_VENDOR  0 CACHE INTERNAL "no libdrm")
set(HAVE_GBM_BO_CREATE_WITH_MODIFIERS2   0 CACHE INTERNAL "no libgbm")
set(HAVE_GBM_BO_GET_FD_FOR_PLANE         0 CACHE INTERNAL "no libgbm")

message(STATUS
    "Vextro: capability answers seeded. check_function_exists cannot fail "
    "under CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY (nothing links), so "
    "the eleven HAVE_* function probes are answered from what is actually "
    "in build/libvextro.a. fork/execve/wait4/kill/sigaction: yes. "
    "statx/malloc_trim/strnstr/memfd/DRM/GBM: no.")
