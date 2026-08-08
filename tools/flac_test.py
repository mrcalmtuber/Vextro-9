#!/usr/bin/env python3
"""Round-trip the FLAC decoder against the reference encoder.

For each generated signal: write a WAV, compress it with `flac`, decode
the result with src/flac.h, and require the samples to come back
*identical*. FLAC is lossless, so anything short of exact is a bug.

The signals are chosen to reach different parts of the decoder rather
than to sound like anything:

  silence      CONSTANT subframes
  full-scale   the clipping edges, and wasted-bits handling
  ramp         FIXED predictors, which model a straight line exactly
  tone         LPC, which is what a periodic signal compresses to
  noise        the Rice escape, where a partition is stored raw
  mono         the single-channel path
  8 and 24 bit the depth conversions

    python3 tools/flac_test.py
"""
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

RATE = 44100


def wav(path, frames, ch, bits):
    """frames: flat list of ints, already interleaved."""
    if bits == 8:
        body = bytes((v + 128) & 0xFF for v in frames)       # WAV 8-bit is unsigned
    elif bits == 16:
        body = b''.join(struct.pack('<h', v) for v in frames)
    else:
        body = b''.join(struct.pack('<i', v << 8)[1:] for v in frames)
    ba = ch * bits // 8
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(body)) + b'WAVEfmt ')
        f.write(struct.pack('<IHHIIHH', 16, 1, ch, RATE, RATE * ba, ba, bits))
        f.write(b'data' + struct.pack('<I', len(body)) + body)
    return body


def signals():
    n = 8000
    lim = 32767
    yield 'silence', [0] * (n * 2), 2, 16
    yield 'full scale', [(lim if (i // 50) % 2 else -lim - 1)
                         for i in range(n * 2)], 2, 16
    yield 'ramp', [((i * 7) % 65536) - 32768 for i in range(n * 2)], 2, 16

    tone = []
    for i in range(n):
        l = int(20000 * math.sin(2 * math.pi * 440 * i / RATE))
        r = int(18000 * math.sin(2 * math.pi * 660 * i / RATE))
        tone += [l, r]
    yield 'tone (stereo)', tone, 2, 16

    random.seed(7)
    yield 'noise', [random.randint(-32768, 32767) for _ in range(n * 2)], 2, 16

    yield 'tone (mono)', [int(15000 * math.sin(2 * math.pi * 330 * i / RATE))
                          for i in range(n)], 1, 16

    yield '8-bit', [int(100 * math.sin(2 * math.pi * 220 * i / RATE))
                    for i in range(n * 2)], 2, 8

    yield '24-bit', [int(4000000 * math.sin(2 * math.pi * 500 * i / RATE))
                     for i in range(n * 2)], 2, 24

    # mid/side is what the encoder picks when the channels are related
    corr = []
    for i in range(n):
        v = int(12000 * math.sin(2 * math.pi * 300 * i / RATE))
        corr += [v, v + (i % 3)]
    yield 'correlated (mid/side)', corr, 2, 16


def main():
    if subprocess.run(['which', 'flac'], capture_output=True).returncode:
        print('the reference `flac` encoder is not installed')
        return 2

    tmp = tempfile.mkdtemp(prefix='flactest')
    fails = 0
    total = 0

    for name, frames, ch, bits in signals():
        total += 1
        w = os.path.join(tmp, 'a.wav')
        c = os.path.join(tmp, 'a.flac')
        r = os.path.join(tmp, 'a.raw')
        wav(w, frames, ch, bits)
        if os.path.exists(c):
            os.unlink(c)
        # -8 is the densest setting: highest LPC orders, most partitions
        p = subprocess.run(['flac', '-8', '-s', '-o', c, w],
                           capture_output=True)
        if p.returncode:
            print('  %-24s ENCODER FAILED %s' % (name, p.stderr[:120]))
            fails += 1
            continue

        p = subprocess.run(['./build/flac_test', c, r], capture_output=True,
                           text=True)
        if p.returncode:
            print('  %-24s DECODE FAILED  %s' % (name, p.stderr.strip()))
            fails += 1
            continue

        got = open(r, 'rb').read()
        mine = list(struct.unpack('<%dh' % (len(got) // 2), got))

        # what the decoder should have produced: the original samples,
        # converted to 16-bit stereo the same way it converts them
        want = []
        for i in range(0, len(frames), ch):
            l = frames[i]
            rr = frames[i + 1] if ch == 2 else l
            if bits == 8:
                l, rr = l << 8, rr << 8
            elif bits == 24:
                l, rr = l >> 8, rr >> 8
            want += [l, rr]

        if mine == want:
            ratio = os.path.getsize(c) * 100 // max(os.path.getsize(w), 1)
            print('  %-24s %6d frames  exact   (%d%% of WAV)'
                  % (name, len(want) // 2, ratio))
        else:
            fails += 1
            bad = next((i for i, (a, b) in enumerate(zip(mine, want))
                        if a != b), min(len(mine), len(want)))
            print('  %-24s MISMATCH at sample %d of %d (got %d, want %d), '
                  'lengths %d vs %d'
                  % (name, bad, len(want),
                     mine[bad] if bad < len(mine) else 0,
                     want[bad] if bad < len(want) else 0,
                     len(mine), len(want)))

    print('%d/%d exact' % (total - fails, total))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
