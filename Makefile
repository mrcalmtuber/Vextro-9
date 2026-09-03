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

# bison is checked by *version*, not by name, and it is the one tool
# here where that matters. libxkbcommon's parser is the only generated
# source in the tree that no tarball ships pre-made, and its very first
# directive -- `%define api.pure` -- is bison 2.3b syntax. macOS ships
# 2.3, which is on the PATH, answers `command -v`, and then stops with
# "syntax error, unexpected identifier, expecting string". Homebrew's
# bison is keg-only and deliberately does *not* shadow it, so BISON in
# the libxkbcommon section looks in the cellar first; this probe asks
# whichever one that resolves to what version it is.
BISON_OK := $(shell b=/opt/homebrew/opt/bison/bin/bison; \
              [ -x $$b ] || b=/usr/local/opt/bison/bin/bison; \
              [ -x $$b ] || b=`command -v bison 2>/dev/null`; \
              [ -n "$$b" ] && "$$b" --version 2>/dev/null | head -1 | \
                awk '{ split($$NF, v, "."); \
                       if (v[1] > 2 || (v[1] == 2 && v[2] >= 4)) print "y" }')
ifneq ($(BISON_OK),y)
MISSING := $(MISSING) bison>=2.4
endif

ifneq ($(MISSING),)
$(info )
$(info   Cannot build. Missing: $(MISSING))
$(info )
$(info   macOS:  brew install x86_64-elf-gcc x86_64-elf-binutils xorriso qemu bison)
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
             libc/wchar.c libc/calendar.c libc/process.c libc/dlfcn.c libc/err.c
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
	@mkdir -p build/scratch build/scratch/many
	@printf "vextro ntfs scratch\\n" > build/scratch/a.txt
	@printf "hello from a subdirectory\\n" > build/scratch/b.txt
	@head -c 300000 /dev/urandom > build/scratch/big.bin
	@rm -f build/scratch/ntfs.img build/scratch/tree.img
	@python3 tools/mkntfs.py build/scratch/ntfs.img 16 \
	         build/scratch/a.txt > /dev/null
	@# 400 names in one directory, which is past what a single index
	@# block holds at any name length -- so /many is a B-tree of at
	@# least two levels whose root node is separators rather than
	@# entries, and it is built by the *formatter*.
	@#
	@# That is a different code path from the one the kernel takes when
	@# it grows a directory, and only the kernel's had a test.
	@# tools/mkntfs.py wrote a single resident $$INDEX_ROOT and said in a
	@# comment that it "keeps every directory small enough that it does
	@# not have to" spill -- which stopped being true the first time a
	@# directory had two hundred names in it, and presented as
	@# `MFT record 104 overflows (12984 > 4096)` with no volume written.
	@i=0; while [ $$i -lt 400 ]; do \
	    printf 'entry %d\n' $$i > build/scratch/many/file-$$i.txt; \
	    i=$$((i + 1)); \
	done
	@python3 tools/mkntfs.py build/scratch/tree.img 2048 \
	         build/scratch/a.txt \
	         build/scratch/b.txt:docs/readme.txt \
	         build/scratch/big.bin:store/pkg/big.bin \
	         $$(for f in build/scratch/many/*.txt; do \
	                echo "$$f:many/$${f##*/}"; done) > /dev/null
	@./build/ntfs_test build/scratch/ntfs.img build/scratch/tree.img
	@python3 tools/ntfsdir.py build/scratch/tree.img /big > /dev/null && \
	 echo "  ok   an independent reader agrees about the B-tree"
	@# The same independent reader on the formatter's tree. It parses the
	@# image itself, so it is the half that would notice a tree only this
	@# formatter can read -- and it checks the order, because a
	@# mis-sorted B-tree is not a tree that loses names, it is one whose
	@# binary search walks past them.
	@n=`python3 tools/ntfsdir.py build/scratch/tree.img /many | \
	    sed -n 's/^.*: \([0-9]*\) entries.*/\1/p'`; \
	 o=`python3 tools/ntfsdir.py build/scratch/tree.img /many | \
	    grep -c ascending`; \
	 if [ "$$n" = "400" ] && [ "$$o" = "1" ]; then \
	     echo "  ok   a formatter-built B-tree: 400 names, in order"; \
	 else \
	     echo "  FAIL a formatter-built directory lost names ($$n of 400)"; \
	     exit 1; \
	 fi
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

# The keymap data's staging directory, hoisted here from the
# libxkbcommon block far below because `disk.img` names it as a
# *prerequisite*, and make expands a prerequisite list immediately --
# a variable defined later in the file is empty at that point, and the
# symptom is a rule that asks for `/.stamp`. Everything else about
# libxkbcommon stays together down there; only this one name has to be
# in scope before line 750.
XKBCONF_STAGE := build/xkbdata

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
# The keymap data is listed by *walking* build/xkbdata rather than by
# naming its files here: it is xkeyboard-config's tree, 251 files under
# four directories, and its shape is not this Makefile's to know. The
# walk is a shell loop for the same reason $big below is one -- the
# directory is produced by a download, so it does not exist when make
# parses this file and a $(wildcard) would quietly expand to nothing.
disk.img: $(ASSET_LIST) build/sqlseed.db | build/hello build/faulter build/mutextest build/threadtest build/wpetest build/fdprobe build/fdtest build/cxxtest build/sqltest build/fttest build/hbtest build/icutest build/vlstest build/jpegtest build/gltest build/gcrypttest build/tasn1test build/xkbtest build/xmltest build/zlibtest build/pngtest build/webptest build/pcre2test build/ffitest build/iconvtest build/zlibref.gz build/zlibbad.gz $(WINAPPS) $(STORE_BINS) $(PIC_SCI) $(MUSIC_WAV) $(MUSIC_FLAC) build/ca-bundle.crt $(XKBCONF_STAGE)/.stamp
	@set -e; \
	big=""; \
	for f in $(ASSET_FILES); do \
	    if [ -f "$$f" ]; then big="$$big $$f"; fi; \
	done; \
	xkb=""; \
	if [ -d $(XKBCONF_STAGE) ]; then \
	    for f in `cd $(XKBCONF_STAGE) && find . -type f ! -name .stamp | sed 's|^\./||'`; do \
	        xkb="$$xkb $(XKBCONF_STAGE)/$$f:etc/xkb/$$f"; \
	    done; \
	fi; \
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
		build/vlstest \
		build/jpegtest \
		build/gltest \
		build/gcrypttest \
		build/tasn1test \
		build/xkbtest \
		build/xmltest \
		build/zlibtest \
		build/pngtest \
		build/webptest \
		build/pcre2test \
		build/ffitest \
		build/iconvtest \
		build/zlibref.gz:zlibref.gz \
		build/zlibbad.gz:zlibbad.gz \
		assets/ComicNeue-Regular.ttf:ComicNeue-Regular.ttf \
		$(ICU_DATA):icudt74l.dat \
		build/sqlseed.db:sqlseed.db \
		$(foreach w,$(WINAPPS),$(w):$(notdir $(w))) \
		apps/welcome.txt:docs/welcome.txt \
		build/ca-bundle.crt:etc/ca-bundle.crt \
		$$big \
		$$xkb \
		$(foreach a,$(STORE_APPS),build/store/$(a).vx:store/pkg/$(a).vx) \
		$(foreach p,$(PIC_NAMES),build/pics/$(p).sci:pics/$(p).sci) \
		$(foreach t,$(MUSIC_NAMES),build/music/$(t).wav:music/$(t).wav) \
		$$(test -f build/music/bell.flac && \
		   echo build/music/bell.flac:music/bell.flac)"; \
	echo "python3 tools/mkntfs.py disk.img $(DISK_MB) ... `echo $$xkb | wc -w | tr -d ' '` files under etc/xkb"; \
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

# --- libjpeg-turbo 3.0.4 ---
#
# The library that is literally next. `OptionsWPE.cmake:10` is
# `find_package(JPEG REQUIRED)` — the third of the fifteen that file
# opens with — and it is
# where every configure run has stopped since ICU and HarfBuzz started
# being found.
#
# ---- why turbo rather than the IJG library ----
#
# Both satisfy FindJPEG, which greps jconfig.h and jpeglib.h for
# JPEG_LIB_VERSION and looks for an archive. They do not both satisfy
# WebKit's decoder: Source/WebCore/platform/image-decoders/jpeg/ uses
# JCS_EXT_RGBX and the other JCS_EXTENSIONS colour spaces, which are
# libjpeg-turbo's addition, and falls back to a slower per-pixel path
# without them. Choosing the one the consumer was written against is
# the point of porting it at all.
#
# ---- three precisions, and why the archive is built three times ----
#
# libjpeg-turbo 3 selects data precision at *run* time, and does it by
# compiling the same twenty-eight sources three times with
# BITS_IN_JSAMPLE at 8, 12 and 16, letting jmorecfg.h rename every
# symbol in the second and third passes. Skipping the 12- and 16-bit
# passes leaves jdmaster.c's dispatch calling functions that are not in
# the archive — an undefined symbol at the far end of a link rather than
# a missing feature. Upstream's CMakeLists builds them unconditionally
# and so does this.
#
# ---- and what is given up ----
#
# The SIMD, which is most of the reason the library is called turbo.
# third_party/libjpeg-port/jconfig.h says why at length: the assembly is
# driven by NASM through upstream's own CMake and selected at run time by
# CPUID and getenv, none of which is reachable from here. jsimd_none.c
# is the portable path upstream ships for exactly this case. Correctness
# is identical; throughput is not, and this file should not pretend
# otherwise.
JPEG_VERSION := 3.0.4
JPEG_TARBALL := libjpeg-turbo-$(JPEG_VERSION).tar.gz
JPEG_URL     := https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/$(JPEG_VERSION)/$(JPEG_TARBALL)
JPEG_SHA256  := 99130559e7d62e8d695f2c0eaeef912c5828d5b84a0537dcb24c9678c9d5b76b
JPEG_DIR     := third_party/libjpeg
JPEG_PORT    := third_party/libjpeg-port

# The three source lists, copied from upstream's CMakeLists.txt rather
# than globbed: a glob would sweep in cjpeg.c, djpeg.c and the fuzzers,
# each of which has a main().
JPEG16_NAMES := jcapistd.c jccolor.c jcdiffct.c jclossls.c jcmainct.c \
                jcprepct.c jcsample.c jdapistd.c jdcolor.c jddiffct.c \
                jdlossls.c jdmainct.c jdpostct.c jdsample.c jutils.c
JPEG12_NAMES := $(JPEG16_NAMES) jccoefct.c jcdctmgr.c jdcoefct.c \
                jddctmgr.c jdmerge.c jfdctfst.c jfdctint.c jidctflt.c \
                jidctfst.c jidctint.c jidctred.c jquant1.c jquant2.c
JPEG8_NAMES  := $(JPEG12_NAMES) jcapimin.c jchuff.c jcicc.c jcinit.c \
                jclhuff.c jcmarker.c jcmaster.c jcomapi.c jcparam.c \
                jcphuff.c jctrans.c jdapimin.c jdatadst.c jdatasrc.c \
                jdhuff.c jdicc.c jdinput.c jdlhuff.c jdmarker.c \
                jdmaster.c jdphuff.c jdtrans.c jerror.c jfdctflt.c \
                jmemmgr.c jmemnobs.c jpeg_nbits.c

JPEG8_OBJ  := $(addprefix build/jpeg/8/,$(JPEG8_NAMES:.c=.o))
JPEG12_OBJ := $(addprefix build/jpeg/12/,$(JPEG12_NAMES:.c=.o))
JPEG16_OBJ := $(addprefix build/jpeg/16/,$(JPEG16_NAMES:.c=.o))

# -I$(JPEG_PORT) comes first so the hand-written jconfig.h and
# jconfigint.h are found ahead of anything upstream's tree might have,
# which is the same ordering FreeType's redirected headers rely on.
#
# NO_GETENV and NO_PUTENV are upstream's own switches for a system
# without an environment, and this one has none: jmemmgr.c reads
# JPEGMEM to override the memory limit and is the only file that asks.
JPEG_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
               -DNO_GETENV -DNO_PUTENV \
               -I$(JPEG_PORT) -I$(JPEG_DIR) -Ilibc/include

build/jpeg/8/%.o: $(JPEG_DIR)/%.c $(JPEG_PORT)/jconfig.h $(JPEG_PORT)/jconfigint.h
	@mkdir -p build/jpeg/8
	$(CC) $(JPEG_CFLAGS) -c $< -o $@

build/jpeg/12/%.o: $(JPEG_DIR)/%.c $(JPEG_PORT)/jconfig.h $(JPEG_PORT)/jconfigint.h
	@mkdir -p build/jpeg/12
	$(CC) $(JPEG_CFLAGS) -DBITS_IN_JSAMPLE=12 -c $< -o $@

build/jpeg/16/%.o: $(JPEG_DIR)/%.c $(JPEG_PORT)/jconfig.h $(JPEG_PORT)/jconfigint.h
	@mkdir -p build/jpeg/16
	$(CC) $(JPEG_CFLAGS) -DBITS_IN_JSAMPLE=16 -c $< -o $@

