CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
HOSTCC  := cc

CFLAGS  := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
            -fno-stack-check -fno-lto -fPIE -m64 -march=x86-64 \
            -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
            -Isrc -Ikernel/include -Ibsdfmt

LDFLAGS := -nostdlib -static -pie --no-dynamic-linker -z text \
            -T linker.ld

# -fno-tree-loop-distribute-patterns: without it GCC turns clear loops
# into memset calls, and there is no libc to link them against.
APP_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector \
              -fno-stack-check -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
              -mno-red-zone -fPIC -fno-tree-loop-distribute-patterns

LIMINE  := limine-binary
ISO     := iso_root

# --- App store packages ---
# Every package is a .bsd image (see bsdfmt/): compiled to ELF64, then
# repacked by bsd_maker into the container the store and the kernel's
# loader both speak.  Seeded onto the disk under /store/pkg, which is the
# repository the Agora store installs from.  `voronoi` is deliberately
# left out so it is only reachable over the network (see `make repo`).
STORE_APPS  := mandel orbit life plasma
REPO_APPS   := $(STORE_APPS) voronoi
STORE_BINS  := $(addprefix build/store/,$(addsuffix .bsd,$(STORE_APPS)))
REPO_BINS   := $(addprefix build/store/,$(addsuffix .bsd,$(REPO_APPS)))

BSD_MAKER   := bsdfmt/bsd_maker

