CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
AR      := x86_64-elf-ar
CXX     := x86_64-elf-g++
HOSTCC  := cc

# --- Kernel flags ---
#
# The floating-point bans are gone. They were there for a reason that no
# longer holds: with no per-thread state to save, any use of an XMM
# register anywhere in the kernel could be clobbered by an interrupt, so
# the only safe rule was to forbid the registers outright and confine
# them to llm.c, which never runs inside a handler.
#
# What replaced that rule is a scheduler that saves 512 bytes of extended
# state on every context switch, and interrupt handlers compiled
# general-regs-only so they cannot touch what it is preserving. With both
# in place there is nothing left for the ban to protect.
#
# -mno-red-zone stays and always will: the 128 bytes below RSP that the
# ABI promises a leaf function are not promises the processor keeps when
# it pushes an interrupt frame there.
#
# -fno-math-errno is what makes sqrt() a single instruction. Without it
# GCC has to assume the call might set errno, so it emits a call to
# sqrtf() — a function that does not exist here — instead of the SQRTSS
# the processor has had since 1999. There is no errno in this kernel, so
# the guarantee it is preserving is one nothing can observe.
#
# Note what is *not* here: -ffast-math. It would let the compiler
# reassociate floating-point sums, which was measured on the inference
# path and found to buy nothing while making the batched and unbatched
# routes through the same arithmetic disagree bit for bit.
CFLAGS  := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
            -fno-stack-check -fno-lto -fPIE -m64 -march=x86-64 \
            -msse -msse2 -mfpmath=sse -fno-math-errno -mno-red-zone \
            -Isrc -Iinclude -Ibuild -Ikernel/include -Ivxfmt $(EXTRA)

# --- Preflight ---
#
# Otherwise the build stops at the first tool it cannot find, with that
# tool's own message. `make: x86_64-elf-gcc: No such file or directory`
# is true, and tells someone who has just cloned this nothing at all
# about what to install. Everything missing is named at once, here,
# before anything is built.
#
# Skipped for clean, which has to work on a machine with no toolchain.
ifeq ($(filter clean cleandisk,$(MAKECMDGOALS)),)
NEED    := $(CC) $(LD) $(HOSTCC) python3 xorriso
MISSING := $(strip $(foreach t,$(NEED), \
             $(if $(shell command -v $(t) 2>/dev/null),,$(t))))
ifneq ($(MISSING),)
$(info )
$(info   Cannot build. Missing: $(MISSING))
$(info )
$(info   macOS:  brew install x86_64-elf-gcc x86_64-elf-binutils xorriso qemu)
$(info )
$(info   Linux:  xorriso and qemu-system-x86 are packaged; an x86_64-elf)
$(info           cross toolchain generally is not. Build one -- the OSDev)
$(info           "GCC Cross-Compiler" guide is the usual route -- or, if)
$(info           your host toolchain already produces ELF binaries:)
$(info             make CC=gcc LD=ld)
$(info )
$(error missing build tools)
endif
endif

# --- Display mode ---
# `resolution` is the key Limine actually reads, and it matches the card's
# advertised VBE mode list *exactly* — ask for a mode the card does not
# list and it silently lands on 1024x768.  (1280x832, the exact half of a
# 2560x1664 Retina panel, is one such mode: verified unavailable even
# with EDID hints, which is why it is not offered here.)
#
# NATIVE=1 renders at the panel's own resolution so the host never
# resamples the guest at all — a 1:1 image with a thin letterbox rather
# than a 1280x800 one filtered up. It costs four times the software fill
# and more VGA memory than QEMU's 16 MB default, so it is opt-in:
#     make run NATIVE=1
NATIVE ?= 0

ifeq ($(NATIVE),1)
RES      ?= 2560x1600x32
FB_MAX_W ?= 2560
FB_MAX_H ?= 1600
QEMU_VGA := -device VGA,vgamem_mb=32,edid=on,xres=2560,yres=1600
else
RES      ?= 1280x800x32
FB_MAX_W ?= 1920
FB_MAX_H ?= 1080
QEMU_VGA := -vga std
endif

# The back buffer, the previous frame and the wallpaper cache are all
# statically sized, so the bound is a build option rather than a constant.
CFLAGS  += -DBUF_MAX_W=$(FB_MAX_W)  -DBUF_MAX_H=$(FB_MAX_H) \
           -DWALL_MAX_W=$(FB_MAX_W) -DWALL_MAX_H=$(FB_MAX_H)

# --- Rebuild when the compiler flags change ---
#
# make compares timestamps, and a flag has none. Without this,
# `make iso EXTRA=-DSTORAGE_SELFTEST` after an ordinary build leaves
# the kernel objects untouched and produces an ISO without the thing
# that was asked for — silently, so the tests then "pass" by not running.
#
# It happens at parse time, before any rule runs, and that placement is
# the load-bearing part. A recipe that deletes the objects is too late:
# macOS ships GNU Make 3.81, which stats every target while building its
# dependency graph and does not look again. It also compares
# modification times to the whole second, so a stamp rewritten in the
# same second as the previous build is not "newer" and changes nothing.
# Deleting before the graph exists sidesteps both.
# build/tp is in here too, and leaving it out cost an evening. lwIP and
# Mbed TLS are configured entirely by preprocessor switches, so
# `make iso EXTRA=-DVX_LWIP_DEBUG` changes what those files compile to
# more than it changes the kernel objects -- and with only the two below
# being deleted, the debug build linked yesterday's silent lwIP and
# printed nothing. Which reads as "the debug flag does not work", and is
# really "the flag was never applied".
BUILD_FLAGS := $(CFLAGS)
$(shell mkdir -p build; \
        [ "`cat build/.flags 2>/dev/null`" = "$(BUILD_FLAGS)" ] || \
        { printf '%s\n' "$(BUILD_FLAGS)" > build/.flags; \
          rm -f build/llm.o build/lwipglue.o build/tlsglue.o; \
          rm -rf build/tp build/core build/sched build/fs build/security; })

LDFLAGS := -nostdlib -static -pie --no-dynamic-linker -z text \
            -T linker.ld

# --- User-space C library ---
#
# There is a libc now, so two flags that were only ever working around
# its absence are gone.
#
# -fno-tree-loop-distribute-patterns was there because GCC turns a clear
# loop into a memset call whether the source says so or not, and there
# was nothing to link that call against. There is.
#
# The floating-point bans are gone for the reason the whole exercise
# exists: user programs run in ring 3 with their own FPU state, saved
# and restored on every context switch, so there is nothing left for
# them to corrupt. `mandel` computing in doubles instead of 16.16 fixed
# point is the visible half of that.
#
# -mno-red-zone stays, and deliberately. The trampoline stubs the loader
# maps into every process push arguments onto the caller's stack, so the
# 128 bytes below RSP that the ABI promises a leaf function are not
# actually untouched here.
#
# ---- what the library grew into ----
#
# Three files became nine, and the four new ones are what a port needs
# rather than what the demos here needed:
#
#   math.c      a complete libm, written from the published algorithms
#               and checked against a reference by `make test`
#   mmap.c      memory obtained as a region rather than by moving a break
#   pthread.c   threads, on SYS_CLONE and the futex that was already here
#   posix.c     time, character classes, setjmp, and the process calls
#   stdlib2.c   the conversions, qsort, and the string helpers
#   crt0.c      thread-local storage and static constructors, before main
#
# crt0.o is *not* in the archive. It has to be named on the link line so
# that its _start is chosen, and a definition of _start sitting in an
# archive would be pulled in only when nothing else defined one -- which
# is the opposite of what a program that wants its own entry point
# expects, and silently wrong either way.
LIBC_SRC  := libc/string.c libc/stdio.c libc/malloc.c libc/math.c \
             libc/mmap.c libc/pthread.c libc/posix.c libc/stdlib2.c \
             libc/file.c libc/socket.c libc/exit.c libc/locale.c \
             libc/wchar.c libc/calendar.c
LIBC_OBJ  := $(patsubst libc/%.c,build/libc/%.o,$(LIBC_SRC))
LIBC      := build/libvextro.a
LIBC_CRT0 := build/libc/crt0.o

# -ftls-model=initial-exec is new and is load-bearing.
#
# Without it GCC emits the general-dynamic TLS sequence for a
# `__thread` variable, which is a call to __tls_get_addr through a
# module table -- machinery that exists to let a shared library be
# loaded at run time. There are no shared libraries here and no dynamic
# loader, so that call has nothing to resolve against. The initial-exec
# model compiles the same variable to a single load at a fixed offset
# from the FS segment, which is what libc/pthread.c actually sets up.
APP_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector \
              -fno-stack-check -mno-red-zone -fPIC \
              -msse -msse2 -mfpmath=sse -fno-math-errno \
              -ftls-model=initial-exec \
              -Ilibc/include -Iapps

# --- The C++ runtime ---
#
# `make webkit` used to stop here with "there is no C++ standard library
# for x86_64-elf: g++ exists, libstdc++ does not". That was true and is
# no longer: libcxx/ is a freestanding C++ runtime and header set built
# against the C library above.
#
# Written rather than ported, for the same reason libc/ was. Building
# GNU's libstdc++ needs the matching GCC source tree and a sysroot
# configure; LLVM's libc++ needs cmake, which is deliberately not
# installed here. Both would also drag in locales, iostreams and a
# threading model this system does not have. What is here is the subset
# that actually gets used, and third_party/wpe-config/README.md says
# plainly which parts of the standard library are not in it.
#
# ---- the four flags that are not negotiable ----
#
#   -nostdinc++    do not look in the host's C++ headers. Without it the
#                  cross compiler happily finds /usr/include/c++ and
#                  compiles against a library for a different operating
#                  system, which links and then does not run.
#   -fno-exceptions  there is no unwinder for this target.
#   -fno-rtti      and no type information to unwind through.
#   -ftls-model=initial-exec  as for every other user-space object here.
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -ffreestanding \
            -fno-exceptions -fno-rtti -fno-stack-protector \
            -fno-stack-check -mno-red-zone -fPIC \
            -msse -msse2 -mfpmath=sse -fno-math-errno \
            -ftls-model=initial-exec \
            -nostdinc++ -Ilibcxx/include -Ilibc/include -Iapps

LIBCXX_SRC := libcxx/src/new.cpp libcxx/src/cxa.cpp \
              libcxx/src/typeinfo.cpp
LIBCXX_OBJ := $(patsubst libcxx/src/%.cpp,build/libcxx/%.o,$(LIBCXX_SRC))
LIBCXX     := build/libvextrocxx.a

