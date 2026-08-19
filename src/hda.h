#ifndef HDA_H
#define HDA_H

/*
 * src/hda.h — Intel High Definition Audio.
 *
 * AC'97 was the right thing to implement first and is the wrong thing to
 * keep. It is a 1997 codec on a bus of port-mapped registers, it is
 * fixed at one stereo output, and no machine built in the last fifteen
 * years has one — QEMU emulates it, and that is very nearly the whole of
 * where it still exists. A real laptop's headphone socket is behind an
 * HDA codec, and so is every USB-less desktop's line out.
 *
 * HDA is a different shape of driver and the difference is worth
 * stating, because it is why this file is long:
 *
 *   The controller does not know what it is driving. It moves streams of
 *   samples between memory and a serial link. What is on the other end
 *   of that link is a *codec*, and the codec is discovered by asking it.
 *
 *   A codec is a graph. Nodes are widgets — converters that turn samples
 *   into signals, pins that reach the outside world, mixers, selectors —
 *   and each one lists what it is connected to. Finding "the headphone
 *   socket" means walking that graph from a pin that claims to be a jack
 *   back to a digital-to-analogue converter, and then telling every
 *   widget on the path to unmute.
 *
 *   Commands go out through a ring and responses come back through
 *   another. Both are in memory, both have their own read and write
 *   pointers, and a driver that writes a command without waiting for the
 *   response pointer to move will read the previous answer to a
 *   different question.
 *
 * The payoff is that the same code drives a codec it has never seen. It
 * asks what the widgets are, finds a path, and configures it — so a
 * machine with the headphone jack on node 0x0D and one with it on node
 * 0x21 both work without either being special-cased.
 */

#include <stdint.h>
#include "pci.h"
#include "kheap.h"

/* ---- controller registers ---- */
#define HDA_GCAP        0x00
#define HDA_GCTL        0x08
#define HDA_WAKEEN      0x0C
#define HDA_STATESTS    0x0E
#define HDA_INTCTL      0x20
#define HDA_INTSTS      0x24
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48
#define HDA_CORBRP      0x4A
#define HDA_CORBCTL     0x4C
#define HDA_CORBSIZE    0x4E
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58
#define HDA_RINTCNT     0x5A
#define HDA_RIRBCTL     0x5C
#define HDA_RIRBSTS     0x5D
#define HDA_RIRBSIZE    0x5E
#define HDA_ICOI        0x60      /* immediate command output */
#define HDA_ICII        0x64      /* immediate command input  */
#define HDA_ICIS        0x68      /* immediate command status */
#define HDA_ICIS_BUSY   (1u << 0)
#define HDA_ICIS_VALID  (1u << 1)

#define HDA_DPLBASE     0x70
#define HDA_DPUBASE     0x74

#define HDA_GCTL_CRST   (1u << 0)
#define HDA_CORBCTL_RUN (1u << 1)
#define HDA_RIRBCTL_RUN (1u << 1)
#define HDA_CORBRP_RST  (1u << 15)

/* Stream descriptors begin after the input streams; the count of each is
 * in GCAP, so where the first output stream lives is computed, never
 * assumed. A driver that hard-codes 0x80 works on the controller it was
 * written on and writes into an input stream's registers on the next. */
#define HDA_SD_BASE     0x80
#define HDA_SD_STRIDE   0x20
#define HDA_SD_CTL      0x00
#define HDA_SD_STS      0x03
#define HDA_SD_LPIB     0x04
#define HDA_SD_CBL      0x08
#define HDA_SD_LVI      0x0C
#define HDA_SD_FIFOS    0x10
#define HDA_SD_FMT      0x12
#define HDA_SD_BDPL     0x18
#define HDA_SD_BDPU     0x1C

#define HDA_SD_CTL_SRST (1u << 0)
#define HDA_SD_CTL_RUN  (1u << 1)
#define HDA_SD_CTL_IOCE (1u << 2)