# --- Pictures ---
# PNG in, .sci out (row filters + LZMA, see src/sci.h).  tools/mkimg.py
# decodes PNG with Python's own zlib, so there is no image dependency.
PIC_SRC     := $(wildcard apps/pics/*.png)
PIC_SCI     := $(patsubst apps/pics/%.png,build/pics/%.sci,$(PIC_SRC))
PIC_NAMES   := $(notdir $(basename $(PIC_SRC)))

.PHONY: all iso run clean cleandisk apps repo bsdtools pics

all: os.iso disk.img

apps: $(REPO_BINS)

pics: $(PIC_SCI)

bsdtools: $(BSD_MAKER) bsdfmt/bsd_run

# --- exFAT system disk ---
# exFAT rather than FAT32 because FAT32 caps a single file at 4 GB, and
# an offline encyclopedia is far past that.  The image is sparse, so an
# 8 GB volume costs only the few megabytes actually used.
#
# Created once and then left alone: it is the OS's writable, persistent
# filesystem. `make cleandisk` resets it to factory contents.
DISK_MB ?= 8192

disk.img: | build/hello $(STORE_BINS) $(PIC_SCI)
	python3 tools/mkexfat.py disk.img $(DISK_MB) \
		apps/about.txt apps/notes.txt build/hello \
		apps/welcome.txt:docs/welcome.txt \
		$(foreach a,$(STORE_APPS),build/store/$(a).bsd:store/pkg/$(a).bsd) \
		$(foreach p,$(PIC_NAMES),build/pics/$(p).sci:pics/$(p).sci)

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

# --- User app: hello ---
build/hello.o: apps/hello.c apps/socrates.h
	@mkdir -p build
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/hello: build/hello.o apps/app.ld
	$(LD) -nostdlib -static -T apps/app.ld build/hello.o -o $@

# --- .bsd toolchain (host) ---
$(BSD_MAKER): bsdfmt/bsd_maker.c bsdfmt/bsd_format.h
	$(HOSTCC) -O2 -Wall -Wextra -std=gnu11 -o $@ $<

bsdfmt/bsd_run: bsdfmt/bsd_run.c bsdfmt/bsd_format.h
	$(HOSTCC) -O2 -Wall -Wextra -std=gnu11 -o $@ $<

# --- Store apps: canvas apps compiled to ELF64, repacked as .bsd ---
# bsd.ld puts .data on its own page so the two segments can carry
# different protections; -z max-page-size stops ld padding to 2 MB.
build/store/%.o: apps/store/%.c apps/socrates.h
	@mkdir -p build/store
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/store/%.elf: build/store/%.o bsdfmt/bsd.ld
	$(LD) -nostdlib -static -z max-page-size=0x1000 -T bsdfmt/bsd.ld $< -o $@

build/store/%.bsd: build/store/%.elf $(BSD_MAKER)
	$(BSD_MAKER) -o $@ -e $<

# --- Pictures: PNG -> .sci ---
build/pics/%.sci: apps/pics/%.png tools/mkimg.py
	@mkdir -p build/pics
	python3 tools/mkimg.py -o $@ $<

# --- Ramdisk: tar archive of apps/ text files + compiled binaries ---
# The store payloads ride along here too, so the storefront still has
# something to install on an ISO-only boot with no disk attached.
build/initrd.tar: $(wildcard apps/*.txt) build/hello $(STORE_BINS)
	@mkdir -p build/initrd_staging/store/pkg
	cp apps/*.txt build/initrd_staging/ 2>/dev/null || true
	cp build/hello build/initrd_staging/
	$(foreach a,$(STORE_APPS),cp build/store/$(a).bsd build/initrd_staging/store/pkg/$(a).bsd;)
	tar --format=ustar -cf $@ -C build/initrd_staging .
	rm -rf build/initrd_staging

# --- Boot animation: video → raw RGB565 + header ---
build/boot_anim.raw kernel/include/boot_animation.h: boot.mp4 tools/convert_video.py
	@mkdir -p build kernel/include
	python3 tools/convert_video.py boot.mp4 build/boot_anim.raw kernel/include/boot_animation.h

build/boot_animation_data.o: src/boot_animation_data.S build/boot_anim.raw
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

# --- Kernel ---
build/kernel.o: src/kernel.c $(wildcard src/*.h) kernel/include/boot_animation.h
	@mkdir -p build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel: build/kernel.o build/boot_animation_data.o linker.ld
	$(LD) $(LDFLAGS) build/kernel.o build/boot_animation_data.o -o $@

# --- ISO root population ---
$(ISO)/boot/kernel: build/kernel
	@mkdir -p $(ISO)/boot/limine $(ISO)/EFI/BOOT
	cp $< $@

$(ISO)/boot/initrd.tar: build/initrd.tar
	@mkdir -p $(ISO)/boot
	cp $< $@

$(ISO)/boot/limine/limine.conf: limine.conf
	@mkdir -p $(ISO)/boot/limine
	cp limine.conf                     $(ISO)/boot/limine/limine.conf
	cp $(LIMINE)/limine-bios.sys       $(ISO)/boot/limine/
	cp $(LIMINE)/limine-bios-cd.bin    $(ISO)/boot/limine/
	cp $(LIMINE)/limine-uefi-cd.bin    $(ISO)/boot/limine/
	cp $(LIMINE)/BOOTX64.EFI          $(ISO)/EFI/BOOT/

# --- Bundle raw Seedance video asset into ISO root ---
$(ISO)/boot/boot_anim.raw: build/boot_anim.raw
	@mkdir -p $(ISO)/boot
	cp $< $@

# --- ISO image (portable El Torito, xorriso/mkisofs compatible) ---
iso: os.iso

os.iso: build/limine-tool $(ISO)/boot/kernel $(ISO)/boot/initrd.tar $(ISO)/boot/boot_anim.raw $(ISO)/boot/limine/limine.conf
	xorriso -as mkisofs \
		-R -J \
		-V "SOCRATES_BSD_9" \
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
QEMU ?= qemu-system-x86_64

QEMU_DISPLAY := $(shell d=$$($(QEMU) -display help 2>/dev/null); \
  if   echo "$$d" | grep -qx sdl;   then echo 'sdl,show-cursor=off,grab-mod=lshift-lctrl-lalt'; \
  elif echo "$$d" | grep -qx gtk;   then echo 'gtk,show-cursor=off,grab-on-hover=on'; \
  elif echo "$$d" | grep -qx cocoa; then echo 'cocoa,show-cursor=off'; \
  else echo none; fi)

# (a shell `case` cannot be used here: the ")" in its patterns would
# close make's own $(shell ...) expansion early)
QEMU_FSKEY := $(if $(findstring cocoa,$(QEMU_DISPLAY)),Ctrl + Cmd + F,Ctrl + Alt + F)

run: os.iso disk.img
	@echo ""
	@echo "  [TIP] Toggle full-screen on/off at any time with: $(QEMU_FSKEY)"
	@echo ""
	$(QEMU) \
		-cdrom os.iso \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-m 256M \
		-vga std \
		-display $(QEMU_DISPLAY) \
		-full-screen \
		-boot d \
		-netdev user,id=net0,net=10.0.2.0/24 \
		-device e1000,netdev=net0

# keep the intermediates make would otherwise delete as chained targets
.PRECIOUS: build/store/%.o build/store/%.elf

clean:
	$(MAKE) -C bsdfmt clean
	rm -rf build os.iso \
		$(ISO)/boot/kernel \
		$(ISO)/boot/initrd.tar \
		$(ISO)/boot/boot_anim.raw \
		$(ISO)/boot/limine \
		$(ISO)/EFI \
		kernel/include/boot_animation.h
