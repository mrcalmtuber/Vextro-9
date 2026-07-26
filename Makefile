CC      := x86_64-elf-gcc
LD      := x86_64-elf-ld
HOSTCC  := cc

CFLAGS  := -O2 -Wall -Wextra -ffreestanding -fno-stack-protector \
            -fno-stack-check -fno-lto -fPIE -m64 -march=x86-64 \
            -mno-80387 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone \
            -Isrc -Ikernel/include

LDFLAGS := -nostdlib -static -pie --no-dynamic-linker -z text \
            -T linker.ld

APP_CFLAGS := -O2 -Wall -ffreestanding -fno-stack-protector \
              -fno-stack-check -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
              -mno-red-zone -fPIC

LIMINE  := limine-binary
ISO     := iso_root

.PHONY: all iso run clean cleandisk

all: os.iso disk.img

# --- FAT32 system disk ---
# Created once and then left alone: it is the OS's writable, persistent
# filesystem. `make cleandisk` resets it to factory contents.
disk.img: | build/hello
	python3 tools/mkfat32.py disk.img 64 \
		apps/about.txt apps/notes.txt build/hello \
		apps/welcome.txt:docs/welcome.txt

cleandisk:
	rm -f disk.img
	$(MAKE) disk.img

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

# --- Ramdisk: tar archive of apps/ text files + compiled binaries ---
build/initrd.tar: $(wildcard apps/*.txt) build/hello
	@mkdir -p build/initrd_staging
	cp apps/*.txt build/initrd_staging/ 2>/dev/null || true
	cp build/hello build/initrd_staging/
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