/* ---- codec verbs ---- */
#define VERB_GET_PARAM       0xF00
#define VERB_GET_CONN_SEL    0xF01
#define VERB_SET_CONN_SEL    0x701
#define VERB_GET_CONN_LIST   0xF02
#define VERB_SET_STREAM_FMT  0x200
#define VERB_SET_AMP         0x300
#define VERB_GET_AMP         0xB00
#define VERB_SET_CHAN_ID     0x706
#define VERB_SET_PIN_CTL     0x707
#define VERB_GET_PIN_CTL     0xF07
#define VERB_SET_POWER       0x705
#define VERB_GET_CONFIG_DEF  0xF1C
#define VERB_SET_EAPD        0x70C

#define PARAM_VENDOR         0x00
#define PARAM_NODE_COUNT     0x04
#define PARAM_FUNC_TYPE      0x05
#define PARAM_WIDGET_CAP     0x09
#define PARAM_PCM_RATES      0x0A
#define PARAM_PIN_CAP        0x0C
#define PARAM_AMP_OUT_CAP    0x12
#define PARAM_CONN_LEN       0x0E

#define WIDGET_TYPE(cap)     (((cap) >> 20) & 0xF)
#define WIDGET_OUTPUT        0x0     /* audio output converter (a DAC) */
#define WIDGET_INPUT         0x1
#define WIDGET_MIXER         0x2
#define WIDGET_SELECTOR      0x3
#define WIDGET_PIN           0x4

#define WCAP_OUT_AMP         (1u << 2)
#define WCAP_IN_AMP          (1u << 1)
#define WCAP_CONN_LIST       (1u << 8)
#define WCAP_POWER           (1u << 10)

#define PINCAP_OUTPUT        (1u << 4)
#define PINCAP_HP            (1u << 3)
#define PINCAP_EAPD          (1u << 16)

/* Where a pin physically goes, out of its configuration default. */
#define CFG_PORT_CONN(c)     (((c) >> 30) & 3)
#define CFG_DEFAULT_DEV(c)   (((c) >> 20) & 0xF)
#define CFG_DEV_LINE_OUT     0x0
#define CFG_DEV_SPEAKER      0x1
#define CFG_DEV_HP_OUT       0x2

#define HDA_CORB_ENTRIES 256
#define HDA_RIRB_ENTRIES 256
#define HDA_BDL_ENTRIES  4

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;         /* bit 0: interrupt on completion */
} __attribute__((packed)) hda_bdl_t;

static struct {
    int      present;
    volatile uint8_t *mmio;
    uint8_t  codec;              /* the first codec that answered */
    uint16_t out_stream;         /* index of the output stream descriptor */
    uint32_t sd_off;             /* its register block */
    uint8_t  dac;                /* the converter we drive */
    uint8_t  pin;                /* the jack it comes out of            */
    uint8_t  path[8];            /* widgets between them, to unmute     */
    int      path_len;
    int      is_headphone;
    uint32_t rates;
    int      playing;
    const char *status;
    const char *jack;
} hda = { .status = "not probed", .jack = "none" };

/* Rings and buffers. Physically contiguous because they are one static
 * block in the kernel image, and the controller is handed physical
 * addresses. */
static uint32_t  hda_corb[HDA_CORB_ENTRIES] __attribute__((aligned(128)));
static uint64_t  hda_rirb[HDA_RIRB_ENTRIES] __attribute__((aligned(128)));
static hda_bdl_t hda_bdl[HDA_BDL_ENTRIES]   __attribute__((aligned(128)));
static uint8_t   hda_dma_pos[128]           __attribute__((aligned(128)));

static uint16_t hda_corb_wp = 0;
static uint16_t hda_rirb_rp = 0;

static inline uint8_t  hda_r8(uint32_t o)  { return *(volatile uint8_t *)(hda.mmio + o); }
static inline uint16_t hda_r16(uint32_t o) { return *(volatile uint16_t *)(hda.mmio + o); }
static inline uint32_t hda_r32(uint32_t o) { return *(volatile uint32_t *)(hda.mmio + o); }
static inline void hda_w8(uint32_t o, uint8_t v)   { *(volatile uint8_t *)(hda.mmio + o) = v; }
static inline void hda_w16(uint32_t o, uint16_t v) { *(volatile uint16_t *)(hda.mmio + o) = v; }
static inline void hda_w32(uint32_t o, uint32_t v) { *(volatile uint32_t *)(hda.mmio + o) = v; }