build/libjpeg.a: $(JPEG8_OBJ) $(JPEG12_OBJ) $(JPEG16_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  JPEG   build/libjpeg.a ($(JPEG_VERSION), no SIMD, 8/12/16-bit)"

.PHONY: jpeg
jpeg: build/libjpeg.a

$(JPEG_DIR)/jpeglib.h:
	@echo "  fetching $(JPEG_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(JPEG_TARBALL) $(JPEG_URL)
	@printf '%s  build/%s\n' "$(JPEG_SHA256)" "build/$(JPEG_TARBALL)" \
		| sed 's| build/build/| build/|' \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(JPEG_TARBALL) does not match its checksum."; \
		     rm -f build/$(JPEG_TARBALL); exit 1; }
	@rm -rf $(JPEG_DIR)
	@mkdir -p $(JPEG_DIR)
	@tar -C $(JPEG_DIR) --strip-components=1 -xzf build/$(JPEG_TARBALL)
	@echo "  JPEG     $(JPEG_DIR) ($(JPEG_VERSION))"

# --- libepoxy 1.5.10 ---
#
# `OptionsWPE.cmake:11` — the frontier the moment JPEG was found, and the
# one dependency on the list whose *build* and whose *usefulness* are
# genuinely different questions.
#
# ---- what epoxy is ----
#
# A dispatcher, not an implementation. It resolves some two thousand
# OpenGL entry points by name at run time and calls through the pointer,
# which is why it builds perfectly well on a machine with no OpenGL: the
# generated dispatch table is portable C, and what decides the outcome is
# what dlsym finds when a program actually calls something.
#
# So this satisfies find_package(Epoxy 1.5.4) honestly — the archive is
# real, built from upstream's own generator over Khronos's registry — and
# third_party/libepoxy-port/vxgl.c is where the truth about what is
# behind it lives. Nine entry points are bound to the framebuffer at
# /dev/dri/renderD128; everything past clearing and reading a rectangle
# is absent from the table, so epoxy's own do_dlsym prints the name it
# could not find and aborts. That is upstream's behaviour, unmodified,
# and it names the exact call rather than drawing a black window.
#
# ---- the generated half ----
#
# Upstream generates gl_generated_dispatch.c and gl_generated.h from
# registry/gl.xml with a Python script at build time; meson does it and
# the tarball does not ship the result. The script is pure Python 3 and
# the XML is in the tarball, so it is run here into build/epoxy/ — which
# is also why the generated files are not in the repository: they are a
# derived artifact of two things that are.
#
# GLX, EGL, X11 and WGL are all off in
# third_party/libepoxy-port/config.h. Each is a binding to a window
# system this machine does not have, and leaving them on would compile
# three more dispatch tables whose every entry resolves to nothing —
# after failing on <X11/Xlib.h>, which is not in this tree.
EPOXY_VERSION := 1.5.10
EPOXY_TARBALL := libepoxy-$(EPOXY_VERSION).tar.gz
EPOXY_URL     := https://github.com/anholt/libepoxy/archive/refs/tags/$(EPOXY_VERSION).tar.gz
EPOXY_SHA256  := a7ced37f4102b745ac86d6a70a9da399cc139ff168ba6b8002b4d8d43c900c15
EPOXY_DIR     := third_party/libepoxy
EPOXY_PORT    := third_party/libepoxy-port
EPOXY_GEN     := build/epoxy

EPOXY_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
                -I$(EPOXY_PORT) -I$(EPOXY_DIR)/include \
                -I$(EPOXY_GEN)/include -I$(EPOXY_DIR)/src -Ilibc/include

# One rule for the generator, producing both halves at once. The header
# is listed as a sibling target so a parallel make does not run the
# script twice.
$(EPOXY_GEN)/src/gl_generated_dispatch.c $(EPOXY_GEN)/include/epoxy/gl_generated.h &: \
		$(EPOXY_DIR)/registry/gl.xml $(EPOXY_DIR)/src/gen_dispatch.py
	@mkdir -p $(EPOXY_GEN)/src $(EPOXY_GEN)/include/epoxy
	@python3 $(EPOXY_DIR)/src/gen_dispatch.py \
		--outputdir $(EPOXY_GEN) \
		--includedir $(EPOXY_GEN)/include/epoxy \
		--srcdir $(EPOXY_GEN)/src \
		$(EPOXY_DIR)/registry/gl.xml
	@echo "  GEN    $(EPOXY_GEN) (gl dispatch, from registry/gl.xml)"

build/epoxy/dispatch_common.o: $(EPOXY_DIR)/src/dispatch_common.c \
                               $(EPOXY_PORT)/config.h \
                               $(EPOXY_GEN)/include/epoxy/gl_generated.h
	@mkdir -p build/epoxy
	$(CC) $(EPOXY_CFLAGS) -c $< -o $@

build/epoxy/gl_generated_dispatch.o: $(EPOXY_GEN)/src/gl_generated_dispatch.c \
                                     $(EPOXY_PORT)/config.h
	@mkdir -p build/epoxy
	$(CC) $(EPOXY_CFLAGS) -c $< -o $@

build/epoxy/vxgl.o: $(EPOXY_PORT)/vxgl.c libc/include/dlfcn.h
	@mkdir -p build/epoxy
	$(CC) $(EPOXY_CFLAGS) -c $< -o $@

EPOXY_OBJ := build/epoxy/dispatch_common.o \
             build/epoxy/gl_generated_dispatch.o \
             build/epoxy/vxgl.o

build/libepoxy.a: $(EPOXY_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  EPOXY  build/libepoxy.a ($(EPOXY_VERSION), gl only, 9 entry points live)"

.PHONY: epoxy
epoxy: build/libepoxy.a

$(EPOXY_DIR)/src/dispatch_common.c:
	@echo "  fetching $(EPOXY_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(EPOXY_TARBALL) $(EPOXY_URL)
	@printf '%s  build/%s\n' "$(EPOXY_SHA256)" "$(EPOXY_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(EPOXY_TARBALL) does not match its checksum."; \
		     rm -f build/$(EPOXY_TARBALL); exit 1; }
	@rm -rf $(EPOXY_DIR)
	@mkdir -p $(EPOXY_DIR)
	@tar -C $(EPOXY_DIR) --strip-components=1 -xzf build/$(EPOXY_TARBALL)
	@echo "  EPOXY    $(EPOXY_DIR) ($(EPOXY_VERSION))"

# --- libgpg-error 1.50 and libgcrypt 1.10.3 ---
#
# `OptionsWPE.cmake:12`, and two libraries rather than one: libgcrypt's
# error type, its locks and its in-memory streams are all libgpg-error's,
# so there is no way to have the second without the first.
#
# ---- what is different about these two ----
#
# Every port before them shipped either a hand-editable configuration
# (FreeType, libjpeg) or a generator that runs standalone (libepoxy's
# Python). These are autotools, and autotools does not merely *write* a
# config.h — it writes several headers by running programs it has just
# compiled. libgpg-error needs four generated headers and two host tools
# to make them, and libgcrypt needs three more.
#
# All of it is reproduced below rather than checked in, because the
# inputs are in the tarballs and the outputs are derived. The one that
# matters is the errno table:
#
#   mkerrcodes.h is produced with the **cross** preprocessor, against
#   libc/include/errno.h, because the table maps this system's errno
#   values onto GPG_ERR_* names. Generated with the host's preprocessor
#   instead it would carry macOS's numbers — EOPNOTSUPP is 102 there and
#   95 here — and every gpg_error_from_errno would be quietly wrong. The
#   tool that consumes it is then compiled *for the host*, because it has
#   to run; its data is the cross compiler's.
#
# ---- and the arch flag that is not an extension ----
#
# libgcrypt is built with no CPU-extension assembly at all — no AES-NI,
# no AVX, no SHA-EXT — because those are selected at run time from CPUID
# through a hwfeatures layer whose .S files upstream's own build
# assembles. HAVE_CPU_ARCH_X86 *is* set, and the long note in
# third_party/libgcrypt-port/config.h explains why that is not a
# contradiction: it selects base `addq`/`adcq` in the bignum inline
# helpers, and without it a 64-bit build silently takes ec-inline.h's
# 32-bit limb path and fails to assemble four files away.
GPGERROR_VERSION := 1.50
GPGERROR_TARBALL := libgpg-error-$(GPGERROR_VERSION).tar.bz2
GPGERROR_URL     := https://gnupg.org/ftp/gcrypt/libgpg-error/$(GPGERROR_TARBALL)
GPGERROR_SHA256  := 69405349e0a633e444a28c5b35ce8f14484684518a508dc48a089992fe93e20a
GPGERROR_DIR     := third_party/libgpg-error
GPGERROR_PORT    := third_party/libgpg-error-port
GPGERROR_GEN     := build/gpgerror

GCRYPT_VERSION := 1.10.3
GCRYPT_TARBALL := libgcrypt-$(GCRYPT_VERSION).tar.bz2
GCRYPT_URL     := https://gnupg.org/ftp/gcrypt/libgcrypt/$(GCRYPT_TARBALL)
GCRYPT_SHA256  := 8b0870897ac5ac67ded568dcfadf45969cfa8a6beb0fd60af2a9eadc2a3272aa
GCRYPT_DIR     := third_party/libgcrypt
GCRYPT_PORT    := third_party/libgcrypt-port
GCRYPT_GEN     := build/gcrypt

# --- libgpg-error's generated headers ---
#
# err-codes.h and err-sources.h ship pre-generated in the tarball; these
# four do not.
$(GPGERROR_GEN)/code-to-errno.h: $(GPGERROR_DIR)/src/errnos.in \
                                 $(GPGERROR_DIR)/src/mkerrnos.awk
	@mkdir -p $(GPGERROR_GEN)
	@awk -f $(GPGERROR_DIR)/src/mkerrnos.awk $< > $@

# The one that has to see *our* errno.h. See the note above.
$(GPGERROR_GEN)/mkerrcodes.h: $(GPGERROR_DIR)/src/errnos.in \
                              libc/include/errno.h
	@mkdir -p $(GPGERROR_GEN)
	@awk -f $(GPGERROR_DIR)/src/mkerrcodes1.awk $< > $(GPGERROR_GEN)/_mkerrcodes.h
	@$(CC) -E -P -Ilibc/include $(GPGERROR_GEN)/_mkerrcodes.h 2>/dev/null \
	  | grep GPG_ERR_ \
	  | awk -f $(GPGERROR_DIR)/src/mkerrcodes.awk > $@
	@rm -f $(GPGERROR_GEN)/_mkerrcodes.h

# Compiled for the *host* because it has to run; fed the table above,
# which the cross compiler produced.
$(GPGERROR_GEN)/code-from-errno.h: $(GPGERROR_GEN)/mkerrcodes.h
	@cc -O1 -I$(GPGERROR_GEN) -I$(GPGERROR_DIR)/src \
	    -o $(GPGERROR_GEN)/mkerrcodes $(GPGERROR_DIR)/src/mkerrcodes.c
	@$(GPGERROR_GEN)/mkerrcodes \
	  | awk -f $(GPGERROR_DIR)/src/mkerrcodes2.awk > $@

# The public header, assembled by upstream's own mkheader from six
# inputs. `--cross x86_64-unknown-linux-gnu` picks the shipped
# lock-obj-pub for that triplet, whose forty reserved bytes comfortably
# hold this system's sixteen-byte pthread_mutex_t; nothing else in the
# header keys on the name.
$(GPGERROR_GEN)/gpg-error.h: $(GPGERROR_PORT)/config.h \
                             $(GPGERROR_DIR)/src/gpg-error.h.in \
                             $(GPGERROR_GEN)/code-from-errno.h \
                             $(GPGERROR_GEN)/code-to-errno.h
	@mkdir -p $(GPGERROR_GEN)
	@cp $(GPGERROR_PORT)/config.h $(GPGERROR_GEN)/config.h
	@cc -O1 -I$(GPGERROR_GEN) -I$(GPGERROR_DIR)/src \
	    -o $(GPGERROR_GEN)/mkheader $(GPGERROR_DIR)/src/mkheader.c
	@# Every path quoted, because this checkout lives in a directory with
	@# a space in its name and mkheader is run from another directory —
	@# the same hazard vextro-toolchain.cmake documents at length, in the
	@# one build step that has to cd.
	@cd $(GPGERROR_DIR)/src && "$(CURDIR)/$(GPGERROR_GEN)/mkheader" \
	    --cross x86_64-unknown-linux-gnu gpg-error.h.in \
	    "$(CURDIR)/$(GPGERROR_GEN)/config.h" 1.50 0x013200 \
	    > "$(CURDIR)/$@"
	@cp $@ $(GPGERROR_GEN)/gpgrt.h
	@echo "  GEN    $(GPGERROR_GEN)/gpg-error.h (errno table from libc/include/errno.h)"

GPGERROR_NAMES := posix-lock posix-thread spawn-posix init version estream \
                  estream-printf strsource strerror code-to-errno \
                  code-from-errno visibility sysutils stringutils \
                  syscall-clamp logging b64dec b64enc argparse
GPGERROR_OBJ := $(addprefix $(GPGERROR_GEN)/obj/,$(addsuffix .o,$(GPGERROR_NAMES)))

GPGERROR_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w -DHAVE_CONFIG_H \
                   -I$(GPGERROR_GEN) -I$(GPGERROR_DIR)/src -I$(GPGERROR_DIR) \
                   -Ilibc/include

$(GPGERROR_GEN)/obj/%.o: $(GPGERROR_DIR)/src/%.c $(GPGERROR_GEN)/gpg-error.h
	@mkdir -p $(GPGERROR_GEN)/obj
	$(CC) $(GPGERROR_CFLAGS) -c $< -o $@

build/libgpg-error.a: $(GPGERROR_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  GPGERR build/libgpg-error.a ($(GPGERROR_VERSION))"

# --- libgcrypt's generated headers ---
#
# gcrypt.h from the .in by substitution; gost-sb.h from a host tool that
# emits the GOST substitution boxes; mod-source-info.h names which bignum
# sources were used, which for this build is the portable C.
$(GCRYPT_GEN)/gost-sb.h: $(GCRYPT_DIR)/cipher/gost-s-box.c
	@mkdir -p $(GCRYPT_GEN)
	@cc -O1 -o $(GCRYPT_GEN)/gost-s-box $<
	@cd "$(GCRYPT_GEN)" && ./gost-s-box gost-sb.h

$(GCRYPT_GEN)/mod-source-info.h:
	@mkdir -p $(GCRYPT_GEN)
	@printf '/* Generated: this build uses the portable C bignum. */\nstatic char mod_source_info[] = "\\n * generic C (no assembly modules)";\n' > $@

# mpi.h says #include "../mpi/mpi-asm-defs.h", which upstream's configure
# creates by linking an architecture directory into place. Resolved here
# through the include path instead — -I$(GCRYPT_GEN) makes
# `../mpi/mpi-asm-defs.h` land on build/mpi/ — so nothing in the vendored
# tree is written to.
build/mpi/mpi-asm-defs.h: $(GCRYPT_DIR)/mpi/generic/mpi-asm-defs.h
	@mkdir -p build/mpi
	@cp $< $@

$(GCRYPT_GEN)/gcrypt.h: $(GCRYPT_PORT)/gcrypt.h
	@mkdir -p $(GCRYPT_GEN)
	@cp $< $@

GCRYPT_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
                 -D_GCRYPT_IN_LIBGCRYPT -DHAVE_CONFIG_H \
                 -I$(GCRYPT_GEN) -I$(GCRYPT_DIR)/src -I$(GCRYPT_DIR)/mpi \
                 -I$(GCRYPT_DIR)/mpi/generic -I$(GCRYPT_DIR) -Ilibc/include

# Every .c in cipher/ that is not written for a particular instruction
# set. The exclusions are by name because upstream's own file naming is
# the only marker: rijndael-aesni.c, sha256-avx2-bmi2-amd64.S and so on.
GCRYPT_CIPHER_SRC := $(shell ls $(GCRYPT_DIR)/cipher/*.c 2>/dev/null | \
    grep -vE '(armv|aarch64|-ppc|s390x|intel|amd64|avx|sse|ssse3|-x86|-arm|neon|aesni|padlock|riscv|-p10|vpmsum|vaes|gfni|-c-|ppc8)')
GCRYPT_MPI_NAMES := mpi-add mpi-bit mpi-cmp mpi-div mpi-gcd mpi-inline \
                    mpi-inv mpi-mul mpi-mod mpi-pow mpi-mpow mpi-scan \
                    mpicoder mpih-div mpih-mul mpih-const-time mpiutil \
                    ec ec-ed25519 ec-nist
GCRYPT_GEN_NAMES := mpih-add1 mpih-lshift mpih-mul1 mpih-mul2 mpih-mul3 \
                    mpih-rshift mpih-sub1 udiv-w-sdiv
GCRYPT_SRC_NAMES := visibility misc global sexp hwfeatures hwf-x86 stdmem \
                    secmem missing-string fips context const-time
GCRYPT_RND_NAMES := random random-csprng random-drbg random-system \
                    rndgetentropy rndhw rndjent

GCRYPT_OBJ := $(patsubst $(GCRYPT_DIR)/cipher/%.c,$(GCRYPT_GEN)/obj/c_%.o,$(GCRYPT_CIPHER_SRC)) \
              $(addprefix $(GCRYPT_GEN)/obj/m_,$(addsuffix .o,$(GCRYPT_MPI_NAMES))) \
              $(addprefix $(GCRYPT_GEN)/obj/g_,$(addsuffix .o,$(GCRYPT_GEN_NAMES))) \
              $(addprefix $(GCRYPT_GEN)/obj/s_,$(addsuffix .o,$(GCRYPT_SRC_NAMES))) \
              $(addprefix $(GCRYPT_GEN)/obj/r_,$(addsuffix .o,$(GCRYPT_RND_NAMES))) \
              $(GCRYPT_GEN)/obj/z_compat.o

GCRYPT_DEPS := $(GCRYPT_GEN)/config.h $(GCRYPT_GEN)/gcrypt.h \
               $(GCRYPT_GEN)/gost-sb.h $(GCRYPT_GEN)/mod-source-info.h \
               build/mpi/mpi-asm-defs.h $(GCRYPT_GEN)/gpg-error.h

# libgcrypt includes <gpg-error.h>, so the generated header is copied
# beside its own rather than reached with a second -I: the two libraries
# are staged into one include directory in the sysroot too, and a build
# that found it only through an extra flag would not match what a
# consumer sees.
$(GCRYPT_GEN)/gpg-error.h: $(GPGERROR_GEN)/gpg-error.h
	@mkdir -p $(GCRYPT_GEN)
	@cp $(GPGERROR_GEN)/gpg-error.h $@
	@cp $(GPGERROR_GEN)/gpgrt.h $(GCRYPT_GEN)/gpgrt.h

$(GCRYPT_GEN)/config.h: $(GCRYPT_PORT)/config.h
	@mkdir -p $(GCRYPT_GEN)
	@cp $< $@

$(GCRYPT_GEN)/obj/c_%.o: $(GCRYPT_DIR)/cipher/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@
$(GCRYPT_GEN)/obj/m_%.o: $(GCRYPT_DIR)/mpi/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@
$(GCRYPT_GEN)/obj/g_%.o: $(GCRYPT_DIR)/mpi/generic/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@
$(GCRYPT_GEN)/obj/s_%.o: $(GCRYPT_DIR)/src/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@
$(GCRYPT_GEN)/obj/r_%.o: $(GCRYPT_DIR)/random/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@
$(GCRYPT_GEN)/obj/z_%.o: $(GCRYPT_DIR)/compat/%.c $(GCRYPT_DEPS)
	@mkdir -p $(GCRYPT_GEN)/obj
	$(CC) $(GCRYPT_CFLAGS) -c $< -o $@

build/libgcrypt.a: $(GCRYPT_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  GCRYPT build/libgcrypt.a ($(GCRYPT_VERSION), portable C, no extension asm)"

.PHONY: gcrypt
gcrypt: build/libgcrypt.a build/libgpg-error.a

$(GPGERROR_DIR)/src/gpg-error.h.in:
	@echo "  fetching $(GPGERROR_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(GPGERROR_TARBALL) $(GPGERROR_URL)
	@printf '%s  build/%s\n' "$(GPGERROR_SHA256)" "$(GPGERROR_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(GPGERROR_TARBALL) does not match its checksum."; \
		     rm -f build/$(GPGERROR_TARBALL); exit 1; }
	@rm -rf $(GPGERROR_DIR)
	@mkdir -p $(GPGERROR_DIR)
	@tar -C $(GPGERROR_DIR) --strip-components=1 -xjf build/$(GPGERROR_TARBALL)

$(GCRYPT_DIR)/src/gcrypt.h.in:
	@echo "  fetching $(GCRYPT_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(GCRYPT_TARBALL) $(GCRYPT_URL)
	@printf '%s  build/%s\n' "$(GCRYPT_SHA256)" "$(GCRYPT_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(GCRYPT_TARBALL) does not match its checksum."; \
		     rm -f build/$(GCRYPT_TARBALL); exit 1; }
	@rm -rf $(GCRYPT_DIR)
	@mkdir -p $(GCRYPT_DIR)
	@tar -C $(GCRYPT_DIR) --strip-components=1 -xjf build/$(GCRYPT_TARBALL)

# --- libtasn1 4.19.0 ---
#
# `OptionsWPE.cmake:13`, and the smallest port in this file.
#
# libtasn1 is a DER encoder and decoder: it reads an ASN.1 module
# definition, builds a tree from it, and then encodes and decodes byte
# strings against that tree. WebKit uses it in exactly one place --
# Source/WebCore/PAL/pal/crypto/tasn1/Utilities.cpp -- to take WebCrypto
# keys apart and put them back together, through eight functions:
# asn1_array2tree, asn1_create_element, asn1_der_decoding2,
# asn1_read_value, asn1_read_value_type, asn1_der_coding,
# asn1_write_value and asn1_delete_structure. apps/tasn1test.c exercises
# those eight and nothing else, because that is the interface that has
# to work.
#
# ---- why this one is short ----
#
# Autotools again, and this time almost none of it matters. libgpg-error
# needed two host tools and four generated headers; libtasn1 needs
# neither. The bison parser is *shipped pre-generated* in the tarball as
# lib/ASN1.c, so there is no bison dependency, and the public header is
# shipped with its version numbers already substituted, so there is no
# mkheader step. What is left is a config.h, which is hand-written in
# third_party/libtasn1-port/ and explains itself.
#
# ---- the eleven objects, and the two that look optional ----
#
# Nine are upstream's lib_LTLIBRARIES sources. The other two come from
# lib/gl/, which is a three-module gnulib slice:
#
#   c-ctype.c      is not dead weight. Under the _GL_INLINE block that
#                  config.h reproduces, this compiler gives c-ctype.h's
#                  functions C99 `inline` semantics, which emit *no*
#                  out-of-line definition -- and c-ctype.c is the one
#                  translation unit that does, via _GL_EXTERN_INLINE. At
#                  -O2 every call inlines and the omission is invisible;
#                  it would surface later as an undefined c_isdigit from
#                  an -O0 build or a taken address. `nm` on the object
#                  shows fifteen T symbols, which is the check.
#   strverscmp.c   is the function this C library does not have, called
#                  by asn1_check_version. gnulib's implementation rather
#                  than one written here: it is upstream's own, it is
#                  already proven against this libc, and it keeps the
#                  LGPL code inside the vendored boundary.
#
# gl/unistd.c is the one gnulib source deliberately left out. It exists
# to house the inline functions of gnulib's *replacement* <unistd.h>,
# which this build does not use -- our own is on the include path -- so
# it would compile to `typedef int dummy;` and nothing else.
#
# ---- and the header this needed from libc ----
#
# parser_aux.c includes <limits.h> with the comment `/`* WORD_BIT *`/`
# beside it and hashes every node name with `h >> (WORD_BIT - 9)`.
# WORD_BIT was not in libc/include/limits.h; it is now, beside LONG_BIT,
# which is where POSIX and musl both put them.
TASN1_VERSION := 4.19.0
TASN1_TARBALL := libtasn1-$(TASN1_VERSION).tar.gz
TASN1_URL     := https://ftp.gnu.org/gnu/libtasn1/$(TASN1_TARBALL)
TASN1_SHA256  := 1613f0ac1cf484d6ec0ce3b8c06d56263cc7242f1c23b30d82d23de345a63f7a
TASN1_DIR     := third_party/libtasn1
TASN1_PORT    := third_party/libtasn1-port
TASN1_GEN     := build/tasn1

TASN1_LIB_NAMES := ASN1 coding decoding element errors gstr parser_aux \
                   structure version
TASN1_GL_NAMES  := c-ctype strverscmp

TASN1_OBJ := $(addprefix $(TASN1_GEN)/obj/l_,$(addsuffix .o,$(TASN1_LIB_NAMES))) \
             $(addprefix $(TASN1_GEN)/obj/g_,$(addsuffix .o,$(TASN1_GL_NAMES)))

# -DASN1_BUILDING is upstream's own AM_CPPFLAGS. With HAVE_VISIBILITY
# left undefined it does not change what ASN1_API expands to -- both
# arms reach the empty spelling, which is the right one for a static
# archive -- but it is what includes/libtasn1.h keys on to know it is
# being read by the library rather than by a consumer, and a port that
# passed only the flags that happen to matter today would be one more
# thing to rediscover.
TASN1_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
                -DHAVE_CONFIG_H -DASN1_BUILDING \
                -I$(TASN1_GEN) -I$(TASN1_DIR)/lib \
                -I$(TASN1_DIR)/lib/includes -I$(TASN1_DIR)/lib/gl \
                -Ilibc/include

$(TASN1_GEN)/config.h: $(TASN1_PORT)/config.h
	@mkdir -p $(TASN1_GEN)
	@cp $< $@

$(TASN1_GEN)/obj/l_%.o: $(TASN1_DIR)/lib/%.c $(TASN1_GEN)/config.h
	@mkdir -p $(TASN1_GEN)/obj
	$(CC) $(TASN1_CFLAGS) -c $< -o $@
$(TASN1_GEN)/obj/g_%.o: $(TASN1_DIR)/lib/gl/%.c $(TASN1_GEN)/config.h
	@mkdir -p $(TASN1_GEN)/obj
	$(CC) $(TASN1_CFLAGS) -c $< -o $@

build/libtasn1.a: $(TASN1_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  TASN1  build/libtasn1.a ($(TASN1_VERSION))"

.PHONY: tasn1
tasn1: build/libtasn1.a

$(TASN1_DIR)/lib/includes/libtasn1.h:
	@echo "  fetching $(TASN1_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(TASN1_TARBALL) $(TASN1_URL)
	@printf '%s  build/%s\n' "$(TASN1_SHA256)" "$(TASN1_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(TASN1_TARBALL) does not match its checksum."; \
		     rm -f build/$(TASN1_TARBALL); exit 1; }
	@rm -rf $(TASN1_DIR)
	@mkdir -p $(TASN1_DIR)
	@tar -C $(TASN1_DIR) --strip-components=1 -xzf build/$(TASN1_TARBALL)

# --- libxkbcommon 1.7.0, and the keymap data it reads ---
#
# `OptionsWPE.cmake:14`, and the first port here that is a library *and*
# a body of data. Both halves are needed and the second is the larger:
# libxkbcommon does not contain a keyboard layout, it contains a compiler
# for the files that describe one, and those files are xkeyboard-config's.
#
# ---- what WebKit does with it ----
#
# One call, in Source/WebKit/WPEPlatform/wpe/WPEKeymapXKB.cpp:180:
#
#     struct xkb_rule_names names = { "evdev", "pc105", "us", "", "" };
#     xkb_keymap_new_from_names(context, &names, ...)
#
# Those five strings are Rules, Model, Layout, Variant and Options, and
# resolving them means *reading files*: rules/evdev is a table that maps
# the five onto a keycodes file, a types file, a compat file and a
# symbols expression, each of which is then found by name under the
# config root. So a build that staged only the archive would produce a
# browser whose keymap is NULL. apps/xkbtest.c makes that exact call.
#
# ---- three host tools, and one of them is not on the PATH ----
#
# **bison.** src/xkbcomp/parser.y is the one generated source that does
# *not* ship pre-made in this tarball (ks_tables.h and keywords.c both
# do), so the parser is generated at build time. macOS's own bison is
# 2.3 and libxkbcommon wants >= 2.3a, which is not a formality: the very
# first directive, `%define api.pure`, is 2.3b syntax and 2.3 stops on it
# with "syntax error, unexpected identifier, expecting string". Homebrew's
# bison 3.8.2 is keg-only -- installing it does **not** change what
# `bison` means on the PATH -- so BISON below looks in the cellar first
# and falls back to the PATH, and the toolchain check at the top of this
# file tests the version rather than the name. Getting that wrong builds
# here and fails on the next clean machine with the error above.
#
# **python3**, already required by tools/mkntfs.py, to run
# xkeyboard-config's own merge.py and map-variants.py.
#
# **pkg-config**, which is new and belongs to the `webkit` target rather
# than to this one. FindLibxkbcommon.cmake is four lines long and has no
# find_path/find_library fallback at all -- `pkg_check_modules(
# LIBXKBCOMMON xkbcommon)` is the whole module -- so unlike every
# package found so far, this one genuinely cannot be discovered without
# it. The .pc file must be named xkbcommon.pc, after the module name in
# that call, not after the library.
XKB_VERSION := 1.7.0
XKB_TARBALL := libxkbcommon-$(XKB_VERSION).tar.xz
XKB_URL     := https://xkbcommon.org/download/$(XKB_TARBALL)
XKB_SHA256  := 65782f0a10a4b455af9c6baab7040e2f537520caa2ec2092805cdfd36863b247
XKB_DIR     := third_party/libxkbcommon
XKB_PORT    := third_party/libxkbcommon-port
XKB_GEN     := build/xkb

XKBCONF_VERSION := 2.41
XKBCONF_TARBALL := xkeyboard-config-$(XKBCONF_VERSION).tar.xz
XKBCONF_URL     := https://www.x.org/archive/individual/data/xkeyboard-config/$(XKBCONF_TARBALL)
XKBCONF_SHA256  := f02cd6b957295e0d50236a3db15825256c92f67ef1f73bf1c77a4b179edf728f
XKBCONF_DIR     := third_party/xkeyboard-config
# XKBCONF_STAGE is defined near ASSET_LIST; see the note there.

# Homebrew's bison is keg-only by design -- macOS ships its own and brew
# refuses to shadow it -- so the cellar path is tried first. See the note
# above for what happens with the 2.3 that would otherwise answer.
BISON := $(shell if [ -x /opt/homebrew/opt/bison/bin/bison ]; then \
                     echo /opt/homebrew/opt/bison/bin/bison; \
                 elif [ -x /usr/local/opt/bison/bin/bison ]; then \
                     echo /usr/local/opt/bison/bin/bison; \
                 else echo bison; fi)

# The parser, generated into build/ rather than committed: it is derived
# from parser.y in the vendored tree, which is the same rule libepoxy's
# gl_generated.h is under.
#
# `-p _xkbcommon_` is upstream's (meson.build:163) and is load-bearing
# rather than cosmetic. It renames every yy* symbol bison emits, so
# yyparse becomes _xkbcommon_parse and yylex becomes _xkbcommon_lex --
# which are the names src/xkbcomp/parser-priv.h *declares* and the names
# scanner.c and parser.y themselves define. Generated without it, the
# parser compiles against nothing: `implicit declaration of yylex`,
# `implicit declaration of yyerror`, and a library that would have
# exported a generic `yyparse` into every program that linked it.
#
# `-d` writes parser.h beside it, which parser-priv.h includes by plain
# name and reaches through -I$(XKB_GEN).
$(XKB_GEN)/parser.c: $(XKB_DIR)/src/xkbcomp/parser.y
	@mkdir -p $(XKB_GEN)
	@$(BISON) -p _xkbcommon_ -d -o $@ $<
	@echo "  GEN    $(XKB_GEN)/parser.c ($$($(BISON) --version | head -1))"

$(XKB_GEN)/config.h: $(XKB_PORT)/config.h
	@mkdir -p $(XKB_GEN)
	@cp $< $@

# Upstream's meson libxkbcommon_sources, minus the headers. Split by
# directory because there are two keymap.c and two parser.c in it --
# src/keymap.c and src/xkbcomp/keymap.c are different files with
# different jobs, and one object directory with no prefixes would have
# silently kept whichever was compiled last.
#
# Not included, and deliberately: src/registry.c and src/util-list.c are
# libxkbregistry, a separate library for *listing* available layouts that
# needs libxml2 and that WebKit does not link; src/x11/ needs xcb.
XKB_SRC_NAMES     := atom context context-priv keysym keysym-utf keymap \
                     keymap-priv state text utf8 utils
XKB_COMP_NAMES    := action ast-build compat expr include keycodes keymap \
                     keymap-dump keywords rules scanner symbols types vmod \
                     xkbcomp
XKB_COMPOSE_NAMES := parser paths state table

XKB_OBJ := $(addprefix $(XKB_GEN)/obj/s_,$(addsuffix .o,$(XKB_SRC_NAMES))) \
           $(addprefix $(XKB_GEN)/obj/c_,$(addsuffix .o,$(XKB_COMP_NAMES))) \
           $(addprefix $(XKB_GEN)/obj/p_,$(addsuffix .o,$(XKB_COMPOSE_NAMES))) \
           $(XKB_GEN)/obj/y_parser.o

XKB_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
              -DHAVE_CONFIG_H -I$(XKB_GEN) -I$(XKB_DIR)/src \
              -I$(XKB_DIR)/include -I$(XKB_DIR) -Ilibc/include

XKB_DEPS := $(XKB_GEN)/config.h $(XKB_GEN)/parser.c

$(XKB_GEN)/obj/s_%.o: $(XKB_DIR)/src/%.c $(XKB_DEPS)
	@mkdir -p $(XKB_GEN)/obj
	$(CC) $(XKB_CFLAGS) -c $< -o $@
$(XKB_GEN)/obj/c_%.o: $(XKB_DIR)/src/xkbcomp/%.c $(XKB_DEPS)
	@mkdir -p $(XKB_GEN)/obj
	$(CC) $(XKB_CFLAGS) -c $< -o $@
$(XKB_GEN)/obj/p_%.o: $(XKB_DIR)/src/compose/%.c $(XKB_DEPS)
	@mkdir -p $(XKB_GEN)/obj
	$(CC) $(XKB_CFLAGS) -c $< -o $@
# The generated parser is compiled from build/, and needs the vendored
# xkbcomp directory on the include path because it includes its own
# parser-priv.h and ast-build.h by relative name.
$(XKB_GEN)/obj/y_parser.o: $(XKB_GEN)/parser.c $(XKB_GEN)/config.h
	@mkdir -p $(XKB_GEN)/obj
	$(CC) $(XKB_CFLAGS) -I$(XKB_DIR)/src/xkbcomp -c $< -o $@

build/libxkbcommon.a: $(XKB_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  XKB    build/libxkbcommon.a ($(XKB_VERSION), no mmap, no x11)"

.PHONY: xkbcommon
xkbcommon: build/libxkbcommon.a

# --- the keymap data, assembled the way xkeyboard-config assembles it ---
#
# rules/evdev does not ship. It is *merged* at build time from 44 parts:
# 28 numbered fragments, 6 legacy-compatibility fragments, and 10 more
# generated by compat/map-variants.py out of two mapping tables. The
# order is by filename, which is why every fragment is numbered, and
# merge.py additionally groups them under their `! model layout = ...`
# section headers and emits each header once.
#
# All of that is upstream's, run here rather than reimplemented: the two
# scripts are python3 and are in the tarball. Reproducing the merge by
# hand would be a second implementation of a file format whose only
# consumer is the library being ported.
#
# The legacy-compatibility parts are included -- meson's `compat-rules`
# option, which defaults to true -- because the symbols tree that ships
# beside them is the complete one. A rules file without them next to 195
# symbols files is an inconsistent volume: the old layout names would
# resolve to nothing and the failure would look like a broken keymap
# rather than a missing table.
XKBCONF_PARTS := 0000-hdr.part 0001-lists.part 0002-RS.lists.part \
                 0004-RS.m_k.part 0005-l1_k.part 0006-l_k.part \
                 0007-o_k.part 0008-ml_g.part 0009-m_g.part \
                 0011-mlv_s.part 0013-ml_s.part 0015-ml1_s.part \
                 0018-ml2_s.part 0020-ml3_s.part 0022-ml4_s.part \
                 0026-RS.m_s.part 0027-RS.ml_s1.part 0033-ml_c.part \
                 0034-ml1_c.part 0035-m_t.part 0036-lo_s.part \
                 0037-l1o_s.part 0038-l2o_s.part 0039-l3o_s.part \
                 0040-l4o_s.part 0042-o_s.part 0043-o_c.part \
                 0044-o_t.part compat/0028-lv_c.part \
                 compat/0029-l1v1_c.part compat/0030-l2v2_c.part \
                 compat/0031-l3v3_c.part compat/0032-l4v4_c.part \
                 compat/0041-o_s.part

# level : the ml_s part it generates : the mlv_s part it generates
XKBCONF_GEN := 0:0012-ml_s.part:0010-mlv_s.part \
               1:0014-ml1_s.part:0016-ml1v1_s.part \
               2:0017-ml2_s.part:0023-ml2v2_s.part \
               3:0019-ml3_s.part:0024-ml3v3_s.part \
               4:0021-ml4_s.part:0025-ml4v4_s.part

$(XKBCONF_STAGE)/.stamp: $(XKBCONF_DIR)/rules/merge.py
	@rm -rf $(XKBCONF_STAGE)
	@mkdir -p $(XKBCONF_STAGE)/rules
	@set -e; \
	R="$(CURDIR)/$(XKBCONF_DIR)/rules"; \
	O="$(CURDIR)/$(XKBCONF_STAGE)"; \
	for ruleset in base evdev; do \
	    P="$$O/parts-$$ruleset"; \
	    mkdir -p "$$P"; \
	    for part in $(XKBCONF_PARTS); do \
	        src=`echo "$$part" | sed "s/RS/$$ruleset/"`; \
	        cp "$$R/$$src" "$$P/`basename $$src`.$$ruleset"; \
	    done; \
	    for spec in $(XKBCONF_GEN); do \
	        n=`echo $$spec | cut -d: -f1`; \
	        a=`echo $$spec | cut -d: -f2`; \
	        b=`echo $$spec | cut -d: -f3`; \
	        (cd "$$R" && python3 compat/map-variants.py --want=mls \
	            --number=$$n "$$P/$$a" compat/layoutsMapping.lst \
	            compat/variantsMapping.lst); \
	        (cd "$$R" && python3 compat/map-variants.py --want=mlvs \
	            --number=$$n "$$P/$$b" compat/variantsMapping.lst); \
	    done; \
	    (cd "$$R" && python3 merge.py "$$P"/*) > "$$O/rules/$$ruleset"; \
	    rm -rf "$$P"; \
	done
	@# The four directories that ship as-is. geometry/ is not among them
	@# and is not an oversight: libxkbcommon parses geometry sections and
	@# then discards them -- it has no notion of where a key physically
	@# is -- so the 1.4 MB would be read and thrown away. The .xml files
	@# are skipped for the same kind of reason: they describe the layout
	@# list for a settings UI and are read only by libxkbregistry, which
	@# is not built here.
	@for d in keycodes types compat symbols; do \
	    mkdir -p $(XKBCONF_STAGE)/$$d; \
	    (cd $(XKBCONF_DIR)/$$d && find . -type f ! -name '*.xml' \
	        ! -name 'meson.build' -print) | while read f; do \
	        mkdir -p "$(XKBCONF_STAGE)/$$d/`dirname $$f`"; \
	        cp "$(XKBCONF_DIR)/$$d/$$f" "$(XKBCONF_STAGE)/$$d/$$f"; \
	    done; \
	done
	@touch $@
	@echo "  XKBDATA $(XKBCONF_STAGE) ($(XKBCONF_VERSION), `find $(XKBCONF_STAGE) -type f | wc -l | tr -d ' '` files, rules merged from 44 parts)"

.PHONY: xkbdata
xkbdata: $(XKBCONF_STAGE)/.stamp

$(XKB_DIR)/src/xkbcomp/parser.y:
	@echo "  fetching $(XKB_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(XKB_TARBALL) $(XKB_URL)
	@printf '%s  build/%s\n' "$(XKB_SHA256)" "$(XKB_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(XKB_TARBALL) does not match its checksum."; \
		     rm -f build/$(XKB_TARBALL); exit 1; }
	@rm -rf $(XKB_DIR)
	@mkdir -p $(XKB_DIR)
	@tar -C $(XKB_DIR) --strip-components=1 -xJf build/$(XKB_TARBALL)

$(XKBCONF_DIR)/rules/merge.py:
	@echo "  fetching $(XKBCONF_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(XKBCONF_TARBALL) $(XKBCONF_URL)
	@printf '%s  build/%s\n' "$(XKBCONF_SHA256)" "$(XKBCONF_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(XKBCONF_TARBALL) does not match its checksum."; \
		     rm -f build/$(XKBCONF_TARBALL); exit 1; }
	@rm -rf $(XKBCONF_DIR)
	@mkdir -p $(XKBCONF_DIR)
	@tar -C $(XKBCONF_DIR) --strip-components=1 -xJf build/$(XKBCONF_TARBALL)

# --- libxml2 2.12.6 ---
#
# `OptionsWPE.cmake:15`, and found by **CMake's own FindLibXml2** rather
# than by a module WebKit ships -- the first package here in that
# position. It matters in one place: that module's REQUIRED_VARS are
# LIBXML2_LIBRARY and LIBXML2_INCLUDE_DIR, and it takes the version by
# regex out of `libxml/xmlversion.h`, so the header staged into the
# sysroot is what answers `find_package(LibXml2 2.8.0 REQUIRED)`.
#
# ---- one library, two configuration files ----
#
# Almost nothing about what libxml2 *does* is decided in a config.h.
# Every one of the 47 sources is compiled unconditionally and the
# feature set is applied from inside, through macros in
# `include/libxml/xmlversion.h` -- a generated header, and the reason
# this section is longer than libtasn1's whole port.
#
# So there are two halves. `third_party/libxml2-port/config.h` answers
# what the C files ask the *platform*, and is hand-written like every
# other port's. The list below answers what they ask the *build*, and is
# substituted into upstream's own `xmlversion.h.in` -- 37 tokens, each
# of which lands in a `#if @WITH_X@` that either defines a
# LIBXML_X_ENABLED or does not.
#
# Substituting rather than hand-writing the header is deliberate:
# xmlversion.h is 500 lines of interlocking `#if`s -- XPointer requires
# XPath, the writer requires output, the reader requires push -- and a
# hand-written copy would be a second implementation of upstream's
# dependency rules that stops matching at the next release.
XML2_VERSION := 2.12.6
XML2_VERNUM  := 21206
XML2_TARBALL := libxml2-$(XML2_VERSION).tar.xz
XML2_URL     := https://download.gnome.org/sources/libxml2/2.12/$(XML2_TARBALL)
XML2_SHA256  := 889c593a881a3db5fdd96cc9318c87df34eb648edfc458272ad46fd607353fbb
XML2_DIR     := third_party/libxml2
XML2_PORT    := third_party/libxml2-port
XML2_GEN     := build/xml2

# The feature set, as sed expressions over xmlversion.h.in.
#
# On, and upstream's defaults: the parser and tree APIs, push, SAX1 and
# SAX2, XPath, XPointer, XInclude, the reader and writer, pattern,
# regexps, RelaxNG/Schemas, Schematron, C14N, catalog, HTML, valid,
# output, debug, threads, ISO8859X.
#
# Off, in three groups, and the long note in
# third_party/libxml2-port/config.h says which is which:
#   absent here    ZLIB LZMA MODULES ICONV
#   present and    ICU -- ported, staged, and deliberately not wired,
#   not wired      because WebKit converts before the parser sees a byte
#   unreachable    FTP HTTP -- WebKit installs its own entity loader so
#                  libxml2 can never fetch, which is the XXE defence
# plus the ones upstream itself defaults off: LEGACY, MEM_DEBUG,
# THREAD_ALLOC, TRIO, XPTR_LOCS.
XML2_SUBST := \
  -e 's/@VERSION@/$(XML2_VERSION)/g' \
  -e 's/@LIBXML_VERSION_NUMBER@/$(XML2_VERNUM)/g' \
  -e 's/@LIBXML_VERSION_EXTRA@//g' \
  -e 's/@MODULE_EXTENSION@/.so/g' \
  -e 's/@WITH_TREE@/1/g'      -e 's/@WITH_OUTPUT@/1/g' \
  -e 's/@WITH_PUSH@/1/g'      -e 's/@WITH_READER@/1/g' \
  -e 's/@WITH_WRITER@/1/g'    -e 's/@WITH_PATTERN@/1/g' \
  -e 's/@WITH_SAX1@/1/g'      -e 's/@WITH_VALID@/1/g' \
  -e 's/@WITH_HTML@/1/g'      -e 's/@WITH_CATALOG@/1/g' \
  -e 's/@WITH_XPATH@/1/g'     -e 's/@WITH_XPTR@/1/g' \
  -e 's/@WITH_XINCLUDE@/1/g'  -e 's/@WITH_C14N@/1/g' \
  -e 's/@WITH_REGEXPS@/1/g'   -e 's/@WITH_SCHEMAS@/1/g' \
  -e 's/@WITH_SCHEMATRON@/1/g' -e 's/@WITH_DEBUG@/1/g' \
  -e 's/@WITH_THREADS@/1/g'   -e 's/@WITH_ISO8859X@/1/g' \
  -e 's/@WITH_FTP@/0/g'       -e 's/@WITH_HTTP@/0/g' \
  -e 's/@WITH_ICONV@/0/g'     -e 's/@WITH_ICU@/0/g' \
  -e 's/@WITH_ZLIB@/0/g'      -e 's/@WITH_LZMA@/0/g' \
  -e 's/@WITH_MODULES@/0/g'   -e 's/@WITH_LEGACY@/0/g' \
  -e 's/@WITH_MEM_DEBUG@/0/g' -e 's/@WITH_THREAD_ALLOC@/0/g' \
  -e 's/@WITH_TRIO@/0/g'      -e 's/@WITH_XPTR_LOCS@/0/g'

# Generated into the layout a consumer sees rather than into a flat
# directory: every public header includes <libxml/xmlversion.h>, so the
# generated one has to sit beside the shipped ones under a `libxml/`
# component. build/xml2/include is that root, and it is also what gets
# copied into the sysroot -- so the headers the library is compiled
# against and the headers WebKit will be compiled against are the same
# files.
$(XML2_GEN)/include/libxml/xmlversion.h: $(XML2_DIR)/include/libxml/xmlversion.h.in
	@mkdir -p $(XML2_GEN)/include/libxml
	@sed $(XML2_SUBST) $< > $@
	@echo "  GEN    $@ ($(XML2_VERSION), no icu/iconv/zlib/http)"

$(XML2_GEN)/config.h: $(XML2_PORT)/config.h
	@mkdir -p $(XML2_GEN)
	@cp $< $@

# All 47, exactly as upstream's LIBXML2_SRCS lists them. The ones whose
# feature is off -- nanoftp, nanohttp, xmlmodule, xzlib -- compile to
# empty objects rather than being dropped, because the list is
# upstream's and a build that curated it would have to be re-curated
# every release.
XML2_NAMES := buf c14n catalog chvalid debugXML dict encoding entities \
              error globals hash HTMLparser HTMLtree legacy list \
              nanoftp nanohttp parser parserInternals pattern relaxng \
              SAX SAX2 schematron threads tree uri valid xinclude xlink \
              xmlIO xmlmemory xmlmodule xmlreader xmlregexp xmlsave \
              xmlschemas xmlschemastypes xmlstring xmlunicode xmlwriter \
              xpath xpointer xzlib

XML2_OBJ := $(addprefix $(XML2_GEN)/obj/,$(addsuffix .o,$(XML2_NAMES)))

# -I$(XML2_GEN)/include comes *before* the vendored include directory so
# that the generated xmlversion.h is the one found; the vendored tree
# has only the .in beside it, so this is belt and braces rather than a
# shadowing trick.
XML2_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
               -DHAVE_CONFIG_H -D_REENTRANT -DSYSCONFDIR='"/etc"' \
               -I$(XML2_GEN) -I$(XML2_GEN)/include \
               -I$(XML2_DIR)/include -I$(XML2_DIR) -Ilibc/include

XML2_DEPS := $(XML2_GEN)/config.h $(XML2_GEN)/include/libxml/xmlversion.h

$(XML2_GEN)/obj/%.o: $(XML2_DIR)/%.c $(XML2_DEPS)
	@mkdir -p $(XML2_GEN)/obj
	$(CC) $(XML2_CFLAGS) -c $< -o $@

build/libxml2.a: $(XML2_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  XML2   build/libxml2.a ($(XML2_VERSION))"

.PHONY: xml2
xml2: build/libxml2.a

$(XML2_DIR)/include/libxml/xmlversion.h.in:
	@echo "  fetching $(XML2_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(XML2_TARBALL) $(XML2_URL)
	@printf '%s  build/%s\n' "$(XML2_SHA256)" "$(XML2_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(XML2_TARBALL) does not match its checksum."; \
		     rm -f build/$(XML2_TARBALL); exit 1; }
	@rm -rf $(XML2_DIR)
	@mkdir -p $(XML2_DIR)
	@tar -C $(XML2_DIR) --strip-components=1 -xJf build/$(XML2_TARBALL)

# --- zlib 1.3.1 ---
#
# `OptionsWPE.cmake:23` in its own right, and `OptionsWPE.cmake:16`
# before that: CMake's FindPNG opens with `find_package(ZLIB)` and does
# nothing else if that fails, so the PNG line reports two missing
# packages and this is the first of them.
#
# Found by CMake's own FindZLIB, which is the *only* find module reached
# so far that never consults pkg-config at all: it wants `zlib.h` on the
# include path and a library called `z`, and it takes the version by
# regex from `#define ZLIB_VERSION` in the staged header.
#
# ---- the one configuration decision, and where it has to be made ----
#
# zlib has no config.h. What it has is two `#ifdef`s at the top of
# zconf.h -- HAVE_UNISTD_H and HAVE_STDARG_H, each with the comment "may
# be set to #if 1 by ./configure" -- and between them they decide whether
# <unistd.h> is included and whether z_off_t is off_t or long.
#
# Both are true here. What is awkward is that they have to be answered
# *twice*, in two different ways, because of how zlib includes its own
# header:
#
#   For the archive, on the command line. zlib.h says `#include
#   "zconf.h"`, and a quoted include is resolved from the includer's own
#   directory first -- so a generated zconf.h anywhere else is silently
#   ignored while zlib's own sources are compiled, whatever -I says.
#   -DHAVE_UNISTD_H -DHAVE_STDARG_H reaches the vendored header instead.
#
#   For the consumer, by generating an edited copy. WebKit will include
#   <zlib.h> from the sysroot with neither define on its command line, so
#   the *staged* zconf.h has to carry the answers itself or it would
#   describe a different library from the archive beside it -- the same
#   hazard the FreeType and JPEG staging notes are about. The two sed
#   expressions below are upstream's own, lifted from configure:568-601,
#   so the generated file is the file ./configure would have written.
#
# The two routes are equivalent by construction: `#ifdef HAVE_UNISTD_H`
# with -D on the command line and `#if 1` in the header both end at
# `#define Z_HAVE_UNISTD_H`, which is the only thing either one does.
ZLIB_VERSION := 1.3.1
ZLIB_TARBALL := zlib-$(ZLIB_VERSION).tar.gz
ZLIB_URL     := https://zlib.net/fossils/$(ZLIB_TARBALL)
ZLIB_SHA256  := 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
ZLIB_DIR     := third_party/zlib
ZLIB_GEN     := build/zlib

# The generated pair, in the layout a consumer sees. libpng below is
# compiled with -I$(ZLIB_GEN)/include for exactly this reason: it must
# see the same zconf.h WebKit will, not the unedited one in the vendored
# tree, or png.h's `#include "zlib.h"` would reach a header describing a
# zlib that was never built.
$(ZLIB_GEN)/include/zconf.h: $(ZLIB_DIR)/zconf.h
	@mkdir -p $(ZLIB_GEN)/include
	@sed -e '/^#ifdef HAVE_UNISTD_H.* may be/s/def HAVE_UNISTD_H\(.*\) may be/ 1\1 was/' \
	     -e '/^#ifdef HAVE_STDARG_H.* may be/s/def HAVE_STDARG_H\(.*\) may be/ 1\1 was/' \
	     $< > $@
	@echo "  GEN    $@ (unistd, stdarg)"

$(ZLIB_GEN)/include/zlib.h: $(ZLIB_DIR)/zlib.h $(ZLIB_GEN)/include/zconf.h
	@mkdir -p $(ZLIB_GEN)/include
	@cp $< $@

# All fifteen, which is the whole library. Nothing here is optional and
# nothing is a platform variant: zlib ships one implementation and the
# only thing a build chooses is the two defines above.
ZLIB_NAMES := adler32 compress crc32 deflate gzclose gzlib gzread gzwrite \
              infback inffast inflate inftrees trees uncompr zutil

ZLIB_OBJ := $(addprefix $(ZLIB_GEN)/obj/,$(addsuffix .o,$(ZLIB_NAMES)))

ZLIB_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
               -DHAVE_UNISTD_H -DHAVE_STDARG_H \
               -I$(ZLIB_DIR) -Ilibc/include

$(ZLIB_GEN)/obj/%.o: $(ZLIB_DIR)/%.c $(ZLIB_DIR)/zlib.h
	@mkdir -p $(ZLIB_GEN)/obj
	$(CC) $(ZLIB_CFLAGS) -c $< -o $@

build/libz.a: $(ZLIB_OBJ) $(ZLIB_GEN)/include/zlib.h
	@rm -f $@
	$(AR) rcs $@ $(ZLIB_OBJ)
	@echo "  ZLIB   build/libz.a ($(ZLIB_VERSION))"

.PHONY: zlib
zlib: build/libz.a

# Two gzip files for the volume, extracted from apps/zlib_ref.h rather
# than compressed here.
#
# zlib's gz* layer is the only part of the library that touches the
# operating system -- open with a mode argument, read, write, lseek,
# close, snprintf, strerror -- and it deserves to be driven against real
# NTFS. But a boot self-test runs with nobody signed in, and creating a
# file in that state is refused at open() by the elevation gateway, which
# is the security property `apps/fdtest.c` asserts rather than works
# around. So the read half needs a gzip file that is already there.
#
# Extracting them from the header keeps one copy of the data: the array
# apps/zlibtest.c compares against and the bytes on the disk are the same
# bytes by construction. /zlibbad.gz carries the original text's CRC-32
# over a flipped payload, so it reads back 431 plausible bytes and then
# fails -- which is the only way a read-only test reaches gz_error().
build/zlibref.gz: apps/zlib_ref.h tools/carray.py
	@mkdir -p build
	@python3 tools/carray.py --from apps/zlib_ref.h \
		--name zref_text_gzip_named --out $@

build/zlibbad.gz: apps/zlib_ref.h tools/carray.py
	@mkdir -p build
	@python3 tools/carray.py --from apps/zlib_ref.h \
		--name zref_text_gzip_corrupt --out $@

$(ZLIB_DIR)/zlib.h:
	@echo "  fetching $(ZLIB_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(ZLIB_TARBALL) $(ZLIB_URL)
	@printf '%s  build/%s\n' "$(ZLIB_SHA256)" "$(ZLIB_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(ZLIB_TARBALL) does not match its checksum."; \
		     rm -f build/$(ZLIB_TARBALL); exit 1; }
	@rm -rf $(ZLIB_DIR)
	@mkdir -p $(ZLIB_DIR)
	@tar -C $(ZLIB_DIR) --strip-components=1 -xzf build/$(ZLIB_TARBALL)

# --- libpng 1.6.58 ---
#
# `OptionsWPE.cmake:16`, behind zlib, and found by CMake's own FindPNG --
# which is why the section above exists: that module's first statement is
# `find_package(ZLIB)` and its whole body is inside `if (ZLIB_FOUND)`.
#
# ---- pnglibconf.h, and why nothing here is hand-written ----
#
# libpng's configuration is one generated header, `pnglibconf.h`, and
# upstream ships the standard one as `scripts/pnglibconf.h.prebuilt` for
# builds that are not running its awk pipeline. It is copied here
# verbatim, unedited, and that is a claim worth making rather than a
# shortcut: every option in it is supportable on this machine, so there
# was nothing to turn off.
#
# The two that would have been in question are both answered:
#
#   PNG_SETJMP_SUPPORTED     libpng's error handling is setjmp/longjmp
#                            and there is no other mode worth having.
#                            libc/setjmp.S is real, and this is the first
#                            code in ring 3 to depend on it -- which is
#                            why apps/pngtest.c goes out of its way to
#                            make libpng longjmp out of a decode.
#
#   PNG_FLOATING_POINT_      pow, floor, ceil, exp, log and modf, for
#   SUPPORTED                gamma. All six are in libc/math.c, and the
#                            no-FPU rule that would once have made this
#                            impossible was repealed when the model
#                            needed floating point.
#
# PNG_INTEL_SSE_ + the other SIMD options stay undefined, and that is
# upstream's default rather than a decision here: unlike ARM's NEON,
# which libpng enables by detection, the x86 intrinsics are explicit
# opt-in. So the fourteen sources below are the complete library, and
# -msse2 in APP_CFLAGS does not quietly change that.
#
# ---- and the include layout it needs ----
#
# png.h includes "pnglibconf.h" and "zlib.h", neither of which is in
# libpng's own directory, so both are found through -I: the generated
# config from build/png, and zlib's *edited* zconf.h pair from
# build/zlib/include. That second one matters -- compiling libpng against
# the unedited zconf.h would build it against a zlib whose z_off_t is
# long rather than off_t, which is the same size here and would be a
# genuine mismatch on a machine where it is not.
PNG_VERSION := 1.6.58
PNG_TARBALL := libpng-$(PNG_VERSION).tar.xz
PNG_URL     := https://download.sourceforge.net/libpng/$(PNG_TARBALL)
PNG_SHA256  := 28eb403f51f0f7405249132cecfe82ea5c0ef97f1b32c5a65828814ae0d34775
PNG_DIR     := third_party/libpng
PNG_GEN     := build/png

$(PNG_GEN)/pnglibconf.h: $(PNG_DIR)/scripts/pnglibconf.h.prebuilt
	@mkdir -p $(PNG_GEN)
	@cp $< $@
	@echo "  GEN    $@ ($(PNG_VERSION), upstream prebuilt, unedited)"

PNG_NAMES := png pngerror pngget pngmem pngpread pngread pngrio pngrtran \
             pngrutil pngset pngtrans pngwio pngwrite pngwtran pngwutil

PNG_OBJ := $(addprefix $(PNG_GEN)/obj/,$(addsuffix .o,$(PNG_NAMES)))

PNG_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
              -I$(PNG_GEN) -I$(PNG_DIR) -I$(ZLIB_GEN)/include -Ilibc/include

PNG_DEPS := $(PNG_GEN)/pnglibconf.h $(ZLIB_GEN)/include/zlib.h

$(PNG_GEN)/obj/%.o: $(PNG_DIR)/%.c $(PNG_DEPS)
	@mkdir -p $(PNG_GEN)/obj
	$(CC) $(PNG_CFLAGS) -c $< -o $@

build/libpng.a: $(PNG_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  PNG    build/libpng.a ($(PNG_VERSION))"

.PHONY: png
png: build/libpng.a

$(PNG_DIR)/scripts/pnglibconf.h.prebuilt:
	@echo "  fetching $(PNG_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(PNG_TARBALL) $(PNG_URL)
	@printf '%s  build/%s\n' "$(PNG_SHA256)" "$(PNG_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(PNG_TARBALL) does not match its checksum."; \
		     rm -f build/$(PNG_TARBALL); exit 1; }
	@rm -rf $(PNG_DIR)
	@mkdir -p $(PNG_DIR)
	@tar -C $(PNG_DIR) --strip-components=1 -xJf build/$(PNG_TARBALL)

# --- libwebp 1.6.0 ---
#
# `OptionsWPE.cmake:20`, and the last unproven entry in the fifteen that
# file opens with. `find_package(WebP REQUIRED COMPONENTS demux)` wants
# two archives, not one, which is why this section builds two.
#
# ---- what FindWebP.cmake actually asks for ----
#
# WebKit ships its own module, and it wants three things: `webp/decode.h`
# somewhere on the include path, a library called `webp`, and — because
# of `COMPONENTS demux` — a second one called `webpdemux`. There is a
# `mux` component too and OptionsWPE does not request it, so
# libwebpmux.a is deliberately not built: nothing in
# `Source/WebCore` names WebPMux, and an archive in the sysroot that
# nothing has ever run is exactly the mismatch the libxkbcommon staging
# note is about.
#
# **A bug in that module, named rather than fixed**, in the tradition of
# FindEpoxy's "withouit" typo. Line 68 reads
#
#     set(WebP_VERSION ${PC_WEBP_CFLAGS_VERSION})
#
# and `pkg_check_modules` sets `PC_WEBP_VERSION`, never
# `PC_WEBP_CFLAGS_VERSION`. So `WebP_VERSION` is empty whether or not
# pkg-config is installed and whether or not the .pc file below exists —
# which is harmless here only because OptionsWPE asks for no minimum, so
# the `VERSION_GREATER` comparison is "" against "" and takes neither
# branch. The .pc files are written anyway, for the reason libtasn1's is.
#
# ---- config.h, and the one decision in it that matters ----
#
# See the long note in third_party/libwebp-port/config.h. In short:
# libwebp's SIMD has two macro families, and the moment HAVE_CONFIG_H is
# defined the config file becomes responsible for the answer —
# WEBP_HAVE_SSE2 is what lets the dispatcher call VP8DspInitSSE2 at all.
# Each WEBP_HAVE_ here is paired with a compiler flag below, because the
# two must agree in both directions: a -m without the define compiles
# the kernels and never calls them, and a define without the -m calls an
# Init function that is not in the archive.
#
# ---- the tarball ----
#
# From Google's own release host over HTTPS, checksum recorded here.
# There is a detached signature beside it, `libwebp-1.6.0.tar.gz.asc`,
# and it is **not** verified: gpg is not installed on this machine and
# installing it was not part of the work. Recorded as a gap rather than
# quietly skipped — the checksum below was taken from the download, so
# it pins the bytes against later tampering and vouches for nothing
# about the first fetch.
WEBP_VERSION := 1.6.0
WEBP_TARBALL := libwebp-$(WEBP_VERSION).tar.gz
WEBP_URL     := https://storage.googleapis.com/downloads.webmproject.org/releases/webp/$(WEBP_TARBALL)
WEBP_SHA256  := e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564
WEBP_DIR     := third_party/libwebp
WEBP_PORT    := third_party/libwebp-port
WEBP_GEN     := build/webp

# Upstream's own layout: the generated config lands at
# $(top_builddir)/src/webp/config.h and -I names the build root, because
# every source that wants it writes `#include "src/webp/config.h"`.
$(WEBP_GEN)/src/webp/config.h: $(WEBP_PORT)/config.h
	@mkdir -p $(WEBP_GEN)/src/webp
	@cp $< $@
	@echo "  GEN    $@ ($(WEBP_VERSION), sse2+sse41+avx2, threads)"

# The source lists are upstream's, read out of the Makefile.am files
# rather than curated here: sharpyuv/Makefile.am, src/dec, src/utils,
# src/dsp, src/enc and src/demux each name theirs, and the split between
# the plain and the _sse41/_avx2 groups is upstream's too — it is what
# decides which files get an extra -m flag.
WEBP_SHARPYUV := sharpyuv sharpyuv_cpu sharpyuv_csp sharpyuv_dsp \
                 sharpyuv_gamma
WEBP_SHARPYUV_SSE2 := sharpyuv_sse2

WEBP_DEC := alpha_dec buffer_dec frame_dec idec_dec io_dec quant_dec \
            tree_dec vp8_dec vp8l_dec webp_dec

WEBP_UTILS := bit_reader_utils color_cache_utils filters_utils \
              huffman_utils palette quant_levels_dec_utils random_utils \
              rescaler_utils thread_utils utils \
              bit_writer_utils huffman_encode_utils quant_levels_utils

WEBP_DSP := alpha_processing cpu dec dec_clip_tables filters lossless \
            rescaler upsampling yuv \
            cost enc lossless_enc ssim

WEBP_DSP_SSE2 := alpha_processing_sse2 dec_sse2 filters_sse2 \
                 lossless_sse2 rescaler_sse2 upsampling_sse2 yuv_sse2 \
                 cost_sse2 enc_sse2 lossless_enc_sse2 ssim_sse2

WEBP_DSP_SSE41 := alpha_processing_sse41 dec_sse41 lossless_sse41 \
                  upsampling_sse41 yuv_sse41 enc_sse41 lossless_enc_sse41

WEBP_DSP_AVX2 := lossless_avx2 lossless_enc_avx2

WEBP_ENC := alpha_enc analysis_enc backward_references_cost_enc \
            backward_references_enc config_enc cost_enc filter_enc \
            frame_enc histogram_enc iterator_enc near_lossless_enc \
            picture_csp_enc picture_enc picture_psnr_enc \
            picture_rescale_enc picture_tools_enc predictor_enc \
            quant_enc syntax_enc token_enc tree_enc vp8l_enc webp_enc

WEBP_DEMUX := anim_decode demux

WEBP_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
               -DHAVE_CONFIG_H -I$(WEBP_GEN) -I$(WEBP_DIR) -Ilibc/include

WEBP_OBJ := \
  $(addprefix $(WEBP_GEN)/obj/y_,$(addsuffix .o,$(WEBP_SHARPYUV))) \
  $(addprefix $(WEBP_GEN)/obj/y2_,$(addsuffix .o,$(WEBP_SHARPYUV_SSE2))) \
  $(addprefix $(WEBP_GEN)/obj/d_,$(addsuffix .o,$(WEBP_DEC))) \
  $(addprefix $(WEBP_GEN)/obj/u_,$(addsuffix .o,$(WEBP_UTILS))) \
  $(addprefix $(WEBP_GEN)/obj/s_,$(addsuffix .o,$(WEBP_DSP))) \
  $(addprefix $(WEBP_GEN)/obj/s_,$(addsuffix .o,$(WEBP_DSP_SSE2))) \
  $(addprefix $(WEBP_GEN)/obj/s41_,$(addsuffix .o,$(WEBP_DSP_SSE41))) \
  $(addprefix $(WEBP_GEN)/obj/sa_,$(addsuffix .o,$(WEBP_DSP_AVX2))) \
  $(addprefix $(WEBP_GEN)/obj/e_,$(addsuffix .o,$(WEBP_ENC)))

WEBP_DEMUX_OBJ := \
  $(addprefix $(WEBP_GEN)/obj/x_,$(addsuffix .o,$(WEBP_DEMUX)))

WEBP_DEPS := $(WEBP_GEN)/src/webp/config.h

$(WEBP_GEN)/obj/y_%.o: $(WEBP_DIR)/sharpyuv/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

$(WEBP_GEN)/obj/y2_%.o: $(WEBP_DIR)/sharpyuv/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

$(WEBP_GEN)/obj/d_%.o: $(WEBP_DIR)/src/dec/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

$(WEBP_GEN)/obj/u_%.o: $(WEBP_DIR)/src/utils/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

$(WEBP_GEN)/obj/s_%.o: $(WEBP_DIR)/src/dsp/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

# The two rules that exist only because of the SIMD gate. Each is the
# other half of a WEBP_HAVE_ in the port's config.h: without the -m the
# intrinsics in these files do not compile in, the Init functions vanish,
# and the dispatcher calls a symbol that is not there.
$(WEBP_GEN)/obj/s41_%.o: $(WEBP_DIR)/src/dsp/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -msse4.1 -c $< -o $@

$(WEBP_GEN)/obj/sa_%.o: $(WEBP_DIR)/src/dsp/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -mavx2 -c $< -o $@

$(WEBP_GEN)/obj/e_%.o: $(WEBP_DIR)/src/enc/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

$(WEBP_GEN)/obj/x_%.o: $(WEBP_DIR)/src/demux/%.c $(WEBP_DEPS)
	@mkdir -p $(WEBP_GEN)/obj
	$(CC) $(WEBP_CFLAGS) -c $< -o $@

build/libwebp.a: $(WEBP_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  WEBP   build/libwebp.a ($(WEBP_VERSION), $(words $(WEBP_OBJ)) objects)"

build/libwebpdemux.a: $(WEBP_DEMUX_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  WEBP   build/libwebpdemux.a ($(WEBP_VERSION))"

.PHONY: webp
webp: build/libwebp.a build/libwebpdemux.a

$(WEBP_DIR)/src/webp/decode.h:
	@echo "  fetching $(WEBP_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(WEBP_TARBALL) $(WEBP_URL)
	@printf '%s  build/%s\n' "$(WEBP_SHA256)" "$(WEBP_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(WEBP_TARBALL) does not match its checksum."; \
		     rm -f build/$(WEBP_TARBALL); exit 1; }
	@rm -rf $(WEBP_DIR)
	@mkdir -p $(WEBP_DIR)
	@tar -C $(WEBP_DIR) --strip-components=1 -xzf build/$(WEBP_TARBALL)

# --- PCRE2 10.48 ---
#
# Not a WebKit dependency. **GLib's.**
#
# `OptionsWPE.cmake:185` is
# `find_package(GLIB 2.70 REQUIRED COMPONENTS gio gio-unix gobject
# gthread gmodule)`, and GLib 2.74's own meson.build:2079 says
#
#     pcre2 = dependency('libpcre2-8', required : true, ...)
#
# -- required, not optional, because GRegex is PCRE2 and GLib stopped
# bundling a copy after 2.72. So this is the first port in this tree
# that WebKit's configure will never look for directly: it is a
# prerequisite of a prerequisite, and it is here because the entry
# above it cannot be attempted without it.
#
# ---- the three generated files, and where each comes from ----
#
# PCRE2 needs `pcre2.h`, `config.h` and `pcre2_chartables.c`, and
# upstream ships a ready-made answer for two of the three:
#
#   pcre2.h              from src/pcre2.h.generic, copied. This is a
#                        real prebuilt -- the version numbers are
#                        already substituted -- and it is what upstream's
#                        own NON-AUTOTOOLS-BUILD instructions say to use.
#
#   pcre2_chartables.c   from src/pcre2_chartables.c.dist, copied. The
#                        C-locale character tables, which is what a
#                        build that does not run its own dftables
#                        generator uses, and what GLib wants: GRegex
#                        works in Unicode rather than in a locale.
#
#   config.h             hand-written, in third_party/pcre2-port/, and
#                        the long note there says why it is not
#                        src/config.h.generic. Briefly: that file is a
#                        floor rather than a configuration -- every
#                        HAVE_ undefined and SUPPORT_UNICODE off -- and
#                        GLib compiles every pattern with PCRE2_UTF and
#                        PCRE2_UCP, which without Unicode support are
#                        refused outright.
PCRE2_VERSION := 10.48
PCRE2_TARBALL := pcre2-$(PCRE2_VERSION).tar.bz2
PCRE2_URL     := https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$(PCRE2_VERSION)/$(PCRE2_TARBALL)
PCRE2_SHA256  := b6c68fdf6f3ac31388b50aa89ff0fc49c00c987c16e7b5146491d12003f2c8ed
PCRE2_DIR     := third_party/pcre2
PCRE2_PORT    := third_party/pcre2-port
PCRE2_GEN     := build/pcre2

$(PCRE2_GEN)/config.h: $(PCRE2_PORT)/config.h
	@mkdir -p $(PCRE2_GEN)
	@cp $< $@

$(PCRE2_GEN)/pcre2.h: $(PCRE2_DIR)/src/pcre2.h.generic
	@mkdir -p $(PCRE2_GEN)
	@cp $< $@
	@echo "  GEN    $@ ($(PCRE2_VERSION), upstream's generic header)"

$(PCRE2_GEN)/pcre2_chartables.c: $(PCRE2_DIR)/src/pcre2_chartables.c.dist
	@mkdir -p $(PCRE2_GEN)
	@cp $< $@

# COMMON_SOURCES from upstream's Makefile.am, read out rather than
# curated. pcre2_jit_compile.c is in the list and stays in it: with
# SUPPORT_JIT undefined it compiles to a handful of functions that
# return PCRE2_ERROR_JIT_BADOPTION, which is upstream's code rather than
# a stub, and is exactly what GLib's gregex.c expects to see -- it calls
# pcre2_jit_compile() and carries on when it fails.
PCRE2_NAMES := pcre2_auto_possess pcre2_chkdint pcre2_compile \
               pcre2_compile_cgroup pcre2_compile_class pcre2_config \
               pcre2_context pcre2_convert pcre2_dfa_match pcre2_error \
               pcre2_extuni pcre2_find_bracket pcre2_jit_compile \
               pcre2_maketables pcre2_match pcre2_match_data \
               pcre2_match_next pcre2_newline pcre2_ord2utf \
               pcre2_pattern_info pcre2_script_run pcre2_serialize \
               pcre2_string_utils pcre2_study pcre2_substitute \
               pcre2_substring pcre2_tables pcre2_ucd pcre2_valid_utf \
               pcre2_xclass

PCRE2_OBJ := $(addprefix $(PCRE2_GEN)/obj/,$(addsuffix .o,$(PCRE2_NAMES))) \
             $(PCRE2_GEN)/obj/pcre2_chartables.o

# PCRE2_CODE_UNIT_WIDTH is not a configuration choice, it is which
# library this is: the same 30 sources compile three times over
# upstream, at 8, 16 and 32 bits, into three archives. Only the 8-bit
# one is built here, because it is the one GLib links.
PCRE2_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
                -DHAVE_CONFIG_H -DPCRE2_CODE_UNIT_WIDTH=8 \
                -I$(PCRE2_GEN) -I$(PCRE2_DIR)/src -Ilibc/include

PCRE2_DEPS := $(PCRE2_GEN)/config.h $(PCRE2_GEN)/pcre2.h

$(PCRE2_GEN)/obj/%.o: $(PCRE2_DIR)/src/%.c $(PCRE2_DEPS)
	@mkdir -p $(PCRE2_GEN)/obj
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

$(PCRE2_GEN)/obj/pcre2_chartables.o: $(PCRE2_GEN)/pcre2_chartables.c \
                                     $(PCRE2_DEPS)
	@mkdir -p $(PCRE2_GEN)/obj
	$(CC) $(PCRE2_CFLAGS) -c $< -o $@

build/libpcre2-8.a: $(PCRE2_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  PCRE2  build/libpcre2-8.a ($(PCRE2_VERSION), unicode, no jit)"

.PHONY: pcre2
pcre2: build/libpcre2-8.a

$(PCRE2_DIR)/src/pcre2.h.generic:
	@echo "  fetching $(PCRE2_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(PCRE2_TARBALL) $(PCRE2_URL)
	@printf '%s  build/%s\n' "$(PCRE2_SHA256)" "$(PCRE2_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(PCRE2_TARBALL) does not match its checksum."; \
		     rm -f build/$(PCRE2_TARBALL); exit 1; }
	@rm -rf $(PCRE2_DIR)
	@mkdir -p $(PCRE2_DIR)
	@tar -C $(PCRE2_DIR) --strip-components=1 -xjf build/$(PCRE2_TARBALL)

# --- libffi 3.5.2 ---
#
# GObject's, not WebKit's: GLib 2.74's meson.build:2102 makes
# `libffi >= 3.0.0` required, and gobject/gclosure.c is the only file in
# GLib that uses it.
#
# **Half the library is built, and the missing half is the point.** The
# long note in third_party/libffi-port/fficonfig.h has it in full;
# briefly: ffi_call marshals arguments into an already-executable
# function, while closures write machine code into memory and jump to
# it. This kernel refuses PROT_WRITE|PROT_EXEC by name
# (src/desktop.h:2449), so closures.c and tramp.c are not compiled and
# ffi_closure_alloc is absent from the archive -- a link error naming
# the symbol rather than a fault inside a heap buffer later. GLib asks
# for ffi_prep_cif, ffi_call and the ffi_type_* descriptors and for
# nothing else, which is measured rather than assumed.
FFI_VERSION := 3.5.2
FFI_TARBALL := libffi-$(FFI_VERSION).tar.gz
FFI_URL     := https://github.com/libffi/libffi/releases/download/v$(FFI_VERSION)/$(FFI_TARBALL)
FFI_SHA256  := f3a3082a23b37c293a4fcd1053147b371f2ff91fa7ea1b2a52e335676bac82dc
FFI_DIR     := third_party/libffi
FFI_PORT    := third_party/libffi-port
FFI_GEN     := build/ffi

# ffi.h is a substitution template, like libxml2's xmlversion.h and for
# the same reason: the six tokens below decide which target's ABI enum
# the header declares, and hand-copying that is a second implementation
# of upstream's own dependency rules.
FFI_SUBST := \
  -e 's/@VERSION@/$(FFI_VERSION)/g' \
  -e 's/@TARGET@/X86_64/g' \
  -e 's/@HAVE_LONG_DOUBLE@/1/g' \
  -e 's/@FFI_EXEC_TRAMPOLINE_TABLE@/0/g' \
  -e 's/@FFI_VERSION_STRING@/$(FFI_VERSION)/g' \
  -e 's/@FFI_VERSION_NUMBER@/30502/g'

$(FFI_GEN)/include/ffi.h: $(FFI_DIR)/include/ffi.h.in
	@mkdir -p $(FFI_GEN)/include
	@sed $(FFI_SUBST) $< > $@
	@echo "  GEN    $@ ($(FFI_VERSION), X86_64, no trampoline table)"

# ffitarget.h is per-architecture and lives in the backend directory;
# every consumer includes it as <ffitarget.h> beside ffi.h, so it is
# copied into the same generated include root rather than reached with a
# second -I. That root is also what gets staged.
$(FFI_GEN)/include/ffitarget.h: $(FFI_DIR)/src/x86/ffitarget.h
	@mkdir -p $(FFI_GEN)/include
	@cp $< $@

$(FFI_GEN)/fficonfig.h: $(FFI_PORT)/fficonfig.h
	@mkdir -p $(FFI_GEN)
	@cp $< $@

# The portable half plus the x86-64 backend. closures.c and tramp.c are
# absent on purpose; ffi.c/sysv.S are the 32-bit x86 backend and win64
# is the Microsoft ABI, which ffiw64.c also serves as FFI_EFI64 -- both
# are in upstream's x86_64 build and are kept.
FFI_C_NAMES  := prep_cif types raw_api java_raw_api
FFI_X86_C    := ffi64 ffiw64
FFI_X86_S    := unix64 win64

FFI_OBJ := $(addprefix $(FFI_GEN)/obj/c_,$(addsuffix .o,$(FFI_C_NAMES))) \
           $(addprefix $(FFI_GEN)/obj/x_,$(addsuffix .o,$(FFI_X86_C))) \
           $(addprefix $(FFI_GEN)/obj/s_,$(addsuffix .o,$(FFI_X86_S)))

FFI_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
              -DHAVE_CONFIG_H -I$(FFI_GEN) -I$(FFI_GEN)/include \
              -I$(FFI_DIR)/include -I$(FFI_DIR)/src -Ilibc/include

FFI_DEPS := $(FFI_GEN)/fficonfig.h $(FFI_GEN)/include/ffi.h \
            $(FFI_GEN)/include/ffitarget.h

$(FFI_GEN)/obj/c_%.o: $(FFI_DIR)/src/%.c $(FFI_DEPS)
	@mkdir -p $(FFI_GEN)/obj
	$(CC) $(FFI_CFLAGS) -c $< -o $@

$(FFI_GEN)/obj/x_%.o: $(FFI_DIR)/src/x86/%.c $(FFI_DEPS)
	@mkdir -p $(FFI_GEN)/obj
	$(CC) $(FFI_CFLAGS) -c $< -o $@

# The assembly goes through the C compiler rather than the assembler so
# that it is preprocessed: unix64.S is full of #if on the fficonfig.h
# and ffitarget.h macros above.
$(FFI_GEN)/obj/s_%.o: $(FFI_DIR)/src/x86/%.S $(FFI_DEPS)
	@mkdir -p $(FFI_GEN)/obj
	$(CC) $(FFI_CFLAGS) -c $< -o $@

build/libffi.a: $(FFI_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  FFI    build/libffi.a ($(FFI_VERSION), call side; no closures)"

.PHONY: ffi
ffi: build/libffi.a

$(FFI_DIR)/include/ffi.h.in:
	@echo "  fetching $(FFI_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(FFI_TARBALL) $(FFI_URL)
	@printf '%s  build/%s\n' "$(FFI_SHA256)" "$(FFI_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(FFI_TARBALL) does not match its checksum."; \
		     rm -f build/$(FFI_TARBALL); exit 1; }
	@rm -rf $(FFI_DIR)
	@mkdir -p $(FFI_DIR)
	@tar -C $(FFI_DIR) --strip-components=1 -xzf build/$(FFI_TARBALL)

# --- GNU libiconv 1.18 ---
#
# The gap this tree has been recording for three sessions. GLib 2.74's
# meson.build:2060 makes `dependency('iconv')` required, and
# `third_party/libxml2-port/config.h` has said since it was written that
# "there is no iconv in this C library". Both were pointing at the same
# missing thing, and this is it.
#
# ---- three objects, and 274 headers that are already generated ----
#
# libiconv's whole converter set is #included into one translation unit:
# lib/iconv.c pulls in converters.h, which pulls in every encoding in
# turn, and the alias, flag and transliteration tables ship
# pre-generated in the tarball. So there is no generator to run and no
# source list to curate -- upstream's lib/Makefile.in:59 names all
# three.
#
# ---- and one substituted header ----
#
# include/iconv.h.in has five tokens. Four of them are empty or a
# constant; the one with a decision in it is USE_MBSTATE_T, which puts
# an mbstate_t inside iconv_allocation_t. It is 1 because <wchar.h> here
# has the type, and a consumer that disagreed about that structure's
# size would overflow it.
ICONV_VERSION := 1.18
ICONV_TARBALL := libiconv-$(ICONV_VERSION).tar.gz
ICONV_URL     := https://ftp.gnu.org/pub/gnu/libiconv/$(ICONV_TARBALL)
ICONV_SHA256  := 3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8
ICONV_DIR     := third_party/libiconv
ICONV_PORT    := third_party/libiconv-port
ICONV_GEN     := build/iconv

# EILSEQ is substituted empty because <errno.h> here already defines it
# (84); iconv.h only fills one in when the platform has none, and an
# empty expansion inside `#ifndef EILSEQ` is never reached.
# ICONV_CONST is empty for the reason the port's config.h gives.
ICONV_SUBST := \
  -e 's/@EILSEQ@//g' \
  -e 's/@ICONV_CONST@//g' \
  -e 's/@USE_MBSTATE_T@/1/g' \
  -e 's/@BROKEN_WCHAR_H@/0/g' \
  -e 's/@DLL_VARIABLE@//g'

$(ICONV_GEN)/include/iconv.h: $(ICONV_DIR)/include/iconv.h.in
	@mkdir -p $(ICONV_GEN)/include
	@sed $(ICONV_SUBST) $< > $@
	@echo "  GEN    $@ ($(ICONV_VERSION), mbstate_t, no extra encodings)"

$(ICONV_GEN)/config.h: $(ICONV_PORT)/config.h
	@mkdir -p $(ICONV_GEN)
	@cp $< $@

# localcharset.h is an internal header that lib/iconv.c includes by
# name, and it lives under libcharset/. Copied next to the generated
# config so one -I reaches both, which is what upstream's own
# lib/Makefile.in does with -I../libcharset/include.
$(ICONV_GEN)/localcharset.h: $(ICONV_DIR)/libcharset/include/localcharset.h.in
	@mkdir -p $(ICONV_GEN)
	@sed -e 's/@HAVE_VISIBILITY@/0/g' $< > $@

ICONV_CFLAGS := $(filter-out -fPIC,$(APP_CFLAGS)) -fPIC -w \
                -DHAVE_CONFIG_H -DBUILDING_LIBICONV -DBUILDING_LIBCHARSET \
                -DLIBDIR='"/lib"' \
                -I$(ICONV_GEN) -I$(ICONV_GEN)/include \
                -I$(ICONV_DIR)/lib -I$(ICONV_DIR)/include \
                -I$(ICONV_DIR)/libcharset/include -Ilibc/include

ICONV_DEPS := $(ICONV_GEN)/config.h $(ICONV_GEN)/include/iconv.h \
              $(ICONV_GEN)/localcharset.h

$(ICONV_GEN)/obj/iconv.o: $(ICONV_DIR)/lib/iconv.c $(ICONV_DEPS)
	@mkdir -p $(ICONV_GEN)/obj
	$(CC) $(ICONV_CFLAGS) -c $< -o $@

$(ICONV_GEN)/obj/compat.o: $(ICONV_DIR)/lib/compat.c $(ICONV_DEPS)
	@mkdir -p $(ICONV_GEN)/obj
	$(CC) $(ICONV_CFLAGS) -c $< -o $@

$(ICONV_GEN)/obj/localcharset.o: \
		$(ICONV_DIR)/libcharset/lib/localcharset.c $(ICONV_DEPS)
	@mkdir -p $(ICONV_GEN)/obj
	$(CC) $(ICONV_CFLAGS) -c $< -o $@

ICONV_OBJ := $(ICONV_GEN)/obj/iconv.o $(ICONV_GEN)/obj/compat.o \
             $(ICONV_GEN)/obj/localcharset.o

build/libiconv.a: $(ICONV_OBJ)
	@rm -f $@
	$(AR) rcs $@ $^
	@echo "  ICONV  build/libiconv.a ($(ICONV_VERSION))"

.PHONY: iconv
iconv: build/libiconv.a

$(ICONV_DIR)/include/iconv.h.in:
	@echo "  fetching $(ICONV_TARBALL)"
	@mkdir -p build third_party
	@curl -fL --retry 3 -o build/$(ICONV_TARBALL) $(ICONV_URL)
	@printf '%s  build/%s\n' "$(ICONV_SHA256)" "$(ICONV_TARBALL)" \
		| shasum -a 256 -c - >/dev/null \
		|| { echo "  $(ICONV_TARBALL) does not match its checksum."; \
		     rm -f build/$(ICONV_TARBALL); exit 1; }
	@rm -rf $(ICONV_DIR)
	@mkdir -p $(ICONV_DIR)
	@tar -C $(ICONV_DIR) --strip-components=1 -xzf build/$(ICONV_TARBALL)

.PHONY: libs-fetch
libs-fetch: $(SQLITE_DIR)/sqlite3.c $(FT_DIR)/include/ft2build.h $(HB_DIR)/src/harfbuzz.cc \
            $(ICU_DIR)/common/unicode/utypes.h $(JPEG_DIR)/jpeglib.h \
            $(EPOXY_DIR)/src/dispatch_common.c \
            $(GPGERROR_DIR)/src/gpg-error.h.in $(GCRYPT_DIR)/src/gcrypt.h.in \
            $(TASN1_DIR)/lib/includes/libtasn1.h \
            $(XKB_DIR)/src/xkbcomp/parser.y $(XKBCONF_DIR)/rules/merge.py \
            $(XML2_DIR)/include/libxml/xmlversion.h.in \
            $(ZLIB_DIR)/zlib.h $(PNG_DIR)/scripts/pnglibconf.h.prebuilt \
            $(WEBP_DIR)/src/webp/decode.h \
            $(PCRE2_DIR)/src/pcre2.h.generic $(FFI_DIR)/include/ffi.h.in \
            $(ICONV_DIR)/include/iconv.h.in

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
                          $(ICU_LIBS) $(LIBHBICU) build/libjpeg.a \
                          build/libepoxy.a build/libgcrypt.a \
                          build/libgpg-error.a build/libtasn1.a \
                          build/libxkbcommon.a build/libxml2.a \
                          build/libz.a build/libpng.a \
                          build/libwebp.a build/libwebpdemux.a \
                          build/libpcre2-8.a build/libffi.a \
                          build/libiconv.a \
                          $(FT_PORT)/ftoption.h $(FT_PORT)/ftmodule.h \
                          $(JPEG_PORT)/jconfig.h $(JPEG_PORT)/jconfigint.h
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
	@# JPEG. The four public headers plus the two hand-written config
	@# ones, and the config pair is copied *last* so it lands on top of
	@# anything upstream's tree carries -- the same hazard FreeType's
	@# redirected headers have and the same fix: the headers in this
	@# directory must describe the archive beside them. jconfigint.h is
	@# staged too, which an installed libjpeg does not do, because
	@# jversion.h reads it and a consumer that includes jversion.h
	@# would otherwise get a compile error about BUILD.
	@cp $(JPEG_DIR)/jpeglib.h $(JPEG_DIR)/jmorecfg.h \
	    $(JPEG_DIR)/jerror.h $(JPEG_DIR)/jpegint.h \
	                                       $(WEBKIT_SYSROOT)/include/
	@cp $(JPEG_PORT)/jconfig.h $(JPEG_PORT)/jconfigint.h \
	    $(JPEG_PORT)/jversion.h            $(WEBKIT_SYSROOT)/include/
	@cp build/libjpeg.a $(WEBKIT_SYSROOT)/lib/libjpeg.a
	@# Epoxy. Two public headers plus the generated one, which is the
	@# reason build/epoxy/include exists at all: gl_generated.h is
	@# derived from registry/gl.xml and is not in either tree.
	@mkdir -p $(WEBKIT_SYSROOT)/include/epoxy
	@cp $(EPOXY_DIR)/include/epoxy/common.h $(EPOXY_DIR)/include/epoxy/gl.h \
	                                       $(WEBKIT_SYSROOT)/include/epoxy/
	@cp $(EPOXY_GEN)/include/epoxy/gl_generated.h \
	                                       $(WEBKIT_SYSROOT)/include/epoxy/
	@cp build/libepoxy.a $(WEBKIT_SYSROOT)/lib/libepoxy.a
	@# libgcrypt and libgpg-error, staged together because
	@# FindLibGcrypt.cmake looks for both and says why in its own
	@# comment: "the libgcrypt.pc file does not list gpg-error as a
	@# dependency, resulting in linking errors". gcrypt.h and
	@# gpg-error.h are the two headers it find_path's for, and
	@# gcrypt.h's GCRYPT_VERSION line is its version fallback when
	@# pkg-config is not consulted.
	@cp $(GCRYPT_GEN)/gcrypt.h            $(WEBKIT_SYSROOT)/include/
	@cp $(GPGERROR_GEN)/gpg-error.h $(GPGERROR_GEN)/gpgrt.h \
	                                      $(WEBKIT_SYSROOT)/include/
	@cp build/libgcrypt.a     $(WEBKIT_SYSROOT)/lib/libgcrypt.a
	@cp build/libgpg-error.a  $(WEBKIT_SYSROOT)/lib/libgpg-error.a
	@# libtasn1: one public header and one archive, and the header is
	@# the one *shipped* in the tarball rather than lib/includes/
	@# libtasn1.h.in beside it. The .in is a substitution template with
	@# @ASN1_VERSION_MAJOR@ where the numbers go, and upstream's dist
	@# ships the finished header next to it; staging the template would
	@# put an unsubstituted ASN1_VERSION into every consumer.
	@cp $(TASN1_DIR)/lib/includes/libtasn1.h $(WEBKIT_SYSROOT)/include/
	@cp build/libtasn1.a      $(WEBKIT_SYSROOT)/lib/libtasn1.a
	@# libxkbcommon. Five of the seven public headers: xkbcommon.h and
	@# the four it includes or that consumers include beside it.
	@# xkbcommon-x11.h and xkbregistry.h are left out on purpose --
	@# they declare the two libraries this build does not produce
	@# (libxkbcommon-x11 needs xcb, libxkbregistry needs libxml2), and
	@# a header in this directory with no archive behind it is exactly
	@# the mismatch the FreeType and JPEG staging notes are about.
	@mkdir -p $(WEBKIT_SYSROOT)/include/xkbcommon
	@cp $(XKB_DIR)/include/xkbcommon/xkbcommon.h \
	    $(XKB_DIR)/include/xkbcommon/xkbcommon-compat.h \
	    $(XKB_DIR)/include/xkbcommon/xkbcommon-compose.h \
	    $(XKB_DIR)/include/xkbcommon/xkbcommon-keysyms.h \
	    $(XKB_DIR)/include/xkbcommon/xkbcommon-names.h \
	                                      $(WEBKIT_SYSROOT)/include/xkbcommon/
	@cp build/libxkbcommon.a  $(WEBKIT_SYSROOT)/lib/libxkbcommon.a
	@# libxml2, and the one port whose *include layout* is load-bearing.
	@#
	@# CMake's own FindLibXml2 -- not a module WebKit ships -- does
	@# `find_path(NAMES libxml/xpath.h PATH_SUFFIXES libxml2)`, so the
	@# headers have to sit at include/libxml2/libxml/ and not at
	@# include/libxml/. That extra component is why an installed libxml2
	@# is reached as `-I/usr/include/libxml2` and included as
	@# <libxml/parser.h>, and it is what the .pc's Cflags says below.
	@#
	@# The generated xmlversion.h is copied *after* the shipped headers
	@# and into the same directory, because every public header includes
	@# <libxml/xmlversion.h> and would otherwise find nothing -- the
	@# vendored tree has only the .in. It is also the file CMake reads
	@# LIBXML2_VERSION_STRING out of by regex, which is what answers
	@# `find_package(LibXml2 2.8.0 REQUIRED)`.
	@mkdir -p $(WEBKIT_SYSROOT)/include/libxml2/libxml
	@cp $(XML2_DIR)/include/libxml/*.h \
	                                      $(WEBKIT_SYSROOT)/include/libxml2/libxml/
	@cp $(XML2_GEN)/include/libxml/xmlversion.h \
	                                      $(WEBKIT_SYSROOT)/include/libxml2/libxml/
	@cp build/libxml2.a       $(WEBKIT_SYSROOT)/lib/libxml2.a
	@# zlib. The archive is staged as libz.a rather than under the name
	@# this Makefile builds it with, because FindZLIB's search list is
	@# `z zlib zdll zlib1 ...` and `z` is first.
	@#
	@# The header pair is the *generated* one from build/zlib/include and
	@# not the vendored tree's, and that is the whole point of generating
	@# it: zlib's configuration is two `#ifdef`s in zconf.h which this
	@# build answers with -D on the command line, and a consumer
	@# compiling with neither define would otherwise get a zconf.h that
	@# describes a different library from the archive next to it. Same
	@# hazard as FreeType's redirected headers, same fix.
	@#
	@# zlib.h is also where FindZLIB reads the version from, by regex on
	@# `#define ZLIB_VERSION`. There is no .pc route: FindZLIB is the one
	@# find module reached so far that never consults pkg-config at all.
	@cp $(ZLIB_GEN)/include/zlib.h $(ZLIB_GEN)/include/zconf.h \
	                                      $(WEBKIT_SYSROOT)/include/
	@cp build/libz.a          $(WEBKIT_SYSROOT)/lib/libz.a
	@# libpng. Three headers flat in include/ -- FindPNG's find_path
	@# tries the no-suffix directory before include/libpng16 and friends,
	@# so this is the shorter of two correct layouts and matches how
	@# jpeglib.h is staged above.
	@#
	@# pnglibconf.h is the generated one, and it is staged for the same
	@# reason zconf.h is: png.h includes it unconditionally, every
	@# structure layout in the library depends on what it says, and a
	@# consumer that found a different one would compile against a
	@# libpng that was never built. It is copied from build/png rather
	@# than from the tarball because the tarball does not contain it --
	@# only scripts/pnglibconf.h.prebuilt, which is what build/png holds
	@# a verbatim copy of.
	@#
	@# png.h is where FindPNG reads PNG_LIBPNG_VER_STRING from, and the
	@# archive is libpng.a because `png` comes before the version-suffixed
	@# names in that module's search list.
	@cp $(PNG_DIR)/png.h $(PNG_DIR)/pngconf.h \
	                                      $(WEBKIT_SYSROOT)/include/
	@cp $(PNG_GEN)/pnglibconf.h           $(WEBKIT_SYSROOT)/include/
	@cp build/libpng.a        $(WEBKIT_SYSROOT)/lib/libpng.a
	@# WebP, and the first package here that is two archives. WebKit's
	@# own FindWebP.cmake wants `webp/decode.h` on the include path, a
	@# library called `webp`, and -- because OptionsWPE says
	@# `COMPONENTS demux` -- a second one called `webpdemux`.
	@#
	@# **Five headers, and the omission is the point.** upstream's
	@# src/webp/ holds seven: decode.h, encode.h, types.h, mux_types.h
	@# and demux.h are staged, and mux.h is not, because
	@# libwebpmux.a is not built in this port -- nothing in
	@# Source/WebCore names WebPMux and OptionsWPE does not ask for the
	@# component. Copying src/webp/*.h wholesale would put a header in
	@# this directory with no archive behind it, which is exactly the
	@# mismatch the libxkbcommon note above is about. config.h.in and
	@# format_constants.h are internal and stay out for the same reason.
	@mkdir -p $(WEBKIT_SYSROOT)/include/webp
	@cp $(WEBP_DIR)/src/webp/decode.h $(WEBP_DIR)/src/webp/encode.h \
	    $(WEBP_DIR)/src/webp/types.h $(WEBP_DIR)/src/webp/mux_types.h \
	    $(WEBP_DIR)/src/webp/demux.h  $(WEBKIT_SYSROOT)/include/webp/
	@cp build/libwebp.a       $(WEBKIT_SYSROOT)/lib/libwebp.a
	@cp build/libwebpdemux.a  $(WEBKIT_SYSROOT)/lib/libwebpdemux.a
	@# PCRE2 and libffi, which are the first two archives staged here
	@# that WebKit's own configure will never look for. They are GLib's
	@# dependencies -- meson.build:2079 and :2102 -- and GLib is
	@# OptionsWPE.cmake:185. They go in this directory because the
	@# toolchain points PKG_CONFIG_LIBDIR at it and nowhere else, so it
	@# is where a GLib cross build will be told to look.
	@#
	@# libffi's two headers travel together: every consumer includes
	@# <ffitarget.h> from inside ffi.h, and upstream installs the
	@# architecture's copy beside it rather than under a subdirectory.
	@cp $(PCRE2_GEN)/pcre2.h  $(WEBKIT_SYSROOT)/include/
	@cp build/libpcre2-8.a    $(WEBKIT_SYSROOT)/lib/libpcre2-8.a
	@cp $(FFI_GEN)/include/ffi.h $(FFI_GEN)/include/ffitarget.h \
	                          $(WEBKIT_SYSROOT)/include/
	@cp build/libffi.a        $(WEBKIT_SYSROOT)/lib/libffi.a
	@# GNU libiconv, staged for two consumers rather than one. GLib's
	@# meson.build:2060 makes `dependency('iconv')` required, and
	@# libxml2's port turns its own iconv support off with a note saying
	@# there is none in this C library -- which stopped being true when
	@# this archive was built.
	@#
	@# localcharset.h goes in beside iconv.h because it is libiconv's
	@# own extension header and upstream installs it; nothing in GLib
	@# includes it, but apps/iconvtest.c does, and a consumer asking
	@# this library which locale it thinks it is in should be able to.
	@cp $(ICONV_GEN)/include/iconv.h $(ICONV_GEN)/localcharset.h \
	                          $(WEBKIT_SYSROOT)/include/
	@cp build/libiconv.a      $(WEBKIT_SYSROOT)/lib/libiconv.a
	@# ---- and the .pc files ----
	@#
	@# Several of WebKit's find modules ask pkg-config first and use
	@# find_path/find_library only as a fallback, so these describe every
	@# archive here the way an installed library would be described.
	@#
	@# **They did nothing at all until libxkbcommon**, and the history is
	@# worth keeping because it corrects a claim this comment used to
	@# make twice over.
	@#
	@# It first said Epoxy could not be found without its .pc, because
	@# FindEpoxy takes Epoxy_VERSION from pkg-config and FPHSA would
	@# refuse version "" against a required 1.5.4. That is wrong.
	@# FindEpoxy.cmake:70-77 guards its FATAL_ERROR with
	@# `if (Epoxy_VERSION)`, so an *empty* version takes the other branch
	@# and prints a warning -- "Cannot determine Epoxy version withouit
	@# pkg-config", upstream's typo included -- and configure carries on.
	@# FPHSA does not reject a package for an unset VERSION_VAR. The
	@# error quoted was HarfBuzz's, whose find module derives a version
	@# from the headers, generalised to Epoxy without being reproduced.
	@#
	@# It then said, correctly at the time, that the files were inert:
	@# pkg-config was not installed, build/webkit/CMakeCache.txt read
	@# PKG_CONFIG_EXECUTABLE-NOTFOUND, and all six packages found so far
	@# had been found by find_path/find_library alone.
	@#
	@# **Libxkbcommon is the one that cannot be.** Its find module is
	@# `pkg_check_modules(LIBXKBCOMMON xkbcommon)` and nothing else --
	@# no find_path, no find_library, no fallback. So pkg-config is now
	@# a build requirement, checked by the `webkit` target, and every
	@# file written here is live. Two things changed the moment it was
	@# installed: xkbcommon is found, and Epoxy stopped warning and
	@# started reporting 1.5.10 -- from this directory, because the
	@# toolchain file points PKG_CONFIG_LIBDIR here and nowhere else, so
	@# the host's own libraries cannot answer a query about this
	@# system's.
	@mkdir -p $(WEBKIT_SYSROOT)/lib/pkgconfig
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libjpeg\nDescription: libjpeg-turbo, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -ljpeg\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(JPEG_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libjpeg.pc
	@# Epoxy's is where the effect is visible: FindEpoxy.cmake takes
	@# Epoxy_VERSION from pkg-config and from nowhere else, so this file
	@# is the only thing that can answer its version check. Before
	@# pkg-config was installed the package was found anyway and
	@# configure warned; now it reports 1.5.10 and does not.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: epoxy\nDescription: libepoxy GL dispatch, Vextro framebuffer provider\nVersion: %s\nLibs: -L$${libdir} -lepoxy\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(EPOXY_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/epoxy.pc
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libgcrypt\nDescription: libgcrypt, built for Vextro ring 3\nVersion: %s\nRequires.private: gpg-error\nLibs: -L$${libdir} -lgcrypt\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(GCRYPT_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libgcrypt.pc
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: gpg-error\nDescription: libgpg-error, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lgpg-error\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(GPGERROR_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/gpg-error.pc
	@# libtasn1's is the opposite case to Epoxy's, and is written anyway.
	@# FindLibtasn1.cmake consults pkg-config only for *hints* and then
	@# hands FPHSA a single REQUIRED_VAR with no VERSION_VAR, so
	@# find_library alone would satisfy it and OptionsWPE asks for no
	@# minimum version. It is here so that every archive in this
	@# directory is described the same way rather than only the ones
	@# that would otherwise fail -- the next find module to be reached
	@# may well be one that reads Version.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libtasn1\nDescription: GNU Libtasn1, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -ltasn1\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(TASN1_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libtasn1.pc
	@# xkbcommon's is the first .pc here that is genuinely required, and
	@# the file name is part of that. FindLibxkbcommon.cmake is four
	@# lines of substance --
	@#
	@#     find_package(PkgConfig QUIET)
	@#     pkg_check_modules(LIBXKBCOMMON xkbcommon)
	@#     find_package_handle_standard_args(Libxkbcommon
	@#         REQUIRED_VARS LIBXKBCOMMON_FOUND FOUND_VAR LIBXKBCOMMON_FOUND)
	@#
	@# -- with no find_path and no find_library to fall back on. Every
	@# package found before this one was found by that fallback. This one
	@# can only be found by pkg-config, and pkg-config finds a module by
	@# the name in the call: **xkbcommon.pc**, not libxkbcommon.pc.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\nxkb_base=/etc/xkb\n\nName: xkbcommon\nDescription: XKB keymap compiler, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lxkbcommon\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(XKB_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/xkbcommon.pc
	@# libxml-2.0 is the module name pkg-config has always known this
	@# library by, and the Cflags carries the extra path component the
	@# headers are staged under. CMake's FindLibXml2 consults it for
	@# hints before falling back to find_path, so it is the faster of
	@# the two routes rather than the only one.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libXML\nDescription: libXML2, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lxml2\nCflags: -I$${includedir}/libxml2\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(XML2_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libxml-2.0.pc
	@# zlib and libpng are both found by CMake's own modules, and neither
	@# of those consults pkg-config at all -- FindZLIB reads the version
	@# out of zlib.h by regex and FindPNG out of png.h. These files are
	@# written for the same reason libtasn1's is: every archive in this
	@# directory is described the same way, and the next find module to
	@# be reached may well be one that asks.
	@#
	@# libpng gets two names because an installed libpng has two: the
	@# real libpng16.pc and libpng.pc beside it, which is a symlink
	@# upstream and a copy here. A consumer may ask for either.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: zlib\nDescription: zlib compression library, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lz\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(ZLIB_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/zlib.pc
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libpng\nDescription: libpng, built for Vextro ring 3\nVersion: %s\nRequires: zlib\nLibs: -L$${libdir} -lpng\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(PNG_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libpng16.pc
	@cp $(WEBKIT_SYSROOT)/lib/pkgconfig/libpng16.pc \
	   $(WEBKIT_SYSROOT)/lib/pkgconfig/libpng.pc
	@# libwebp's .pc files, and a bug in FindWebP worth naming rather
	@# than working around -- the FindEpoxy "withouit" precedent.
	@# FindWebP.cmake:68 reads
	@#
	@#     set(WebP_VERSION ${PC_WEBP_CFLAGS_VERSION})
	@#
	@# and pkg_check_modules sets PC_WEBP_VERSION; there is no such
	@# variable as PC_WEBP_CFLAGS_VERSION. So WebP_VERSION is empty
	@# whether or not pkg-config is installed and whether or not these
	@# files exist, and the module's own comment above that line --
	@# "A version can only be found through pkg-config" -- describes
	@# something the code does not do. It is harmless here only because
	@# OptionsWPE asks for no minimum, so the VERSION_GREATER test is
	@# "" against "" and takes neither branch.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libwebp\nDescription: WebP image codec, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lwebp\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(WEBP_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libwebp.pc
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libwebpdemux\nDescription: WebP container demuxer, built for Vextro ring 3\nVersion: %s\nRequires: libwebp\nLibs: -L$${libdir} -lwebpdemux\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(WEBP_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libwebpdemux.pc
	@# PCRE2's and libffi's, and these two are *not* decoration: GLib
	@# is a meson build, meson finds dependencies through pkg-config
	@# first and last, and `dependency('libpcre2-8')` and
	@# `dependency('libffi')` are both `required : true`. When GLib is
	@# attempted these are the files that will answer.
	@#
	@# libffi's Cflags carries no subdirectory because ffi.h and
	@# ffitarget.h are staged flat, and its Version is what meson's
	@# `version : '>= 3.0.0'` is compared against.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libpcre2-8\nDescription: PCRE2, 8-bit, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lpcre2-8\nCflags: -I$${includedir} -DPCRE2_STATIC\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(PCRE2_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libpcre2-8.pc
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: libffi\nDescription: Foreign function interface, call side only, Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -lffi\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(FFI_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/libffi.pc
	@# libiconv has no upstream .pc -- autotools installs none, and
	@# meson finds it through its own `dependency('iconv')` logic, which
	@# looks for the iconv_open symbol in libc first and for a libiconv
	@# second. One is written anyway so that the archive is described
	@# like every other in this directory, and because a cross build
	@# cannot run the symbol probe that meson would otherwise use.
	@printf 'prefix=%s\nexec_prefix=$${prefix}\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: iconv\nDescription: GNU libiconv, built for Vextro ring 3\nVersion: %s\nLibs: -L$${libdir} -liconv\nCflags: -I$${includedir}\n' \
	    "$(CURDIR)/$(WEBKIT_SYSROOT)" "$(ICONV_VERSION)" \
	    > $(WEBKIT_SYSROOT)/lib/pkgconfig/iconv.pc
	@touch $@
	@echo "  SYSROOT  $(WEBKIT_SYSROOT) (icu, harfbuzz, freetype, sqlite3, wpe, jpeg, epoxy, gcrypt, tasn1, xkbcommon, xml2, zlib, png, webp, pcre2, ffi, iconv)"

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
# stands here now is the dependency list itself -- 15 unconditional
# REQUIRED packages at the head of OptionsWPE.cmake and two more further
# down. All fifteen at the head are found; the two further down, LibSoup
# and GLIB, are one project and are where it stops.
# third_party/wpe-config/README.md has the current frontier and the exact
# error.
.PHONY: webkit
webkit: $(WEBKIT_SRC)/CMakeLists.txt $(LIBWPE) $(LIBC) $(LIBCXX) \
        $(WEBKIT_SYSROOT)/.stamp
	@missing=""; \
	command -v cmake >/dev/null || missing="$$missing cmake"; \
	command -v ninja >/dev/null || missing="$$missing ninja"; \
	command -v ruby  >/dev/null || missing="$$missing ruby"; \
	command -v gperf >/dev/null || missing="$$missing gperf"; \
	command -v pkg-config >/dev/null || missing="$$missing pkg-config"; \
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
	    echo "    eighteen upstream libraries"; \
	    echo "                          done  ported, and green in ring 3"; \
	    echo ""; \
	    echo "  pkg-config is not optional here, unlike the rest:"; \
	    echo "  FindLibxkbcommon.cmake has no find_path fallback at all,"; \
	    echo "  so xkbcommon can only be discovered through it."; \
	    echo ""; \
	    echo "  What remains after installing the above is the dependency"; \
	    echo "  list: 17 packages WebKit marks REQUIRED without a"; \
	    echo "  condition. All fifteen at the head of OptionsWPE.cmake"; \
	    echo "  are found. LibSoup at line 172 is where it stops, and"; \
	    echo "  behind it is GLib -- one project seen twice, and a"; \
	    echo "  second runtime rather than a port."; \
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

# --- User app: vlstest ---
#
# The Linux subset, asked to prove itself on the machine. It is a native
# Vextro program that makes Linux calls by adding the bias, which is the
# whole reason the bias exists as well as the personality flag: setting
# the personality would take printf away on the first call, and a test
# that cannot report is not a test.
#
# It depends on include/vls.h without including it, and that is worth
# saying rather than leaving to be discovered. The structures it reads —
# sigaction, siginfo, struct stat, linux_dirent64 — are written out in
# the test from the published ABI rather than shared with the kernel,
# because a layout shared with the code under test would make every
# check circular. The dependency below is what rebuilds it when the
# kernel's copy changes, so the two are compared rather than assumed.
build/vlstest.o: apps/vlstest.c apps/vextro.h include/vls.h \
                 libc/include/sys/syscall.h libc/include/stdio.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/vlstest: build/vlstest.o apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/vlstest.o $(LIBC) -o $@

# --- User app: jpegtest ---
#
# libjpeg-turbo in ring 3. The reference bitstream in apps/jpeg_ref.h was
# encoded by macOS's own `sips` rather than by this library, which is the
# whole value of the test: a round trip through our own encoder and back
# would prove the two halves of one library agree with each other, and
# they would even if both were wrong about the DCT.
build/jpegtest.o: apps/jpegtest.c apps/jpeg_ref.h apps/vextro.h \
                  $(JPEG_PORT)/jconfig.h $(JPEG_DIR)/jpeglib.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(JPEG_PORT) -I$(JPEG_DIR) -c $< -o $@

build/jpegtest: build/jpegtest.o build/libjpeg.a apps/app.ld $(LIBC)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		build/jpegtest.o build/libjpeg.a $(LIBC) -o $@

# --- User app: gltest ---
#
# libepoxy resolving entry points on a machine with no OpenGL. Every GL
# call in it goes through epoxy's generated dispatch, so one call
# exercises the whole chain — dispatch stub, dlopen, dlsym, the provider
# table, the render node, the window's pixels — and the last link is
# checked from the other end, through the canvas mapping the kernel hands
# the program at startup.
build/gltest.o: apps/gltest.c apps/vextro.h $(EPOXY_PORT)/config.h \
                $(EPOXY_GEN)/include/epoxy/gl_generated.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(EPOXY_DIR)/include -I$(EPOXY_GEN)/include \
		-c $< -o $@

build/gltest: build/gltest.o build/libepoxy.a apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/gltest.o build/libepoxy.a $(LIBC) -o $@

# --- User app: gcrypttest ---
#
# libgcrypt in ring 3, against the vectors printed in FIPS 197, RFC 4231,
# RFC 6070 and NIST's SHA examples rather than against itself. The
# portable C implementations are the ones running here — every extension
# assembly path is off — so "the same answer as a distribution build" is
# a claim this test has to make rather than assume.
build/gcrypttest.o: apps/gcrypttest.c apps/vextro.h $(GCRYPT_GEN)/gcrypt.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(GCRYPT_GEN) -I$(GPGERROR_GEN) -c $< -o $@

build/gcrypttest: build/gcrypttest.o build/libgcrypt.a build/libgpg-error.a \
                  apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/gcrypttest.o build/libgcrypt.a \
		build/libgpg-error.a $(LIBC) -o $@

# --- User app: tasn1test ---
#
# libtasn1 in ring 3, against DER the build machine's OpenSSL produced —
# apps/tasn1_ref.h has the commands — and against the ASN.1 module WPE
# WebKit itself compiles in, copied verbatim from
# Source/WebCore/PAL/pal/crypto/tasn1/Utilities.cpp. Both of those are
# deliberate: an encoder checked against its own decoder agrees with
# itself while both are wrong, and a module written to be easy would not
# have shown whether WebKit's builds.
#
# The public header comes from the vendored tree rather than from the
# sysroot, which is the same arrangement every other test here uses: the
# sysroot is what WebKit's configure looks at, and a test that read from
# it would be checking the staging step rather than the library.
build/tasn1test.o: apps/tasn1test.c apps/tasn1_ref.h apps/vextro.h \
                   $(TASN1_DIR)/lib/includes/libtasn1.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(TASN1_DIR)/lib/includes -c $< -o $@

build/tasn1test: build/tasn1test.o build/libtasn1.a \
                 apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/tasn1test.o build/libtasn1.a \
		$(LIBC) -o $@

# --- User app: xkbtest ---
#
# libxkbcommon in ring 3, compiling the keymap WebKit asks for out of the
# xkeyboard-config data on the volume. It is the only test here whose
# subject is half data: the archive can be perfect and the suite still
# fail, because a keymap is a program in a language that lives in
# /etc/xkb and has to be read off NTFS at run time.
#
# Expected values come from Linux's input-event-codes.h (keycode = kernel
# code + 8), X11's keysymdef.h and Unicode — three sources outside this
# system — for the reason apps/jpeg_ref.h and apps/tasn1_ref.h exist.
build/xkbtest.o: apps/xkbtest.c apps/vextro.h \
                 $(XKB_DIR)/include/xkbcommon/xkbcommon.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(XKB_DIR)/include -c $< -o $@

build/xkbtest: build/xkbtest.o build/libxkbcommon.a \
               apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/xkbtest.o build/libxkbcommon.a \
		$(LIBC) -o $@

# --- User app: xmltest ---
#
# libxml2 in ring 3, driven the way WebKit drives it: a push parser with
# XML_PARSE_NOENT | XML_PARSE_HUGE, an external entity loader that
# refuses, and errors read through xmlSetStructuredErrorFunc. It also
# asserts the four features this build compiles *out* -- zlib, iconv,
# modules, http -- because each is an absence the port chose rather than
# one it stumbled into.
build/xmltest.o: apps/xmltest.c apps/vextro.h \
                 $(XML2_GEN)/include/libxml/xmlversion.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(XML2_GEN)/include -I$(XML2_DIR)/include \
		-c $< -o $@

build/xmltest: build/xmltest.o build/libxml2.a \
               apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/xmltest.o build/libxml2.a \
		$(LIBC) -o $@

# --- User app: zlibtest ---
#
# Compiled against build/zlib/include and *nothing else* -- not the
# vendored tree, and without -DHAVE_UNISTD_H or -DHAVE_STDARG_H, which
# the archive itself is built with. That is deliberate and it is section
# 1 of the test: this file sees exactly what WebKit will see when it
# includes <zlib.h> out of the sysroot, so if the generated zconf.h ever
# stops answering those two questions the same way the command line
# does, it fails here as a wrong type size rather than five libraries
# later as a link error.
build/zlibtest.o: apps/zlibtest.c apps/zlib_ref.h apps/vextro.h \
                  $(ZLIB_GEN)/include/zlib.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(ZLIB_GEN)/include -c $< -o $@

build/zlibtest: build/zlibtest.o build/libz.a \
                apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/zlibtest.o build/libz.a \
		$(LIBC) -o $@

# --- User app: pngtest ---
#
# The include path is the same three directories libpng itself is built
# with, in the same order, because png.h includes "pnglibconf.h" and
# "zlib.h" and both come from generated directories rather than from the
# vendored tree. It has to be the generated pnglibconf.h in particular:
# every structure layout in the library depends on it, and a test
# compiled against a different one would be reading png_struct at the
# wrong offsets.
#
# libpng.a comes before libz.a on the link line and both before libc,
# because ld resolves an archive once, in order: libpng calls deflate,
# zlib calls malloc, and nothing calls back.
build/pngtest.o: apps/pngtest.c apps/png_ref.h apps/vextro.h \
                 $(PNG_GEN)/pnglibconf.h $(ZLIB_GEN)/include/zlib.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(PNG_GEN) -I$(PNG_DIR) -I$(ZLIB_GEN)/include \
		-c $< -o $@

build/pngtest: build/pngtest.o build/libpng.a build/libz.a \
               apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/pngtest.o build/libpng.a build/libz.a \
		$(LIBC) -o $@

# --- User app: webptest ---
#
# Compiled with the same -I chain and the same -DHAVE_CONFIG_H the
# library is, because section 1 asserts the SIMD gate from the port's
# config.h and section 8 includes `src/dsp/cpu.h` -- an *internal*
# header -- to reach VP8GetCPUInfo. That hook is how libwebp's own tests
# force the dispatcher down a different path, and it is the only way to
# check the SSE2 and SSE4.1 kernels against the portable C sitting in
# the same archive. No consumer would include it; this is not a
# consumer.
#
# libwebpdemux.a comes before libwebp.a on the link line: the demuxer
# calls into the decoder to size a frame, and ld resolves an archive
# once, in order.
build/webptest.o: apps/webptest.c apps/webp_ref.h apps/vextro.h \
                  $(WEBP_GEN)/src/webp/config.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -DHAVE_CONFIG_H -I$(WEBP_GEN) -I$(WEBP_DIR) \
		-c $< -o $@

build/webptest: build/webptest.o build/libwebpdemux.a build/libwebp.a \
                apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/webptest.o build/libwebpdemux.a \
		build/libwebp.a $(LIBC) -o $@

# --- User app: pcre2test ---
#
# Compiled against build/pcre2 alone -- the generated pcre2.h and
# nothing from the vendored src/ -- because that is what a consumer
# sees. PCRE2_CODE_UNIT_WIDTH is set in the source rather than on the
# command line, which is how a consumer is supposed to do it: the header
# is shared by all three width libraries and refuses to compile without
# it.
#
# It is *not* compiled -DHAVE_CONFIG_H. The port's config.h is the
# library's business; a consumer that needed it would be a consumer that
# could not use an installed PCRE2.
build/pcre2test.o: apps/pcre2test.c apps/vextro.h $(PCRE2_GEN)/pcre2.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(PCRE2_GEN) -c $< -o $@

build/pcre2test: build/pcre2test.o build/libpcre2-8.a \
                 apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/pcre2test.o build/libpcre2-8.a \
		$(LIBC) -o $@

# --- User app: ffitest ---
#
# Compiled against build/ffi/include, which holds the generated ffi.h
# and the architecture's ffitarget.h beside it -- the layout a consumer
# sees, and the one staged into the sysroot.
#
# The link is where this port's shape shows: nothing here references
# ffi_closure_alloc, and if anything did it would fail at link naming
# the symbol, because src/closures.c is deliberately not in the archive.
build/ffitest.o: apps/ffitest.c apps/vextro.h $(FFI_GEN)/include/ffi.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(FFI_GEN)/include -c $< -o $@

build/ffitest: build/ffitest.o build/libffi.a \
               apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/ffitest.o build/libffi.a \
		$(LIBC) -o $@

# --- User app: iconvtest ---
#
# Compiled against build/iconv alone: the generated iconv.h, and
# localcharset.h beside it. That second one is not part of what GLib
# will include -- it is libiconv's own extension, and section 1 uses it
# to check which locale the library decided it was in, which is the only
# question this library asks the operating system.
build/iconvtest.o: apps/iconvtest.c apps/vextro.h \
                   $(ICONV_GEN)/include/iconv.h $(ICONV_GEN)/localcharset.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -I$(ICONV_GEN)/include -I$(ICONV_GEN) \
		-c $< -o $@

build/iconvtest: build/iconvtest.o build/libiconv.a \
                 apps/app.ld $(LIBC) $(LIBC_CRT0)
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T apps/app.ld \
		$(LIBC_CRT0) build/iconvtest.o build/libiconv.a \
		$(LIBC) -o $@

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
                src/sched/vls_core.c \
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
