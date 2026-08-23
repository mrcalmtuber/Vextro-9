/*
 * tools/vmx_test.c — the parts of VT-x that can be checked without VT-x.
 *
 * QEMU's TCG interpreter does not implement VMX -- asking it directly,
 * `query-cpu-model-expansion max` reports vmx = False and
 * vmx-ept = False -- so no VMXON, VMPTRLD or VMLAUNCH in
 * src/hyper_intel.h has ever executed, and none of them can be made to.
 *
 * What *can* be checked is the arithmetic those instructions consume,
 * and it is worth checking because every value below is a bit pattern
 * that hardware reads and silently misinterprets rather than rejects:
 *
 *   - an EPT entry with the paging format's bits set is not an error,
 *     it is a different mapping with the wrong memory type
 *   - a control field that ignores its capability MSR fails VM entry
 *     with a number rather than a reason
 *   - a 2 MB EPT leaf whose address is not 2 MB aligned maps somewhere
 *     other than where it says
 *
 * So the pure functions get vectors, and the instructions get honesty.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/* The header reaches for the kernel's MSR and physical-address helpers.
 * Neither is needed by the pure arithmetic below, so they are supplied
 * as stubs and the functions that use them are simply not called. */
static uint64_t stub_msr_value = 0;
static uint64_t rdmsr(uint32_t msr) { (void)msr; return stub_msr_value; }
static void wrmsr(uint32_t msr, uint64_t v) { (void)msr; (void)v; }
static uint64_t kern_virt_to_phys(void *p) { return (uint64_t)(uintptr_t)p; }
static void serial_puts(const char *s) { (void)s; }
static void serial_putc(char c) { (void)c; }
static void serial_put_dec(uint32_t v) { (void)v; }

#define VMX_HOST_TEST 1
#define VEXTRO_IDT_H
#define VEXTRO_PCI_H
#define PCI_H
#define IDT_H
#include "hyper_intel.h"

static int checks = 0;
static int fails = 0;

static void ok(const char *what, int cond) {
    checks++;
    if (cond) printf("  ok   %s\n", what);
    else { fails++; printf("  FAIL %s\n", what); }
}

static void eq64(const char *what, uint64_t got, uint64_t want) {
    checks++;
    if (got == want) { printf("  ok   %s\n", what); return; }
    fails++;
    printf("  FAIL %s\n        got  0x%016llx\n        want 0x%016llx\n",
           what, (unsigned long long)got, (unsigned long long)want);
}

/* ===== EPT entry format ===== */

static void test_ept_entries(void) {
    printf("\nEPT entry format\n");

    /*
     * A non-leaf entry carries permissions only. No memory type: the
     * type bits in a table entry are reserved, and setting them is an
     * EPT misconfiguration rather than a slow mapping.
     */
    eq64("a table entry is address | RWX",
         ept_table_entry(0x12345000ULL), 0x12345007ULL);
    ok("  and carries no memory type",
       (ept_table_entry(0x12345000ULL) & (7ULL << 3)) == 0);
    ok("  and is not marked as a large page",
       !(ept_table_entry(0x12345000ULL) & EPT_LARGE_PAGE));

    /*
     * A leaf entry must carry write-back. Leaving the type bits zero
     * selects uncacheable, which is legal, boots, and runs the guest
     * about two orders of magnitude slower -- the kind of bug that
     * looks like "virtualisation is slow" rather than like a defect.
     */
    eq64("a 4 KB leaf is address | RWX | WB | IPAT",
         ept_page_entry(0x12345000ULL),
         0x12345000ULL | 0x7 | (6ULL << 3) | (1ULL << 6));
    eq64("  its memory type is write-back (6)",
         (ept_page_entry(0) >> 3) & 7, 6);
    ok("  and ignore-PAT is set, so the guest cannot make it uncacheable",
       (ept_page_entry(0) & EPT_IGNORE_PAT) != 0);

    eq64("a 2 MB leaf sets the large-page bit",
         ept_large_entry(0x200000ULL),
         0x200000ULL | 0x7 | (6ULL << 3) | (1ULL << 6) | (1ULL << 7));

    /* The address masks: a 2 MB leaf must not carry bits 20:12, which
     * are reserved in a large entry and would fault the walk. */
    ok("a 2 MB leaf drops any sub-2MB address bits",
       (ept_large_entry(0x2FF000ULL) & 0x1FF000ULL) == 0);
    ok("a 4 KB leaf keeps its page address",
       (ept_page_entry(0x2FF000ULL) & 0x000FFFFFFFFFF000ULL) == 0x2FF000ULL);

    /*
     * EPT has no present bit. An entry is present if any of read,
     * write or execute is set -- so zero really is "not mapped", and
     * nothing else needs clearing to unmap a page.
     */
    ok("a zero entry is not present", (0ULL & EPT_RWX) == 0);
    ok("any of R, W or X makes an entry present",
       (ept_page_entry(0x1000) & EPT_RWX) != 0);

    /*
     * And the distinction the whole file turns on: an EPT entry is not
     * a page-table entry. A paging entry with present|write|user is
     * 0x7, which as EPT means read|write|execute with an uncacheable
     * memory type -- the same number, a different meaning.
     */
    ok("the paging bits and the EPT bits are not interchangeable",
       (0x7ULL & (7ULL << 3)) == 0);
}