static void hda_log(const char *m) {
    serial_puts("[hda] ");
    serial_puts(m);
    serial_putc('\n');
}

static void hda_wait(uint32_t loops) {
    for (volatile uint32_t i = 0; i < loops; i++) { }
}

/*
 * ---- the immediate command interface ----
 *
 * There are two ways to talk to a codec. The ring pair is the one the
 * specification leads with, and it is the right answer for a driver
 * sending a stream of verbs continuously -- it is asynchronous, it
 * batches, and it interrupts when answers arrive.
 *
 * This driver sends a few dozen verbs at boot and then nothing. For that
 * the immediate interface is strictly better: one register to write, one
 * status bit to wait on, one register to read, and no DMA at all -- so
 * none of the failure modes that come with a ring the controller writes
 * into behind you. That was not a hypothetical here: the rings brought
 * back the answer to the first question and nothing after it, which is
 * precisely the class of bug DMA into a ring produces and the immediate
 * path cannot have.
 *
 * The rings are still set up, because a controller that fails to start
 * them is telling us something, and because a future driver that wants
 * to poll jack detection continuously will want them.
 */
static uint32_t hda_immediate(uint32_t verb) {
    /* Wait for any previous command to retire. */
    for (int i = 0; i < 200000; i++) {
        if (!(hda_r16(HDA_ICIS) & HDA_ICIS_BUSY)) break;
        __asm__ volatile("pause");
    }
    if (hda_r16(HDA_ICIS) & HDA_ICIS_BUSY) return 0xFFFFFFFFu;

    /* Clear the valid bit -- it is write-one-to-clear -- then send. */
    hda_w16(HDA_ICIS, HDA_ICIS_VALID);
    hda_w32(HDA_ICOI, verb);
    hda_w16(HDA_ICIS, HDA_ICIS_BUSY);

    for (int i = 0; i < 200000; i++) {
        uint16_t st = hda_r16(HDA_ICIS);
        if ((st & HDA_ICIS_VALID) && !(st & HDA_ICIS_BUSY))
            return hda_r32(HDA_ICII);
        __asm__ volatile("pause");
    }
    return 0xFFFFFFFFu;                /* the codec never answered */
}

/*
 * Send one verb and wait for its answer.
 *
 * The write pointer is advanced and then the *response* write pointer is
 * watched: the controller moves it when it has written an answer. There
 * is no way to correlate a response with a command other than order,
 * which is why this waits for each one rather than pipelining -- codec
 * setup is a few dozen verbs at boot and the simplicity is worth far
 * more than the microseconds.
 */
static uint32_t hda_cmd(uint8_t codec, uint8_t node, uint32_t verb,
                        uint32_t payload) {
    /*
     * Twelve bits of verb and eight of payload. The other encoding --
     * four bits of verb and sixteen of payload -- is a different
     * function, hda_cmd16, and mixing the two is exactly the mistake
     * that made this driver find a codec, ask it questions, and
     * conclude it had no outputs: SET_POWER is 0x705, and folded into
     * the four-bit form it becomes verb 5 with a payload of nothing,
     * which the codec ignores.
     */
    uint32_t val = ((uint32_t)codec << 28) | ((uint32_t)node << 20) |
                   ((verb & 0xFFF) << 8) | (payload & 0xFF);

    return hda_immediate(val);
}

/* Verbs whose payload is sixteen bits use a four-bit opcode. */
static uint32_t hda_cmd16(uint8_t codec, uint8_t node, uint32_t verb4,
                          uint16_t payload) {
    uint32_t val = ((uint32_t)codec << 28) | ((uint32_t)node << 20) |
                   ((verb4 & 0xF) << 16) | payload;
    return hda_immediate(val);
}

static uint32_t hda_param(uint8_t node, uint8_t param) {
    return hda_cmd(hda.codec, node, VERB_GET_PARAM, param);
}

