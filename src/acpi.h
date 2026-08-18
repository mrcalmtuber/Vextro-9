#ifndef ACPI_H
#define ACPI_H

/*
 * src/acpi.h — what the firmware knows about this machine.
 *
 * Everything before this asked the hardware directly: probe PCI by
 * walking config space, find the local APIC through an MSR, guess that
 * the PIT exists because it always has. That works for the fixed parts
 * of a PC and stops working for everything else. How many processors are
 * there? Which of them are hyperthreads sharing one core's execution
 * units? Where is the I/O APIC, and which interrupt line did the
 * firmware wire the PIT to? Is there an HPET? Where does extended PCI
 * configuration space live?
 *
 * None of those have an answer you can probe for. They are described, in
 * tables, in memory, and the bootloader hands over a pointer to the root
 * of them. This file walks that tree.
 *
 * The tables are checksummed and the checksums are checked. A machine
 * whose MADT is corrupt would otherwise be told it has four hundred
 * processors, and the code that believes it would allocate stacks for
 * all of them.
 */

#include <stdint.h>
#include "pmm.h"
#include "vmm.h"

/* ---- common header on every description table ---- */
typedef struct {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_t;

typedef struct {
    char     signature[8];
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    /* revision >= 2 continues: */
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) acpi_rsdp_t;

/* ---- MADT: interrupt controllers, and the processors behind them ---- */
typedef struct {
    acpi_sdt_t hdr;
    uint32_t   lapic_address;
    uint32_t   flags;              /* bit 0: dual-8259 present */
} __attribute__((packed)) acpi_madt_t;

#define MADT_LAPIC          0
#define MADT_IOAPIC         1
#define MADT_INT_OVERRIDE   2
#define MADT_NMI_SOURCE     3
#define MADT_LAPIC_NMI      4
#define MADT_LAPIC_ADDR_OVR 5
#define MADT_X2APIC         9

typedef struct {
    uint8_t type;
    uint8_t length;
} __attribute__((packed)) madt_entry_t;

/* ---- what the walk produces ---- */
#define ACPI_MAX_CPUS   64
#define ACPI_MAX_IOAPIC 8
#define ACPI_MAX_OVR    24

typedef struct {
    uint8_t  acpi_id;
    uint32_t apic_id;
    int      enabled;
    int      online_capable;
    /* Filled in by the topology pass below. */
    uint32_t package;          /* which physical socket        */
    uint32_t core;             /* which core within it         */
    uint32_t thread;           /* which thread within the core */
    int      is_hyperthread;   /* thread != 0                  */
} acpi_cpu_t;

typedef struct {
    uint8_t  id;
    uint32_t address;
    uint32_t gsi_base;
} acpi_ioapic_t;

typedef struct {
    uint8_t  bus;
    uint8_t  source;           /* the ISA IRQ as software knows it */
    uint32_t gsi;              /* where it actually arrives        */
    uint16_t flags;
} acpi_override_t;

static struct {
    int            valid;
    int            revision;
    uint64_t       lapic_phys;
    int            dual_8259;

    acpi_cpu_t     cpu[ACPI_MAX_CPUS];
    int            cpu_count;
    int            core_count;
    int            package_count;
    int            thread_count;

    acpi_ioapic_t  ioapic[ACPI_MAX_IOAPIC];
    int            ioapic_count;

    acpi_override_t ovr[ACPI_MAX_OVR];
    int            ovr_count;

    uint64_t       hpet_phys;
    uint64_t       mcfg_base;      /* PCIe extended config window */
    uint8_t        mcfg_bus_start, mcfg_bus_end;
    uint16_t       pm1a_cnt;       /* for the shutdown handshake  */
    uint16_t       pm1b_cnt;
    uint16_t       slp_typa, slp_typb;
    int            can_poweroff;
    const char    *status;
} acpi = { .status = "not probed" };

/* A table is only believed if its bytes sum to zero, which is what the
 * checksum field exists to arrange. */
static int acpi_checksum_ok(const void *p, uint32_t len) {
    const uint8_t *b = (const uint8_t *)p;
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + b[i]);
    return sum == 0;
}

