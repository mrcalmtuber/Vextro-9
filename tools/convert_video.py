#!/usr/bin/env python3
"""
Convert a video file into a raw RGB565 binary blob + C header for kernel boot animation.

Usage:
    python3 tools/convert_video.py <input.mp4> <output.raw> <output.h>

Wants ffmpeg in PATH, and does not insist on it.

The animation is the first thing the machine draws and it is entirely
decorative -- the kernel loops over BOOT_ANIM_FRAME_COUNT frames, so zero
of them simply means it starts at the login screen instead. ffmpeg is not
otherwise a dependency of this project, and it used to be one only here,
fatally: a clone on a machine without it failed the whole build on the
boot animation and produced no operating system at all. Now it says what
is missing, writes an empty animation, and lets the build finish.
"""

import shutil
import subprocess
import struct
import sys
import os

WIDTH  = 320
HEIGHT = 240
FPS    = 24

def write_header(header_path, frame_count):
    os.makedirs(os.path.dirname(header_path) or ".", exist_ok=True)
    with open(header_path, "w") as f:
        f.write(f"""\
#ifndef BOOT_ANIMATION_H
#define BOOT_ANIMATION_H

#include <stdint.h>

#define BOOT_ANIM_W           {WIDTH}
#define BOOT_ANIM_H           {HEIGHT}
#define BOOT_ANIM_FPS         {FPS}
#define BOOT_ANIM_FRAME_COUNT {frame_count}
#define BOOT_ANIM_FRAME_BYTES ({WIDTH} * {HEIGHT} * 2)
#define BOOT_ANIM_TOTAL_BYTES ({frame_count} * {WIDTH} * {HEIGHT} * 2)

extern const uint8_t boot_anim_data[];
extern const uint8_t boot_anim_data_end[];

#endif
""")
    print(f"Header written to {header_path}")


def skip(raw_path, header_path, why):
    """No animation, but still a build.

    The .S that carries the frames does an .incbin of the raw file, so the
    file has to exist even when it is empty -- an absent one is an
    assembler error rather than an empty animation.
    """
    print(f"  boot animation: {why}")
    os.makedirs(os.path.dirname(raw_path) or ".", exist_ok=True)
    with open(raw_path, "wb"):
        pass
    write_header(header_path, 0)
    return 0


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.mp4> <output.raw> <output.h>")
        sys.exit(1)

    video_path = sys.argv[1]
    raw_path   = sys.argv[2]
    header_path = sys.argv[3]

    # Either of these means "no animation", not "no build".
    if not os.path.isfile(video_path):
        return skip(raw_path, header_path,
                    f"input video '{video_path}' not found")
    if shutil.which("ffmpeg") is None:
        return skip(raw_path, header_path,
                    "ffmpeg is not installed, so the boot animation cannot "
                    "be built\n  install it (brew install ffmpeg) and re-run "
                    "make to get the animation back")

    cmd = [
        "ffmpeg", "-y",
        "-i", video_path,
        "-vf", f"scale={WIDTH}:{HEIGHT}",
        "-r", str(FPS),
        "-pix_fmt", "rgb565le",
        "-f", "rawvideo",
        raw_path,
    ]

    print(f"Extracting frames: {WIDTH}x{HEIGHT} @ {FPS}fps RGB565LE ...")
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return skip(raw_path, header_path,
                    "ffmpeg could not read the video:\n" +
                    result.stderr.strip().splitlines()[-1])

    raw_size = os.path.getsize(raw_path)
    frame_bytes = WIDTH * HEIGHT * 2  # 2 bytes per pixel (RGB565)
    frame_count = raw_size // frame_bytes

    if raw_size % frame_bytes != 0:
        # Truncate to whole frames
        frame_count = raw_size // frame_bytes
        with open(raw_path, "r+b") as f:
            f.truncate(frame_count * frame_bytes)
        raw_size = frame_count * frame_bytes

    print(f"Generated {frame_count} frames, {raw_size} bytes ({raw_size / 1024 / 1024:.1f} MB)")

    write_header(header_path, frame_count)
    return 0

if __name__ == "__main__":
    sys.exit(main())
