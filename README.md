<h1 align="center">Vextro 9</h1>

<p align="center">
  <b>A desktop operating system written from nothing.</b><br>
  No libc. No kernel to fork from. No graphics library, no font renderer,<br>
  no network stack, no decompressor, no inference runtime — all of it is in this repository.
</p>

<p align="center">
  <img alt="x86_64" src="https://img.shields.io/badge/arch-x86__64-1f2430?style=flat-square">
  <img alt="bare metal" src="https://img.shields.io/badge/target-bare%20metal-d4af37?style=flat-square">
  <img alt="no libc" src="https://img.shields.io/badge/libc-none-1f2430?style=flat-square">
  <img alt="lines" src="https://img.shields.io/badge/kernel-29k%20lines%20of%20C-1f2430?style=flat-square">
  <a href="../../releases"><img alt="releases" src="https://img.shields.io/badge/download-ISO-d4af37?style=flat-square"></a>
  <img alt="license" src="https://img.shields.io/badge/license-Apache--2.0-1f2430?style=flat-square">
  <a href="https://github.com/mrcalmtuber/vextro-arm64"><img alt="arm64 port" src="https://img.shields.io/badge/also%20on-aarch64-d4af37?style=flat-square"></a>
</p>

<p align="center">
  <img src="docs/desktop.png" width="90%" alt="The Vextro 9 desktop">
</p>