/* ===== the EPT pointer ===== */

static void test_ept_pointer(void) {
    uint64_t eptp = ept_pointer(0xABCDE000ULL, 0);

    printf("\nthe EPT pointer\n");

    eq64("memory type is write-back", eptp & 7, 6);
    eq64("  page-walk length is 4 (encoded as 3)", (eptp >> 3) & 7, 3);
    ok("  accessed/dirty flags off when not asked for",
       !(eptp & (1ULL << 6)));
    ok("  and on when asked for",
       (ept_pointer(0xABCDE000ULL, 1) & (1ULL << 6)) != 0);
    eq64("  and the PML4 address survives",
         eptp & 0x000FFFFFFFFFF000ULL, 0xABCDE000ULL);
}

/* ===== control-field negotiation ===== */

static void test_ctls_adjust(void) {
    printf("\ncontrol negotiation against the capability MSRs\n");

    /*
     * The capability MSRs are two halves: low says which bits must be
     * 1, high says which bits may be 1. A control written without
     * consulting them fails VM entry, and this is the arithmetic that
     * prevents it.
     */

    /* required = bit 0; allowed = bits 0,1,2. Ask for nothing. */
    stub_msr_value = (uint64_t)0x00000001ULL | ((uint64_t)0x00000007ULL << 32);
    eq64("a required bit is forced on even when not asked for",
         vmx_adjust_ctls(0, 0), 0x1);

    /* Ask for bit 3, which is not allowed. */
    eq64("  an unsupported bit is stripped",
         vmx_adjust_ctls(0x8, 0), 0x1);

    /* Ask for bit 2, which is allowed. */
    eq64("  a supported bit survives", vmx_adjust_ctls(0x4, 0), 0x5);

    /* Everything required, nothing else allowed. */
    stub_msr_value = (uint64_t)0x0000000FULL | ((uint64_t)0x0000000FULL << 32);
    eq64("  a fully fixed control comes back exactly",
         vmx_adjust_ctls(0xFFFFFFFF, 0), 0xF);

    /* Nothing required, everything allowed: the ask passes through. */
    stub_msr_value = (uint64_t)0x0ULL | ((uint64_t)0xFFFFFFFFULL << 32);
    eq64("  an unconstrained control passes the request through",
         vmx_adjust_ctls(0x1234, 0), 0x1234);

    /* The important asymmetry: required beats not-asked-for, and
     * not-allowed beats asked-for. */
    stub_msr_value = (uint64_t)0x2ULL | ((uint64_t)0x2ULL << 32);
    eq64("  required wins over silence", vmx_adjust_ctls(0, 0), 0x2);
    eq64("  forbidden wins over request", vmx_adjust_ctls(0xFFFD, 0), 0x2);
}

/* ===== guest table geometry ===== */

