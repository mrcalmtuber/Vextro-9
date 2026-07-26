# Socrates BSD 9

A bare-metal x86_64 operating system built from scratch — custom kernel, TrueType font rasterizer, window manager, TCP/IP stack, web browser, boot animation and desktop UI, all without a libc or external OS dependencies.

---

## Features

### Desktop
- **Window manager** — z-ordered, click-to-focus windows with titlebar drag, close buttons, drop shadows and spawn animations
- **Menubar** — working Socrates / Apps menus (About, Restart, Shut Down, app launchers), live clock + date, network status indicator
- **Dock** — pictogram icons with hover tooltips, running indicators, bottom/left/right placement, adjustable size
- **Wallpaper themes** — five gradient themes with the dragon emblem, switchable live from Settings

### Filesystem
- **Writable FAT32** on a real ATA disk (`disk.img`) — files survive reboots
- ATA PIO driver (LBA28) + FAT32 driver: subdirectories, create/write/append, delete, mkdir, long-filename reading, NT case flags, FSInfo upkeep
- `disk.img` is a standard image: **mount it on your host** (macOS: `hdiutil attach -imagekey diskimage-class=CRawDiskImage disk.img`) to exchange files with the OS
- Login keycode persists on disk (`keycode.sys`) — delete it to re-register
- ustar ramdisk remains as a read-only fallback for ISO-only boots

### Terminal
- Crisp monospace grid rendering (8x8 bitmap font) with a blinking block cursor
- Command history (Up/Down), line editing (Left/Right/Home/End/Del), 240-line scrollback (PgUp/PgDn)
- Working directory (`cd` / `pwd`, shown in the prompt) and output redirection: `ls > list.txt`, `echo hi >> notes.txt`
- Commands: `help` `clear` `ls` `cat` `cd` `pwd` `rm` `mkdir` `cp` `df` `run` `echo` `date` `uptime` `mem` `net` `arp` `ping` `dns` `fetch` `open` `history` `reboot` `shutdown`

### Networking
- **Full TCP/IP stack** — IPv4, ICMP (ping), UDP, DNS resolver, polled TCP client, async HTTP/1.0 client with redirects
- **Intel E1000 NIC driver** — PCI discovery, MMIO page mapping, RX/TX descriptor rings
- Works against real websites through QEMU user networking (`ping`, `dns`, `fetch`, and the browser)

### Browser
- Loads real `http://` pages over the in-kernel TCP stack (no TLS — bare metal has no secrets)
- HTML-to-text renderer: headings, paragraphs, lists, `<pre>`, entities, word wrap
- **Clickable links**, Back/Reload, editable address bar, scrollbar, status bar with load progress
- Internal pages: `socrates://home`, `socrates://help`, `socrates://about`, `socrates://file/<name>`

### Apps
- **Files** — ramdisk explorer; double-click text files to view them in the browser
- **Goldsmith** — mouse paint app with palette, brush sizes and eraser
- **Monolith** — live system monitor (uptime, memory, pointer, TCP state, ARP cache)
- **Matrix** — falling glyph rain demo
- **hello** — userland ELF64 app rendering into its own window via `int 0x80` syscalls

### Graphics
- Portable base: the firmware (GOP/VESA) linear framebuffer — works on any GPU vendor, any VM
- **Intel Gen9/9.5 iGPU driver** (Skylake → Comet Lake): maps BAR0 GTTMMADR, programs a private GGTT window, brings up the BCS blitter ring in legacy submission mode, and executes `XY_COLOR_BLT` packets — with a CPU-verified self-test at boot
- Register/command encodings adapted from the Linux i915 driver; probe-then-bail structure after SerenityOS — display modesetting is deliberately left to firmware
- If no supported iGPU is present the OS silently stays on the CPU renderer (`gpu` in the terminal shows which path is live; `gpu test` blits to the visible framebuffer on real hardware)

### Core
- **Custom TrueType rasterizer** — integer-only engine rendering Comic Neue (OFL) with 4x4 supersampled AA; no floats, no GPU
- **Boot animation** — full-color video playback via raw RGB565 frames embedded at link time
- **Login screen** — first-boot keycode registration (persisted to disk) with melt animation on bad passwords
- **HAL** — IDT, PIT, PS/2 mouse + keyboard (incl. extended scancodes), ATA PIO, AC97 audio
- **Generic PCI layer** — full bus/device/function enumeration (multifunction aware), class-code lookup, BAR sizing incl. 64-bit BARs, shared page-table MMIO mapper
- **Syscall interface** — `int 0x80` gateway with a minimal userland ABI
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
| `python3` + `opencv-python` + `numpy` | Boot animation conversion (only if `boot.mp4` changes) |
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
3. Bundle the initrd (ustar tar) and assemble the bootable ISO at `os.iso`
4. Create `disk.img` — a 64 MB FAT32 system disk seeded with the starter files

`disk.img` is created **once** and then left alone so your files survive
rebuilds. `make cleandisk` resets it to factory contents.

---

## Run

```sh
make run
```

Launches in QEMU with SDL display, E1000 networking, and 256 MB RAM. Toggle full-screen with **Ctrl+Alt+F** (left-side modifiers).

First boot asks you to choose a master keycode; it is saved to disk, so
subsequent boots only ask you to log in. Once on the desktop, try:

```
open browser            (or click the globe in the dock)
ping 10.0.2.2
fetch http://example.com
echo hello disk > hi.txt
mkdir projects && cp hi.txt projects/copy.txt
cat projects/copy.txt   (still there after `reboot`)
```

---

## Project Layout

```
src/            Kernel source
  kernel.c      Entry point, render loop, login flow, syscall stubs
  desktop.h     Window manager, menubar, dock, wallpaper, ELF loader
  term.h        Terminal (commands, history, scrollback)
  browser.h     Browser (HTML renderer, navigation, links)
  apps.h        Files / Settings / Goldsmith / Monolith / Matrix / About
  gfx.h         Theme palette + drawing primitives + monospace text
  netstack.h    IPv4 / ICMP / UDP / DNS / TCP / HTTP
  fat32.h       FAT32 driver (read/write)   ata.h  ATA PIO disk driver
  pci.h         Generic PCI enumeration + BAR sizing + MMIO mapper
  igpu.h        Intel Gen9 iGPU blitter (GGTT + BCS ring + XY_COLOR_BLT)
  e1000.h       Intel NIC driver        ttf.h  TrueType rasterizer
  keyboard.h    PS/2 keyboard           mouse.h  PS/2 mouse
apps/           Userland app source + seed files for the disk
tools/          mkfat32.py (disk formatter), video converter,
                QEMU test driver (qemu_drive.py)
limine-binary/  Pre-built Limine bootloader binaries
Makefile        Top-level build system
```

---

## Download

Pre-built ISOs are available under [Releases](../../releases).

---

## License

Source code is released under the [Apache License 2.0](LICENSE).
Comic Neue font is licensed under the [SIL Open Font License 1.1](assets/OFL.txt).
Limine bootloader is [BSD 2-Clause](https://github.com/limine-bootloader/limine).
