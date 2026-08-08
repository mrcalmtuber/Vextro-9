#ifndef VEXTRO_MEDIA_H
#define VEXTRO_MEDIA_H

/*
 * src/media.h — the media player.
 *
 * Four formats, all decoded here:
 *
 *   .flac            lossless, LPC and Rice coded          (src/flac.h)
 *   .wav  PCM        uncompressed 16-bit
 *   .wav  IMA ADPCM  4 bits per sample                    (src/adpcm.h)
 *   .wav  A/mu-law   G.711 companding                     (src/adpcm.h)
 *
 * It says on its own face what it can read, rather than failing
 * mysteriously on anything else. MP3, AAC and every video codec are
 * still absent -- those are months of work each and the README says so.
 *
 * What it does do is real end to end -- the file is parsed here, the
 * samples go to the AC97 driver's bus-master path, and the sound was
 * measured coming out of the emulator, not assumed. The FLAC decoder is
 * checked against the reference encoder for bit-exact output, which a
 * lossless format makes possible and a lossy one does not.
 */

#include "flac.h"
#include "adpcm.h"

/*
 * Not every port has a sound device. Where there is none the player still
 * lists tracks and still parses them -- so the file handling is exercised
 * and the reason for the silence is stated -- but it cannot play, and it
 * says so instead of pretending.
 */
#ifndef VEXTRO_HAVE_AUDIO
static int  ac97_found = 0;
static int  ac97_play(const int16_t *p, uint32_t n, uint32_t r) {
    (void)p; (void)n; (void)r; return -1;
}
static void ac97_stop(void) { }
static int  ac97_busy(void) { return 0; }
#endif

#define MEDIA_MAX_SAMPLES (1u << 20)     /* 2 MB: ~11 s of 48 kHz stereo */
#define MEDIA_MAX_TRACKS  32
#define MEDIA_NAME_MAX    64

static int16_t media_pcm[MEDIA_MAX_SAMPLES];
static uint32_t media_nsamples = 0;
static uint32_t media_rate = 48000;
static int      media_channels = 2;

static char media_tracks[MEDIA_MAX_TRACKS][MEDIA_NAME_MAX];
static int  media_track_n = 0;
static int  media_sel = 0;
static char media_status[96] = "";
static int  media_status_err = 0;
static char media_now[MEDIA_NAME_MAX] = "";

/* Where the player looks. A dedicated directory rather than the whole
 * volume, so the list is short and predictable. */
#define MEDIA_DIR "/music"

static uint32_t media_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t media_rd16(const uint8_t *p) {
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}
static int media_tag(const uint8_t *p, const char *t) {
    return p[0] == (uint8_t)t[0] && p[1] == (uint8_t)t[1] &&
           p[2] == (uint8_t)t[2] && p[3] == (uint8_t)t[3];
}

/*
 * Parse a RIFF/WAVE file into media_pcm.
 *
 * The chunk walk is bounded at every step. This reads files off a volume
 * that anything could have written, so a chunk whose declared size runs
 * past the end of the buffer has to stop the walk rather than advance the
 * cursor past it -- that is the one mistake that turns a parser into a
 * read primitive.
 */
static const char *media_parse_wav(const uint8_t *d, uint64_t n) {
    if (n < 44) return "too small to be a WAVE file";
    if (!media_tag(d, "RIFF") || !media_tag(d + 8, "WAVE"))
        return "not a RIFF/WAVE file";

    uint64_t off = 12;
    int have_fmt = 0;
    uint16_t fmt = 0, ch = 2, bits = 16, align = 0;
    uint32_t rate = 48000;

    while (off + 8 <= n) {
        const uint8_t *hdr = d + off;
        const uint32_t sz = media_rd32(hdr + 4);
        const uint64_t body = off + 8;
        if (sz > n - body) break;            /* declared past the end */

        if (media_tag(hdr, "fmt ") && sz >= 16) {
            fmt   = media_rd16(d + body);
            ch    = media_rd16(d + body + 2);
            rate  = media_rd32(d + body + 4);
            align = media_rd16(d + body + 12);
            bits  = media_rd16(d + body + 14);
            have_fmt = 1;
        } else if (media_tag(hdr, "data") && have_fmt) {
            if (ch != 1 && ch != 2) return "only mono or stereo";
            if (rate < 4000 || rate > 96000) return "unusual sample rate";

            const uint8_t *src = d + body;
            uint32_t out = 0;

            if (fmt == 1) {                       /* uncompressed PCM */
                if (bits != 16) return "only 16-bit PCM is supported";
                const uint32_t in_samples = sz / 2;
                if (ch == 2) {
                    for (uint32_t i = 0; i < in_samples && out < MEDIA_MAX_SAMPLES; i++)
                        media_pcm[out++] = (int16_t)media_rd16(src + i * 2);
                } else {
                    /* Mono is duplicated into both channels here rather
                     * than asking the codec to do it: the BDL carries
                     * interleaved stereo and nothing downstream knows
                     * about mono. */
                    for (uint32_t i = 0; i < in_samples && out + 1 < MEDIA_MAX_SAMPLES; i++) {
                        const int16_t v = (int16_t)media_rd16(src + i * 2);
                        media_pcm[out++] = v;
                        media_pcm[out++] = v;
                    }
                }
            } else if (fmt == 0x11) {             /* IMA ADPCM */
                if (bits != 4) return "IMA ADPCM must be 4 bits per sample";
                out = adpcm_decode_ima(src, sz, align, ch,
                                       media_pcm, MEDIA_MAX_SAMPLES);
                if (!out) return "malformed IMA ADPCM blocks";
            } else if (fmt == 6 || fmt == 7) {    /* G.711 A-law / mu-law */
                if (bits != 8) return "G.711 must be 8 bits per sample";
                out = g711_decode(src, sz, ch, fmt == 6,
                                  media_pcm, MEDIA_MAX_SAMPLES);
            } else {
                return "unsupported WAVE codec";
            }

            media_nsamples = out;
            media_rate = rate;
            media_channels = 2;
            return out ? 0 : "no audio in the data chunk";
        }
        off = body + sz + (sz & 1);          /* chunks are word aligned */
    }
    return have_fmt ? "no data chunk" : "no format chunk";
}

