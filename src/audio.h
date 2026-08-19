#ifndef AUDIO_H
#define AUDIO_H

/*
 * src/audio.h — one way to make a sound, and a mixer behind it.
 *
 * There were two problems with the audio in this system and they were
 * the same problem twice.
 *
 * The first is the hardware. AC'97 is a 1997 codec that no machine built
 * this decade has; HD Audio is what a real headphone socket is behind.
 * Both are now here and neither is named by anything above this file:
 * the media player asks for a sound and gets one, on whichever
 * controller the machine turned out to have.
 *
 * The second is that only one thing could make a sound at a time.
 * Playing a track meant the interface could not chime, and chiming meant
 * the track stopped -- because "play" meant "hand this buffer to the
 * hardware", and hardware has one stream. A mixer is what turns that
 * into as many voices as anyone wants: the voices are summed into one
 * buffer and that buffer is what the hardware plays.
 *
 * Summing is where naive mixers go wrong. Two signals that each reach
 * the top of the range add to twice it, which wraps to a large negative
 * number, and a wrap is heard as a click far louder than either sound.
 * Saturating instead is quieter, correct, and one comparison.
 */

#include <stdint.h>
#include "hda.h"
#include "ac97.h"

#define AUDIO_NONE 0
#define AUDIO_HDA  1
#define AUDIO_AC97 2

#define AUDIO_MIX_VOICES 8
#define AUDIO_MIX_RATE   48000
/* A quarter of a second at 48 kHz stereo. Long enough that a voice
 * started mid-block is not audibly late, short enough that stopping a
 * sound takes effect while a person still associates it with the click
 * that stopped it. */
#define AUDIO_MIX_FRAMES 12000
#define AUDIO_MIX_SAMPLES (AUDIO_MIX_FRAMES * 2)

static int audio_backend = AUDIO_NONE;

/* The buffer the hardware actually reads. Page-aligned because it is
 * handed to a bus-mastering device as a physical address. */
static int16_t audio_mixbuf[AUDIO_MIX_SAMPLES] __attribute__((aligned(4096)));

typedef struct {
    const int16_t *pcm;
    uint32_t       samples;      /* total, interleaved                */
    uint32_t       pos;
    uint32_t       rate;
    int32_t        volume;       /* 0..256, 256 being unattenuated    */
    int            loop;
    int            active;
} audio_voice_t;

static audio_voice_t audio_voices[AUDIO_MIX_VOICES];
static int audio_mix_running = 0;

static const char *audio_backend_name(void) {
    switch (audio_backend) {
    case AUDIO_HDA:  return "HD Audio";
    case AUDIO_AC97: return "AC'97";
    default:         return "none";
    }
}

static const char *audio_output_name(void) {
    if (audio_backend == AUDIO_HDA) return hda.jack;
    if (audio_backend == AUDIO_AC97) return "line out";
    return "none";
}

/*
 * Bring up whatever this machine has.
 *
 * HD Audio first, and not merely because it is newer: a machine with
 * both fitted is a machine where AC'97 is the legacy path and the
 * sockets a person can actually see are on the HDA codec.
 */
static void audio_init(void) {
    hda_init();
    if (hda.present) {
        audio_backend = AUDIO_HDA;
        serial_puts("[audio] HD Audio, output to ");
        serial_puts(hda.jack);
        serial_puts("\n");
        return;
    }

    ac97_init();
    if (ac97_found) {
        audio_backend = AUDIO_AC97;
        serial_puts("[audio] no HD Audio; falling back to AC'97\n");
        return;
    }

    serial_puts("[audio] no audio controller on this machine\n");
}

static int audio_present(void) { return audio_backend != AUDIO_NONE; }

static int audio_hw_play(const int16_t *pcm, uint32_t nsamples, uint32_t rate) {
    if (audio_backend == AUDIO_HDA)  return hda_play(pcm, nsamples, rate);
    if (audio_backend == AUDIO_AC97) return ac97_play(pcm, nsamples, rate);
    return -1;
}

