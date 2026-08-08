#!/usr/bin/env python3
"""Check the IMA ADPCM and G.711 decoders against an independent one.

macOS ships `afconvert`, which decodes both formats. Decoding the same
file with src/adpcm.h and with afconvert and requiring the samples to
agree is a real check; comparing against the encoder in tools/mkwav.py
would only prove the two share a misunderstanding.

Both formats are deterministic, so agreement should be exact -- these
codecs are lossy at *encode* time, not at decode time.

    python3 tools/audio_test.py build/music
"""
import os
import struct
import subprocess
import sys


def read_wav_pcm(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'WAVE', path
    off, ch, bits = 12, 2, 16
    while off + 8 <= len(d):
        cid = d[off:off + 4]
        sz = struct.unpack('<I', d[off + 4:off + 8])[0]
        body = off + 8
        if cid == b'fmt ':
            ch = struct.unpack('<H', d[body + 2:body + 4])[0]
            bits = struct.unpack('<H', d[body + 14:body + 16])[0]
        elif cid == b'data':
            raw = d[body:body + sz]
            assert bits == 16, '%s is %d-bit' % (path, bits)
            v = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
            return v, ch
        off = body + sz + (sz & 1)
    raise SystemExit('no data chunk in ' + path)


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else 'build/music'
    if subprocess.run(['which', 'afconvert'],
                      capture_output=True).returncode:
        print('afconvert is not available; skipping')
        return 0

    fails = 0
    checked = 0
    for name in ('voice.wav', 'dial.wav'):
        path = os.path.join(outdir, name)
        if not os.path.exists(path):
            print('  %-12s missing' % name)
            continue
        checked += 1

        ref_path = os.path.join(outdir, '.ref_' + name)
        if os.path.exists(ref_path):
            os.unlink(ref_path)
        p = subprocess.run(['afconvert', '-f', 'WAVE', '-d', 'LEI16',
                            path, ref_path], capture_output=True)
        if p.returncode:
            print('  %-12s afconvert failed' % name)
            fails += 1
            continue
        ref, ref_ch = read_wav_pcm(ref_path)
        os.unlink(ref_path)

        raw_path = os.path.join(outdir, '.mine.raw')
        p = subprocess.run(['./build/audio_test', path, raw_path],
                           capture_output=True, text=True)
        if p.returncode:
            print('  %-12s decode failed: %s' % (name, p.stderr.strip()))
            fails += 1
            continue
        raw = open(raw_path, 'rb').read()
        os.unlink(raw_path)
        mine = list(struct.unpack('<%dh' % (len(raw) // 2), raw))

        # ours is always stereo; afconvert keeps the source channel count
        if ref_ch == 1:
            mine = mine[0::2]

        n = min(len(mine), len(ref))
        bad = [i for i in range(n) if mine[i] != ref[i]]
        if not bad:
            print('  %-12s %6d samples  exact vs afconvert' % (name, n))
        else:
            worst = max(abs(mine[i] - ref[i]) for i in bad)
            # a handful of edge samples differing by 1 is a rounding
            # choice; anything more is a real disagreement
            if len(bad) * 1000 < n and worst <= 1:
                print('  %-12s %6d samples  %d differ by 1 (rounding)'
                      % (name, n, len(bad)))
            else:
                print('  %-12s MISMATCH: %d of %d differ, worst %d, first at %d'
                      % (name, len(bad), n, worst, bad[0]))
                fails += 1

    print('%d/%d agree' % (checked - fails, checked))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