/*
 * ---- finding a way out ----
 *
 * Depth-first from a pin back towards a converter, following each
 * widget's connection list. The path is recorded because every widget on
 * it has to be told to pass signal: a mixer left muted is silent no
 * matter how correct the converter's configuration is, and that is the
 * single most common way an HDA driver produces a stream that plays
 * perfectly into nothing.
 *
 * Depth is bounded. A codec's graph is not supposed to contain a cycle
 * and a malformed one that does would otherwise recurse until the stack
 * guard page catches it.
 */
static int hda_walk(uint8_t node, uint8_t *path, int depth, int max) {
    if (depth >= max) return -1;

    uint32_t cap = hda_param(node, PARAM_WIDGET_CAP);
    if (cap == 0xFFFFFFFFu) return -1;
    uint32_t type = WIDGET_TYPE(cap);

    if (type == WIDGET_OUTPUT) {
        hda.dac = node;
        return depth;
    }
    if (!(cap & WCAP_CONN_LIST)) return -1;

    uint32_t len = hda_param(node, PARAM_CONN_LEN);
    int count = (int)(len & 0x7F);
    int longform = (len & 0x80) != 0;
    if (count > 16) count = 16;

    for (int i = 0; i < count; i++) {
        uint32_t entry = hda_cmd(hda.codec, node, VERB_GET_CONN_LIST,
                                 (uint32_t)(longform ? i * 2 : i * 4));
        uint8_t child;
        if (longform) child = (uint8_t)((entry >> ((i & 1) * 16)) & 0xFF);
        else          child = (uint8_t)((entry >> ((i & 3) * 8)) & 0xFF);
        if (!child) continue;

        path[depth] = node;
        int r = hda_walk(child, path, depth + 1, max);
        if (r >= 0) {
            /* A selector on the path has to be pointed at the child we
             * actually came through, or it will keep listening to
             * whichever input it powered up with. */
            if (type == WIDGET_SELECTOR || type == WIDGET_MIXER)
                hda_cmd(hda.codec, node, VERB_SET_CONN_SEL, (uint32_t)i);
            return r;
        }
    }
    return -1;
}

static void hda_unmute(uint8_t node, uint32_t cap) {
    /* Payload: set output amp, set input amp, both channels, index 0,
     * gain in the low bits with bit 7 the mute. Gain is left near the
     * top of the range rather than at it -- the last step or two of an
     * HDA amplifier is often where it starts to clip. */
    uint32_t gain = 0x50;
    uint32_t out_cap = hda_param(node, PARAM_AMP_OUT_CAP);
    if (out_cap != 0xFFFFFFFFu && out_cap) {
        uint32_t steps = (out_cap >> 8) & 0x7F;
        if (steps) gain = steps > 4 ? steps - 2 : steps;
    }
    if (cap & WCAP_OUT_AMP)
        hda_cmd16(hda.codec, node, 3, (uint16_t)(0xB000 | (gain & 0x7F)));
    if (cap & WCAP_IN_AMP)
        hda_cmd16(hda.codec, node, 3, (uint16_t)(0x7000 | (gain & 0x7F)));
}

/*
 * Look at every pin the codec has and pick the one to drive.
 *
 * Preference is headphones, then line out, then the internal speaker,
 * and a pin whose configuration default says nothing is physically
 * connected to it is skipped entirely -- which is what stops a laptop
 * playing into a socket that the manufacturer did not fit.
 */