static void audio_hw_stop(void) {
    if (audio_backend == AUDIO_HDA)  hda_stop();
    if (audio_backend == AUDIO_AC97) ac97_stop();
}

static int audio_hw_busy(void) {
    if (audio_backend == AUDIO_HDA)  return hda_busy();
    if (audio_backend == AUDIO_AC97) return ac97_busy();
    return 0;
}

/* ===== THE MIXER ===== */

/*
 * Add a voice. Returns its index, or -1 if every slot is in use.
 *
 * `volume` is out of 256 rather than out of 100, because the scaling is
 * then a multiply and a shift rather than a divide -- and this runs once
 * per sample per voice.
 */
static int audio_voice_start(const int16_t *pcm, uint32_t samples,
                             uint32_t rate, int volume, int loop) {
    if (!pcm || !samples) return -1;
    for (int i = 0; i < AUDIO_MIX_VOICES; i++) {
        if (audio_voices[i].active) continue;
        audio_voices[i].pcm     = pcm;
        audio_voices[i].samples = samples;
        audio_voices[i].pos     = 0;
        audio_voices[i].rate    = rate ? rate : AUDIO_MIX_RATE;
        audio_voices[i].volume  = volume < 0 ? 0 : (volume > 256 ? 256 : volume);
        audio_voices[i].loop    = loop;
        audio_voices[i].active  = 1;
        return i;
    }
    return -1;
}

static void audio_voice_stop(int idx) {
    if (idx >= 0 && idx < AUDIO_MIX_VOICES) audio_voices[idx].active = 0;
}

static void audio_stop_all(void) {
    for (int i = 0; i < AUDIO_MIX_VOICES; i++) audio_voices[i].active = 0;
    audio_hw_stop();
    audio_mix_running = 0;
}

static int audio_voices_active(void) {
    int n = 0;
    for (int i = 0; i < AUDIO_MIX_VOICES; i++) if (audio_voices[i].active) n++;
    return n;
}

/*
 * Fill one block from every active voice.
 *
 * Rate conversion is nearest-neighbour, by stepping through the source
 * at a fixed-point rate ratio. It is the cheapest possible resampler and
 * it is audibly so on a sine sweep; what it is not is wrong about pitch,
 * which is the failure people actually notice. Anything better is a
 * filter, and a filter per voice per sample is a different order of
 * cost than this system has spare.
 */
static void audio_mix_block(void) {
    for (uint32_t i = 0; i < AUDIO_MIX_SAMPLES; i++) audio_mixbuf[i] = 0;

    for (int v = 0; v < AUDIO_MIX_VOICES; v++) {
        audio_voice_t *a = &audio_voices[v];
        if (!a->active) continue;

        /* 16.16 fixed point: how far to advance in the source per
         * output frame. */
        uint32_t step = (uint32_t)(((uint64_t)a->rate << 16) / AUDIO_MIX_RATE);
        uint32_t frac = 0;
        uint32_t src  = a->pos;

        for (uint32_t f = 0; f < AUDIO_MIX_FRAMES; f++) {
            if (src + 1 >= a->samples) {
                if (!a->loop) { a->active = 0; break; }
                src = 0;
                frac = 0;
            }
            int32_t l = a->pcm[src];
            int32_t r = a->pcm[src + 1];

            l = (l * a->volume) >> 8;
            r = (r * a->volume) >> 8;

            int32_t ol = audio_mixbuf[f * 2]     + l;
            int32_t or_ = audio_mixbuf[f * 2 + 1] + r;

            /* Saturate. Two loud sounds together are loud, not a click. */
            if (ol >  32767) ol =  32767;
            if (ol < -32768) ol = -32768;
            if (or_ >  32767) or_ =  32767;
            if (or_ < -32768) or_ = -32768;

            audio_mixbuf[f * 2]     = (int16_t)ol;
            audio_mixbuf[f * 2 + 1] = (int16_t)or_;

            frac += step;
            src  += (frac >> 16) * 2;
            frac &= 0xFFFF;
        }
        if (a->active) a->pos = src;
    }
}

