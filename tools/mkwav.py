#!/usr/bin/env python3
"""Generate the sample tracks the media player ships with.

Uncompressed 16-bit PCM in a RIFF/WAVE container, because that is what
the system can actually decode -- there is no MP3 or AAC decoder in this
kernel and the README says so.

Kept deliberately short. These live on the volume, and a minute of 48 kHz
stereo is 11 MB of repository for something whose only job is to prove the
audio path works end to end.

Stdlib only, like every other tool here.
"""
import math
import struct
import sys
import os

RATE = 48000


def wav(path, frames):
    """frames: list of (left, right) ints in -32768..32767"""
    body = b''.join(struct.pack('<hh', l, r) for l, r in frames)
    with open(path, 'wb') as f:
        f.write(b'RIFF')
        f.write(struct.pack('<I', 36 + len(body)))
        f.write(b'WAVEfmt ')
        f.write(struct.pack('<IHHIIHH', 16, 1, 2, RATE, RATE * 4, 4, 16))
        f.write(b'data')
        f.write(struct.pack('<I', len(body)))
        f.write(body)
    return len(body) + 44


def envelope(i, n):
    """Short attack, long decay -- a flat note ends on a click, and a
    click is exactly what a listener notices when nothing else is wrong."""
    attack = int(RATE * 0.01)
    if i < attack:
        return i / attack
    t = (i - attack) / max(1, n - attack)
    return math.exp(-3.0 * t)


def note(freq, seconds, amp=0.22, detune=0.0):
    n = int(RATE * seconds)
    out = []
    for i in range(n):
        e = envelope(i, n)
        # two partials: a fundamental and a quiet octave, so it reads as a
        # tone rather than a test signal
        v = math.sin(2 * math.pi * freq * i / RATE)
        v += 0.3 * math.sin(4 * math.pi * freq * i / RATE)
        s = int(32767 * amp * e * v / 1.3)
        if detune:
            vr = math.sin(2 * math.pi * (freq + detune) * i / RATE)
            sr = int(32767 * amp * e * vr)
            out.append((s, sr))
        else:
            out.append((s, s))
    return out


def chime():
    """A rising arpeggio: A4, C#5, E5, A5."""
    seq = [(440.0, 0.35), (554.37, 0.35), (659.25, 0.35), (880.0, 0.9)]
    frames = []
    for f, d in seq:
        frames.extend(note(f, d))
    return frames


def sweep():
    """A slow sweep, which makes a resampling or rate bug audible in a way
    a single tone does not."""
    n = int(RATE * 2.0)
    frames = []
    phase = 0.0
    for i in range(n):
        f = 220.0 + (1760.0 - 220.0) * (i / n)
        phase += 2 * math.pi * f / RATE
        e = envelope(i, n) if i > n * 0.8 else 1.0
        s = int(32767 * 0.18 * e * math.sin(phase))
        frames.append((s, s))
    return frames


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'build/music'
    os.makedirs(outdir, exist_ok=True)
    for name, gen in (('chime.wav', chime), ('sweep.wav', sweep)):
        p = os.path.join(outdir, name)
        n = wav(p, gen())
        print('%s (%d bytes)' % (p, n))


if __name__ == '__main__':
    main()
