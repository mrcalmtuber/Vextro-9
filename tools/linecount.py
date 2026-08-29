#!/usr/bin/env python3
"""
tools/linecount.py — how much of this is actually ours.

The README carries a line count, and a number written into prose goes
stale the moment anything is added to the tree. It had drifted by
twenty-four thousand lines before anyone noticed, which is not a
rounding error -- it is most of the system.

The distinction this draws is the one that matters now that lwIP and
Mbed TLS are vendored: a quarter of a million lines live under
third_party/ and none of them were written here. Folding those into a
headline figure would inflate it sevenfold and claim credit for other
people's work, so they are counted separately and always will be.

Three categories:

  logic      C written in this repository that does something
  data       generated tables -- a typeface, a sine table -- and
             interface headers taken from a specification. Real lines,
             not authored ones.
  vendored   lwIP and Mbed TLS as published, plus the port that reaches
             them, which *is* ours and is counted apart from both.

    python3 tools/linecount.py            the summary the README quotes
    python3 tools/linecount.py --check    fail if the README disagrees
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOCAL_DIRS = ["src", "libc", "libcxx", "apps", "tools", "vxfmt", "include"]

# Real lines, but nobody sat down and wrote them.
DATA_FILES = {
    "src/comicneue_ttf.h",   # a typeface, converted to a header
    "src/sincos_lut.h",      # an integer sine table, generated
    "src/limine.h",          # the boot protocol's own header
}

# The port. Ours, but it lives on both sides of the fence.
PORT_FILES = {
    "src/lwipglue.c", "src/tlsglue.c", "src/vxport.h",
    "src/vxport_impl.h", "src/vxnet.h",
    "third_party/vxport.c",
    "third_party/lwip-port/lwipopts.h",
    "third_party/lwip-port/arch/cc.h",
    "third_party/lwip-port/arch/sys_arch.h",
    "third_party/mbedtls/vextro_config.h",
    "third_party/mbedtls/threading_alt.h",
    "third_party/wpe-port/vxwpe.c",
    "third_party/wpe-port/vxwpe.h",
    "third_party/wpe-port/pasteboard-noop.c",
    "third_party/wpe-port/include/EGL/eglplatform.h",
    "third_party/sqlite-port/vx_vfs.c",
    "third_party/freetype-port/ftoption.h",
    "third_party/freetype-port/ftmodule.h",
}

# Fetched rather than vendored, and so not counted at all.
#
# The line between the two is whether a clone of this repository contains
# it. lwIP, Mbed TLS and libwpe are in the tree, so they are reported as
# vendored upstream. WPE WebKit is forty megabytes compressed and two
# million lines; `make webkit-fetch` brings it down and .gitignore keeps
# it out, exactly as for wiki.zim and the language models. Counting a
# directory that is absent from a fresh clone would make this number
# depend on whether somebody had run a download.
# SQLite, FreeType and HarfBuzz join WPE WebKit here rather than in the
# vendored count, and for the same test: a fresh clone does not contain
# them. Each is downloaded by its own Makefile target, verified against a
# checksum recorded there, and kept out by .gitignore — 131 megabytes
# between the three, which is not something to put in a repository when
# a pinned download says the same thing.
#
# The *ports* onto them are ours and are counted: the SQLite VFS is in
# PORT_FILES above, and FreeType's two configuration headers with it.
FETCHED_PREFIXES = (
    "third_party/wpewebkit-",
    "third_party/sqlite/",
    "third_party/freetype/",
    "third_party/harfbuzz/",
)


def lines(path):
    with open(path, errors="ignore") as f:
        return sum(1 for _ in f)


def walk(top):
    """Every source file under `top`.

    The extension test grew a second half when the C++ runtime arrived.
    A standard C++ header has no extension at all -- the file is called
    `vector`, not `vector.h` -- so matching on a suffix finds the
    implementation in libcxx/src and none of the twenty-eight headers
    it exists to support. Anything with no dot in its name under
    libcxx/include is one of those.
    """
    for root, _, files in os.walk(os.path.join(ROOT, top)):
        rel_root = os.path.relpath(root, ROOT)
        for f in files:
            if f.startswith("."):
                continue
            is_source = f.endswith((".c", ".h", ".cpp"))
            is_cxx_header = ("." not in f and
                             rel_root.startswith(os.path.join("libcxx",
                                                              "include")))
            if is_source or is_cxx_header:
                full = os.path.join(root, f)
                yield os.path.relpath(full, ROOT), full


def main():
    logic = data = 0
    logic_files = 0

    for d in LOCAL_DIRS:
        if not os.path.isdir(os.path.join(ROOT, d)):
            continue
        for rel, full in walk(d):
            n = lines(full)
            if rel.replace(os.sep, "/") in DATA_FILES:
                data += n
            else:
                logic += n
                logic_files += 1

    upstream = port = 0
    upstream_files = 0
    per_lib = {}
    if os.path.isdir(os.path.join(ROOT, "third_party")):
        for rel, full in walk("third_party"):
            key = rel.replace(os.sep, "/")
            if key.startswith(FETCHED_PREFIXES):
                continue
            n = lines(full)
            if key in PORT_FILES or key.startswith("third_party/include/"):
                port += n
            else:
                upstream += n
                upstream_files += 1
                lib = key.split("/")[1]
                per_lib[lib] = per_lib.get(lib, 0) + n

    print(f"written here      {logic:>9,}  in {logic_files} files")
    print(f"  + data/interface{data:>9,}  (typeface, sine table, boot header)")
    print(f"  = local total   {logic + data:>9,}")
    print()
    print(f"vendored upstream {upstream:>9,}  in {upstream_files} files")
    for lib, n in sorted(per_lib.items()):
        print(f"    {lib:<14}{n:>9,}")
    print(f"  port to it      {port:>9,}  (ours; the third_party/ half)")

    if "--check" in sys.argv:
        readme = open(os.path.join(ROOT, "README.md"), errors="ignore").read()
        want = f"{logic:,} lines of C written here"
        if want not in readme:
            m = re.search(r"\*\*([\d,]+) lines of C written here\*\*", readme)
            says = m.group(1) if m else "nothing"
            print(f"\nREADME says {says}; the tree says {logic:,}", file=sys.stderr)
            return 1
        print("\nREADME agrees with the tree")
    return 0


if __name__ == "__main__":
    sys.exit(main())