/*
 * Called once a frame. If anything is playing and the hardware has run
 * out of samples, mix the next block and hand it over.
 *
 * The check is "has the hardware finished" rather than a timer, because
 * the two clocks are independent: the frame clock is the display's and
 * the sample clock is the codec's, and a driver that assumes they agree
 * produces a gap or an overlap every few seconds.
 */
static void audio_poll(void) {
    if (!audio_present()) return;

    if (!audio_voices_active()) {
        if (audio_mix_running && !audio_hw_busy()) audio_mix_running = 0;
        return;
    }
    if (audio_hw_busy()) return;

    audio_mix_block();
    audio_hw_play(audio_mixbuf, AUDIO_MIX_SAMPLES, AUDIO_MIX_RATE);
    audio_mix_running = 1;
}

/* ===== SYSTEM SOUNDS =====
 *
 * Short tones the interface makes, generated rather than stored: a
 * sample file per event would be kilobytes on the volume for something
 * a few lines of arithmetic produce exactly.
 *
 * They are shaped at both ends. A tone that starts and stops abruptly
 * has a step in it, and a step is a click -- which is louder and more
 * annoying than the tone it bookends.
 */
#define SND_FRAMES 4800                     /* a tenth of a second */
static int16_t snd_buf[SND_FRAMES * 2];

typedef enum {
    SND_INFO = 0,
    SND_WARN,
    SND_ERROR,
    SND_OPEN,
    SND_CLOSE
} sound_id_t;

static void audio_make_tone(uint32_t hz, uint32_t frames, int32_t amp) {
    if (frames > SND_FRAMES) frames = SND_FRAMES;
    for (uint32_t i = 0; i < frames; i++) {
        /* A triangle rather than a sine: no table, no library, and at a
         * tenth of a second nobody is listening for the harmonics. */
        uint32_t period = AUDIO_MIX_RATE / (hz ? hz : 440);
        uint32_t phase  = period ? (i % period) : 0;
        int32_t  tri    = period ? (int32_t)((phase * 4 * 32767) / period) : 0;
        if (tri > 32767) tri = 65534 - tri;
        if (tri < -32768) tri = -65536 - tri;

        /* Ten per cent of the tone fades in, twenty fades out. */
        int32_t env = 256;
        uint32_t in = frames / 10, out = frames / 5;
        if (i < in)              env = (int32_t)((i * 256) / (in ? in : 1));
        else if (i > frames - out)
            env = (int32_t)(((frames - i) * 256) / (out ? out : 1));

        int32_t s = (tri * amp / 256) * env / 256;
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        snd_buf[i * 2]     = (int16_t)s;
        snd_buf[i * 2 + 1] = (int16_t)s;
    }
    for (uint32_t i = frames; i < SND_FRAMES; i++) {
        snd_buf[i * 2] = snd_buf[i * 2 + 1] = 0;
    }
}

/*
 * ---- the notification router ----
 *
 * What the interface does when something happens. Tying a sound to an
 * event here rather than at each call site means every notification of
 * the same kind sounds the same, which is most of what makes a system
 * feel coherent rather than assembled.
 */
static int audio_enabled = 1;

static void audio_play_sound(sound_id_t which) {
    if (!audio_enabled || !audio_present()) return;

    uint32_t hz;
    uint32_t frames = SND_FRAMES;
    int32_t  amp = 90;
    switch (which) {
    case SND_INFO:  hz = 880; frames = 2400; break;
    case SND_WARN:  hz = 494; frames = 3600; amp = 110; break;
    case SND_ERROR: hz = 247; frames = 4800; amp = 120; break;
    case SND_OPEN:  hz = 1175; frames = 1800; amp = 60; break;
    case SND_CLOSE: hz = 587; frames = 1800; amp = 60; break;
    default:        hz = 880; break;
    }
    audio_make_tone(hz, frames, amp);
    audio_voice_start(snd_buf, frames * 2, AUDIO_MIX_RATE, 256, 0);
}

#endif /* AUDIO_H */
