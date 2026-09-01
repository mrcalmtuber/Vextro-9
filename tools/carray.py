#!/usr/bin/env python3
"""tools/carray.py -- write a C byte array back out as a file.

    python3 tools/carray.py --from apps/zlib_ref.h \
                            --name zref_text_gzip_named \
                            --out build/zlibref.gz

Used to put reference data on the volume without a second copy of it in
the tree. apps/zlib_ref.h holds streams that came off the build machine's
own compressor; apps/zlibtest.c needs some of them as *files* on NTFS so
that zlib's gz* layer -- the only part of zlib that touches the operating
system -- can be driven against the real filesystem rather than a buffer.

Extracting the file from the header rather than generating both from the
source data means the array the test compares against and the bytes on
the disk cannot drift apart: they are the same bytes by construction,
which is a property the test then checks anyway.
"""

import argparse
import re
import sys


def extract(path, name):
    src = open(path, "r", encoding="utf-8").read()
    m = re.search(
        r"^\s*static\s+const\s+unsigned\s+char\s+%s\s*\[\s*\]\s*=\s*\{"
        r"(.*?)\};" % re.escape(name),
        src, re.S | re.M)
    if not m:
        sys.exit("carray.py: no array named %s in %s" % (name, path))
    body = m.group(1)
    # Refuse anything that is not a plain list of byte literals rather
    # than silently skipping it: a macro or a nested initialiser here
    # would produce a shorter file and a mystery on the machine.
    stripped = re.sub(r"0x[0-9a-fA-F]{2}|[\s,]", "", body)
    if stripped:
        sys.exit("carray.py: %s is not a plain list of byte literals" % name)
    return bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--from", dest="src", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    data = extract(a.src, a.name)
    with open(a.out, "wb") as f:
        f.write(data)
    print("  CARRAY %s (%d bytes from %s)" % (a.out, len(data), a.name))


if __name__ == "__main__":
    main()