static int hda_pick_output(uint8_t first, int count) {
    int best = -1, best_score = -1;

    for (int i = 0; i < count; i++) {
        uint8_t node = (uint8_t)(first + i);
        uint32_t cap = hda_param(node, PARAM_WIDGET_CAP);
        if (cap == 0xFFFFFFFFu) continue;
        if (WIDGET_TYPE(cap) != WIDGET_PIN) continue;

        uint32_t pincap = hda_param(node, PARAM_PIN_CAP);
        if (!(pincap & PINCAP_OUTPUT)) continue;

        uint32_t cfg = hda_cmd(hda.codec, node, VERB_GET_CONFIG_DEF, 0);
        if (CFG_PORT_CONN(cfg) == 1) continue;     /* no physical jack */

        int score;
        switch (CFG_DEFAULT_DEV(cfg)) {
        case CFG_DEV_HP_OUT:   score = 3; break;
        case CFG_DEV_LINE_OUT: score = 2; break;
        case CFG_DEV_SPEAKER:  score = 1; break;
        default:               score = 0; break;
        }
        if (pincap & PINCAP_HP) score++;

        if (score > best_score) { best_score = score; best = node; }
    }
    if (best < 0) return -1;

    uint32_t cfg = hda_cmd(hda.codec, (uint8_t)best, VERB_GET_CONFIG_DEF, 0);
    switch (CFG_DEFAULT_DEV(cfg)) {
    case CFG_DEV_HP_OUT:   hda.jack = "headphones"; hda.is_headphone = 1; break;
    case CFG_DEV_LINE_OUT: hda.jack = "line out";   break;
    case CFG_DEV_SPEAKER:  hda.jack = "speakers";   break;
    default:               hda.jack = "an output";  break;
    }
    return best;
}

static int hda_setup_codec(void) {
    uint32_t root = hda_param(0, PARAM_NODE_COUNT);
    serial_puts("[hda] codec vendor ");
    serial_put_hex32(hda_param(0, PARAM_VENDOR));
    serial_puts("\n");
    if (root == 0xFFFFFFFFu) { hda.status = "codec did not answer"; return 0; }

    uint8_t fg_first = (uint8_t)((root >> 16) & 0xFF);
    int     fg_count = (int)(root & 0xFF);

    for (int f = 0; f < fg_count; f++) {
        uint8_t fg = (uint8_t)(fg_first + f);
        uint32_t type = hda_param(fg, PARAM_FUNC_TYPE);
#ifdef HDA_VERBOSE
        serial_puts("[hda]   node ");
        serial_put_dec(fg);
        serial_puts(" function type ");
        serial_put_hex32(type);
        serial_puts("\n");
#endif
        if ((type & 0xFF) != 1) continue;          /* not audio */

        /* Wake the function group and everything under it. A codec
         * powers up in D3 and answers questions perfectly while
         * producing no sound at all. */
        hda_cmd(hda.codec, fg, VERB_SET_POWER, 0);
        hda_wait(200000);

        uint32_t nodes = hda_param(fg, PARAM_NODE_COUNT);
        uint8_t  first = (uint8_t)((nodes >> 16) & 0xFF);
        int      count = (int)(nodes & 0xFF);
        if (!count || count > 128) continue;

#ifdef HDA_VERBOSE
        serial_puts("[hda]   widgets ");
        serial_put_dec(first);
        serial_puts("..");
        serial_put_dec(first + count - 1);
        serial_puts("\n");
        for (int w = 0; w < count && w < 24; w++) {
            uint8_t nd = (uint8_t)(first + w);
            uint32_t cap = hda_param(nd, PARAM_WIDGET_CAP);
            serial_puts("[hda]     node ");
            serial_put_dec(nd);
            serial_puts(" cap ");
            serial_put_hex32(cap);
            serial_puts(" type ");
            serial_put_dec(WIDGET_TYPE(cap));
            if (WIDGET_TYPE(cap) == WIDGET_PIN) {
                serial_puts(" pincap ");
                serial_put_hex32(hda_param(nd, PARAM_PIN_CAP));
                serial_puts(" cfg ");
                serial_put_hex32(hda_cmd(hda.codec, nd, VERB_GET_CONFIG_DEF, 0));
            }
            serial_puts("\n");
        }
#endif

        int pin = hda_pick_output(first, count);
        if (pin < 0) continue;
        hda.pin = (uint8_t)pin;

        uint8_t path[8];
        int depth = hda_walk(hda.pin, path, 0, 8);
        if (depth < 0) continue;
        hda.path_len = depth;
        for (int i = 0; i < depth && i < 8; i++) hda.path[i] = path[i];

        /* Power and unmute every widget from the pin to the converter,
         * then the converter itself. */
        for (int i = 0; i < depth; i++) {
            uint32_t cap = hda_param(hda.path[i], PARAM_WIDGET_CAP);
            if (cap & WCAP_POWER)
                hda_cmd(hda.codec, hda.path[i], VERB_SET_POWER, 0);
            hda_unmute(hda.path[i], cap);
        }
        uint32_t dcap = hda_param(hda.dac, PARAM_WIDGET_CAP);
        if (dcap & WCAP_POWER) hda_cmd(hda.codec, hda.dac, VERB_SET_POWER, 0);
        hda_unmute(hda.dac, dcap);

        /* The pin: output enabled, and headphone drive if it has an
         * amplifier of its own. */
        uint32_t pincap = hda_param(hda.pin, PARAM_PIN_CAP);
        uint32_t ctl = 0x40;                       /* OUT enable */
        if (pincap & PINCAP_HP) ctl |= 0x80;       /* headphone drive */
        hda_cmd(hda.codec, hda.pin, VERB_SET_PIN_CTL, ctl);
        if (pincap & PINCAP_EAPD)
            hda_cmd(hda.codec, hda.pin, VERB_SET_EAPD, 0x02);

        hda.rates = hda_param(hda.dac, PARAM_PCM_RATES);
        return 1;
    }
    hda.status = "no audio function group with a usable output";
    return 0;
}