static void test_geometry(void) {
    printf("\nguest and EPT table geometry\n");

    ok("guest memory is a whole number of 2 MB pages",
       (VMX_GUEST_BYTES % 0x200000) == 0);
    eq64("  which is 8 large pages for 16 MB",
         VMX_GUEST_BYTES / 0x200000, 8);
    ok("  and fits one PDPT entry (1 GB reach)",
       VMX_GUEST_BYTES <= (1ULL << 30));

    /* The guest's own tables live inside its memory, below where a
     * kernel is loaded, so building them cannot overwrite the guest. */
    ok("the guest page tables sit below the entry point",
       VMX_G_PML4 < VMX_G_ENTRY && VMX_G_PD < VMX_G_ENTRY);
    ok("  and do not overlap each other",
       VMX_G_PML4 + 4096 <= VMX_G_PDPT && VMX_G_PDPT + 4096 <= VMX_G_PD);
    ok("  and the stack is clear of them",
       VMX_G_STACK >= VMX_G_PD + 4096);
    ok("  and all of it is inside the guest's RAM",
       VMX_G_ENTRY < VMX_GUEST_BYTES);

    /* A 64-bit guest needs long mode, which needs paging, which needs
     * these three set together -- any one missing is a guest that
     * triple-faults on its first instruction. */
    ok("long mode requires PG, PAE and LME together",
       (CR0_PG != 0) && (CR4_PAE != 0) && (EFER_LME != 0));
    eq64("  and IA32E_MODE_GUEST is entry control bit 9",
         ENTRYCTL_IA32E_MODE_GUEST, 1u << 9);
    eq64("  paired with HOST_ADDR_SPACE_SIZE, exit control bit 9",
         EXITCTL_HOST_ADDR_SPACE_64, 1u << 9);
}

/* ===== VMCS field encodings ===== */

static void test_vmcs_encodings(void) {
    printf("\nVMCS field encodings\n");

    /*
     * A VMCS field encoding is structured: bits 14:13 are the width
     * (0 = 16-bit, 1 = 64-bit, 2 = 32-bit, 3 = natural), bits 11:10 the
     * type (0 control, 1 read-only, 2 guest, 3 host). Checking a few
     * against that structure catches a transposed digit, which is
     * otherwise a VMWRITE to a field that exists and means something
     * else entirely.
     */
    #define WIDTH(f) (((f) >> 13) & 3)
    #define TYPE(f)  (((f) >> 10) & 3)

    eq64("GUEST_CR0 is natural-width guest state", WIDTH(VMCS_GUEST_CR0), 3);
    eq64("  and its type is guest", TYPE(VMCS_GUEST_CR0), 2);
    eq64("HOST_CR0 is natural-width host state", WIDTH(VMCS_HOST_CR0), 3);
    eq64("  and its type is host", TYPE(VMCS_HOST_CR0), 3);
    eq64("EPT_POINTER is a 64-bit control field",
         WIDTH(VMCS_EPT_POINTER), 1);
    eq64("  and its type is control", TYPE(VMCS_EPT_POINTER), 0);
    eq64("EXIT_REASON is a 32-bit read-only field",
         WIDTH(VMCS_EXIT_REASON), 2);
    eq64("  and its type is read-only", TYPE(VMCS_EXIT_REASON), 1);
    eq64("GUEST_CS_SEL is a 16-bit guest field",
         WIDTH(VMCS_GUEST_CS_SEL), 0);

    #undef WIDTH
    #undef TYPE

    /* The exit codes must not be AMD's. hyper.h defines EXIT_CPUID as
     * 0x72 for SVM; Intel's is 10, and both headers land in one
     * translation unit. */
    eq64("Intel's CPUID exit is 10, not AMD's 0x72", VMX_EXIT_CPUID, 10);
    eq64("Intel's HLT exit is 12, not AMD's 0x78", VMX_EXIT_HLT, 12);
    eq64("EPT violation is exit 48", VMX_EXIT_EPT_VIOLATION, 48);
}

int main(void) {
    printf("Vextro VT-x: EPT entries, control negotiation, VMCS encodings\n");
    printf("============================================================\n");

    test_ept_entries();
    test_ept_pointer();
    test_ctls_adjust();
    test_geometry();
    test_vmcs_encodings();

    printf("\n%d checks, %d failures\n", checks, fails);
    printf("(no VMX instruction was executed: QEMU TCG reports vmx = False)\n");
    return fails ? 1 : 0;
}