static int acpi_sig_eq(const char *a, const char *b) {
    for (int i = 0; i < 4; i++) if (a[i] != b[i]) return 0;
    return 1;
}

/*
 * Make sure a physical range is reachable before reading it.
 *
 * The direct map covers memory the firmware called usable, and ACPI
 * tables are by definition not that — they sit in reserved regions, and
 * the root pointer on a PC is traditionally in the last 128 KB below one
 * megabyte, which nothing maps. Reading it through the direct map
 * without checking gives a page fault at an address that looks correct,
 * which is exactly what happened the first time this ran.
 *
 * So the pages are mapped on demand, into the shared kernel half, read
 * only in effect and never executable. Pages already present are left
 * alone, which is the common case for a machine whose bootloader mapped
 * everything.
 */
static void acpi_ensure(uint64_t phys, uint64_t len) {
    if (!vmm_ready || !len) return;
    uint64_t v0 = PAGE_ALIGN_DOWN(phys + hal_hhdm_offset);
    uint64_t v1 = PAGE_ALIGN_UP(phys + hal_hhdm_offset + len);
    for (uint64_t v = v0; v < v1; v += PAGE_SIZE) {
        uint64_t *pte = vmm_walk(&vmm_kernel_as, v, 0);
        if (pte && (*pte & PTE_PRESENT)) continue;
        vmm_map(&vmm_kernel_as, v, v - hal_hhdm_offset, PTE_WRITE | PTE_NX);
    }
}

static acpi_sdt_t *acpi_map_sdt(uint64_t phys) {
    /* The header first, because its length field says how much more. */
    acpi_ensure(phys, sizeof(acpi_sdt_t));
    acpi_sdt_t *t = (acpi_sdt_t *)(uintptr_t)phys_to_virt(phys);
    if (t->length > sizeof(acpi_sdt_t) && t->length < (1u << 20))
        acpi_ensure(phys, t->length);
    return t;
}

/* Walk the root table looking for one signature. */
static acpi_sdt_t *acpi_find(uint64_t root_phys, int use_xsdt,
                             const char *sig) {
    acpi_sdt_t *root = acpi_map_sdt(root_phys);
    if (!acpi_checksum_ok(root, root->length)) return 0;

    uint32_t stride = use_xsdt ? 8 : 4;
    uint32_t n = (root->length - (uint32_t)sizeof(acpi_sdt_t)) / stride;
    const uint8_t *ent = (const uint8_t *)root + sizeof(acpi_sdt_t);

    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = use_xsdt
            ? *(const uint64_t *)(ent + (uint64_t)i * 8)
            : (uint64_t)*(const uint32_t *)(ent + (uint64_t)i * 4);
        if (!p) continue;
        acpi_sdt_t *t = acpi_map_sdt(p);
        if (acpi_sig_eq(t->signature, sig) &&
            acpi_checksum_ok(t, t->length))
            return t;
    }
    return 0;
}

/*
 * ---- topology ----
 *
 * An APIC ID is not a number line. It is a bit field whose layout the
 * processor describes through CPUID leaf 0x0B: some low bits identify
 * the thread within a core, the next identify the core within a package,
 * and what is left is the package. Splitting it correctly is the whole
 * difference between "eight processors" and "four cores, each with two
 * threads that share one set of execution units" — which is a scheduling
 * decision, not a cosmetic one, because two threads on one core do not
 * run twice as fast.
 *
 * Leaf 0x0B is the extended topology enumeration. Where it is absent the
 * fallback is CPUID leaf 1 and 4, and where both are absent every
 * processor is treated as its own core, which is the right answer for a
 * machine old enough not to have the leaves.
 */
static void cpuid_count(uint32_t leaf, uint32_t sub, uint32_t *a,
                        uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(sub));
}

static uint32_t cpuid_max_leaf(void) {
    uint32_t a, b, c, d;
    cpuid_count(0, 0, &a, &b, &c, &d);
    return a;
}

