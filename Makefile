CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
HOSTCC  := cc

CFLAGS  := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
            -fno-stack-check -fno-lto -fPIE -m64 -march=x86-64 \
            -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
            -Isrc -Ikernel/include

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
# Seeded onto the disk under /store/pkg, which is the repository the
# Agora store installs from.  `voronoi` is deliberately left out so it
# is only reachable through the network repository (see `make repo`).
STORE_APPS  := mandel orbit life plasma
REPO_APPS   := $(STORE_APPS) voronoi
STORE_BINS  := $(addprefix build/store/,$(STORE_APPS))
REPO_BINS   := $(addprefix build/store/,$(REPO_APPS))

.PHONY: all iso run clean cleandisk apps repo

all: os.iso disk.img

apps: $(REPO_BINS)

# --- FAT32 system disk ---
# Created once and then left alone: it is the OS's writable, persistent
# filesystem. `make cleandisk` resets it to factory contents.
disk.img: | build/hello $(STORE_BINS)
	python3 tools/mkfat32.py disk.img 64 \
		apps/about.txt apps/notes.txt build/hello \
		apps/welcome.txt:docs/welcome.txt \
		$(foreach a,$(STORE_APPS),build/store/$(a):store/pkg/$(a).elf)

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

# --- Store apps: standalone ELF64 canvas apps ---
build/store/%.o: apps/store/%.c apps/socrates.h
	@mkdir -p build/store
	$(CC) $(APP_CFLAGS) -c $< -o $@

build/store/%: build/store/%.o apps/app.ld
	$(LD) -nostdlib -static -T apps/app.ld $< -o $@

# --- Ramdisk: tar archive of apps/ text files + compiled binaries ---
# The store payloads ride along here too, so the storefront still has
# something to install on an ISO-only boot with no disk attached.
build/initrd.tar: $(wildcard apps/*.txt) build/hello $(STORE_BINS)
	@mkdir -p build/initrd_staging/store/pkg
	cp apps/*.txt build/initrd_staging/ 2>/dev/null || true
	cp build/hello build/initrd_staging/
	$(foreach a,$(STORE_APPS),cp build/store/$(a) build/initrd_staging/store/pkg/$(a).elf;)
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
run: os.iso disk.img
	@echo ""
	@echo "  [TIP] Toggle full-screen on/off at any time with: Ctrl + Alt + F"
	@echo ""
	qemu-system-x86_64 \
		-cdrom os.iso \
		-drive file=disk.img,format=raw,index=0,media=disk \
		-m 256M \
		-vga std \
		-display sdl,show-cursor=off,grab-mod=lshift-lctrl-lalt \
		-full-screen \
		-boot d \
		-netdev user,id=net0,net=10.0.2.0/24 \
		-device e1000,netdev=net0

clean:
	rm -rf build os.iso \
		$(ISO)/boot/kernel \
		$(ISO)/boot/initrd.tar \
		$(ISO)/boot/boot_anim.raw \
		$(ISO)/boot/limine \
		$(ISO)/EFI \
		kernel/include/boot_animation.h
