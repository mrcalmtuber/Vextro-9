#ifndef UCODE_H
#define UCODE_H

/*
 * src/ucode.h — the processor's own firmware.
 *
 * A modern x86 core does not execute x86. It executes an internal
 * instruction set, and a patch layer in ROM translates. That layer is
 * amendable: the vendor ships updates that fix errata found after the
 * silicon was cut, and on a PC it is the BIOS or the operating system
 * that applies them. If neither does, the machine runs with whatever was
 * burned in at the factory — which for some parts means known-wrong
 * behaviour in specific instruction sequences.
 *
 * This does three things and reports all of them:
 *
 *   1. Reads the revision the processor is currently running, which is
 *      the only way to know whether the firmware already patched it. The
 *      protocol for that is peculiar and worth naming: write zero to
 *      IA32_BIOS_SIGN_ID, execute CPUID to make the processor publish
 *      the value, then read the MSR back. Skipping the CPUID reads
 *      whatever was in the register before.
 *
 *   2. Applies an update if one has been supplied — validated first, and
 *      then confirmed by re-reading the revision rather than by assuming
 *      the write worked.
 *
 *   3. Says what it found either way. "No update available" and "update
 *      applied" and "the processor refused it" are three different
 *      states and a machine that reports one message for all of them is
 *      not telling you anything.
 *
 * No update blob ships with this system. Intel and AMD distribute them
 * under terms that do not permit redistribution here, so what exists is
 * the loader and the reporting; a blob dropped at /firmware/ucode.bin is
 * picked up at boot.
 */

#include <stdint.h>
#include "pci.h"

#define MSR_IA32_BIOS_UPDT_TRIG  0x79
#define MSR_IA32_BIOS_SIGN_ID    0x8B
#define MSR_AMD_PATCH_LEVEL      0x0000008B
#define MSR_AMD_PATCH_LOADER     0xC0010020

/* Intel's update header, which is also its container format. */
typedef struct {
    uint32_t header_version;
    uint32_t update_revision;
    uint32_t date;
    uint32_t processor_signature;
    uint32_t checksum;
    uint32_t loader_revision;
    uint32_t processor_flags;
    uint32_t data_size;
    uint32_t total_size;
    uint32_t reserved[3];
} __attribute__((packed)) intel_ucode_hdr_t;

static struct {
    int      probed;
    int      is_intel;
    int      is_amd;
    uint32_t signature;        /* CPUID leaf 1 EAX             */
    uint32_t platform_id;      /* which of eight flag bits      */
    uint32_t revision_before;
    uint32_t revision_after;
    int      applied;
    const char *status;
    char     vendor[13];
} ucode = { .status = "not probed" };

static uint32_t ucode_read_revision(int intel) {
    if (intel) {
        /* Zero, then CPUID, then read: the processor publishes the
         * signature as a side effect of CPUID and not before. */
        wrmsr(MSR_IA32_BIOS_SIGN_ID, 0);
        uint32_t a, b, c, d;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                                 : "a"(1) : "memory");
        (void)a; (void)b; (void)c; (void)d;
        return (uint32_t)(rdmsr(MSR_IA32_BIOS_SIGN_ID) >> 32);
    }
    return (uint32_t)rdmsr(MSR_AMD_PATCH_LEVEL);
}

/*
 * Intel selects an update by three things at once: the processor
 * signature, one bit of a platform flag byte, and the update having a
 * higher revision than what is already loaded. An update that matches
 * the first two but not the third is not an error — it is the normal
 * case on a machine whose firmware already did this.
 */
static int intel_ucode_matches(const intel_ucode_hdr_t *h) {
    if (h->processor_signature != ucode.signature) return 0;
    if (h->processor_flags && ucode.platform_id &&
        !(h->processor_flags & ucode.platform_id)) return 0;
    return 1;
}

static int intel_ucode_checksum_ok(const intel_ucode_hdr_t *h) {
    uint32_t total = h->total_size ? h->total_size : 2048;
    if (total < sizeof(*h) || (total & 3)) return 0;
    const uint32_t *w = (const uint32_t *)h;
    uint32_t sum = 0;
    for (uint32_t i = 0; i < total / 4; i++) sum += w[i];
    return sum == 0;
}