build/libcxx/%.o: libcxx/src/%.cpp $(wildcard libcxx/include/*)
	@mkdir -p build/libcxx
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(LIBCXX): $(LIBCXX_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(LIBCXX_OBJ)

.PHONY: libcxx
libcxx: $(LIBCXX)

LIMINE  := limine-binary
ISO     := iso_root

# --- App store packages ---
# Every package is a .vx image (see vxfmt/): compiled to ELF64, then
# repacked by vx_maker into the container the store and the kernel's
# loader both speak.  Seeded onto the disk under /store/pkg, which is the
# repository the Agora store installs from.  `voronoi` is deliberately
# left out so it is only reachable over the network (see `make repo`).
STORE_APPS  := mandel orbit life plasma
REPO_APPS   := $(STORE_APPS) voronoi
STORE_BINS  := $(addprefix build/store/,$(addsuffix .vx,$(STORE_APPS)))
REPO_BINS   := $(addprefix build/store/,$(addsuffix .vx,$(REPO_APPS)))

VX_MAKER   := vxfmt/vx_maker

# --- Pictures ---
# PNG in, .sci out (row filters + LZMA, see src/sci.h).  tools/mkimg.py
# decodes PNG with Python's own zlib, so there is no image dependency.
PIC_SRC     := $(wildcard apps/pics/*.png)
PIC_SCI     := $(patsubst apps/pics/%.png,build/pics/%.sci,$(PIC_SRC))
PIC_NAMES   := $(notdir $(basename $(PIC_SRC)))

.PHONY: verifydisk all iso run clean cleandisk apps repo vxtools pics test FORCE

# Say which target a bare `make` builds, rather than letting it fall to
# whichever rule happens to appear first. It fell to FORCE -- a real
# target, so make chose it, and it has no recipe -- so `make` on a fresh
# clone printed "Nothing to be done for `FORCE'" and built nothing at
# all. Everything still worked here because `make run` and `make all`
# name their goal, which is exactly why it went unnoticed.
.DEFAULT_GOAL := all

FORCE:

all: os.iso disk.img

# The article layout engine is pure computation over a byte buffer, so it
# runs on the host. The property it exists for -- that a link must not end
# the line -- cannot be seen in a screenshot and is awkward to assert from
# inside the kernel, so it is checked here instead.
test: build/wikidoc_test build/profile_test build/ttfhint_test build/crypto_test \
      build/ntcrypto_test build/aes_test build/krb5_test build/wifi_test \
      build/media_test build/rdp_test build/ntfs_test build/vmx_test \
      build/av_test build/mbedtls_test build/math_test build/cxx_test \
      build/rtti_test
	@./build/math_test
	@./build/cxx_test
	@./build/rtti_test
	@./build/wikidoc_test
	@./build/profile_test
	@./build/ttfhint_test
	@./build/crypto_test
	@./build/ntcrypto_test
	@./build/aes_test
	@./build/krb5_test
	@./build/wifi_test
	@./build/media_test
	@./build/rdp_test
	@mkdir -p build/scratch
	@printf "vextro ntfs scratch\\n" > build/scratch/a.txt
	@printf "hello from a subdirectory\\n" > build/scratch/b.txt
	@head -c 300000 /dev/urandom > build/scratch/big.bin
	@rm -f build/scratch/ntfs.img build/scratch/tree.img
	@python3 tools/mkntfs.py build/scratch/ntfs.img 16 \
	         build/scratch/a.txt > /dev/null
	@python3 tools/mkntfs.py build/scratch/tree.img 2048 \
	         build/scratch/a.txt \
	         build/scratch/b.txt:docs/readme.txt \
	         build/scratch/big.bin:store/pkg/big.bin > /dev/null
	@./build/ntfs_test build/scratch/ntfs.img build/scratch/tree.img
	@python3 tools/ntfsdir.py build/scratch/tree.img /big > /dev/null && \
	 echo "  ok   an independent reader agrees about the B-tree"
	@./build/vmx_test
	@./build/av_test
	@./build/mbedtls_test $(TLS_HOST) $(TLS_PORT)
	@python3 tools/tailwind.py --check
	@python3 tools/linecount.py --check

# The stripped Mbed TLS, proved on the host before it is trusted on the
# wire. Without a server it runs the offline half -- the allocator hooks,
# the entropy hook, AES-GCM and SHA-256 against published vectors -- and
# says which part it skipped.
#
# With one, it does a real TLS 1.3 handshake:
#     openssl s_server -accept 14433 -cert c.pem -key k.pem -tls1_3 -www
#     make test TLS_HOST=127.0.0.1 TLS_PORT=14433
#
# Debugging a configuration here beats debugging it over a serial line
# in a kernel with no debugger, which is the entire reason it exists:
# every switch turned off in vextro_config.h is one that could silently
# remove something the handshake needs, and the failure would otherwise
# first appear as an error code twelve frames deep on a machine that
# cannot print a backtrace.
TLS_HOST ?=
TLS_PORT ?=

# Defined here rather than beside LWIP_DIR further down, because
# MBED_HOST_OBJ below expands it immediately with := -- and with the
# definition six hundred lines later the wildcard found nothing, so the
# host test linked against zero Mbed TLS objects. It only ever worked
# because a binary built before the bug existed was never relinked;
# `make clean` deleted it and the failure appeared.
MBED_DIR  := third_party/mbedtls

MBED_HOST_OBJ := $(patsubst $(MBED_DIR)/library/%.c,build/mbedhost/%.o,\
                   $(wildcard $(MBED_DIR)/library/*.c))

build/mbedhost/%.o: $(MBED_DIR)/library/%.c $(MBED_DIR)/vextro_config.h
	@mkdir -p build/mbedhost
	@$(HOSTCC) -c -O1 -w -I$(MBED_DIR)/include -I$(MBED_DIR) \
		-DMBEDTLS_CONFIG_FILE='"vextro_config.h"' -o $@ $<

# --- The RTTI cases, on the host ---
#
# apps/rtti_cases.h compiled against the host's own C++ runtime, which
# makes it a reference rather than a second opinion. The identical file
# runs in ring 3 inside cxxtest over libcxx/src/typeinfo.cpp, and the
# two must agree.
#
# Built with the host's headers and -frtti on purpose: nothing about
# this binary should come from libcxx/, or it would be checking the
# implementation against itself.
build/rtti_test: tools/rtti_test.cpp apps/rtti_cases.h
	@mkdir -p build
	@$(HOSTCC) -std=c++17 -O2 -Wall -Wextra -frtti -Iapps \
		-x c++ tools/rtti_test.cpp -o $@ -lc++ 2>/dev/null || \
	 $(HOSTCC) -std=c++17 -O2 -Wall -Wextra -frtti -Iapps \
		-x c++ tools/rtti_test.cpp -o $@ -lstdc++

# The C++ containers, on the host, against the same headers the target
# builds against.
#
# The host's compiler and the host's operator new, with -nostdinc++ so
# that the only C++ headers in scope are ours. What that buys is the
# volume a boot test cannot afford: sorting twenty thousand elements in
# five adversarial orders, every string length across the small-buffer
# boundary, ten thousand tracked objects checked for leaks. What it
# cannot check is anything that is not computation -- the allocator, the
# static constructors, the guard variables under real threads -- and
# apps/cxxtest.cpp does that on the machine.
#
# -include tools/cxx_hostshim.h supplies the one name libc/ has and a
# hosted C library does not; see that file.
build/cxx_test: tools/cxx_test.cpp tools/cxx_hostshim.h \
                $(wildcard libcxx/include/*)
	@mkdir -p build
	@$(HOSTCC) -std=c++20 -O1 -Wall -Wextra -nostdinc++ \
		-Ilibcxx/include -include tools/cxx_hostshim.h \
		-x c++ tools/cxx_test.cpp -o $@ -lc++ 2>/dev/null || \
	 $(HOSTCC) -std=c++20 -O1 -Wall -Wextra -nostdinc++ \
		-Ilibcxx/include -include tools/cxx_hostshim.h \
		-x c++ tools/cxx_test.cpp -o $@ -lstdc++

build/mbedtls_test: tools/mbedtls_test.c $(MBED_HOST_OBJ)
	@mkdir -p build
	@$(HOSTCC) -O1 -w -I$(MBED_DIR)/include -I$(MBED_DIR) \
		-DMBEDTLS_CONFIG_FILE='"vextro_config.h"' \
		-o $@ tools/mbedtls_test.c $(MBED_HOST_OBJ)

# --- The C library's arithmetic, against a reference ---
#
# libc/math.c is compiled a second time for the *host*, with every
# function renamed by tools/math_rename.h, so that our exp() and the
# host's can be linked into one program and called side by side. There is
# no other way to check a transcendental function: the only other
# implementation available to this repository is the one being checked.
#
# -w rather than -Wall, and deliberately. The file is compiled here for
# an architecture it was not written for, and the warnings that produces
# -- about the x86 assembly it does not use on this path -- would bury
# the ones that matter. It is compiled with warnings on for its real
# target by the rule below it.
build/math_host.o: libc/math.c tools/math_rename.h libc/include/math.h
	@mkdir -p build
	@$(HOSTCC) -O1 -w -std=gnu11 -Ilibc/include \
		-include tools/math_rename.h -c $< -o $@

build/math_test: tools/math_test.c build/math_host.o
	@mkdir -p build
	@$(HOSTCC) -O1 -w -std=gnu11 -o $@ $< build/math_host.o -lm

build/wikidoc_test: tools/wikidoc_test.c src/wikidoc.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -o $@ $<

# The profile isolation guard, against the ways around it rather than
# against itself. Denying "/Documents and Settings/bob" proves nothing;
# what has to be denied is every spelling of it -- backslashes, a drive
# letter, doubled slashes, the wrong case, a `..` -- while an account
# whose name merely starts the same way is still kept out.
build/profile_test: tools/profile_test.c src/profile.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -o $@ $<

# The TrueType bytecode interpreter, measured rather than asserted.
# Every failure path in src/ttfhint.h falls back to the unhinted
# outline, so an interpreter that errors on every glyph still draws
# readable text -- "it builds and looks fine" cannot tell that apart
# from one that works. This counts how often a hinted outline lands on
# a pixel boundary and compares it with how often chance does.
build/ttfhint_test: tools/ttfhint_test.c src/ttfhint.h src/ttf.h src/comicneue_ttf.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The cipher is checked against the RFC's published vectors, not against
# itself: an implementation that is merely self-consistent round-trips
# perfectly and protects nothing.
build/crypto_test: tools/crypto_test.c src/chacha20.h src/sha256.h src/tls.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -o $@ $<

# The algorithms Windows networking still runs on -- MD4, MD5, SHA-1,
# RC4 -- against RFC 1320, RFC 1321, FIPS 180-4, RFC 2202 and the worked
# NTLMv2 example in MS-NLMP 4.2.4. Every one of them is broken for its
# original purpose and every one is required to reach a domain.
build/ntcrypto_test: tools/ntcrypto_test.c src/ntcrypto.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# AES against FIPS-197, RFC 3962's ciphertext-stealing vectors and RFC
# 4493's CMACs.
#
# What runs here is the *portable* implementation, and that is the point
# rather than a limitation: this repository is edited on an arm64 Mac,
# which has no AESENC, so the fallback path gets exercised on the host
# and the AES-NI path is compared against it inside the kernel. Without
# this, the software AES would be dead code on every machine that could
# run it and would rot unnoticed.
build/aes_test: tools/aes_test.c src/aes.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The Kerberos encryption profile: n-fold, PBKDF2, DK and string-to-key
# against RFC 3961 appendix A and RFC 3962 appendix B.
#
# Worth running before anything talks to a KDC, because every one of
# these failing looks identical from the far end -- the KDC says the
# password is wrong, and means it, because the key it derived does not
# match the one this end derived.
build/krb5_test: tools/krb5_test.c src/krb5crypto.h src/aes.h src/ntcrypto.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The wireless stack, minus the radio.
#
# There is no wireless device in QEMU and the chipset back-ends need
# proprietary firmware, so nothing in src/net/wifi.c can be exercised on
# the machines this is built on. What can be, and is here, is every part
# a single wrong byte would break silently: the PMK against 802.11i's
# published passphrase vectors, the group key wrap against RFC 3394,
# CCMP's authentication of the frame header, and the whole 4-way
# handshake driven against an authenticator written separately in the
# test from the same standard.
#
# The handshake is the reason this matters. A wrong PTK does not fail
# visibly -- it associates, exchanges four messages, and then every
# encrypted frame is discarded by the access point with no diagnostic
# on either side.
build/wifi_test: tools/wifi_test.c src/net/wpa2.h src/net/ieee80211.h \
                 src/aes.h src/ntcrypto.h src/sha256.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The video decoder, minus the video engine.
#
# QEMU models no Intel GPU, so igpu_init() finds nothing and everything
# from the VCS ring downward is unreachable on the machines this is
# built on. The MFX command encodings in src/media/mfx.h come from
# Intel's public documentation and have never touched silicon.
#
# What this checks is the two ends that surround them: the H.264 header
# parsing that produces the hardware state, and the surface geometry and
# colour conversion that turn its output into window pixels. Both are
# places where a wrong constant produces a picture that is subtly wrong
# rather than absent -- a mis-parsed crop offset shifts every frame by a
# few pixels, and the wrong colour matrix tints the whole video.
build/media_test: tools/media_test.c src/media/h264.h src/media/csc.h \
                  src/media/mfx.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The RDP wire format.
#
# Unlike the wireless and video work in this tree, the remote desktop
# runs on the machines it is built on -- it is software over TCP and the
# network stack is up in QEMU. This checks the encoders against the
# bytes the specification prescribes; tools/rdp_probe.py drives a real
# connection against a booted system and checks the replies.
#
# Worth having separately because RDP writes lengths four different ways
# in one packet, and a length in the wrong encoding makes a client close
# the socket with no diagnostic at either end.
build/rdp_test: tools/rdp_test.c src/net/rdpwire.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

# The NTFS driver, against a volume made by tools/mkntfs.py.
#
# A filesystem reader can be checked by reading. A writer can only be
# checked by writing and reading back, and the only volume the kernel
# has is the one it booted from -- which is the worst place to discover
# that a cluster allocator hands the same run out twice. This compiles
# the identical source the kernel runs against an image file, so being
# wrong about NTFS costs nothing.
build/ntfs_test: tools/ntfs_test.c src/fs/ntfs/ntfs_ops.c \
                 src/fs/ntfs/ntfs.h src/fs/ntfs/ntfs_hostshim.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function \
	           -Isrc -Iinclude -o $@ $<

# The volume the build produced, read by the driver that will boot it.
#
# Not part of `make test`, because it needs disk.img -- which is 8 GB and
# is not built on a machine that only wants the unit checks. It is the
# gate before trusting a formatter change:
#
#     make verifydisk
#
# mkntfs.py writes NTFS; ntfs_ops.c reads it. They were written from the
# same specification but they are not the same code, and a formatter is
# only trustworthy when a different implementation agrees about what it
# produced. This mounts the real image on the host and checks every
# seeded file byte for byte, including the 937 MB archive through the
# ranged-read path a running system actually uses.
build/ntfs_verify: tools/ntfs_verify.c src/fs/ntfs/ntfs_ops.c \
                   src/fs/ntfs/ntfs.h src/fs/ntfs/ntfs_hostshim.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function \
	           -Isrc -Iinclude -o $@ $<

verifydisk: build/ntfs_verify disk.img
	@python3 tools/ntfsdir.py disk.img / > /dev/null && \
	 echo "  ok   tools/ntfsdir.py reads the volume independently"
	@./build/ntfs_verify disk.img \
	    /about.txt=apps/about.txt \
	    /notes.txt=apps/notes.txt \
	    /hello=build/hello \
	    /faulter=build/faulter \
	    /docs/welcome.txt=apps/welcome.txt \
	    /etc/ca-bundle.crt=build/ca-bundle.crt \
	    $(foreach a,$(STORE_APPS),/store/pkg/$(a).vx=build/store/$(a).vx) \
	    $(foreach p,$(PIC_NAMES),/pics/$(p).sci=build/pics/$(p).sci) \
	    $(foreach t,$(MUSIC_NAMES),/music/$(t).wav=build/music/$(t).wav) \
	    $(foreach f,$(ASSET_FILES),/$(notdir $(f))=$(f))

# The scanner, against the search it replaced.
#
# src/security/anti_virus.c is an Aho-Corasick automaton where there used
# to be a loop per signature. Both are still in the file -- the direct
# search is the fallback if the node pool overflows -- so this runs them
# side by side over twenty thousand generated buffers and requires they
# agree. A matcher rewrite checked only by "it still finds EICAR" is a
# matcher rewrite with no evidence behind it.
#
# Sanitisers on: the scan loop indexes a table with a byte from the
# input, and reading one state off the end of that table is exactly the
# bug that would not show up in the answers.
build/av_test: tools/av_test.c src/security/anti_virus.c \
               src/security/anti_virus.h include/kernel_shared.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -fsanitize=address,undefined \
	           -Isrc -Iinclude -o $@ $<

# VT-x, minus VT-x.
#
# QEMU TCG reports vmx = False, so no VMXON or VMLAUNCH in
# src/hyper_intel.h can execute here. What this checks is the
# arithmetic those instructions consume -- EPT entry format, control
# negotiation against the capability MSRs, VMCS field encodings -- each
# of which is a bit pattern hardware misreads silently rather than
# rejecting.
build/vmx_test: tools/vmx_test.c src/hyper_intel.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -Isrc -o $@ $<

apps: $(REPO_BINS)

pics: $(PIC_SCI)

vxtools: $(VX_MAKER) vxfmt/vx_run

# --- exFAT system disk ---
# exFAT rather than FAT32 because FAT32 caps a single file at 4 GB, and
# an offline encyclopedia is far past that.  The image is sparse, so an
# 8 GB volume costs only the few megabytes actually used.
#
# Created once and then left alone: it is the OS's writable, persistent
# filesystem. `make cleandisk` resets it to factory contents.
DISK_MB ?= 8192

# --- The encyclopedia and the model ---
#
# Neither can live in the repository: wiki.zim is about 980 MB and
# qwen2.gguf about 380 MB, and GitHub refuses any blob over 100 MB. So
# they are fetched from where they are published and written into the
# volume, which is what makes a fresh clone come up with an encyclopedia
# rather than without one.
#
# Both are optional and the fetch never fails the build. Without them the
# Wikipedia window reports no archive and the prompt after login has
# nothing to offer -- a working machine, with less on it.
#
# ASSETS=0 skips the download entirely; ASSETS=1 takes it without asking,
# which is what CI wants.
ASSETS ?= ask

# assets/explain.gguf is the model fine-tuned in this repository (see
# tools/train_explainer.py). It is listed like the others so that a tree
# without it still builds -- the disk rule skips assets that are absent,
# and the kernel falls back to qwen2.gguf when the volume has no
# explain.gguf on it.
ASSET_FILES := assets/wiki.zim assets/qwen2.gguf assets/explain.gguf

# Outside build/, and that is the whole point.
#
# disk.img depends on this list, and `clean` deletes build/ -- so with
# the list living there, `make clean && make` regenerated it, found it
# newer than the volume, and repacked 8 GB. Which is slow, and worse
# than slow: it resets the registry, so the account someone created and
# every file they saved were gone because they asked for a clean build
# of the *kernel*.
#
# Nothing else in the tree wants this file, it is rewritten only when
# its contents change, and .gitignore covers it.
ASSET_LIST  := .assets.list

.PHONY: assets
assets:
	@python3 tools/fetch_assets.py --dest assets --ask-again \
		$(if $(filter 1,$(ASSETS)),--yes,)
	@rm -f $(ASSET_LIST)

# --- What is actually on hand ---
#
# The fetch cannot live in the disk.img recipe, and the reason is a trap
# worth naming in full, because it cost a working feature and looked like
# success while it did.
#
# make expands a whole recipe before running any line of it. So
# `$(wildcard assets/wiki.zim)` on the mkexfat line was evaluated while
# assets/ was still empty; the fetch on the line above then downloaded
# 1.4 GB, and mkexfat packed none of it. The build printed every sign of
# having worked -- both files downloaded, disk written -- and the machine
# came up with no encyclopedia and no model. It never corrected itself
# either: disk.img existed afterwards, and nothing said it was wrong.
#
# Fetching therefore happens here, in a prerequisite, which is finished
# before that recipe is expanded. And this writes down what it found, so
# disk.img can depend on the *list* rather than on the files: a disk built
# before the encyclopedia arrived is out of date the moment it arrives,
# which is what makes the broken state above repair itself. The list is
# rewritten only when its contents change, so an ordinary rebuild does not
# repack 8 GB for nothing.
$(ASSET_LIST): FORCE
	@mkdir -p build
ifneq ($(ASSETS),0)
	@python3 tools/fetch_assets.py --dest assets $(if $(filter 1,$(ASSETS)),--yes,) || true
endif
	@for f in $(ASSET_FILES); do \
	    if [ -f "$$f" ]; then printf '%s %s\n' "$$f" "$$(wc -c < "$$f")"; fi; \
	done > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# Sample audio for the media player: one track per format it decodes,
# so what ships exercises every decoder and not only the easy one. Short,
# because they live on the volume.
#   chime, sweep  uncompressed PCM
#   voice         IMA ADPCM
#   dial          G.711 mu-law
#   bell.flac     FLAC, if the reference encoder is installed
MUSIC_NAMES := chime sweep voice dial
MUSIC_WAV   := $(foreach t,$(MUSIC_NAMES),build/music/$(t).wav)
MUSIC_FLAC  := build/music/bell.flac

$(MUSIC_WAV) $(MUSIC_FLAC): tools/mkwav.py
	@python3 tools/mkwav.py build/music

# The certificate authority store.
#
# Copied from whatever the build machine trusts rather than downloaded,
# so the build stays offline and the roots are ones already vouched for
# by a system somebody maintains. Without it TLS still works and still
# encrypts -- it just cannot prove who is on the far end, which is what
# vxsec_verifies_certificates() reports and what the browser says
# instead of showing a padlock.
CA_CANDIDATES := /etc/ssl/cert.pem /etc/ssl/certs/ca-certificates.crt \
                 /usr/local/etc/openssl/cert.pem \
                 /opt/homebrew/etc/openssl@3/cert.pem
CA_BUNDLE := $(firstword $(foreach c,$(CA_CANDIDATES),$(wildcard $(c))))

build/ca-bundle.crt:
	@mkdir -p build
	@if [ -n "$(CA_BUNDLE)" ]; then \
	    cp "$(CA_BUNDLE)" $@; \
	    echo "  CA     $(CA_BUNDLE) -> $@ ($$(grep -c 'BEGIN CERT' $@) roots)"; \
	else \
	    : > $@; \
	    echo "  CA     no host CA bundle found; TLS will not verify peers"; \
	fi

# The system volume is NTFS.
#
# It was exFAT for as long as there was a filesystem, and the driver for
# it is still here and still mounts -- what changed is what `make` lays
# down. The formatter preallocates pagefile.sys as one contiguous run,
# because src/swap.h resolves its backing store to a single absolute LBA
# at boot and cannot use a fragmented one; and it allocates every file
# sequentially, because without $ATTRIBUTE_LIST a fragmented 937 MB
# archive's run list would not fit in its own MFT record.
disk.img: $(ASSET_LIST) build/sqlseed.db | build/hello build/faulter build/mutextest build/threadtest build/wpetest build/fdprobe build/fdtest build/cxxtest build/sqltest build/fttest build/hbtest build/icutest $(WINAPPS) $(STORE_BINS) $(PIC_SCI) $(MUSIC_WAV) $(MUSIC_FLAC) build/ca-bundle.crt
	@set -e; \
	big=""; \
	for f in $(ASSET_FILES); do \
	    if [ -f "$$f" ]; then big="$$big $$f"; fi; \
	done; \
	cmd="python3 tools/mkntfs.py disk.img $(DISK_MB) \
		apps/about.txt apps/notes.txt build/hello build/faulter \
		build/mutextest \
		build/threadtest \
		build/wpetest \
		build/fdprobe \
		build/fdtest \
		build/cxxtest \
		build/sqltest \
		build/fttest \
		build/hbtest \
		build/icutest \
		assets/ComicNeue-Regular.ttf:ComicNeue-Regular.ttf \
		$(ICU_DATA):icudt74l.dat \
		build/sqlseed.db:sqlseed.db \
		$(foreach w,$(WINAPPS),$(w):$(notdir $(w))) \
		apps/welcome.txt:docs/welcome.txt \
		build/ca-bundle.crt:etc/ca-bundle.crt \
		$$big \
		$(foreach a,$(STORE_APPS),build/store/$(a).vx:store/pkg/$(a).vx) \
		$(foreach p,$(PIC_NAMES),build/pics/$(p).sci:pics/$(p).sci) \
		$(foreach t,$(MUSIC_NAMES),build/music/$(t).wav:music/$(t).wav) \
		$$(test -f build/music/bell.flac && \
		   echo build/music/bell.flac:music/bell.flac)"; \
	echo "$$cmd"; \
	$$cmd

cleandisk:
	rm -f disk.img
	$(MAKE) disk.img

# --- Network package repository (http://10.0.2.2:8000 from the guest) ---
repo: $(REPO_BINS)
	python3 tools/serve_repo.py --out build/repo $(REPO_BINS)

# --- Host Limine tool (needed for BIOS boot-sector install) ---
build/limine-tool: $(LIMINE)/limine.c
	@mkdir -p build
	$(HOSTCC) -O2 -o $@ $<

# --- User-space C library ---
build/libc/%.o: libc/%.c $(wildcard libc/include/*.h) $(wildcard libc/include/sys/*.h)
	@mkdir -p build/libc
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/libc/crt0.o: libc/crt0.c libc/include/pthread.h
	@mkdir -p build/libc
	$(CC) $(APP_CFLAGS) -c $< -o $@

$(LIBC): $(LIBC_OBJ)
	@rm -f $@
	$(AR) rcs $@ $(LIBC_OBJ)

# --- User app: hello ---
# -z max-page-size=0x1000 so ld does not pad segments to its 2 MB
# default: the kernel's loader maps an image page by page and gives each
# page the protection its segment asked for, so the segments have to be
# page-aligned and no coarser.
build/hello.o: apps/hello.c apps/vextro.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/hello: build/hello.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/hello.o $(LIBC) -o $@

# --- Windows executable ---
#
# A genuine PE64, built by a genuine Windows toolchain, so the loader in
# src/pe.h is tested against what it actually claims to read rather than
# against a file this repository wrote to suit it.
#
# --dynamicbase is what produces the relocation table, which is the whole
# reason a PE can be loaded anywhere: without it the image can only go to
# the address in its own header. The import library is generated from a
# .def file, so the .exe names vextro.dll and its functions exactly the
# way any Windows program names the libraries it needs.
#
# Optional. A machine without mingw-w64 builds everything else and skips
# this; the loader is still there and still refuses malformed images.
MINGW := $(shell command -v x86_64-w64-mingw32-gcc 2>/dev/null)
ifneq ($(MINGW),)
WINAPPS := build/win/winhello.exe build/win/wintry.exe
else
WINAPPS :=
endif

build/win/libvextro.a: apps/win/vextro.def
	@mkdir -p build/win
	x86_64-w64-mingw32-dlltool -d $< -l $@

build/win/winhello.exe: apps/win/winhello.c build/win/libvextro.a
	@mkdir -p build/win
	x86_64-w64-mingw32-gcc -O2 -Wall -ffreestanding -fno-stack-protector \
		-c $< -o build/win/winhello.o
	x86_64-w64-mingw32-gcc -nostdlib -nostartfiles -Wl,-e,PeMain \
		-Wl,--dynamicbase -o $@ build/win/winhello.o build/win/libvextro.a

# The SEH test. Its guarded ranges are written with the assembler's own
# .seh_* directives, which emit exactly the .pdata and .xdata a Microsoft
# compiler emits for __try -- so what the kernel reads is a table the
# toolchain produced, not one this repository wrote to suit itself.
# It also carries a real .rsrc string table, compiled by windres, so the
# resource reader is tested against what a resource compiler emits rather
# than bytes this repository laid out to suit itself.
build/win/wintry.exe: apps/win/wintry.c apps/win/wintry.rc build/win/libvextro.a
	@mkdir -p build/win
	x86_64-w64-mingw32-gcc -O2 -Wall -ffreestanding -fno-stack-protector \
		-c $< -o build/win/wintry.o
	x86_64-w64-mingw32-windres apps/win/wintry.rc -O coff -o build/win/wintry_res.o
	x86_64-w64-mingw32-gcc -nostdlib -nostartfiles -Wl,-e,PeMain \
		-Wl,--dynamicbase -o $@ build/win/wintry.o build/win/wintry_res.o \
		build/win/libvextro.a

.PHONY: winapps
winapps: $(WINAPPS)

# --- User app: faulter ---
# A program that writes through a null pointer, so that the containment
# can be tested rather than assumed. Runs under `make iso
# EXTRA=-DFAULT_SELFTEST`; otherwise it just sits on the volume.
# --- User app: mutextest ---
#
# The futex, exercised from ring 3 across a fork. Built like hello --
# same linker script, same C library -- because it is an ordinary
# application and the point is that a mutex is now something an ordinary
# application can have.
build/mutextest.o: apps/mutextest.c apps/vextro.h libc/include/vxmutex.h \
                   libc/include/sys/syscall.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/mutextest: build/mutextest.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/mutextest.o $(LIBC) -o $@

# --- libwpe, and the Vextro backend for it ---
#
# WPE is WebKit with the platform taken out: everything an engine assumes
# about a window system sits behind libwpe, and a *backend* supplies it.
# third_party/libwpe is vendored unmodified; third_party/wpe-port is the
# backend, which is where this system's window actually gets its pixels.
#
# Built with upstream's src/loader-static.c rather than src/loader.c,
# which is the whole of what it takes to make libwpe work without a
# dynamic loader: the static form does no dlopen and expects
# `_wpe_loader_interface` to be linked in, which the port defines.
# input-xkb.c and gamepad.c are left out because they need libxkbcommon
# and a device this machine does not have.
#
# WPE_COMPILATION is what libwpe's headers check before letting anything
# but <wpe/wpe.h> be included; the sources include each other directly.
WPE_DIR     := third_party/libwpe
WPE_PORT    := third_party/wpe-port
WPE_INC     := -I$(WPE_DIR)/include -I$(WPE_DIR)/src \
               -I$(WPE_PORT) -I$(WPE_PORT)/include
WPE_SRC     := $(WPE_DIR)/src/alloc.c $(WPE_DIR)/src/loader-static.c \
               $(WPE_DIR)/src/process.c $(WPE_DIR)/src/renderer-backend-egl.c \
               $(WPE_DIR)/src/renderer-host.c $(WPE_DIR)/src/version.c \
               $(WPE_DIR)/src/view-backend.c $(WPE_DIR)/src/pasteboard.c \
               $(WPE_DIR)/src/key-unicode.c \
               $(WPE_PORT)/vxwpe.c $(WPE_PORT)/pasteboard-noop.c
WPE_OBJ     := $(addprefix build/wpe/,$(notdir $(WPE_SRC:.c=.o)))
LIBWPE      := build/libwpe.a

# One pattern rule per source directory, because make matches on the
# whole path and the two trees are not siblings.
build/wpe/%.o: $(WPE_DIR)/src/%.c
	@mkdir -p build/wpe
	$(CC) $(APP_CFLAGS) -DWPE_COMPILATION $(WPE_INC) -c $< -o $@

build/wpe/%.o: $(WPE_PORT)/%.c $(WPE_PORT)/vxwpe.h
	@mkdir -p build/wpe
	$(CC) $(APP_CFLAGS) -DWPE_COMPILATION $(WPE_INC) -c $< -o $@

$(LIBWPE): $(WPE_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(WPE_OBJ)

.PHONY: libwpe
libwpe: $(LIBWPE)

# ============================================================
#  SQLite
# ============================================================
#
# The amalgamation, vendored as lwIP and Mbed TLS are, and pinned by
# checksum for the same reason WEBKIT_SHA256 is: a download that is not
# checked is a download that can be replaced, and this one is compiled
# and run.
#
# ---- the flags, and why each one ----
#
#   SQLITE_OS_OTHER=1        excludes os_unix.c and os_win.c entirely.
#                            third_party/sqlite-port/vx_vfs.c is the
#                            operating system instead.
#   SQLITE_OMIT_WAL          write-ahead logging needs xShmMap, which is
#                            shared memory between processes. There is
#                            none. Rollback journalling needs only files.
#   SQLITE_TEMP_STORE=3      temporary tables in memory. Against a
#                            filesystem that rewrites a whole file per
#                            sync, temp-file churn is pathological.
#   SQLITE_OMIT_LOAD_EXTENSION   no dynamic loader.
#   SQLITE_OMIT_LOCALTIME    no timezone database, and inventing one
#                            would be worse than the absence.
#   SQLITE_THREADSAFE=1      the engine's serialisation logic is present.
#                            The mutexes themselves are *not* selected
#                            here: SQLITE_OS_OTHER forces
#                            SQLITE_MUTEX_NOOP whatever the command line
#                            says, and adding SQLITE_MUTEX_PTHREADS
#                            compiles both implementations at once. The
#                            port installs real ones at run time through
#                            sqlite3_config(SQLITE_CONFIG_MUTEX); see the
#                            long note in vx_vfs.c.
#   SQLITE_DEFAULT_PAGE_SIZE=4096  the NTFS cluster this volume is
#                            formatted with, so a page write is one
#                            cluster write.
#
# -w for the same reason the Mbed TLS objects carry it: this is vendored
# code compiled unmodified, and the zero-warning claim is about code
# written here.
SQLITE_VERSION := 3450100
SQLITE_ZIP     := sqlite-amalgamation-$(SQLITE_VERSION).zip
SQLITE_URL     := https://sqlite.org/2024/$(SQLITE_ZIP)
SQLITE_SHA256  := 5592243caf28b2cdef41e6ab58d25d653dfc53deded8450eb66072c929f030c4

SQLITE_DIR   := third_party/sqlite
SQLITE_PORT  := third_party/sqlite-port
SQLITE_DEFS  := -DSQLITE_OS_OTHER=1 -DSQLITE_OMIT_WAL \
                -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_LOAD_EXTENSION \
                -DSQLITE_OMIT_LOCALTIME -DSQLITE_THREADSAFE=1 \
                -DSQLITE_DEFAULT_PAGE_SIZE=4096 \
                -DSQLITE_DEFAULT_MEMSTATUS=0
SQLITE_INC   := -I$(SQLITE_DIR)
LIBSQLITE    := build/libsqlite.a

build/sqlite/sqlite3.o: $(SQLITE_DIR)/sqlite3.c $(SQLITE_DIR)/sqlite3.h
	@mkdir -p build/sqlite
	$(CC) $(APP_CFLAGS) -w $(SQLITE_DEFS) $(SQLITE_INC) -c $< -o $@

build/sqlite/vx_vfs.o: $(SQLITE_PORT)/vx_vfs.c $(SQLITE_DIR)/sqlite3.h
	@mkdir -p build/sqlite
	$(CC) $(APP_CFLAGS) $(SQLITE_DEFS) $(SQLITE_INC) -c $< -o $@

$(LIBSQLITE): build/sqlite/sqlite3.o build/sqlite/vx_vfs.o
	@mkdir -p build
	$(AR) rcs $@ $^

.PHONY: sqlite
sqlite: $(LIBSQLITE)

# ============================================================
#  FreeType
# ============================================================
#
# Vendored at 2.13.2 and compiled by naming its sources, exactly as lwIP
# and Mbed TLS are — upstream's own build is autoconf or cmake, and
# neither is needed to compile a list of C files with a cross compiler.
#
# Four modules out of nineteen megabytes of source: TrueType outlines,
# the SFNT container they live in, the anti-aliasing rasteriser, and the
# PostScript name table sfnt needs. third_party/freetype-port/ftmodule.h
# is the list and says why.
#
# ---- the two headers that redirect the configuration ----
#
# FT_CONFIG_OPTIONS_H and FT_CONFIG_MODULES_H are FreeType's documented
# way to supply your own without editing the tree, which is what keeps
# third_party/freetype/ a clean copy of the release.
#
# ---- ftsystem.c is upstream's ANSI one, unmodified ----
#
# It is written against fopen, fseek, ftell, fread, fclose, malloc,
# realloc and free — every one of which this C library now has. That is
# the whole "port": there is no Vextro-specific FreeType code at all,
# because the C library grew to the point where the stock file compiles.
FREETYPE_VERSION := 2.13.2
FREETYPE_TARBALL := freetype-$(FREETYPE_VERSION).tar.gz
FREETYPE_URL     := https://download.savannah.gnu.org/releases/freetype/$(FREETYPE_TARBALL)
FREETYPE_SHA256  := 1ac27e16c134a7f2ccea177faba19801131116fd682efc1f5737037c5db224b5

FT_DIR  := third_party/freetype
FT_PORT := third_party/freetype-port
FT_INC  := -I$(FT_DIR)/include -I$(FT_PORT)
FT_DEFS := -DFT2_BUILD_LIBRARY \
           -DFT_CONFIG_OPTIONS_H='<ftoption.h>' \
           -DFT_CONFIG_MODULES_H='<ftmodule.h>'

# Each of these is itself an amalgamation: FreeType's sources #include
# their siblings, so one object per module rather than per file.
#
# ftmm.c is here because the TrueType driver references
# FT_Set_Named_Instance unconditionally — variable-font support is not
# something ftoption.h can switch off at this seam, so the file that
# defines it has to be linked whether or not any variable font is ever
# opened. Found the way such things are: an undefined symbol at the
# first link, not at the first compile.
FT_SRC := $(FT_DIR)/src/base/ftsystem.c $(FT_DIR)/src/base/ftinit.c \
          $(FT_DIR)/src/base/ftdebug.c  $(FT_DIR)/src/base/ftbase.c \
          $(FT_DIR)/src/base/ftbitmap.c $(FT_DIR)/src/base/ftglyph.c \
          $(FT_DIR)/src/base/ftmm.c \
          $(FT_DIR)/src/sfnt/sfnt.c     $(FT_DIR)/src/truetype/truetype.c \
          $(FT_DIR)/src/smooth/smooth.c $(FT_DIR)/src/psnames/psnames.c
FT_OBJ := $(patsubst $(FT_DIR)/src/%.c,build/freetype/%.o,$(FT_SRC))
LIBFT  := build/libfreetype.a

build/freetype/%.o: $(FT_DIR)/src/%.c $(FT_DIR)/include/ft2build.h \
                    $(FT_PORT)/ftoption.h $(FT_PORT)/ftmodule.h
	@mkdir -p $(dir $@)
	$(CC) $(APP_CFLAGS) -w $(FT_DEFS) $(FT_INC) -c $< -o $@

$(LIBFT): $(FT_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(FT_OBJ)

.PHONY: freetype
freetype: $(LIBFT)

# ============================================================
#  HarfBuzz
# ============================================================
#
# One translation unit. HarfBuzz ships src/harfbuzz.cc, which #includes
# every other source in the library — the same amalgamation idea SQLite
# uses — so its meson and cmake builds are not needed to produce an
# object, any more than lwIP's Makefile was.
#
# ---- what it needed from this system, and what it did not ----
#
# It compiled against libcxx/ with three additions and no patches:
# <inttypes.h>, which this C library simply did not have; <cfloat>; and
# the is_trivially_copy_assignable family, which hb-meta.hh uses to
# choose between a memmove and an element loop. That is the whole
# integration. Nothing in third_party/harfbuzz/ is modified.
#
#   HB_NO_MT        one lock-free path. HarfBuzz's atomics would work —
#                   <atomic> is real here — but nothing shapes text from
#                   two threads, and the single-threaded path is smaller
#                   and has fewer places to be wrong.
#   HB_TINY         drops the shapers and tables a browser on this
#                   machine will not reach, which is most of the size.
#   HAVE_FREETYPE   pulls in hb-ft.cc, so an hb_font can be built from an
#                   FT_Face. That is upstream's own integration between
#                   the two libraries and is the right seam: HarfBuzz
#                   asks FreeType for glyph metrics rather than parsing
#                   the font a third time.
HARFBUZZ_VERSION := 8.5.0
HARFBUZZ_TARBALL := harfbuzz-$(HARFBUZZ_VERSION).tar.xz
HARFBUZZ_URL     := https://github.com/harfbuzz/harfbuzz/releases/download/$(HARFBUZZ_VERSION)/$(HARFBUZZ_TARBALL)
HARFBUZZ_SHA256  := 77e4f7f98f3d86bf8788b53e6832fb96279956e1c3961988ea3d4b7ca41ddc27

HB_DIR  := third_party/harfbuzz
HB_INC  := -I$(HB_DIR)/src
HB_DEFS := -DHB_NO_MT -DHB_TINY -DHAVE_FREETYPE=1
LIBHB   := build/libharfbuzz.a

build/harfbuzz/harfbuzz.o: $(HB_DIR)/src/harfbuzz.cc
	@mkdir -p build/harfbuzz
	$(CXX) $(CXXFLAGS) -w $(HB_DEFS) $(HB_INC) $(FT_INC) $(FT_DEFS) \
		-c $< -o $@

$(LIBHB): build/harfbuzz/harfbuzz.o
	@mkdir -p build
	$(AR) rcs $@ $^

.PHONY: harfbuzz
harfbuzz: $(LIBHB)


# ===================================================================
#  ICU 74.2 — the character and locale tables, in ring 3
# ===================================================================
#
# The largest thing ported to this system: 445 translation units and
# 384,000 lines, plus a thirty-megabyte data archive. WebKit requires it
# outright -- OptionsWPE.cmake asks for `ICU 61.2 COMPONENTS data i18n
# uc` and for a HarfBuzz built against it -- and nothing else can supply
# what it holds: the Unicode character database, collation for every
# language, the segmentation rules, the legacy character encodings a
# browser has to decode, and the IANA timezone database.
#
# ---- the data is prebuilt, and that is the whole story ----
#
# ICU's reputation for being unbuildable comes from its data: normally
# you compile *host* tools -- genrb, gencnv, icupkg -- run them over a
# few thousand locale source files, and package the result. That is a
# second toolchain and it is genuinely the hard part.
#
# It is also unnecessary. The source tarball ships the finished archive
# at data/in/icudt74l.dat: thirty megabytes, already packaged, and the
# `l` on the end means little-endian, which is this machine. The host
# bootstrap exists to *filter* that archive down, not to create it. So
# there is no data build here at all -- the file is copied onto the
# volume and ICU is told where to find it.
#
# ---- the configuration, and why each one ----
#
# U_STATIC_IMPLEMENTATION   there are no shared objects on this target
# U_COMMON_IMPLEMENTATION   } which half is being compiled; ICU uses
# U_I18N_IMPLEMENTATION     } these to decide what to export
# U_HAVE_MMAP=0             there is no file-backed mmap, so umapfile.cpp
#                           takes its stdio path -- fopen and fread,
#                           which this C library does have
# U_ENABLE_DYLOAD=0         no dlopen, no runtime linker
# U_CHARSET_IS_UTF8=1       the default codepage. True of every byte
#                           string in this system, and it removes the
#                           charset-detection path entirely
# U_HAVE_NL_LANGINFO_CODESET=0
#                           there is no langinfo.h, because there is no
#                           locale to interrogate
# U_HAVE_TZSET / TIMEZONE / TZNAME = 0
#                           there is no timezone database *outside* ICU;
#                           the one inside its own data is the real one.
#                           See libc/include/time.h.
#
# -frtti is the one that is not a subtraction. Everything else in this
# repository is built -fno-rtti; ICU 74 uses dynamic_cast in 117 places
# and typeid in about forty, with no fallback -- utypeinfo.h includes
# <typeinfo> unconditionally. So libcxx/src/typeinfo.cpp exists, and
# apps/rtti_cases.h checks it against the host's own C++ runtime.
#
# C++17 rather than the C++20 everything else uses, because that is what
# ICU 74 is written and tested against.
ICU_VERSION := 74.2
ICU_TARBALL := icu4c-74_2-src.tgz
ICU_URL     := https://github.com/unicode-org/icu/releases/download/release-74-2/$(ICU_TARBALL)
ICU_SHA256  := 68db082212a96d6f53e35d60f47d38b962e9f9d207a74cfac78029ae8ff5e08c

ICU_DIR  := third_party/icu
ICU_DATA := $(ICU_DIR)/data/in/icudt74l.dat
ICU_INC  := -I$(ICU_DIR)/common -I$(ICU_DIR)/i18n

ICU_DEFS := -DU_STATIC_IMPLEMENTATION -DU_HAVE_MMAP=0 -DU_ENABLE_DYLOAD=0 \
            -DU_CHARSET_IS_UTF8=1 -DU_HAVE_NL_LANGINFO_CODESET=0 \
            -DU_HAVE_TZSET=0 -DU_HAVE_TIMEZONE=0 -DU_HAVE_TZNAME=0

# CXXFLAGS minus the two that ICU cannot be built with, plus the two it
# needs. Written as a filter rather than as a second list so that a flag
# added to CXXFLAGS later reaches here too.
ICU_CXXFLAGS := $(filter-out -fno-rtti -std=c++20,$(CXXFLAGS)) -std=c++17 -frtti -w

ICU_UC_SRC   := $(wildcard $(ICU_DIR)/common/*.cpp)
ICU_I18N_SRC := $(wildcard $(ICU_DIR)/i18n/*.cpp)
ICU_DATA_SRC := $(wildcard $(ICU_DIR)/stubdata/*.cpp)

ICU_UC_OBJ   := $(patsubst $(ICU_DIR)/%.cpp,build/icu/%.o,$(ICU_UC_SRC))
ICU_I18N_OBJ := $(patsubst $(ICU_DIR)/%.cpp,build/icu/%.o,$(ICU_I18N_SRC))
ICU_DATA_OBJ := $(patsubst $(ICU_DIR)/%.cpp,build/icu/%.o,$(ICU_DATA_SRC))

LIBICUUC   := build/libicuuc.a
LIBICUI18N := build/libicui18n.a
LIBICUDATA := build/libicudata.a
ICU_LIBS   := $(LIBICUI18N) $(LIBICUUC) $(LIBICUDATA)

build/icu/common/%.o: $(ICU_DIR)/common/%.cpp $(ICU_DIR)/common/unicode/utypes.h
	@mkdir -p build/icu/common
	$(CXX) $(ICU_CXXFLAGS) -DU_COMMON_IMPLEMENTATION $(ICU_DEFS) $(ICU_INC) -c $< -o $@

# stubdata is one file and it gets an archive of its own, named
# libicudata.a, because that is the name every consumer looks for --
# WebKit asks for `ICU COMPONENTS data i18n uc` and a build with the
# first one missing is rejected however complete the other two are.
#
# What it contains is the *absence* of linked-in data: the symbol a
# built-in archive would define, left empty, so that the archive is
# found on disk at run time instead. That is the arrangement this system
# uses -- icudt74l.dat sits on the volume and u_setDataDirectory points
# at it.
build/icu/stubdata/%.o: $(ICU_DIR)/stubdata/%.cpp $(ICU_DIR)/common/unicode/utypes.h
	@mkdir -p build/icu/stubdata
	$(CXX) $(ICU_CXXFLAGS) -DU_COMMON_IMPLEMENTATION $(ICU_DEFS) $(ICU_INC) -c $< -o $@

build/icu/i18n/%.o: $(ICU_DIR)/i18n/%.cpp $(ICU_DIR)/common/unicode/utypes.h
	@mkdir -p build/icu/i18n
	$(CXX) $(ICU_CXXFLAGS) -DU_I18N_IMPLEMENTATION $(ICU_DEFS) $(ICU_INC) -c $< -o $@

$(LIBICUUC): $(ICU_UC_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(ICU_UC_OBJ)

$(LIBICUDATA): $(ICU_DATA_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(ICU_DATA_OBJ)

$(LIBICUI18N): $(ICU_I18N_OBJ)
	@mkdir -p build
	$(AR) rcs $@ $(ICU_I18N_OBJ)

.PHONY: icu
icu: $(ICU_LIBS)

# ---- HarfBuzz, built against ICU ----
#
# A separate archive because WebKit asks for it separately:
# `find_package(HarfBuzz 1.4.2 REQUIRED COMPONENTS ICU)` looks for
# libharfbuzz-icu alongside libharfbuzz, and a HarfBuzz without it is
# rejected outright however new it is. That was the exact error
# `make webkit` stopped on before ICU existed.
#
# It is one upstream source file. hb-icu.cc gives HarfBuzz a
# hb_unicode_funcs_t backed by ICU's character database instead of the
# compact copy HarfBuzz carries itself -- the same properties from the
# same tables the rest of the system uses.
LIBHBICU := build/libharfbuzz-icu.a

build/harfbuzz/hb-icu.o: $(HB_DIR)/src/hb-icu.cc $(ICU_DIR)/common/unicode/utypes.h
	@mkdir -p build/harfbuzz
	$(CXX) $(ICU_CXXFLAGS) $(HB_DEFS) -DHAVE_ICU=1 -DHAVE_ICU_BUILTIN=1 \
		$(HB_INC) $(ICU_INC) $(ICU_DEFS) -DU_SHOW_CPLUSPLUS_API=0 \
		-c $< -o $@

$(LIBHBICU): build/harfbuzz/hb-icu.o
	@mkdir -p build
	$(AR) rcs $@ $^


# ---- fetching the three, and why they are not committed ----
#
# Each is downloaded and checked against the hash recorded above before
# anything is unpacked. A download that is not verified is a download
# that can be replaced, and all three of these are compiled and run in
# ring 3 on this machine.
#
# They are fetched rather than vendored because together they are 131
# megabytes of source, against lwIP and Mbed TLS which are committed at
# eight. The line is the same one wiki.zim and the language models sit
# on: a pinned download says exactly what a committed copy says and does
# not put it in everybody's clone.
#
# Each rule keys off one file, so `make` brings a library down the first
# time it is needed and never again.

$(SQLITE_DIR)/sqlite3.c:
	@echo "  fetching $(SQLITE_ZIP)"
	@mkdir -p build $(SQLITE_DIR)
	@curl -fL --retry 3 -o build/$(SQLITE_ZIP) $(SQLITE_URL)
	@printf '%s  build/%s\n' "$(SQLITE_SHA256)" "$(SQLITE_ZIP)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(SQLITE_ZIP) does not match its checksum; not unpacked."; \
		     rm -f build/$(SQLITE_ZIP); exit 1; }
	@cd build && rm -rf sqlite-amalgamation-$(SQLITE_VERSION) && unzip -q $(SQLITE_ZIP)
	@cp build/sqlite-amalgamation-$(SQLITE_VERSION)/sqlite3.c \
	    build/sqlite-amalgamation-$(SQLITE_VERSION)/sqlite3.h \
	    build/sqlite-amalgamation-$(SQLITE_VERSION)/sqlite3ext.h $(SQLITE_DIR)/
	@echo "  SQLITE   $(SQLITE_DIR) ($(SQLITE_VERSION))"

$(SQLITE_DIR)/sqlite3.h: $(SQLITE_DIR)/sqlite3.c

$(FT_DIR)/include/ft2build.h:
	@echo "  fetching $(FREETYPE_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(FREETYPE_TARBALL) $(FREETYPE_URL)
	@printf '%s  build/%s\n' "$(FREETYPE_SHA256)" "$(FREETYPE_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(FREETYPE_TARBALL) does not match its checksum."; \
		     rm -f build/$(FREETYPE_TARBALL); exit 1; }
	@rm -rf $(FT_DIR) third_party/freetype-$(FREETYPE_VERSION)
	@tar -C third_party -xzf build/$(FREETYPE_TARBALL)
	@mv third_party/freetype-$(FREETYPE_VERSION) $(FT_DIR)
	@echo "  FREETYPE $(FT_DIR) ($(FREETYPE_VERSION))"

$(HB_DIR)/src/harfbuzz.cc:
	@echo "  fetching $(HARFBUZZ_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(HARFBUZZ_TARBALL) $(HARFBUZZ_URL)
	@printf '%s  build/%s\n' "$(HARFBUZZ_SHA256)" "$(HARFBUZZ_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(HARFBUZZ_TARBALL) does not match its checksum."; \
		     rm -f build/$(HARFBUZZ_TARBALL); exit 1; }
	@rm -rf $(HB_DIR) third_party/harfbuzz-$(HARFBUZZ_VERSION)
	@tar -C third_party -xf build/$(HARFBUZZ_TARBALL)
	@mv third_party/harfbuzz-$(HARFBUZZ_VERSION) $(HB_DIR)
	@echo "  HARFBUZZ $(HB_DIR) ($(HARFBUZZ_VERSION))"

$(ICU_DIR)/common/unicode/utypes.h:
	@echo "  fetching $(ICU_TARBALL) (26 MB compressed, 113 MB unpacked)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(ICU_TARBALL) $(ICU_URL)
	@printf '%s  build/%s\n' "$(ICU_SHA256)" "$(ICU_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(ICU_TARBALL) does not match its checksum."; \
		     rm -f build/$(ICU_TARBALL); exit 1; }
	@rm -rf $(ICU_DIR) build/icu-unpack
	@mkdir -p build/icu-unpack
	@tar -C build/icu-unpack -xzf build/$(ICU_TARBALL)
	@mv build/icu-unpack/icu/source $(ICU_DIR)
	@rm -rf build/icu-unpack
	@echo "  ICU      $(ICU_DIR) ($(ICU_VERSION))"

.PHONY: libs-fetch
libs-fetch: $(SQLITE_DIR)/sqlite3.c $(FT_DIR)/include/ft2build.h $(HB_DIR)/src/harfbuzz.cc \
            $(ICU_DIR)/common/unicode/utypes.h

# --- Fetching WPE WebKit itself ---
#
# libwpe is vendored, as lwIP and Mbed TLS are, because it is eight
# thousand lines and a clone of this repository should build the same
# system a year from now. WPE WebKit is not: it is some millions of
# lines and half a gigabyte of tarball, which is the same reason
# wiki.zim and qwen2.gguf are downloaded rather than committed.
#
# The tarball is verified against a checksum recorded here. A download
# that is not checked is a download that can be replaced, and this one
# would be compiled and run.
WEBKIT_VERSION := 2.46.5
WEBKIT_TARBALL := wpewebkit-$(WEBKIT_VERSION).tar.xz
WEBKIT_URL     := https://wpewebkit.org/releases/$(WEBKIT_TARBALL)
# Recorded from the published tarball rather than trusted from it: a
# download that is not checked is a download that can be replaced, and
# this one would be compiled and run in ring 3 on this machine.
WEBKIT_SHA256  := 2efd4831efcf86e29546c028d6f17a7b775b61b6499ed62399a00da8f06ea456
WEBKIT_SRC     := third_party/wpewebkit-$(WEBKIT_VERSION)

.PHONY: webkit-fetch
webkit-fetch: $(WEBKIT_SRC)/CMakeLists.txt

$(WEBKIT_SRC)/CMakeLists.txt:
	@echo "  fetching $(WEBKIT_TARBALL) (39 MB compressed)"
	@mkdir -p third_party
	@curl -fL --retry 3 -o build/$(WEBKIT_TARBALL) $(WEBKIT_URL)
	@echo "  verifying"
	@printf '%s  build/%s\n' "$(WEBKIT_SHA256)" "$(WEBKIT_TARBALL)" \
		| shasum -a 256 -c - \
		|| { echo ""; \
		     echo "  The tarball does not match the checksum in the Makefile."; \
		     echo "  Either the release was re-rolled upstream or the download"; \
		     echo "  was tampered with. It has NOT been unpacked."; \
		     echo ""; \
		     rm -f build/$(WEBKIT_TARBALL); exit 1; }
	@tar -C third_party -xf build/$(WEBKIT_TARBALL)

# --- A sysroot for cmake to look in ---
#
# WebKit finds its dependencies with pkg-config first and find_path /
# find_library second. There is no pkg-config here and the four
# libraries this system does have are not laid out the way a Unix
# installation lays them out -- the archives are flat in build/ under
# the names this Makefile gave them, and the headers are wherever their
# upstream tarball put them.
#
# So they are staged into one directory that looks like a sysroot, and
# vextro-toolchain.cmake points CMAKE_FIND_ROOT_PATH at it. Nothing is
# built here; every file is copied from something that was already
# compiled and already ran its tests on the machine.
#
# Two of the copies are renames, and both are because the find modules
# search for the *installed* name rather than ours:
#
#   libsqlite.a -> libsqlite3.a    find_library(NAMES sqlite3)
#   libwpe.a    -> libwpe-1.0.a    find_library(NAMES wpe-1.0)
#
# ---- and one copy that would be a lie without it ----
#
# FreeType is compiled here with FT_CONFIG_OPTIONS_H and
# FT_CONFIG_MODULES_H redirected on the command line to the two headers
# in third_party/freetype-port/. Staging upstream's include/ tree alone
# would hand a consumer the *default* configuration -- a different set
# of FT_CONFIG_OPTION_ defines from the one libfreetype.a was built
# with, and some of those change structure layouts. The redirected pair
# is therefore copied over the stock ones, so the headers in the sysroot
# describe the archive beside them rather than some other build of the
# same version.
#
# ICU's headers are the other case worth a line. Upstream keeps them in
# two directories -- common/unicode and i18n/unicode -- and every
# consumer includes them as one <unicode/...>, because that is how an
# installed ICU lays them out. The two are merged here for the same
# reason: a build that only saw common/unicode would compile
# unicode/uchar.h and fail on unicode/ucol.h with no hint that the
# second half exists.
#
# HarfBuzz has the same shape of hazard and not the same severity: it is
# built -DHB_TINY -DHB_NO_MT, which turns features off inside the
# library without moving anything in the public headers. It is recorded
# here because "the headers match the archive" is a claim this directory
# makes, and it should be a checked one.
WEBKIT_SYSROOT := build/webkit-sysroot

.PHONY: webkit-sysroot
webkit-sysroot: $(WEBKIT_SYSROOT)/.stamp

$(WEBKIT_SYSROOT)/.stamp: $(LIBSQLITE) $(LIBFT) $(LIBHB) $(LIBWPE) \
                          $(ICU_LIBS) $(LIBHBICU) \
                          $(FT_PORT)/ftoption.h $(FT_PORT)/ftmodule.h
	@rm -rf $(WEBKIT_SYSROOT)
	@mkdir -p $(WEBKIT_SYSROOT)/include/harfbuzz \
	          $(WEBKIT_SYSROOT)/include/freetype2 \
	          $(WEBKIT_SYSROOT)/include/wpe \
	          $(WEBKIT_SYSROOT)/lib
	@cp $(HB_DIR)/src/hb*.h                $(WEBKIT_SYSROOT)/include/harfbuzz/
	@cp $(SQLITE_DIR)/sqlite3.h $(SQLITE_DIR)/sqlite3ext.h \
	                                       $(WEBKIT_SYSROOT)/include/
	@cp $(WPE_DIR)/include/wpe/*.h         $(WEBKIT_SYSROOT)/include/wpe/
	@cp $(FT_DIR)/include/ft2build.h       $(WEBKIT_SYSROOT)/include/freetype2/
	@cp -R $(FT_DIR)/include/freetype      $(WEBKIT_SYSROOT)/include/freetype2/
	@cp $(FT_PORT)/ftoption.h $(FT_PORT)/ftmodule.h \
	    $(WEBKIT_SYSROOT)/include/freetype2/freetype/config/
	@cp $(LIBHB)       $(WEBKIT_SYSROOT)/lib/libharfbuzz.a
	@cp $(LIBFT)       $(WEBKIT_SYSROOT)/lib/libfreetype.a
	@cp $(LIBSQLITE)   $(WEBKIT_SYSROOT)/lib/libsqlite3.a
	@cp $(LIBWPE)      $(WEBKIT_SYSROOT)/lib/libwpe-1.0.a
	@cp $(LIBHBICU)    $(WEBKIT_SYSROOT)/lib/libharfbuzz-icu.a
	@cp $(LIBICUUC)    $(WEBKIT_SYSROOT)/lib/libicuuc.a
	@cp $(LIBICUI18N)  $(WEBKIT_SYSROOT)/lib/libicui18n.a
	@cp $(LIBICUDATA)  $(WEBKIT_SYSROOT)/lib/libicudata.a
	@cp -R $(ICU_DIR)/common/unicode $(WEBKIT_SYSROOT)/include/
	@cp $(ICU_DIR)/i18n/unicode/*.h  $(WEBKIT_SYSROOT)/include/unicode/
	@touch $@
	@echo "  SYSROOT  $(WEBKIT_SYSROOT) (icu, harfbuzz, freetype, sqlite3, wpe)"

# --- Building it ---
#
# This target names everything that is missing at once rather than
# stopping at the first, which is the same courtesy the toolchain check
# at the top of this file extends -- discovering four prerequisites one
# build at a time is four builds.
#
# It does not succeed today, and the reason has moved twice. It was "no
# C++ standard library for x86_64-elf"; libcxx/ answered that. It was
# then a FATAL_ERROR in WebKit's own OS detection, which had no branch
# for a target without an operating system; the injection layer in
# third_party/wpe-config/ answers that, and configure now runs through
# WebKit's feature detection and reaches its dependency list. What
# stands here now is the dependency list itself -- 22 REQUIRED packages,
# of which this system has four. third_party/wpe-config/README.md has
# the current frontier and the exact error.
.PHONY: webkit
webkit: $(WEBKIT_SRC)/CMakeLists.txt $(LIBWPE) $(LIBC) $(LIBCXX) \
        $(WEBKIT_SYSROOT)/.stamp
	@missing=""; \
	command -v cmake >/dev/null || missing="$$missing cmake"; \
	command -v ninja >/dev/null || missing="$$missing ninja"; \
	command -v ruby  >/dev/null || missing="$$missing ruby"; \
	command -v gperf >/dev/null || missing="$$missing gperf"; \
	command -v $(CXX) >/dev/null || missing="$$missing $(CXX)"; \
	if [ -n "$$missing" ]; then \
	    echo ""; \
	    echo "  WPE WebKit cannot be built here yet. Missing:$$missing"; \
	    echo ""; \
	    echo "  These are package installs rather than work. What used to"; \
	    echo "  be in the way was not:"; \
	    echo ""; \
	    echo "    the C++ runtime       done  libcxx/, 35 headers over libc/"; \
	    echo "    descriptors in ring 3 done  src/vfs.h, 19 system calls"; \
	    echo "    sockets in ring 3     done  over src/vxnet.h"; \
	    echo "    the OS gate           done  third_party/wpe-config/"; \
	    echo "    sqlite, freetype, harfbuzz"; \
	    echo "                          done  ported, and green in ring 3"; \
	    echo ""; \
	    echo "  What remains after installing the above is the dependency"; \
	    echo "  list: 22 packages WebKit marks REQUIRED, of which this"; \
	    echo "  system has four. ICU is the first of the eighteen, and"; \
	    echo "  each is a port onto interfaces that now exist and are"; \
	    echo "  tested, rather than an interface to be invented."; \
	    echo ""; \
	    echo "  third_party/wpe-config/README.md has the full order."; \
	    echo ""; \
	    exit 1; \
	fi; \
	mkdir -p build/webkit; \
	cd build/webkit && cmake -G Ninja \
	    -DCMAKE_TOOLCHAIN_FILE="$(CURDIR)/third_party/wpe-config/vextro-toolchain.cmake" \
	    -C "$(CURDIR)/third_party/wpe-config/vextro-wpe.cmake" \
	    -DCMAKE_INSTALL_PREFIX="$(CURDIR)/build/webkit/root" \
	    "$(CURDIR)/$(WEBKIT_SRC)" && ninja

# --- The browser, packaged ---
#
# The .vx container the store and the loader both speak. Built from
# whatever object files exist: today that is the backend and a shell
# around it, and when WebKit builds it is the engine as well.
#
# Kept as a rule rather than left until the engine exists, because the
# packaging is a real question with a real answer -- a .vx is an ELF
# repacked by vx_maker, and an engine linked into one is loaded exactly
# like `mandel` is.
build/store/browser.elf: build/wpetest.o $(LIBWPE) $(LIBC) $(LIBC_CRT0) vxfmt/vx.ld
	@mkdir -p build/store
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T vxfmt/vx.ld \
		build/wpetest.o $(LIBC_CRT0) $(LIBWPE) $(LIBC) -o $@

# --- User app: wpetest ---
#
# The backend, exercised in ring 3 without WebKit. Everything about a WPE
# backend that does not need the engine can be checked here -- that
# libwpe links and runs against this C library at all, that the static
# loader resolves, that a view dispatches its size, that the blit puts
# the right pixels in the right places against a source stride that does
# not match the window's, and that input arrives as events rather than as
# a state repeated every frame.
#
# It links crt0, unlike every other application here, because libwpe is
# ordinary POSIX C that expects a main() and a running C library rather
# than a bare _start.
build/wpetest.o: apps/wpetest.c $(WPE_PORT)/vxwpe.h apps/vextro.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) $(WPE_INC) -c $< -o $@

build/wpetest: build/wpetest.o apps/app.ld $(LIBWPE) $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/wpetest.o $(LIBC_CRT0) $(LIBWPE) $(LIBC) -o $@

# --- User app: threadtest ---
#
# The rest of the C library, exercised in ring 3 on the real machine.
# `make test` checks the arithmetic on the host, where a reference
# exists; nothing about threads, thread-local storage or a lazily backed
# mapping can be checked that way, because none of them is a computation
# -- they are the behaviour of the kernel underneath. This is where that
# is checked.
#
# It keeps the old entry convention rather than linking crt0, so that it
# also demonstrates the case a program written before any of this still
# has to work in.
build/threadtest.o: apps/threadtest.c apps/vextro.h libc/include/pthread.h \
                    libc/include/sys/mman.h libc/include/math.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/threadtest: build/threadtest.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/threadtest.o $(LIBC) -o $@

# --- User app: fdprobe ---
#
# The smallest program that can say whether descriptors and sockets work
# at all, written against the raw system calls rather than the library
# built on them. It exists because the two things that could not be
# settled by reading code -- whether a blocking socket call made from a
# system call comes back, and whether the loopback interface routes --
# are cheap to find out with fifty lines and expensive to find out
# underneath five thousand.
build/fdprobe.o: apps/fdprobe.c apps/vextro.h libc/include/sys/syscall.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/fdprobe: build/fdprobe.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/fdprobe.o $(LIBC) -o $@

# --- User app: fdtest ---
#
# The same ground fdprobe covers, one layer up: through open(), fopen(),
# opendir(), socket() and the errno convention, rather than through the
# raw calls. The two together say which side a failure is on -- the
# kernel or the library -- which without a debugger is worth a second
# program.
build/fdtest.o: apps/fdtest.c apps/vextro.h libc/include/fcntl.h \
                libc/include/dirent.h libc/include/sys/socket.h \
                libc/include/sys/stat.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/fdtest: build/fdtest.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/fdtest.o $(LIBC) -o $@

# --- User app: icutest ---
#
# ICU in ring 3. Compiled -frtti because it uses dynamic_cast on ICU's
# own C++ API -- see the note at the head of the file about why that is
# the interesting part -- and linked with crt0 because ICU has static
# constructors that must run before main.
#
# $(LIBGCC) is on the link for the same reason hbtest needs it: the
# compiler emits calls to its own helpers (__popcountdi2 and friends)
# that no C library provides.
build/icutest.o: apps/icutest.cpp apps/vextro.h $(ICU_DIR)/common/unicode/utypes.h \
                 $(wildcard libcxx/include/*)
	@mkdir -p build
	$(CXX) $(ICU_CXXFLAGS) $(ICU_DEFS) $(ICU_INC) -c $< -o $@

build/icutest: build/icutest.o apps/app.ld $(ICU_LIBS) $(LIBCXX) $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/icutest.o $(LIBC_CRT0) $(ICU_LIBS) \
		$(LIBCXX) $(LIBC) $(LIBGCC) -o $@

# --- User app: cxxtest ---
#
# The first C++ program this system runs, and the only place several
# properties of the runtime can be checked at all: that operator new
# reaches the ring-3 allocator, that static constructors run before main
# and destructors after it, that a function-local static is constructed
# once with four threads racing for it, and that a vtable survives a
# loader which maps an image page by page under W^X.
#
# Linked with crt0, unlike the other tests, because the whole point is
# that .init_array is walked.
build/cxxtest.o: apps/cxxtest.cpp apps/vextro.h $(wildcard libcxx/include/*)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

# The one object here built with RTTI on.
#
# -fno-rtti is in CXXFLAGS and stays there; this file is the exception
# because typeid and dynamic_cast do not compile without it, and the
# whole point of apps/rtti_cases.h is to run them. Filtering the flag out
# rather than writing a second flag list keeps the two in step: anything
# added to CXXFLAGS later applies here too.
#
# It is also the arrangement ICU is built with -- one library compiled
# -frtti linked into programs that are not -- so this object checks the
# mixture as well as the casts.
build/rtti_probe.o: apps/rtti_probe.cpp apps/rtti_cases.h $(wildcard libcxx/include/*)
	@mkdir -p build
	$(CXX) $(filter-out -fno-rtti,$(CXXFLAGS)) -frtti -c $< -o $@

build/cxxtest: build/cxxtest.o build/rtti_probe.o apps/app.ld \
               $(LIBCXX) $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/cxxtest.o build/rtti_probe.o \
		$(LIBC_CRT0) $(LIBCXX) $(LIBC) -o $@

# --- User app: sqltest ---
#
# SQLite in ring 3, on this system's own filesystem. Links the vendored
# engine, the VFS over the descriptor calls, and the C library.
build/sqltest.o: apps/sqltest.c apps/vextro.h $(SQLITE_DIR)/sqlite3.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) $(SQLITE_INC) -c $< -o $@

build/sqltest: build/sqltest.o apps/app.ld $(LIBSQLITE) $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/sqltest.o $(LIBSQLITE) $(LIBC) -o $@

# --- The seeded database ---
#
# Written by the *host's* sqlite3, which is a separate implementation at
# a different version. A port that reads this is a port that reads
# databases other engines wrote — a much stronger claim than reading back
# something it wrote itself, and the same reason tools/ntfsdir.py exists
# to check the filesystem driver against an independent reader.
build/sqlseed.db: Makefile
	@mkdir -p build
	@rm -f $@
	@command -v sqlite3 >/dev/null || { \
	    echo "  sqlite3 is not installed; the seeded-database check"; \
	    echo "  cannot be built. Install it or the machine test will"; \
	    echo "  report the read path as untested."; exit 1; }
	@sqlite3 $@ \
	  "CREATE TABLE seeded(key TEXT PRIMARY KEY, value TEXT); \
	   INSERT INTO seeded VALUES('engine','written by another sqlite'); \
	   INSERT INTO seeded VALUES('purpose','the VFS read path'); \
	   INSERT INTO seeded VALUES('volume','NTFS'); \
	   INSERT INTO seeded VALUES('zebra','last by sort order');"
	@echo "  SEED   $@ (`sqlite3 $@ 'select count(*) from seeded'` rows, \
by sqlite3 `sqlite3 -version | cut -d' ' -f1`)"

# --- User app: fttest ---
#
# FreeType in ring 3, reading the same face the kernel has embedded, so
# two independent TrueType parsers can be asked the same questions about
# the same bytes.
build/fttest.o: apps/fttest.c apps/vextro.h $(FT_PORT)/ftoption.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) $(FT_INC) -c $< -o $@

build/fttest: build/fttest.o apps/app.ld $(LIBFT) $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/fttest.o $(LIBFT) $(LIBC) -o $@

# --- User app: hbtest ---
#
# HarfBuzz shaping text in ring 3, over an FT_Face, over the C library.
# C++ because HarfBuzz is, so this links libvextrocxx as well.
build/hbtest.o: apps/hbtest.cpp apps/vextro.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(HB_INC) $(FT_INC) -c $< -o $@

# $(LIBGCC) is on this link and on no other app's, which is worth a line.
# HarfBuzz counts bits — hb_popcount over a 64-bit word — and GCC lowers
# __builtin_popcountll to a call into its own runtime rather than to an
# instruction, because POPCNT is not in the base x86-64 instruction set.
# Applications link -nostdlib and so have never needed libgcc before; the
# kernel has always linked it. Found as three undefined references to
# __popcountdi2 at the first link, which is the only way this shows up.
build/hbtest: build/hbtest.o apps/app.ld $(LIBHB) $(LIBFT) $(LIBCXX) \
              $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/hbtest.o $(LIBC_CRT0) $(LIBHB) $(LIBFT) $(LIBCXX) \
		$(LIBC) $(LIBGCC) -o $@

build/faulter.o: apps/faulter.c apps/vextro.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/faulter: build/faulter.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/faulter.o $(LIBC) -o $@

# --- .vx toolchain (host) ---
$(VX_MAKER): vxfmt/vx_maker.c vxfmt/vx_format.h
	$(HOSTCC) -O2 -Wall -Wextra -std=gnu11 -o $@ $<

vxfmt/vx_run: vxfmt/vx_run.c vxfmt/vx_format.h
	$(HOSTCC) -O2 -Wall -Wextra -std=gnu11 -o $@ $<

# --- Store apps: canvas apps compiled to ELF64, repacked as .vx ---
# vx.ld puts .data on its own page so the two segments can carry
# different protections; -z max-page-size stops ld padding to 2 MB.
build/store/%.o: apps/store/%.c apps/vextro.h
	@mkdir -p build/store
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/store/%.elf: build/store/%.o vxfmt/vx.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T vxfmt/vx.ld \
		$< $(LIBC) -o $@

build/store/%.vx: build/store/%.elf $(VX_MAKER)
	$(VX_MAKER) -o $@ -e $<

# --- Pictures: PNG -> .sci ---
build/pics/%.sci: apps/pics/%.png tools/mkimg.py
	@mkdir -p build/pics
	python3 tools/mkimg.py -o $@ $<

# --- Ramdisk: tar archive of apps/ text files + compiled binaries ---
# The store payloads ride along here too, so the storefront still has
# something to install on an ISO-only boot with no disk attached.
build/initrd.tar: $(wildcard apps/*.txt) build/hello build/faulter \
                  build/mutextest build/threadtest build/wpetest build/fdprobe \
                  build/fdtest build/cxxtest build/sqltest build/fttest \
                  build/hbtest \
                  $(WINAPPS) $(STORE_BINS)
	@mkdir -p build/initrd_staging/store/pkg
	cp apps/*.txt build/initrd_staging/ 2>/dev/null || true
	cp build/hello build/faulter build/mutextest build/threadtest build/wpetest \
	   build/fdprobe build/fdtest build/cxxtest build/sqltest build/fttest build/hbtest \
	   build/initrd_staging/
	$(foreach w,$(WINAPPS),cp $(w) build/initrd_staging/;)
	$(foreach a,$(STORE_APPS),cp build/store/$(a).vx build/initrd_staging/store/pkg/$(a).vx;)
	tar --format=ustar -cf $@ -C build/initrd_staging .
	rm -rf build/initrd_staging

# --- Kernel ---
#
# Four objects, not one.
#
# src/core/main.c is the composition root: it includes the driver and
# desktop headers, and those genuinely share static state -- one MMIO
# map, one heap, one window list -- so they stay a single translation
# unit. The .c files under src/net and src/media are included into it
# for the same reason and are dependencies of that object rather than
# objects of their own.
#
# The three modules below came out because their interfaces are narrow
# enough to measure: the scheduler needs fifteen symbols and exports
# twenty-four, NTFS needs nine and exports eight, the scanner needs five
# and exports fourteen. Everything crossing those boundaries is declared
# in include/kernel_shared.h.
#
# The rule that keeps this honest is that the three module sources
# include only that seam header and their own declaration header. If one
# of them ever includes a driver header directly it will compile, link,
# and give that object a private copy of the driver's state -- so the
# discipline is in the source, and the narrow dependency lines below are
# what make a violation visible as a rebuild that should not have
# happened.
KERN_MODULES := src/sched/scheduler.c \
                src/fs/ntfs/ntfs_ops.c \
                src/security/anti_virus.c
KERN_MODULE_OBJ := $(patsubst src/%.c,build/%.o,$(KERN_MODULES))

# Wildcards over every directory that now holds kernel headers. Leaving
# one out means make reports success having rebuilt nothing -- which
# reads as "the fix did not work" and is really "the fix was never
# compiled". That has cost this build two debugging sessions, once for
# src/net and once for lwipopts.h, so the lists are deliberately wide.
KERN_HDRS := $(wildcard src/*.h) $(wildcard include/*.h) \
             $(wildcard src/net/*.h) $(wildcard src/media/*.h) \
             $(wildcard src/sched/*.h) $(wildcard src/fs/ntfs/*.h) \
             $(wildcard src/security/*.h)

# --- The interface skin ---
#
# assets/ui/*.vxml carries the browser shell as Tailwind utility classes;
# tools/tailwind.py resolves them against assets/ui/tokens.tw and emits a
# node table the compositor draws (src/vxui.h) plus the equivalent
# stylesheet for the day WebKit builds.
#
# The dependency below is named explicitly and that matters: KERN_HDRS is
# a wildcard over src/ and include/, and the generated header is under
# build/. Without this line a change to a colour token would leave the
# kernel object untouched and the old palette on screen — silently, which
# is the same class of stale-build bug the note beside BUILD_FLAGS at the
# top of this file describes.
UI_TOKENS := assets/ui/tokens.tw
UI_SHELL  := assets/ui/browser.vxml
UI_GEN    := build/ui/vxui_gen.h
UI_CSS    := build/ui/vextro.css

$(UI_GEN) $(UI_CSS): tools/tailwind.py $(UI_TOKENS) $(UI_SHELL)
	@mkdir -p build/ui
	@python3 tools/tailwind.py

.PHONY: ui
ui: $(UI_GEN)

build/core/main.o: src/core/main.c $(KERN_HDRS) $(UI_GEN) \
                   $(wildcard src/net/*.c) $(wildcard src/media/*.c) \
                   build/res.stamp
	@mkdir -p build/core
	$(CC) $(CFLAGS) -c $< -o $@

# One rule for all three modules. Each depends on the whole header set
# rather than on its own two files: the seam header is shared, and a
# change to it changes what every module compiles to.
#
# A *static* pattern rule, scoped to exactly these three objects. Written
# as a plain `build/%.o: src/%.c` it would also match build/llm.o,
# build/lwipglue.o and build/tlsglue.o, which have their own rules and
# their own flags -- lwIP and Mbed TLS need -w and a stack of -I paths
# that the kernel proper does not. Explicit rules do win over pattern
# rules, so it would have worked; it would also have left a rule quietly
# claiming to build three objects it must never build.
$(KERN_MODULE_OBJ): build/%.o: src/%.c $(KERN_HDRS) $(UI_GEN)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The inference unit used to be the one place floats were allowed. It no
# longer is — the whole kernel has them — so what is left here is the
# optimisation level, and one flag that stays absent on purpose.
#
# -O3 for this translation unit, and deliberately *not* -ffast-math.
#
# Letting the compiler reassociate floating-point sums was tried, on the
# theory that a dot product cannot vectorise without it. It was worth
# about nothing — 40.8 s against 38.1 s on the same question, slightly
# the wrong side of noise — because dequantisation dominates and the
# arithmetic was never the bottleneck. `llm bench` says so directly: two
# milliseconds to expand the model's largest weight, under one to
# multiply by it.
#
# It also cost something real. With reassociation permitted, the batched
# and unbatched paths through the same maths vectorise differently and
# stop agreeing bit-for-bit, so which of the two ran changed the answer.
# Paying determinism for nothing is a bad trade.
LLM_CFLAGS := $(filter-out -O2,$(CFLAGS)) -O3

build/llm.o: src/llm.c src/llm.h
	@mkdir -p build
	$(CC) $(LLM_CFLAGS) -c $< -o $@

# --- The network stack: lwIP 2.2.1 and Mbed TLS 3.6.4 ---
#
# Vendored under third_party/, compiled as their own translation units
# and linked like build/llm.o. They cannot be included into the
# composition root and it is not a matter of effort: src/core/main.c is
# still one large unit of `static` functions, and `static` is exactly
# what makes them unreachable from another file. lwIP also macro-defines
# htons() over the top of the one in src/netstack.h, and declares a
# `struct sockaddr` and an `fd_set` that a translation unit this size has
# no room for.
#
# The kernel-side split into four objects did not change any of that --
# it moved three modules out from behind the same wall, through
# include/kernel_shared.h rather than around it.
#
# So the seam is src/vxport.h -- twenty-odd functions, native types, no
# foreign header -- and everything on the far side of it is compiled
# here.
#
# -w on the vendored files only. They are correct and they are not ours;
# -Wall -Wextra on a quarter of a million lines of someone else's code
# produces several thousand warnings, and a build whose warnings are
# always ignored is a build with no warnings at all. The glue in src/ is
# compiled with the kernel's full warning set.
LWIP_DIR  := third_party/lwip

NET_INC   := -Ithird_party/include \
             -I$(LWIP_DIR)/src/include -Ithird_party/lwip-port \
             -I$(MBED_DIR)/include -I$(MBED_DIR)
NET_DEF   := -DMBEDTLS_CONFIG_FILE='"vextro_config.h"'
NET_FLAGS := $(CFLAGS) $(NET_INC) $(NET_DEF)

LWIP_SRC  := $(shell find $(LWIP_DIR)/src -name '*.c' 2>/dev/null)
MBED_SRC  := $(wildcard $(MBED_DIR)/library/*.c)
TP_SRC    := $(LWIP_SRC) $(MBED_SRC) third_party/vxport.c
TP_OBJ    := $(patsubst third_party/%.c,build/tp/%.o,$(TP_SRC))

# lwIP and Mbed TLS are configured entirely by their headers, so a
# change to lwipopts.h or vextro_config.h changes what every one of
# these objects compiles to. Without naming them here, editing a buffer
# size rebuilds nothing and the old value stays linked -- which reads as
# "the tuning had no effect" and is really "the tuning was never built".
build/tp/%.o: third_party/%.c third_party/lwip-port/lwipopts.h \
              $(MBED_DIR)/vextro_config.h
	@mkdir -p $(dir $@)
	$(CC) $(NET_FLAGS) -w -c $< -o $@

# The glue is ours, so it is compiled the way the rest of the kernel is.
build/lwipglue.o: src/lwipglue.c src/vxport.h src/vxnet.h \
                  third_party/lwip-port/lwipopts.h \
                  third_party/lwip-port/arch/cc.h \
                  third_party/lwip-port/arch/sys_arch.h
	@mkdir -p build
	$(CC) $(NET_FLAGS) -c $< -o $@

build/tlsglue.o: src/tlsglue.c src/vxport.h src/vxnet.h \
                 $(MBED_DIR)/vextro_config.h $(MBED_DIR)/threading_alt.h
	@mkdir -p build
	$(CC) $(NET_FLAGS) -c $< -o $@

NET_OBJ := $(TP_OBJ) build/lwipglue.o build/tlsglue.o

# libgcc, and it is not a standard library.
#
# GCC does not promise to compile every C construct into instructions.
# Some of them -- 128-bit division being the one that turns up here --
# it compiles into a *call*, to a routine in libgcc, on the assumption
# that libgcc is always linked. -nostdlib turns that assumption off
# along with the C library, which is why this has to be named
# explicitly.
#
# Mbed TLS's bignum divides `unsigned __int128` by a 64-bit limb, which
# is __udivti3. Without this the link fails with an undefined reference
# in a file nobody has touched, and the obvious reading -- that
# something is missing from the vendored sources -- is the wrong one.
#
# It is part of the compiler, not the platform: it needs no operating
# system, calls nothing, and is exactly as freestanding as the code that
# calls it.
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

build/kernel: build/core/main.o $(KERN_MODULE_OBJ) build/llm.o \
              $(NET_OBJ) linker.ld
	$(LD) $(LDFLAGS) build/core/main.o $(KERN_MODULE_OBJ) build/llm.o \
	      $(NET_OBJ) $(LIBGCC) -o $@

# --- ISO root population ---
$(ISO)/boot/kernel: build/kernel
	@mkdir -p $(ISO)/boot/limine $(ISO)/EFI/BOOT
	cp $< $@

$(ISO)/boot/initrd.tar: build/initrd.tar
	@mkdir -p $(ISO)/boot
	cp $< $@

# Framebuffer mode.  `resolution` is the key Limine actually reads; the
# framebuffer_width/height/bpp trio that used to live in limine.conf is
# not part of the config format at all, so it was quietly ignored and the
# mode came from whatever the display's EDID preferred.  Any size up to
# the BUF_MAX_W x BUF_MAX_H back buffer in src/core/main.c works:
#     make run RES=1920x1080x32

# make compares timestamps, and a variable has none — without recording
# RES somewhere on disk, changing it would leave the previous mode baked
# into an ISO that looks up to date.
#
# EXTRA is recorded for the same reason and it bites harder, because the
# symptom is silence: `make iso EXTRA=-DSTORAGE_SELFTEST` after an
# ordinary build leaves the kernel objects untouched, produces an ISO with no
# self-test in it, and says nothing at all. The tests then "pass" by not
# running.
# EXTRA really is recorded here now. It was not: the recipe wrote RES and
# the framebuffer bounds and nothing else, so the failure described just
# above was live rather than guarded against -- `make iso
# EXTRA=-DSWAP_SELFTEST` after an ordinary build left the objects untouched,
# produced an ISO with no self-test in it, and said nothing. The test then
# "passed" by never running, which is the worst way for a test to pass.
build/res.stamp: FORCE
	@mkdir -p build
	@echo '$(RES) $(FB_MAX_W)x$(FB_MAX_H) $(EXTRA)' | cmp -s - $@ || \
	  echo '$(RES) $(FB_MAX_W)x$(FB_MAX_H) $(EXTRA)' > $@

$(ISO)/boot/limine/limine.conf: limine.conf build/res.stamp
	@mkdir -p $(ISO)/boot/limine
	sed 's|^\( *resolution:\).*|\1 $(RES)|' limine.conf > $@
	cp $(LIMINE)/limine-bios.sys       $(ISO)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin    $(ISO)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin    $(ISO)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI          $(ISO)/EFI/BOOT/

# --- ISO image (portable El Torito, xorriso/mkisofs compatible) ---
iso: os.iso

os.iso: build/limine-tool $(ISO)/boot/kernel $(ISO)/boot/initrd.tar $(ISO)/boot/limine/limine.conf
	xorriso -as mkisofs \
		-R -J \
		-V "VEXTRO_9" \
		-b boot/limine/limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot boot/limine/limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image \
		--protective-msdos-label \
		-o os.iso \
		$(ISO) 2>&1
	build/limine-tool bios-install os.iso

# --- Run ---
# Not every QEMU is built with the same display backends: Homebrew's
# macOS build ships Cocoa and no SDL, most Linux builds have GTK and SDL.
# Ask this one what it has rather than hard-coding a backend, and only
# pass sub-options the chosen backend actually accepts (grab-mod is
# SDL-only, and QEMU rejects the whole option if it does not know it).
#
# -cpu max is not cosmetic: it is what exposes AMD-V (svm) and nested
# paging (npt) to the guest, which the hypervisor in src/hyper.h needs.
# The default qemu64 model advertises neither, and Chamber then correctly
# reports that the processor has no virtualisation support.
#
# zoom-to-fit matters more than it sounds: without it, Cocoa and GTK draw
# the guest at 1:1 in the middle of the full-screen window and surround it
# with black, which looks exactly like a desktop that refuses to resize.
QEMU ?= qemu-system-x86_64

QEMU_DISPLAY := $(shell d=$$($(QEMU) -display help 2>/dev/null); \
  if   echo "$$d" | grep -qx sdl;   then echo 'sdl,show-cursor=off,grab-mod=lshift-lctrl-lalt'; \
  elif echo "$$d" | grep -qx gtk;   then echo 'gtk,show-cursor=off,grab-on-hover=on,zoom-to-fit=on'; \
  elif echo "$$d" | grep -qx cocoa; then echo 'cocoa,show-cursor=off,zoom-to-fit=on'; \
  else echo none; fi)

# (a shell `case` cannot be used here: the ")" in its patterns would
# close make's own $(shell ...) expansion early)
QEMU_FSKEY := $(if $(findstring cocoa,$(QEMU_DISPLAY)),Ctrl + Cmd + F,Ctrl + Alt + F)

run: os.iso disk.img
	@command -v $(QEMU) >/dev/null || { \
	    echo ""; \
	    echo "  $(QEMU) is not installed."; \
	    echo ""; \
	    echo "  It is only needed to run the system here. os.iso is"; \
	    echo "  already built and boots on real hardware, or in any"; \
	    echo "  other virtual machine."; \
	    echo ""; \
	    echo "    macOS:  brew install qemu"; \
	    echo "    Debian: sudo apt install qemu-system-x86"; \
	    echo ""; \
	    exit 1; }
	@echo ""
	@echo "  [TIP] Toggle full-screen on/off at any time with: $(QEMU_FSKEY)"
	@echo "  [TIP] The pointer is absolute — just move it, no click to grab."
	@echo ""
	$(QEMU) \
		-cdrom os.iso \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-m 2048M \
		-cpu max \
		$(QEMU_VGA) \
		-display $(QEMU_DISPLAY) \
		-full-screen \
		-boot d \
		-netdev user,id=net0,net=10.0.2.0/24 \
		-device e1000,netdev=net0

# keep the intermediates make would otherwise delete as chained targets
.PRECIOUS: build/store/%.o build/store/%.elf

clean:
	$(MAKE) -C vxfmt clean
	rm -rf build os.iso \
		$(ISO)/boot/kernel \
		$(ISO)/boot/initrd.tar \
		$(ISO)/boot/limine \
		$(ISO)/EFI \