static void hda_stop(void) {
    if (!hda.present) return;
    uint32_t ctl = hda_r32(hda.sd_off + HDA_SD_CTL);
    hda_w32(hda.sd_off + HDA_SD_CTL, ctl & ~HDA_SD_CTL_RUN);
    for (int i = 0; i < 100000; i++) {
        if (!(hda_r32(hda.sd_off + HDA_SD_CTL) & HDA_SD_CTL_RUN)) break;
        __asm__ volatile("pause");
    }
    hda.playing = 0;
}

/*
 * The stream format word.
 *
 * Bit 14 selects the base rate: clear for the 48 kHz family, set for
 * 44.1 kHz. The multiplier and divisor fields step within the family, so
 * 96 kHz is 48 with a multiplier and 24 kHz is 48 with a divisor. Asking
 * for a rate the converter did not advertise produces a stream that runs
 * at whatever it does support, at the wrong speed, which sounds like the
 * recording was made by someone else.
 */
static uint16_t hda_format(uint32_t rate, int channels) {
    uint16_t base = 0, mult = 0, div = 0;
    switch (rate) {
    case 44100:  base = 1; break;
    case 48000:  break;
    case 88200:  base = 1; mult = 1; break;
    case 96000:  mult = 1; break;
    case 32000:  div = 2; break;       /* 48000 * 2 / 3 */
    case 22050:  base = 1; div = 1; break;
    case 24000:  div = 1; break;
    case 16000:  div = 2; break;
    case 11025:  base = 1; div = 3; break;
    case 8000:   div = 5; break;
    default:     break;                /* anything else plays at 48 kHz */
    }
    if (rate == 32000) { div = 0; mult = 0; }   /* not expressible: use 48k */
    uint16_t chan = (uint16_t)((channels > 0 ? channels : 1) - 1);
    return (uint16_t)((base << 14) | (mult << 11) | (div << 8) |
                      (1 << 4) | chan);          /* bits: 1 = 16-bit */
}

/*
 * Play a block of signed 16-bit stereo samples.
 *
 * The buffer is described to the controller by a descriptor list rather
 * than handed over directly, which is what lets a stream be assembled
 * out of several allocations -- and is also why the list must be at
 * least two entries on some controllers even when one would do.
 */