/*
 * Apply one update image. `data` is the whole file; several updates may
 * be concatenated and only the matching one is used.
 *
 * Returns 1 if the processor's revision actually changed, which is the
 * only evidence worth reporting. WRMSR to the trigger register does not
 * fail visibly if the update is rejected.
 */
static int ucode_apply(const void *data, uint64_t len) {
    if (!ucode.probed || !data || len < 48) return 0;

    if (ucode.is_intel) {
        const uint8_t *p = (const uint8_t *)data;
        uint64_t off = 0;
        while (off + sizeof(intel_ucode_hdr_t) <= len) {
            const intel_ucode_hdr_t *h = (const intel_ucode_hdr_t *)(p + off);
            uint32_t total = h->total_size ? h->total_size : 2048;
            if (total < sizeof(*h) || off + total > len) break;

            if (intel_ucode_matches(h) &&
                h->update_revision > ucode.revision_before &&
                intel_ucode_checksum_ok(h)) {
                /* The address handed over is the payload, which begins
                 * immediately after the 48-byte header. */
                uint64_t payload = (uint64_t)(uintptr_t)(p + off + 48);
                __asm__ volatile("" ::: "memory");
                wrmsr(MSR_IA32_BIOS_UPDT_TRIG, payload);
                ucode.revision_after = ucode_read_revision(1);
                if (ucode.revision_after > ucode.revision_before) {
                    ucode.applied = 1;
                    return 1;
                }
                ucode.status = "the processor refused the update";
                return 0;
            }
            off += total;
        }
        ucode.status = "no update in the file matches this processor";
        return 0;
    }

    if (ucode.is_amd) {
        /* AMD takes the container address directly and reads its own
         * equivalence table out of it. */
        wrmsr(MSR_AMD_PATCH_LOADER, (uint64_t)(uintptr_t)data);
        ucode.revision_after = ucode_read_revision(0);
        if (ucode.revision_after > ucode.revision_before) {
            ucode.applied = 1;
            return 1;
        }
        ucode.status = "the processor refused the update";
        return 0;
    }

    return 0;
}

static void ucode_init(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    *(uint32_t *)(ucode.vendor + 0) = b;
    *(uint32_t *)(ucode.vendor + 4) = d;
    *(uint32_t *)(ucode.vendor + 8) = c;
    ucode.vendor[12] = '\0';

    ucode.is_intel = (b == 0x756E6547u);      /* "Genu" */
    ucode.is_amd   = (b == 0x68747541u);      /* "Auth" */

    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1));
    ucode.signature = a;

    if (ucode.is_intel) {
        /* Platform ID is three bits of MSR 0x17, expanded into the
         * one-hot byte the update header is matched against. */
        uint64_t pid = rdmsr(0x17);
        ucode.platform_id = 1u << ((pid >> 50) & 7);
    }

    ucode.revision_before = ucode_read_revision(ucode.is_intel);
    ucode.revision_after  = ucode.revision_before;
    ucode.probed = 1;
    ucode.status = ucode.is_intel || ucode.is_amd
                 ? "no update file on the volume"
                 : "unknown vendor - no loader for it";

    serial_puts("[ucode] ");
    serial_puts(ucode.vendor);
    serial_puts(" signature ");
    serial_put_hex32(ucode.signature);
    serial_puts(", running revision ");
    serial_put_hex32(ucode.revision_before);
    serial_puts("\n");
}

/* Called once the filesystem is up. Separate from ucode_init because the
 * revision has to be read before anything else touches the MSR, and the
 * disk is not mounted that early. */
static void ucode_load_from_disk(const void *data, uint64_t len) {
    if (!data || !len) return;
    serial_puts("[ucode] update file present, ");
    serial_put_dec((uint32_t)len);
    serial_puts(" bytes\n");
    if (ucode_apply(data, len)) {
        ucode.status = "update applied";
        serial_puts("[ucode] applied: revision ");
        serial_put_hex32(ucode.revision_before);
        serial_puts(" -> ");
        serial_put_hex32(ucode.revision_after);
        serial_puts("\n");
    } else {
        serial_puts("[ucode] ");
        serial_puts(ucode.status);
        serial_puts("\n");
    }
}

#endif /* UCODE_H */