static void acpi_topology(void) {
    uint32_t smt_shift = 0, core_shift = 0;
    int have_leaf_b = 0;

    if (cpuid_max_leaf() >= 0x0B) {
        for (uint32_t sub = 0; sub < 4; sub++) {
            uint32_t a, b, c, d;
            cpuid_count(0x0B, sub, &a, &b, &c, &d);
            uint32_t type = (c >> 8) & 0xFF;
            if (type == 0) break;                 /* invalid level */
            have_leaf_b = 1;
            if (type == 1) smt_shift  = a & 0x1F; /* SMT level     */
            if (type == 2) core_shift = a & 0x1F; /* core level    */
        }
    }
    if (!have_leaf_b || core_shift == 0) {
        /* No enumeration: assume every APIC ID is a separate core. */
        smt_shift = 0;
        core_shift = 0;
    }

    int packages = 0, cores = 0, threads = 0;
    uint32_t seen_pkg[ACPI_MAX_CPUS];
    uint32_t seen_core[ACPI_MAX_CPUS];
    int npkg = 0, ncore = 0;

    for (int i = 0; i < acpi.cpu_count; i++) {
        acpi_cpu_t *c = &acpi.cpu[i];
        uint32_t id = c->apic_id;

        c->thread  = smt_shift  ? (id & ((1u << smt_shift) - 1u)) : 0;
        c->core    = core_shift ? ((id >> smt_shift) &
                                   ((1u << (core_shift - smt_shift)) - 1u))
                                : id;
        c->package = core_shift ? (id >> core_shift) : 0;
        c->is_hyperthread = c->thread != 0;

        threads++;

        int found = 0;
        for (int k = 0; k < npkg; k++) if (seen_pkg[k] == c->package) found = 1;
        if (!found && npkg < ACPI_MAX_CPUS) { seen_pkg[npkg++] = c->package; packages++; }

        uint32_t core_key = (c->package << 16) | c->core;
        found = 0;
        for (int k = 0; k < ncore; k++) if (seen_core[k] == core_key) found = 1;
        if (!found && ncore < ACPI_MAX_CPUS) { seen_core[ncore++] = core_key; cores++; }
    }

    acpi.package_count = packages ? packages : 1;
    acpi.core_count    = cores    ? cores    : acpi.cpu_count;
    acpi.thread_count  = threads;
}

static void acpi_parse_madt(acpi_madt_t *m) {
    acpi.lapic_phys = m->lapic_address;
    acpi.dual_8259  = (m->flags & 1) != 0;

    const uint8_t *p   = (const uint8_t *)m + sizeof(acpi_madt_t);
    const uint8_t *end = (const uint8_t *)m + m->hdr.length;

    while (p + 2 <= end) {
        const madt_entry_t *e = (const madt_entry_t *)p;
        if (e->length < 2 || p + e->length > end) break;

        switch (e->type) {
        case MADT_LAPIC: {
            if (acpi.cpu_count < ACPI_MAX_CPUS) {
                acpi_cpu_t *c = &acpi.cpu[acpi.cpu_count++];
                c->acpi_id  = p[2];
                c->apic_id  = p[3];
                uint32_t fl = *(const uint32_t *)(p + 4);
                c->enabled        = (fl & 1) != 0;
                c->online_capable = (fl & 2) != 0;
            }
            break;
        }
        case MADT_X2APIC: {
            if (acpi.cpu_count < ACPI_MAX_CPUS) {
                acpi_cpu_t *c = &acpi.cpu[acpi.cpu_count++];
                c->apic_id  = *(const uint32_t *)(p + 4);
                uint32_t fl = *(const uint32_t *)(p + 8);
                c->acpi_id  = (uint8_t)*(const uint32_t *)(p + 12);
                c->enabled        = (fl & 1) != 0;
                c->online_capable = (fl & 2) != 0;
            }
            break;
        }
        case MADT_IOAPIC: {
            if (acpi.ioapic_count < ACPI_MAX_IOAPIC) {
                acpi_ioapic_t *io = &acpi.ioapic[acpi.ioapic_count++];
                io->id       = p[2];
                io->address  = *(const uint32_t *)(p + 4);
                io->gsi_base = *(const uint32_t *)(p + 8);
            }
            break;
        }
        case MADT_INT_OVERRIDE: {
            /* The firmware is allowed to wire ISA IRQ n somewhere other
             * than global interrupt n, and on most machines it wires the
             * timer somewhere else. Code that assumes otherwise
             * unmasks the wrong line and waits forever. */
            if (acpi.ovr_count < ACPI_MAX_OVR) {
                acpi_override_t *o = &acpi.ovr[acpi.ovr_count++];
                o->bus    = p[2];
                o->source = p[3];
                o->gsi    = *(const uint32_t *)(p + 4);
                o->flags  = *(const uint16_t *)(p + 8);
            }
            break;
        }
        case MADT_LAPIC_ADDR_OVR:
            acpi.lapic_phys = *(const uint64_t *)(p + 4);
            break;
        default:
            break;
        }
        p += e->length;
    }
}