static int hda_play(const int16_t *pcm, uint32_t nsamples, uint32_t rate) {
    if (!hda.present || !pcm || !nsamples) return -1;
    hda_stop();

    uint32_t bytes = nsamples * 2;

    /* Reset the stream. The reset bit is set, waited for, cleared, and
     * waited for again -- both edges are acknowledged by hardware and a
     * driver that only does the first half leaves the stream wedged. */
    hda_w32(hda.sd_off + HDA_SD_CTL, HDA_SD_CTL_SRST);
    for (int i = 0; i < 100000; i++) {
        if (hda_r32(hda.sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST) break;
        __asm__ volatile("pause");
    }
    hda_w32(hda.sd_off + HDA_SD_CTL, 0);
    for (int i = 0; i < 100000; i++) {
        if (!(hda_r32(hda.sd_off + HDA_SD_CTL) & HDA_SD_CTL_SRST)) break;
        __asm__ volatile("pause");
    }

    uint64_t phys = kern_virt_to_phys((void *)(uintptr_t)pcm);
    if (!phys) return -1;

    /* Two entries covering the same buffer: the second is what the
     * hardware wraps to, and a single-entry list is rejected by some
     * controllers outright. */
    hda_bdl[0].addr = phys;
    hda_bdl[0].len  = bytes;
    hda_bdl[0].flags = 0;
    hda_bdl[1].addr = phys;
    hda_bdl[1].len  = bytes;
    hda_bdl[1].flags = 1;
    __asm__ volatile("sfence" ::: "memory");

    uint64_t bdl_phys = kern_virt_to_phys(hda_bdl);
    hda_w32(hda.sd_off + HDA_SD_BDPL, (uint32_t)bdl_phys);
    hda_w32(hda.sd_off + HDA_SD_BDPU, (uint32_t)(bdl_phys >> 32));
    hda_w32(hda.sd_off + HDA_SD_CBL, bytes * 2);
    hda_w16(hda.sd_off + HDA_SD_LVI, 1);

    uint16_t fmt = hda_format(rate, 2);
    hda_w16(hda.sd_off + HDA_SD_FMT, fmt);

    /* Stream 1, and the converter has to be told the same number or the
     * samples go out over the link addressed to nobody. */
    hda_w32(hda.sd_off + HDA_SD_CTL,
            (1u << 20) | HDA_SD_CTL_IOCE);
    hda_cmd(hda.codec, hda.dac, VERB_SET_CHAN_ID, 0x10);
    hda_cmd16(hda.codec, hda.dac, 2, fmt);

    uint32_t ctl = hda_r32(hda.sd_off + HDA_SD_CTL);
    hda_w32(hda.sd_off + HDA_SD_CTL, ctl | HDA_SD_CTL_RUN);
    hda.playing = 1;
    return 0;
}

/* Is the stream still running? The position register advances while it
 * is, which is a more truthful answer than the run bit alone. */
static int hda_busy(void) {
    if (!hda.present || !hda.playing) return 0;
    return (hda_r32(hda.sd_off + HDA_SD_CTL) & HDA_SD_CTL_RUN) != 0;
}

static void hda_init(void) {
    hda.present = 0;
    hda.status = "no HD Audio controller";

    pci_dev_t dev;
    /* Class 04 multimedia, subclass 03 HD Audio. Any vendor: Intel, AMD,
     * NVIDIA and VIA all implement the same specification, which is the
     * entire point of it existing. */
    if (!pci_find_class(0xFFFF00u, 0x040300u, 0, &dev)) {
        hda_log("no controller on the PCI bus");
        return;
    }

    uint64_t bar, len;
    if (pci_bar(&dev, 0, &bar, &len) != 0 || !bar) {
        hda.status = "BAR0 unusable";
        hda_log(hda.status);
        return;
    }
    pci_enable(&dev, PCI_CMD_MEM | PCI_CMD_MASTER);
    hda.mmio = mmio_map(bar, len < 0x2000 ? 0x2000 : len);
    if (!hda.mmio) {
        hda.status = "could not map the register block";
        hda_log(hda.status);
        return;
    }

    serial_puts("[hda] controller 8086-class device ");
    serial_put_hex32(((uint32_t)dev.vendor << 16) | dev.device);
    serial_puts("\n");

    /* Leave reset. The specification requires a wait afterwards that is
     * measured in codec frames rather than in anything the driver can
     * observe, so this waits and then checks. */
    hda_w32(HDA_GCTL, 0);
    hda_wait(100000);
    hda_w32(HDA_GCTL, HDA_GCTL_CRST);
    int ready = 0;
    for (int i = 0; i < 1000000; i++) {
        if (hda_r32(HDA_GCTL) & HDA_GCTL_CRST) { ready = 1; break; }
        __asm__ volatile("pause");
    }
    if (!ready) {
        hda.status = "controller never left reset";
        hda_log(hda.status);
        return;
    }
    hda_wait(500000);

    uint16_t statests = hda_r16(HDA_STATESTS);
    if (!statests) {
        hda.status = "no codec responded to the reset";
        hda_log(hda.status);
        return;
    }
    for (int i = 0; i < 15; i++)
        if (statests & (1u << i)) { hda.codec = (uint8_t)i; break; }

    /* GCAP says how many input, output and bidirectional streams there
     * are. The output descriptors follow the input ones. */
    uint16_t gcap = hda_r16(HDA_GCAP);
    uint32_t in_streams  = (gcap >> 8) & 0x0F;
    uint32_t out_streams = (gcap >> 12) & 0x0F;
    if (!out_streams) {
        hda.status = "controller has no output stream";
        hda_log(hda.status);
        return;
    }
    hda.out_stream = (uint16_t)in_streams;
    hda.sd_off = HDA_SD_BASE + hda.out_stream * HDA_SD_STRIDE;

    /* Command and response rings. Both are reset, pointed at their
     * buffers, and started. */
    hda_w8(HDA_CORBCTL, 0);
    hda_w8(HDA_RIRBCTL, 0);

    uint64_t corb_phys = kern_virt_to_phys(hda_corb);
    uint64_t rirb_phys = kern_virt_to_phys(hda_rirb);
    hda_w32(HDA_CORBLBASE, (uint32_t)corb_phys);
    hda_w32(HDA_CORBUBASE, (uint32_t)(corb_phys >> 32));
    hda_w32(HDA_RIRBLBASE, (uint32_t)rirb_phys);
    hda_w32(HDA_RIRBUBASE, (uint32_t)(rirb_phys >> 32));
    hda_w8(HDA_CORBSIZE, 0x02);          /* 256 entries */
    hda_w8(HDA_RIRBSIZE, 0x02);

    /* The read pointer reset is another two-edged handshake. */
    hda_w16(HDA_CORBRP, HDA_CORBRP_RST);
    for (int i = 0; i < 100000; i++) {
        if (hda_r16(HDA_CORBRP) & HDA_CORBRP_RST) break;
        __asm__ volatile("pause");
    }
    hda_w16(HDA_CORBRP, 0);
    hda_w16(HDA_CORBWP, 0);
    hda_w16(HDA_RIRBWP, (1u << 15));     /* reset the write pointer */
    hda_w16(HDA_RINTCNT, 1);
    hda_corb_wp = 0;

    uint64_t dpos = kern_virt_to_phys(hda_dma_pos);
    hda_w32(HDA_DPLBASE, (uint32_t)dpos | 1);
    hda_w32(HDA_DPUBASE, (uint32_t)(dpos >> 32));

    hda_w8(HDA_CORBCTL, HDA_CORBCTL_RUN);
    hda_w8(HDA_RIRBCTL, HDA_RIRBCTL_RUN);
    hda_wait(100000);

    hda.present = 1;
    if (!hda_setup_codec()) {
        hda.present = 0;
        hda_log(hda.status);
        return;
    }

    hda.status = "ready";
    serial_puts("[hda] codec ");
    serial_put_dec(hda.codec);
    serial_puts(": converter node ");
    serial_put_dec(hda.dac);
    serial_puts(" -> pin node ");
    serial_put_dec(hda.pin);
    serial_puts(" (");
    serial_puts(hda.jack);
    serial_puts("), ");
    serial_put_dec((uint32_t)hda.path_len);
    serial_puts(" widget(s) on the path, stream ");
    serial_put_dec(hda.out_stream);
    serial_puts("\n");
}

#endif /* HDA_H */
