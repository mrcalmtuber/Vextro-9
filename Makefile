CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
AR      := x86_64-elf-ar
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
            -Isrc -Iinclude -Ikernel/include -Ivxfmt $(EXTRA)

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
LIBC_SRC  := libc/string.c libc/stdio.c libc/malloc.c
LIBC_OBJ  := $(patsubst libc/%.c,build/libc/%.o,$(LIBC_SRC))
LIBC      := build/libvextro.a

APP_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector \
              -fno-stack-check -mno-red-zone -fPIC \
              -msse -msse2 -mfpmath=sse -fno-math-errno \
              -Ilibc/include -Iapps

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
      build/av_test build/mbedtls_test
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
	@./build/vmx_test
	@./build/av_test
	@./build/mbedtls_test $(TLS_HOST) $(TLS_PORT)
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

build/mbedtls_test: tools/mbedtls_test.c $(MBED_HOST_OBJ)
	@mkdir -p build
	@$(HOSTCC) -O1 -w -I$(MBED_DIR)/include -I$(MBED_DIR) \
		-DMBEDTLS_CONFIG_FILE='"vextro_config.h"' \
		-o $@ tools/mbedtls_test.c $(MBED_HOST_OBJ)

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
disk.img: $(ASSET_LIST) | build/hello build/faulter $(WINAPPS) $(STORE_BINS) $(PIC_SCI) $(MUSIC_WAV) $(MUSIC_FLAC) build/ca-bundle.crt
	@set -e; \
	big=""; \
	for f in $(ASSET_FILES); do \
	    if [ -f "$$f" ]; then big="$$big $$f"; fi; \
	done; \
	cmd="python3 tools/mkntfs.py disk.img $(DISK_MB) \
		apps/about.txt apps/notes.txt build/hello build/faulter \
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
build/initrd.tar: $(wildcard apps/*.txt) build/hello build/faulter $(WINAPPS) $(STORE_BINS)
	@mkdir -p build/initrd_staging/store/pkg
	cp apps/*.txt build/initrd_staging/ 2>/dev/null || true
	cp build/hello build/faulter build/initrd_staging/
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

build/core/main.o: src/core/main.c $(KERN_HDRS) \
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
$(KERN_MODULE_OBJ): build/%.o: src/%.c $(KERN_HDRS)
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
