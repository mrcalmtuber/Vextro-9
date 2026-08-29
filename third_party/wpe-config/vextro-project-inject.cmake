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