/* Where a legacy ISA interrupt actually lands. */
static uint32_t acpi_gsi_for_irq(uint8_t irq) {
    for (int i = 0; i < acpi.ovr_count; i++)
        if (acpi.ovr[i].source == irq) return acpi.ovr[i].gsi;
    return irq;
}

static void acpi_parse_fadt(acpi_sdt_t *f) {
    const uint8_t *b = (const uint8_t *)f;
    if (f->length >= 68) {
        acpi.pm1a_cnt = (uint16_t)*(const uint32_t *)(b + 64);
        acpi.pm1b_cnt = (uint16_t)*(const uint32_t *)(b + 68 - 4);
    }
    /* S5 sleep values live in the DSDT's \_S5_ package, which needs an
     * AML interpreter to reach. The values below are what every PC in
     * practice uses, and the poweroff path checks that the write took
     * rather than assuming it. */
    acpi.slp_typa = 5;
    acpi.slp_typb = 5;
    acpi.can_poweroff = acpi.pm1a_cnt != 0;
}

static void acpi_parse_mcfg(acpi_sdt_t *t) {
    /* One or more allocation entries after a reserved qword. */
    const uint8_t *p = (const uint8_t *)t + sizeof(acpi_sdt_t) + 8;
    if ((uint32_t)(p - (const uint8_t *)t) + 16 > t->length) return;
    acpi.mcfg_base      = *(const uint64_t *)p;
    acpi.mcfg_bus_start = p[10];
    acpi.mcfg_bus_end   = p[11];
}

/*
 * The bootloader hands over the RSDP, and which *kind* of address it
 * hands over changed underneath this.
 *
 * Limine's older revisions returned a pointer already inside the direct
 * map. Base revision 3 -- which this kernel asks for -- returns the
 * physical address instead. Dereferencing that directly reads virtual
 * address 0x000F52E0, which is not mapped, and the machine faults in
 * ring 0 before a single ACPI table has been looked at.
 *
 * Rather than encode a revision number, the value is classified: an
 * address below the direct map's own base cannot be a direct-map pointer
 * and must therefore be physical. That is true of both conventions and
 * will stay true of any third one.
 */
static void *acpi_fix_pointer(void *p) {
    uint64_t v = (uint64_t)(uintptr_t)p;
    if (!v) return 0;
    if (v < hal_hhdm_offset) {
        acpi_ensure(v, 64);
        return (void *)(uintptr_t)phys_to_virt(v);
    }
    return p;
}

