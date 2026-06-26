# Socrates BSD 9

A bare-metal x86_64 operating system built from scratch — custom kernel, TrueType font rasterizer, boot animation, desktop UI, and network stack, all without a libc or external OS dependencies.

---

## Features

- **Custom TrueType Rasterizer** — integer-only engine rendering Comic Neue (OFL) directly to the framebuffer; no floating-point, no GPU required
- **Boot Animation** — full-color video playback via raw RGB565 frames embedded at link time
- **Login Screen & Desktop** — pixel-perfect UI with mouse cursor, keyboard input, and windowed desktop
- **Hardware Abstraction Layer (HAL)** — IDT, PS/2 mouse, PS/2 keyboard, PCI enumeration
- **Ramdisk (Tarfs)** — ustar-format initrd loaded by Limine, browseable with `ls` and `cat`
- **Intel E1000 NIC driver** — userland-accessible networking via custom TCP/IP stack
- **AC97 Audio driver** — PCM audio output over the Intel AC97 codec
- **Syscall interface** — int 0x80 gateway with a minimal userland ABI
- **Limine bootloader** — BIOS + UEFI dual-boot, El Torito ISO

---

## Demo

▶️ **[Watch the boot animation](boot.mp4)** — a full-color RGB565 video decoded and played by the kernel itself at boot (no GPU, no codec library).

> Sequence: boot animation → login screen → windowed desktop.

<!-- Screenshots: drop PNGs in docs/ and embed, e.g. ![desktop](docs/desktop.png) -->

---

## Requirements

| Tool | Notes |
|------|-------|
| `x86_64-elf-gcc` | Cross-compiler targeting bare-metal ELF |
| `x86_64-elf-ld` | Matching cross-linker |
| `xorriso` | ISO creation |
| `python3` + `opencv-python` + `numpy` | Boot animation conversion |
| `qemu-system-x86_64` | Running in a VM |

On macOS, install the cross-toolchain with Homebrew:

```sh
brew install x86_64-elf-gcc x86_64-elf-binutils xorriso qemu
pip3 install opencv-python numpy
```

---

## Build

```sh
make
```

This will:
1. Compile the kernel and userland `hello` app
2. Convert `boot.mp4` to raw RGB565 frames and embed them
3. Bundle the initrd (ustar tar)
4. Assemble the bootable ISO at `os.iso`

---

## Run

```sh
make run
```

Launches in QEMU with SDL display, E1000 networking, and 256 MB RAM. Toggle full-screen with **Ctrl+Alt+F** (left-side modifiers).

---

## Project Layout

```
src/          Kernel source (kernel.c, TTF, HAL, desktop, networking)
apps/         Userland app source + initrd text files
kernel/       Kernel-specific headers and generated includes
include/      Shared headers
tools/        Boot animation video converter
assets/       Comic Neue font (OFL licensed)
limine-binary/ Pre-built Limine bootloader binaries
Makefile      Top-level build system
linker.ld     Kernel linker script
limine.conf   Boot configuration
```

---

## Download

Pre-built ISOs are available under [Releases](../../releases).

---

## License

Source code is released under the [MIT License](LICENSE).  
Comic Neue font is licensed under the [SIL Open Font License 1.1](assets/OFL.txt).  
Limine bootloader is [BSD 2-Clause](https://github.com/limine-bootloader/limine).
