#!/usr/bin/env python3
"""Generate the sample tracks the media player ships with.

One track per format the system can decode, so that what ships exercises
every decoder rather than only the easy one:

    chime.wav    uncompressed 16-bit PCM
    sweep.wav    uncompressed 16-bit PCM
    bell.flac    FLAC, via the reference encoder if it is installed
    voice.wav    IMA ADPCM, ~4:1
    dial.wav     G.711 mu-law

Kept deliberately short. These live on the volume, and a minute of 48 kHz
stereo is 11 MB of repository for something whose only job is to prove the
audio path works end to end.

Stdlib only for everything but the FLAC encode, which shells out to
`flac`; without it that one track is skipped rather than faked.
"""
import math
import struct
import subprocess
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


# ---- the compressed formats ------------------------------------------

IMA_STEP = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37,
    41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173,
    190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289,
    16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
IMA_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def ima_encode(samples, block_samples=505):
    """Mono IMA ADPCM. The encoder mirrors the decoder exactly, so the
    predictor it stores is the one the decoder will reconstruct."""
    align = 4 + (block_samples - 1) // 2
    out = bytearray()
    for start in range(0, len(samples), block_samples):
        blk = samples[start:start + block_samples]
        if len(blk) < block_samples:
            blk = blk + [blk[-1]] * (block_samples - len(blk))
        pred, idx = blk[0], 0
        out += struct.pack('<hBB', pred, idx, 0)
        nibbles = []
        for s in blk[1:]:
            step = IMA_STEP[idx]
            diff = s - pred
            code = 8 if diff < 0 else 0
            if diff < 0:
                diff = -diff
            delta = 0
            for bit, mag in ((4, step), (2, step >> 1), (1, step >> 2)):
                if diff >= mag:
                    diff -= mag
                    delta |= bit
            code |= delta
            # reproduce the decoder's reconstruction
            d = step >> 3
            if delta & 1:
                d += step >> 2
            if delta & 2:
                d += step >> 1
            if delta & 4:
                d += step
            pred += -d if code & 8 else d
            pred = max(-32768, min(32767, pred))
            idx = max(0, min(88, idx + IMA_INDEX[code]))
            nibbles.append(code)
        for i in range(0, len(nibbles), 2):
            lo = nibbles[i]
            hi = nibbles[i + 1] if i + 1 < len(nibbles) else 0
            out.append(lo | (hi << 4))
        while len(out) % align:
            out.append(0)
    return bytes(out), align, block_samples


def ulaw_encode(s):
    """G.711 mu-law, the inverse of what src/adpcm.h decodes."""
    BIAS = 0x84
    sign = 0x80 if s < 0 else 0
    if s < 0:
        s = -s
    s = min(s, 32635)
    s += BIAS
    exp = 7
    mask = 0x4000
    while exp > 0 and not (s & mask):
        exp -= 1
        mask >>= 1
    man = (s >> (exp + 3)) & 0x0F
    return (~(sign | (exp << 4) | man)) & 0xFF


def wav_raw(path, body, fmt, ch, bits, align, samples_per_block=0):
    """A WAVE file whose data chunk is already encoded."""
    extra = b''
    cb = 0
    if fmt == 0x11:
        extra = struct.pack('<H', samples_per_block)
        cb = 2
    fmt_chunk = struct.pack('<HHIIHH', fmt, ch, RATE,
                            RATE * align // max(samples_per_block or 1, 1)
                            if fmt == 0x11 else RATE * ch * bits // 8,
                            align, bits)
    if cb:
        fmt_chunk += struct.pack('<H', cb) + extra
    facts = b''
    if fmt == 0x11:
        nsamp = len(body) // align * samples_per_block
        facts = b'fact' + struct.pack('<II', 4, nsamp)
    with open(path, 'wb') as f:
        payload = (b'WAVE' + b'fmt ' + struct.pack('<I', len(fmt_chunk)) +
                   fmt_chunk + facts + b'data' +
                   struct.pack('<I', len(body)) + body)
        f.write(b'RIFF' + struct.pack('<I', len(payload)) + payload)
    return len(payload) + 8


def bell():
    """A struck bell: a few inharmonic partials, which is the kind of
    signal LPC prediction handles well and a sine does not."""
    n = int(RATE * 1.6)
    out = []
    partials = ((523.25, 1.0), (1046.5, 0.5), (1567.98, 0.28),
                (2093.0, 0.16), (2637.0, 0.09))
    for i in range(n):
        t = i / RATE
        v = 0.0
        for f, a in partials:
            v += a * math.sin(2 * math.pi * f * i / RATE) * math.exp(-2.2 * a * t)
        s = int(28000 * v / 2.1)
        s = max(-32768, min(32767, s))
        out.append((s, s))
    return out


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'build/music'
    os.makedirs(outdir, exist_ok=True)
    for name, gen in (('chime.wav', chime), ('sweep.wav', sweep)):
        p = os.path.join(outdir, name)
        n = wav(p, gen())
        print('%s (%d bytes)' % (p, n))

    # FLAC, through the reference encoder: the decoder in src/flac.h is
    # checked against this same encoder, so shipping its output is the
    # honest thing to ship.
    frames = bell()
    tmp = os.path.join(outdir, '.bell.wav')
    raw = wav(tmp, frames)
    target = os.path.join(outdir, 'bell.flac')
    if os.path.exists(target):
        os.unlink(target)
    try:
        subprocess.run(['flac', '-8', '-s', '-o', target, tmp], check=True)
        print('%s (%d bytes, %d%% of %d raw)'
              % (target, os.path.getsize(target),
                 os.path.getsize(target) * 100 // raw, raw))
    except (OSError, subprocess.CalledProcessError):
        print('  (skipped bell.flac: the `flac` encoder is not installed)')
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)

    # IMA ADPCM, mono, from the sweep -- a signal whose step size has to
    # keep adapting, which is the whole point of the format.
    mono = [l for l, _ in sweep()]
    body, align, spb = ima_encode(mono)
    p = os.path.join(outdir, 'voice.wav')
    n = wav_raw(p, body, 0x11, 1, 4, align, spb)
    print('%s (%d bytes, IMA ADPCM %d:1)'
          % (p, n, (len(mono) * 2) // max(len(body), 1)))

    # G.711 mu-law
    body = bytes(ulaw_encode(v) for v, _ in chime())
    p = os.path.join(outdir, 'dial.wav')
    n = wav_raw(p, body, 7, 1, 8, 1)
    print('%s (%d bytes, mu-law)' % (p, n))


if __name__ == '__main__':
    main()