> **There is an ARM64 port** —
> **[vextro-arm64](https://github.com/mrcalmtuber/vextro-arm64)**.
> Same desktop, same browser, same Wikipedia, same language model, on
> aarch64: virtio devices, a GICv2 and the generic timer instead of the PIC
> and the PIT, and Raspberry Pi 4 drivers. On Apple silicon it runs under
> hardware virtualisation rather than emulation, which is the difference
> between a language model answering in seconds and in minutes. That
> repository documents the machine layer; this one documents the system.

---

## Building it

A bare-metal kernel cannot be built with the compiler that targets your
own operating system, so this needs an **`x86_64-elf` cross toolchain**.
On macOS that is four Homebrew formulae:

```
brew install x86_64-elf-gcc x86_64-elf-binutils xorriso qemu
```

On Linux, `xorriso` and `qemu-system-x86` are packaged everywhere; the
cross toolchain generally is not, and `gcc-x86-64-linux-gnu` is *not* a
substitute — it targets Linux rather than bare metal. Build one, or, if
your host toolchain already emits ELF, `make CC=gcc LD=ld` may do.

`python3` builds the disk images and fetches the assets. Nothing else is
needed — the boot animation is computed by the kernel rather than decoded
from a video, so there is no ffmpeg and no media file in the repository.
`make` names everything missing at once rather than stopping at the first
one.

You will want about **11 GB free**: an 8 GB sparse volume plus 1.4 GB of
downloads.

```
git clone https://github.com/mrcalmtuber/vextro
cd vextro
make            # builds the ISO and the 8 GB volume
make run
```

The first build offers to fetch the two things the repository cannot
carry: **Simple English Wikipedia** (~980 MB) and a **Qwen2 0.5B**
language model (~380 MB). GitHub refuses any file over 100 MB, so they are
downloaded from Kiwix and Hugging Face and written into `disk.img`.

Both are optional and the download never fails the build:

```
make ASSETS=1     # take them without asking (what CI wants)
make ASSETS=0     # skip entirely
make assets       # fetch later, or after saying no
```

Without them the system still boots and runs — the Wikipedia window
reports no archive, and the prompt after login has nothing to offer. Say
no now and `make assets` later, and the next `make` notices and rebuilds
the volume around them; nothing has to be cleaned by hand.

---

## The short version

It boots on a real machine, draws a windowed desktop with anti-aliased type,
talks to the internet over its own TCP/IP stack, reads a complete offline
Wikipedia, installs applications from a package store, and **runs a
transformer language model on the CPU** — all with no operating system
underneath it and no C library beside it.

Every layer that a normal application takes for granted had to be built
first. There is a TrueType rasteriser because there was no way to draw a
letter. There is a Zstandard decompressor because Wikipedia archives are
compressed with it. There is an NVMe driver because a modern machine has
nowhere else to keep a 900 MB encyclopedia.

---

## First light

<p align="center">
  <img src="docs/boot.png" width="90%" alt="The boot animation: the dragon breathing fire, and the burn front eating the screen">
</p>

**The boot animation is not a video.** The dragon off the desktop wallpaper
draws breath and sets fire to the screen, the screen burns away to nothing,
and then you log in.

It is the same dragon. `wall_dragon()` takes a centre and a scale, so the
wallpaper draws it full size in the middle and the boot draws it half size
and left of centre, from one set of polygons — rather than a second dragon
that has to be kept in step with the first.

Two fields do the work, and they are deliberately different mechanisms
because they are different things:

**The fire is advected.** Each cell pulls heat from the cell to its left and
the ones below, so the flame streams away from the mouth and rises. The mix
shifts over the sequence: the jet leaves the mouth almost flat, and once the
breath stops what is left of it stands up and gutters out.

**The burn is a front.** Fire is not what destroys the screen — what fire
leaves behind is — so the char is a separate field that ignites where the
jet lands and then eats outward on its own, ember rim ahead of cold char. It
spreads in a distance metric squashed hard downrange, so it runs out ahead
of itself the way the breath went; a front that spreads evenly is a circle,
and a circle expanding out of a dragon's mouth reads as a shockwave rather
than as something catching light.

Neither needs a square root. The front compares squared distance against a
squared radius, perturbed per cell by a tiled noise field, which is what
makes its edge ragged. Integer throughout, on the same 360-entry sine table
the login screen uses, because this runs *before* the floating-point unit is
switched on — before the interrupt table exists, before a single driver has
been probed.

It used to be a recording, and recordings are heavy: 121 frames of 320×240
RGB565 is **18.5 MB of raw pixels**, linked into the kernel *and* copied into
the ISO, generated from a 6.8 MB `.mp4` by ffmpeg — a build dependency
nothing else here needed.

| | before | after |
|---|---:|---:|
| ISO | 40 MB | **4.6 MB** |
| in the repository | 6.8 MB of video | **none** |
| ffmpeg | required | **not used at all** |

`src/bootanim.h` is 373 lines and byte-identical on both architectures.

---

## What it looks like

<table>
<tr>
<td width="50%"><img src="docs/terminal.png" alt="Terminal"></td>
<td width="50%"><img src="docs/browser.png" alt="Browser"></td>
</tr>
<tr>
<td><b>Shell</b> — a real filesystem, and <code>ping</code> and
<code>fetch</code> going out over a TCP/IP stack that is 1,200 lines of
this repository.</td>
<td><b>Browser</b> — <code>info.cern.ch</code>, the first website, fetched
and rendered on bare metal. HTTP only: there is no TLS, because there are
no secrets on a machine with no users.</td>
</tr>
<tr>
<td><img src="docs/wikipedia.png" alt="Wikipedia search"></td>
<td><img src="docs/article.png" alt="An article"></td>
</tr>
<tr>
<td><b>Offline Wikipedia</b> — live prefix search across
<b>399,853 entries</b>, answered by binary search over the archive's sorted
path list. About twenty reads, and it barely slows as the archive grows.</td>
<td><b>Articles</b> render through the browser at <code>zim://</code>, with
internal links resolved back into the archive — so you can read from one
article to the next, with Back working.</td>
</tr>
<tr>
<td><img src="docs/store.png" alt="Agora app store"></td>
<td><img src="docs/store-app.png" alt="An installed app running"></td>
</tr>
<tr>
<td><b>Agora</b> — a package store. Install writes a validated payload to
the system volume and records it in a registry, so installed apps survive
a reboot.</td>
<td><b>…and they run.</b> Installed apps join the dock and open in their own
window. This Mandelbrot is 16.16 fixed point — the kernel is compiled with
no FPU at all.</td>
</tr>
<tr>
<td><img src="docs/photos.png" alt="Photos"></td>
<td><img src="docs/login.png" alt="Login"></td>
</tr>
<tr>
<td><b>Photos</b> — a custom image format: PNG-style row prediction filters
over an LZMA stream. The status bar is not marketing, it is the file:
<b>623 KB → 12 KB</b>.</td>
<td><b>Login</b> — the keycode is stored on the volume, so the machine
remembers you. A wrong one melts the screen.</td>
</tr>
</table>

---

## Things that are more interesting than they sound

### A language model, on the metal

`src/llm.c` is a complete transformer: byte-level BPE tokeniser, GGUF
parsing, dequantisation for every weight type the model uses, and the
forward pass. Drop a Qwen2 GGUF on the volume and the Wikipedia app grows a
chat panel that retrieves an article and answers from it.

<p align="center">
  <img src="docs/llm.png" width="88%" alt="The transformer running a token at a time">
</p>

The prediction sharpens as context arrives — `The` → ` following`,
`The capital` → ` city`, `The capital of France` → ` is`, and then
**` Paris`**. That is a real forward pass, not a lookup: 24 layers, 14 query
heads over 2 key/value heads, a 151,936-token vocabulary, and 373 MB of
weights resident, on a machine with no libc and no GPU.

And it is not just predicting tokens in the abstract — the Wikipedia window
puts the whole thing together:

<p align="center">
  <img src="docs/chat.png" width="88%" alt="The Wikipedia window answering a question about photosynthesis from a retrieved article">
</p>

Asked *what is photosynthesis*, it pulled the distinctive words out of the
question, binary-searched 399,853 sorted titles, read
`[context: Photosynthesis]` off a 980 MB archive, and answered from that
article — retrieval-augmented generation with no index, no database and no
network. The wording is the model's own, mangled grammar and all; a 0.5B
model running a token at a time on bare metal sounds like that, and tidying
it up here would misrepresent it.

That capture is from the **arm64** build under hardware virtualisation,
which is the honest reason the port exists. The same question on the x86_64
build under full emulation was still consuming the prompt five minutes
later.

The model loads **by itself, in the background**, while the desktop stays
live — there is nothing to type and nothing to wait for. Q4_K
dequantisation is checked against a reference implementation rather than
eyeballed, and `llm probe` compares intermediate tensors to the digit.

It is the one translation unit in the build allowed to touch the FPU.
Everything else is compiled `-mno-sse -mno-80387`, so no interrupt handler
can quietly acquire a floating-point dependency.

### Wikipedia, offline, from the raw archive

`src/zim.h` reads Kiwix ZIM files straight off the disk, a window at a
time — nothing is loaded whole, and only one decompressed cluster is ever
in memory. Since 2021 those archives are Zstandard, which meant writing
`src/zstd.h`: FSE/tANS, Huffman, sequence reconstruction with repeat
offsets, from RFC 8878. Older xz clusters work too, through a complete
LZMA2 decoder in `src/lzma.h`.

Verified against a real 937 MB Simple English dump: the kernel lands on the
same clusters and byte counts as a host-side reference run.

### Storage that works on hardware built this decade

Three buses, probed newest first: **NVMe**, **AHCI/SATA**, and legacy ATA
PIO. A machine bought in the last ten years has no IDE controller at all,
so without the first two the OS boots and finds nothing.

Two details that are easy to skip and expensive to get wrong:

- **Physical contiguity.** A DMA engine is handed physical addresses, and a
  buffer that is one array to C is one array to the device only if its
  pages happen to be adjacent. They are, under this bootloader — which is
  exactly how a driver works on the machine it was written on and corrupts
  memory on the next one. Buffers are resolved page by page and coalesced.
- **Block size.** NVMe namespaces are increasingly not 512 bytes, and every
  filesystem above expects that they are. The translation includes the
  read-modify-write a partial write needs; without it, writing one sector
  on a 4K drive destroys the seven either side, silently.

### Type, without a font library

`src/ttf.h` is an integer-only TrueType rasteriser — glyph outlines, 8×8
supersampled anti-aliasing, no floats and no GPU. Baselines and glyph
origins snap to whole pixels (leave them fractional and a 13px baseline
lands on a half-pixel and fringes every letter), and each glyph's coverage
mask is cached per size rather than re-rasterised every frame, which is
what makes the finer sampling affordable at 60 fps.

### Its own executable format

Store packages are `.bsd` images: an 80-byte header, page-separated text
and data, no relocations. The same `bsd_validate()` runs in the host
packer, the kernel loader and the store's download path — one validator,
three consumers, so a payload cannot be accepted by one and rejected by
another.

`bsdfmt/bsd_run.c` is a POSIX loader for the same format, which means an
image can be checked on your laptop before it is ever handed to the kernel.
Text and data never share a page, so `mprotect()` can give them different
protections and **W^X holds** — and on hosts with pages coarser than 4 KB
it refuses rather than silently mapping RWX.

### A GPU driver that reports its own failures

`src/igpu.h` drives the Intel Gen9 blitter: a private GGTT window, the BCS
ring in legacy submission mode, `XY_COLOR_BLT` packets, and a CPU-verified
self-test at boot. When a submission's breadcrumb never lands it latches
the i915-style error state — EIR/ESR, the exact command header that broke
the pipeline decoded by name, ACTHD, INSTDONE, GGTT faults, the ring
contents around the parse point — attempts an engine reset, and falls back
to CPU rendering after repeated hangs. `gpu error` prints the report.

---

## Try it

```sh
make          # builds the ISO and an 8 GB sparse exFAT system disk
make run      # QEMU, full screen, networking up
```

Pre-built ISOs are under [**Releases**](../../releases) if you would rather
not build a cross-compiler.

First boot asks you to choose a keycode; it is saved to the volume. Then:

```
open browser                    or click the globe in the dock
store install mandel            then look at the dock, and reboot
ping 10.0.2.2
fetch http://example.com
echo hello disk > hi.txt
mkdir projects && cp hi.txt projects/copy.txt
cat projects/copy.txt           still there after a reboot
```

**Just move the mouse.** The pointer is absolute — no click-to-grab, no
capture to escape. A PS/2 mouse can only report *relative* motion, which a
host cannot turn into a position without capturing the real cursor first,
so the guest asks for the VMware backdoor pointer and falls back to PS/2
only when there is no hypervisor to ask.

<details>
<summary><b>Building from source</b></summary>

| Tool | Why |
|------|-----|
| `x86_64-elf-gcc` / `x86_64-elf-ld` | Bare-metal cross toolchain |
| `xorriso` | ISO creation |
| `qemu-system-x86_64` | Running it |
| `python3` + `opencv-python` + `numpy` | Only if `boot.mp4` changes |

```sh
brew install x86_64-elf-gcc x86_64-elf-binutils xorriso qemu
pip3 install opencv-python numpy
make
```

`make` compiles the kernel, the userland app and the store packages (each
linked to ELF64 then repacked as `.bsd`), converts the boot video to raw
RGB565 frames, assembles `os.iso`, and creates `disk.img` — an 8 GB sparse
exFAT volume seeded with the starter files, the package repository and the
sample pictures.

`disk.img` is created **once** and then left alone, so your files survive
rebuilds. `make cleandisk` resets it to factory contents.

The display mode is 1280x800 by default, up to the back buffer's bound:

```sh
make run RES=1920x1080x32
```

`disk.img` is a standard image — mount it on your host to exchange files
with the OS, including dropping in a multi-gigabyte archive:

```sh
hdiutil attach -imagekey diskimage-class=CRawDiskImage disk.img   # macOS
```

</details>

<details>
<summary><b>Serving the package repository over the network</b></summary>

The store's **Refresh** button queries an HTTP repository. To run one:

```sh
make repo
```

That stages and serves the compiled packages on port 8000. QEMU maps the
host to `10.0.2.2`, which is the store's default repository URL, so
**Refresh** just works — it picks up `voronoi`, which is deliberately *not*
on the disk, and installing it downloads the payload over the kernel's own
TCP stack.

Point it elsewhere with `store repo <url>`.

</details>

---

## What is actually in here

**29,106 lines of C**, no libc, compiled as a single translation unit plus
one for inference. (32,052 counting the embedded typeface and the integer
sine table, which are data rather than logic.)

| | |
|---|---|
| **Desktop** | Window manager (z-order, drag, focus, shadows), menu bar, dock, five wallpaper themes, absolute pointer, scroll wheel routed to the focused window |
| **Filesystem** | Read/write exFAT with 64-bit sizes, FAT32 fallback, ustar ramdisk, MBR partitions, range reads out of files too big to buffer |
| **Storage** | NVMe, AHCI/SATA, ATA PIO — behind one 512-byte sector view |
| **Network** | IPv4, ICMP, UDP, DNS, TCP, async HTTP/1.0 with redirects; Intel e1000 driver |
| **Graphics** | Integer TrueType rasteriser, Intel Gen9 blitter with hang capture, firmware framebuffer fallback |
| **Compression** | Zstandard (RFC 8878), LZMA/LZMA2/xz, both written from the specifications |
| **Inference** | GGUF parsing, BPE tokeniser, dequantisation, transformer forward pass |
| **Userland** | `.bsd` container format, `int 0x80` syscall ABI, a package store, five shipped apps |
| **Boot** | Limine, BIOS *and* UEFI, El Torito ISO; a boot animation decoded from raw RGB565 by the kernel itself |

<details>
<summary><b>Source layout</b></summary>

```
src/
  kernel.c      Entry point, render loop, login flow, syscalls
  desktop.h     Window manager, menu bar, dock, wallpaper, loader
  term.h        Terminal: commands, history, scrollback, redirection
  browser.h     HTML renderer, navigation, links
  apps.h        Files / Settings / Photos / Wikipedia + chat / About
  store.h       Agora: catalog, installer, registry, storefront
  llm.h llm.c   Transformer inference (the one FPU translation unit)
  zim.h         ZIM archive reader (offline Wikipedia)
  zstd.h        Zstandard decompressor
  lzma.h        LZMA / LZMA2 / xz decompressor
  sci.h         Compressed image format + decoder
  ttf.h         TrueType rasteriser        gfx.h  Drawing primitives
  netstack.h    IPv4 / ICMP / UDP / DNS / TCP / HTTP
  e1000.h       Intel NIC driver
  blk.h         Block layer: one sector view over three buses
  nvme.h        NVMe: admin + I/O queues, PRP lists, 4K blocks
  ahci.h        AHCI/SATA: command lists, PRDT scatter/gather
  ata.h         ATA PIO + LBA48 (legacy fallback)
  exfat.h       exFAT read/write      fat32.h  FAT32 fallback
  pci.h         PCI enumeration, BAR sizing, MMIO mapper
  igpu.h        Intel Gen9 blitter + GPU hang capture
  keyboard.h    PS/2 keyboard         mouse.h  Pointer + wheel
  vmmouse.h     VMware backdoor (absolute pointer, no grab)
  xhci.h        USB HID — incomplete, off by default, honest about it
bsdfmt/         The .bsd executable format, its packer and a POSIX loader
apps/           Userland source, store packages, seed files, pictures
tools/          exFAT/FAT32 formatters, image and video converters,
                package repository server, QEMU test driver
limine-binary/  Pre-built bootloader
```

</details>

---

## License

Source released under the [Apache License 2.0](LICENSE).
Comic Neue is under the [SIL Open Font License 1.1](assets/OFL.txt).
Limine is [BSD 2-Clause](https://github.com/limine-bootloader/limine).

<p align="center"><sub>There is also an <a href="https://github.com/mrcalmtuber/vextro-arm64">ARM64 port</a>, which runs under hardware virtualisation on Apple silicon.</sub></p>