/* Does `name` end with `ext`, either case? The volume is FAT32, which
 * stores short names upper-case, so both spellings turn up on one disk. */
static int media_ext_is(const char *name, const char *ext) {
    const int n = str_len(name), e = str_len(ext);
    if (n <= e) return 0;
    const char *p = name + n - e;
    for (int i = 0; i < e; i++) {
        char a = p[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (a != ext[i]) return 0;
    }
    return 1;
}

static void media_scan_cb(const char *name, uint32_t size, int is_dir) {
    (void)size;
    if (is_dir || name[0] == '.') return;
    if (media_track_n >= MEDIA_MAX_TRACKS) return;
    if (!media_ext_is(name, ".wav") && !media_ext_is(name, ".flac") &&
        !media_ext_is(name, ".fla"))
        return;
    str_copy(media_tracks[media_track_n++], name, MEDIA_NAME_MAX);
}

static void media_scan(void) {
    media_track_n = 0;
    fs_list(MEDIA_DIR, media_scan_cb);
    if (media_sel >= media_track_n) media_sel = 0;
    if (media_track_n == 0)
        str_copy(media_status, "no .wav or .flac files in " MEDIA_DIR,
                 sizeof(media_status));
}

static void media_play_selected(void) {
    media_status_err = 0;
    if (media_sel < 0 || media_sel >= media_track_n) return;

    char path[160];
    str_copy(path, MEDIA_DIR "/", sizeof(path));
    str_append(path, media_tracks[media_sel], sizeof(path));

    uint64_t n = 0;
    const void *d = fs_read_file(path, &n);
    if (!d) {
        str_copy(media_status, "cannot read that file", sizeof(media_status));
        media_status_err = 1;
        return;
    }

    /*
     * Which decoder, decided by what the file actually starts with
     * rather than by its name. An extension is a hint from whoever wrote
     * the file; the magic is the file itself.
     */
    const uint8_t *bytes = (const uint8_t *)d;
    const char *bad;
    if (n >= 4 && bytes[0] == 'f' && bytes[1] == 'L' &&
        bytes[2] == 'a' && bytes[3] == 'C') {
        bad = flac_decode(bytes, n, media_pcm, MEDIA_MAX_SAMPLES,
                          &media_nsamples, &media_rate);
        media_channels = 2;
    } else {
        bad = media_parse_wav(bytes, n);
    }
    if (bad) {
        str_copy(media_status, bad, sizeof(media_status));
        media_status_err = 1;
        return;
    }

    if (!ac97_found) {
        str_copy(media_status, "no audio device on this machine",
                 sizeof(media_status));
        media_status_err = 1;
        return;
    }
    if (ac97_play(media_pcm, media_nsamples, media_rate) != 0) {
        str_copy(media_status, "the device refused the buffer", sizeof(media_status));
        media_status_err = 1;
        return;
    }
    str_copy(media_now, media_tracks[media_sel], sizeof(media_now));
    str_copy(media_status, "playing", sizeof(media_status));
}

static void media_stop(void) {
    ac97_stop();
    str_copy(media_status, "stopped", sizeof(media_status));
    media_status_err = 0;
}

/* ===== the window ===== */

#define MEDIA_ROW_H 22

static void media_draw(uint32_t *buf, uint32_t w, uint32_t h,
                       int32_t cx, int32_t cy, int32_t cw, int32_t chh,
                       int32_t mx, int32_t my) {
    gfx_rect(buf, w, h, cx, cy, cw, chh, 0x14171Fu);

    ttf_draw_string(buf, (int)w, (int)h, cx + 14, cy + 10, "Media Player",
                    C_GOLD, 15);
    ttf_draw_string(buf, (int)w, (int)h, cx + cw - 118, cy + 14,
                    "PCM  FLAC  ADPCM  G.711", C_TEXT_DIM, 10);
    gfx_rect(buf, w, h, cx + 14, cy + 34, cw - 28, 1, 0x2A3142u);

    /* transport */
    const int32_t by = cy + 44;
    static const char *const lbl[2] = { "Play", "Stop" };
    for (int i = 0; i < 2; i++) {
        const int32_t bx = cx + 14 + i * 78;
        const int hot = mx >= bx && mx < bx + 70 && my >= by && my < by + 26;
        gfx_rect(buf, w, h, bx, by, 70, 26, hot ? 0x2A2410u : 0x1C2130u);
        gfx_rect_outline(buf, w, h, bx, by, 70, 26, hot ? C_GOLD : 0x2A3142u);
        const int tw = ttf_text_width(lbl[i], 12);
        ttf_draw_string(buf, (int)w, (int)h, bx + (70 - tw) / 2, by + 6,
                        lbl[i], hot ? C_GOLD : C_TEXT, 12);
    }

    /* the level meter doubles as the running indicator: it only moves
     * while the controller is actually walking the descriptor list */
    const int playing = ac97_busy();
    const int32_t mx0 = cx + 178, mw = cw - 192;
    if (mw > 40) {
        gfx_rect(buf, w, h, mx0, by + 8, mw, 10, 0x1A1E2Au);
        if (playing) {
            /* A travelling bar rather than a level: this is drawn from the
             * frame counter, not from the samples, and dressing it up as a
             * VU meter would be claiming to measure something it does not
             * look at. It says "running", which is what it knows. */
            const int32_t seg = mw / 16;
            for (int i = 0; i < 16; i++) {
                const uint32_t ph = (desktop_tick / 3 + (uint32_t)i) % 16u;
                if (ph < 5)
                    gfx_rect(buf, w, h, mx0 + i * seg, by + 8, seg - 2, 10,
                             gfx_mix(C_GOLD, 0x1A1E2Au, 60u + ph * 40u));
            }
        }
    }

    /* track list */
    int32_t y = cy + 82;
    ttf_draw_string(buf, (int)w, (int)h, cx + 14, y, MEDIA_DIR, C_TEXT_DIM, 11);
    y += 20;
    for (int i = 0; i < media_track_n && y < cy + chh - 34; i++, y += MEDIA_ROW_H) {
        const int hot = mx >= cx + 8 && mx < cx + cw - 8 &&
                        my >= y - 3 && my < y + MEDIA_ROW_H - 3;
        if (i == media_sel)
            gfx_rect(buf, w, h, cx + 8, y - 3, cw - 16, MEDIA_ROW_H, 0x232A3Cu);
        else if (hot)
            gfx_rect(buf, w, h, cx + 8, y - 3, cw - 16, MEDIA_ROW_H, 0x1C2130u);
        const int cur = str_eq(media_tracks[i], media_now) && playing;
        if (cur) gfx_circle(buf, w, h, cx + 18, y + 7, 3, C_GOLD);
        ttf_draw_string_clip(buf, (int)w, (int)h, cx + 28, y, media_tracks[i],
                             i == media_sel ? C_TEXT : C_TEXT_DIM, 12,
                             cx + cw - 14);
    }
    if (media_track_n == 0)
        ttf_draw_string(buf, (int)w, (int)h, cx + 28, y,
                        "Put a 16-bit PCM .wav in " MEDIA_DIR, C_TEXT_DIM, 12);

    /* status */
    gfx_rect(buf, w, h, cx, cy + chh - 24, cw, 24, 0x10131Cu);
    if (media_status[0])
        ttf_draw_string_clip(buf, (int)w, (int)h, cx + 14, cy + chh - 20,
                             media_status,
                             media_status_err ? C_RED : C_TEXT_DIM, 11,
                             cx + cw - 14);
}

static void media_mouse(int32_t mx, int32_t my, uint8_t lmb, uint8_t prev_lmb,
                        int32_t cx, int32_t cy, int32_t cw, int32_t chh) {
    if (!(lmb && !prev_lmb)) return;

    const int32_t by = cy + 44;
    for (int i = 0; i < 2; i++) {
        const int32_t bx = cx + 14 + i * 78;
        if (mx >= bx && mx < bx + 70 && my >= by && my < by + 26) {
            if (i == 0) media_play_selected();
            else        media_stop();
            return;
        }
    }

    int32_t y = cy + 102;
    for (int i = 0; i < media_track_n && y < cy + chh - 34; i++, y += MEDIA_ROW_H) {
        if (mx >= cx + 8 && mx < cx + cw - 8 &&
            my >= y - 3 && my < y + MEDIA_ROW_H - 3) {
            media_sel = i;
            return;
        }
    }
}

static void media_key(char ch) {
    switch (ch) {
    case KEY_UP:   if (media_sel > 0) media_sel--; break;
    case KEY_DOWN: if (media_sel + 1 < media_track_n) media_sel++; break;
    case '\n':     media_play_selected(); break;
    case ' ':      if (ac97_busy()) media_stop(); else media_play_selected(); break;
    default: break;
    }
}

#endif /* VEXTRO_MEDIA_H */
