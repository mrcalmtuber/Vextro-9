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
            -Isrc -Ikernel/include -Ivxfmt $(EXTRA)

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
# kernel.o untouched and produces an ISO that does not contain the thing
# that was asked for — silently, so the tests then "pass" by not running.
#
# It happens at parse time, before any rule runs, and that placement is
# the load-bearing part. A recipe that deletes the objects is too late:
# macOS ships GNU Make 3.81, which stats every target while building its
# dependency graph and does not look again. It also compares
# modification times to the whole second, so a stamp rewritten in the
# same second as the previous build is not "newer" and changes nothing.
# Deleting before the graph exists sidesteps both.
BUILD_FLAGS := $(CFLAGS)
$(shell mkdir -p build; \
        [ "`cat build/.flags 2>/dev/null`" = "$(BUILD_FLAGS)" ] || \
        { printf '%s\n' "$(BUILD_FLAGS)" > build/.flags; \
          rm -f build/kernel.o build/llm.o; })

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

.PHONY: all iso run clean cleandisk apps repo vxtools pics test FORCE

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
test: build/wikidoc_test build/crypto_test
	@./build/wikidoc_test
	@./build/crypto_test

build/wikidoc_test: tools/wikidoc_test.c src/wikidoc.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -o $@ $<

# The cipher is checked against the RFC's published vectors, not against
# itself: an implementation that is merely self-consistent round-trips
# perfectly and protects nothing.
build/crypto_test: tools/crypto_test.c src/chacha20.h src/sha256.h src/tls.h
	@mkdir -p build
	@$(HOSTCC) -O1 -Wall -Wextra -std=gnu11 -Wno-unused-function -o $@ $<

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
ASSET_LIST  := build/assets.list

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

disk.img: $(ASSET_LIST) | build/hello build/faulter $(WINAPPS) $(STORE_BINS) $(PIC_SCI) $(MUSIC_WAV) $(MUSIC_FLAC)
	@set -e; \
	big=""; \
	for f in $(ASSET_FILES); do \
	    if [ -f "$$f" ]; then big="$$big $$f"; fi; \
	done; \
	cmd="python3 tools/mkexfat.py disk.img $(DISK_MB) \
		apps/about.txt apps/notes.txt build/hello build/faulter \
		$(foreach w,$(WINAPPS),$(w):$(notdir $(w))) \
		apps/welcome.txt:docs/welcome.txt \
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
WINAPPS := build/win/winhello.exe
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
build/kernel.o: src/kernel.c $(wildcard src/*.h) build/res.stamp
	@mkdir -p build
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

build/kernel: build/kernel.o build/llm.o linker.ld
	$(LD) $(LDFLAGS) build/kernel.o build/llm.o -o $@

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
# the BUF_MAX_W x BUF_MAX_H back buffer in src/kernel.c works:
#     make run RES=1920x1080x32

# make compares timestamps, and a variable has none — without recording
# RES somewhere on disk, changing it would leave the previous mode baked
# into an ISO that looks up to date.
#
# EXTRA is recorded for the same reason and it bites harder, because the
# symptom is silence: `make iso EXTRA=-DSTORAGE_SELFTEST` after an
# ordinary build leaves kernel.o untouched, produces an ISO with no
# self-test in it, and says nothing at all. The tests then "pass" by not
# running.
build/res.stamp: FORCE
	@mkdir -p build
	@echo '$(RES) $(FB_MAX_W)x$(FB_MAX_H)' | cmp -s - $@ || \
	  echo '$(RES) $(FB_MAX_W)x$(FB_MAX_H)' > $@

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