static void acpi_init(void *rsdp_ptr) {
    rsdp_ptr = acpi_fix_pointer(rsdp_ptr);
    if (!rsdp_ptr) {
        acpi.status = "no RSDP from the bootloader";
        serial_puts("[acpi] no RSDP - falling back to probing\n");
        return;
    }

    acpi_rsdp_t *r = (acpi_rsdp_t *)rsdp_ptr;
    if (!acpi_checksum_ok(r, 20)) {
        acpi.status = "RSDP checksum failed";
        serial_puts("[acpi] RSDP checksum failed\n");
        return;
    }

    uint64_t root;
    int use_xsdt = 0;
    if (r->revision >= 2 && acpi_checksum_ok(r, r->length) &&
        r->xsdt_address) {
        root = r->xsdt_address;
        use_xsdt = 1;
    } else {
        root = r->rsdt_address;
    }
    acpi.revision = r->revision;

    acpi_sdt_t *madt = acpi_find(root, use_xsdt, "APIC");
    if (madt) acpi_parse_madt((acpi_madt_t *)madt);

    acpi_sdt_t *fadt = acpi_find(root, use_xsdt, "FACP");
    if (fadt) acpi_parse_fadt(fadt);

    acpi_sdt_t *hpet = acpi_find(root, use_xsdt, "HPET");
    if (hpet && hpet->length >= 52)
        acpi.hpet_phys = *(const uint64_t *)((const uint8_t *)hpet + 44);

    acpi_sdt_t *mcfg = acpi_find(root, use_xsdt, "MCFG");
    if (mcfg) acpi_parse_mcfg(mcfg);

    acpi_topology();
    acpi.valid = 1;
    acpi.status = "tables parsed";

    serial_puts("[acpi] rev ");
    serial_put_dec((uint32_t)acpi.revision);
    serial_puts(", ");
    serial_put_dec((uint32_t)acpi.cpu_count);
    serial_puts(" processors = ");
    serial_put_dec((uint32_t)acpi.package_count);
    serial_puts(" package(s), ");
    serial_put_dec((uint32_t)acpi.core_count);
    serial_puts(" cores, ");
    serial_put_dec((uint32_t)acpi.thread_count);
    serial_puts(" threads\n[acpi] ");
    serial_put_dec((uint32_t)acpi.ioapic_count);
    serial_puts(" I/O APIC(s), ");
    serial_put_dec((uint32_t)acpi.ovr_count);
    serial_puts(" interrupt override(s)");
    if (acpi.hpet_phys) serial_puts(", HPET present");
    if (acpi.mcfg_base) serial_puts(", PCIe extended config present");
    serial_puts("\n");

    for (int i = 0; i < acpi.cpu_count && i < 8; i++) {
        serial_puts("[acpi]   cpu ");
        serial_put_dec((uint32_t)i);
        serial_puts(": apic ");
        serial_put_dec(acpi.cpu[i].apic_id);
        serial_puts(" package ");
        serial_put_dec(acpi.cpu[i].package);
        serial_puts(" core ");
        serial_put_dec(acpi.cpu[i].core);
        serial_puts(acpi.cpu[i].is_hyperthread ? " (hyperthread)" : " (primary)");
        serial_puts(acpi.cpu[i].enabled ? "\n" : " [disabled]\n");
    }
}

/*
 * Ask the firmware to turn the machine off.
 *
 * The sleep type values come from the DSDT in principle and from
 * convention here, so the write is checked rather than trusted: if the
 * machine is still running afterwards, the caller is told and can halt
 * instead of pretending it powered down.
 */
static int acpi_poweroff(void) {
    if (!acpi.can_poweroff) return -1;
    outb(0x64, 0xFE);                     /* keyboard-controller reset,
                                           * harmless if it does nothing */
    uint16_t v = (uint16_t)((acpi.slp_typa & 7) << 10) | (1u << 13);
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(acpi.pm1a_cnt));
    if (acpi.pm1b_cnt) {
        uint16_t v2 = (uint16_t)((acpi.slp_typb & 7) << 10) | (1u << 13);
        __asm__ volatile("outw %0, %1" :: "a"(v2), "Nd"(acpi.pm1b_cnt));
    }
    for (volatile int i = 0; i < 1000000; i++) { }
    return -1;                            /* still here: it did not work */
}

#endif /* ACPI_H */
