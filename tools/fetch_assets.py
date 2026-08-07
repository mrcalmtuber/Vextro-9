#!/usr/bin/env python3
"""
Fetch the encyclopedia and the language model.

These two files are what make the machine interesting, and they are the
two things a clone of this repository cannot contain. wiki.zim is about
980 MB and qwen2.gguf about 380 MB; GitHub refuses any blob over 100 MB
and Git stores no delta worth having for compressed data, so committing
them would break the repository rather than furnish it. Git LFS moves the
problem to a quota rather than solving it.

So they are downloaded from where they are published, once, and written
into disk.img. `make disk.img` runs this, which is what makes a fresh
clone come up with an encyclopedia and a model rather than without them.

Both are optional. The system boots and runs without either: the
Wikipedia window says there is no archive, and the AI prompt after login
declines itself. Skipping is a supported outcome, not a broken install.

Usage:
    tools/fetch_assets.py [--dest DIR] [--only zim|model] [--yes]

Environment overrides, for a mirror or a different model:
    VEXTRO_ZIM_URL, VEXTRO_MODEL_URL
"""
import argparse
import os
import sys
import urllib.request
import urllib.error

# Simple English Wikipedia, no pictures: the whole encyclopedia in under a
# gigabyte, which is the only reason a full copy is practical here.
#
# Kiwix dates its filenames -- wikipedia_en_simple_all_nopic_2026-05.zim --
# so a pinned URL stops working the month after it is written. The index
# is listed and the newest build taken, which is what a person would do.
ZIM_DIR = "https://download.kiwix.org/zim/wikipedia/"
ZIM_PATTERN = r'href="(wikipedia_en_simple_all_nopic_[0-9]{4}-[0-9]{2}\.zim)"'


def newest_zim():
    """The most recent dated build, or None if the index cannot be read."""
    import re
    try:
        req = urllib.request.Request(ZIM_DIR,
                                     headers={"User-Agent": "vextro/9"})
        with urllib.request.urlopen(req, timeout=60) as r:
            index = r.read().decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        print(f"  could not list {ZIM_DIR}: {e}", file=sys.stderr)
        return None
    names = sorted(set(re.findall(ZIM_PATTERN, index)))
    return ZIM_DIR + names[-1] if names else None


ZIM_URL = os.environ.get("VEXTRO_ZIM_URL")

# Qwen2 0.5B Instruct, Q5_0. Small enough to load in seconds and to run a
# token at a time inside a render loop; the quantisations this kernel
# decodes are Q4_K, Q5_0, Q6_K and Q8_0.
MODEL_URL = os.environ.get(
    "VEXTRO_MODEL_URL",
    "https://huggingface.co/Qwen/Qwen2-0.5B-Instruct-GGUF/resolve/main/"
    "qwen2-0_5b-instruct-q5_0.gguf",
)

# What is wanted, and the smallest size that could plausibly be the real
# thing rather than an error page served with a 200.
WANTED = [
    ("wiki.zim",   200 * 1024 * 1024, "Simple English Wikipedia (~980 MB)"),
    ("qwen2.gguf", 100 * 1024 * 1024, "Qwen2 0.5B Instruct, Q5_0 (~380 MB)"),
]


def source(name):
    """Where one of them comes from.

    Kept separate from WANTED, and called only for a file that is actually
    missing: resolving the encyclopedia's URL means listing Kiwix's index,
    and a build that already has both files should not touch the network
    at all. This runs on every make now, so that matters.
    """
    if name == "wiki.zim":
        return ZIM_URL or newest_zim()
    return MODEL_URL


def human(n):
    for unit in ("B", "KiB", "MiB", "GiB"):
        if n < 1024 or unit == "GiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024.0


def fetch(url, dest, min_size, label):
    """Download to a temporary name and rename only on success.

    Renaming last is what makes this safe to interrupt: a half-written
    file never takes the real name, so a second run does not mistake it
    for a complete one and skip it.
    """
    tmp = dest + ".part"
    print(f"  {label}\n    {url}")

    try:
        req = urllib.request.Request(url, headers={"User-Agent": "vextro/9"})
        with urllib.request.urlopen(req) as r, open(tmp, "wb") as out:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            while True:
                chunk = r.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
                got += len(chunk)
                if total:
                    pct = got * 100 // total
                    print(f"\r    {pct:3d}%  {human(got)} of {human(total)}",
                          end="", flush=True)
                else:
                    print(f"\r    {human(got)}", end="", flush=True)
        print()
    except (urllib.error.URLError, OSError) as e:
        print(f"\n    failed: {e}", file=sys.stderr)
        if os.path.exists(tmp):
            os.unlink(tmp)
        return False

    size = os.path.getsize(tmp)
    if size < min_size:
        # Almost always an HTML error page served with a 200, which is
        # why the check is on size rather than on the status code.
        print(f"    refusing: got {human(size)}, expected far more -- "
              f"the URL probably returned an error page", file=sys.stderr)
        os.unlink(tmp)
        return False

    os.replace(tmp, dest)
    print(f"    saved {dest} ({human(size)})")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default="assets",
                    help="where to put the files (default: assets/)")
    ap.add_argument("--only", choices=["zim", "model"],
                    help="fetch just one of them")
    ap.add_argument("--yes", action="store_true",
                    help="do not ask before downloading over a gigabyte")
    ap.add_argument("--ask-again", action="store_true",
                    help="ask even if a previous run was declined")
    args = ap.parse_args()

    os.makedirs(args.dest, exist_ok=True)

    # Saying no is remembered. This runs on every make now -- it has to,
    # so that a disk built before the encyclopedia arrived is noticed --
    # and re-asking a question already answered would be worse than the
    # bug that made it necessary.
    declined = os.path.join(args.dest, ".declined")
    if args.ask_again and os.path.exists(declined):
        os.unlink(declined)

    want = WANTED
    if args.only == "zim":
        want = [a for a in want if a[0] == "wiki.zim"]
    elif args.only == "model":
        want = [a for a in want if a[0] == "qwen2.gguf"]

    # Decided from the filesystem alone, before any URL is resolved.
    todo = []
    for name, min_size, label in want:
        path = os.path.join(args.dest, name)
        if os.path.exists(path) and os.path.getsize(path) >= min_size:
            print(f"  {name}: already here ({human(os.path.getsize(path))})")
            continue
        todo.append((name, min_size, label, path))

    if not todo:
        print("  nothing to fetch")
        return 0

    if os.path.exists(declined) and not args.yes:
        print("  assets: declined earlier — `make assets` to change your mind")
        return 0

    print("\nThese are large, and they are optional -- the system boots and")
    print("runs without them. Skipping is a supported outcome.\n")
    for _, _, label, _ in todo:
        print(f"  - {label}")

    if not args.yes and sys.stdin.isatty():
        try:
            if input("\nDownload now? [y/N] ").strip().lower() not in ("y", "yes"):
                print("skipped; run `make assets` later to change your mind")
                open(declined, "w").close()
                return 0
        except EOFError:
            return 0

    ok = True
    for name, min_size, label, path in todo:
        url = source(name)
        if not url:
            print(f"  {name}: no build found; set VEXTRO_ZIM_URL to choose one",
                  file=sys.stderr)
            ok = False
            continue
        if not fetch(url, path, min_size, label):
            ok = False

    if not ok:
        print("\nSome assets did not arrive. The build still works; the")
        print("Wikipedia window will report no archive and the AI prompt")
        print("after login will have nothing to load.")
    return 0        # never fail the build over an optional download


if __name__ == "__main__":
    sys.exit(main())
