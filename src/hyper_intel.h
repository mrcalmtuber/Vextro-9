#ifndef VEXTRO_HYPER_INTEL_H
#define VEXTRO_HYPER_INTEL_H

#include "idt.h"    /* rdmsr / wrmsr */
#include "pci.h"    /* kern_virt_to_phys */

/*
 * src/hyper_intel.h — the Intel half of the hypervisor: VT-x, EPT, and
 * a guest that runs in long mode.
 *
 * src/hyper.h runs a guest on AMD-V. This does the same job on Intel
 * silicon, and it is not a port so much as a second implementation:
 * the two vendors solved the same problem with different hardware and
 * almost nothing transfers.
 *
 *   AMD-V                              VT-x
 *   VMCB, a 4 KB struct you fill in    VMCS, opaque; every field is
 *   with ordinary stores               written with VMWRITE by encoding
 *   VMRUN takes its address            VMPTRLD makes one current
 *   nested paging uses ordinary        EPT entries are a different
 *   page-table bits (P|W|U)            format entirely -- see below
 *   controls are what you wrote        controls must be *negotiated*
 *                                      against capability MSRs
 *
 * The last two are where a first attempt fails, and both are handled
 * explicitly further down.
 *
 * ---- 64-bit guests ----
 *
 * hyper.h runs its guest in 32-bit protected mode with paging off,
 * which nested paging makes possible: with no guest paging, guest-
 * physical *is* guest-linear and the guest needs no tables of its own.
 * That is the smallest thing that demonstrates hardware virtualisation
 * and it will not boot anything real.
 *
 * A modern operating system is long mode, and long mode has no unpaged
 * form -- CR0.PG is required, so the guest must have page tables. So
 * the 64-bit path here builds two levels of translation:
 *
 *   guest page tables   guest-virtual -> guest-physical, built by us
 *                       on the guest's behalf, identity mapping its
 *                       own RAM so that a kernel loaded at a physical
 *                       address is reachable at that virtual address
 *
 *   EPT                 guest-physical -> host-physical, so the guest's
 *                       idea of address zero is a page of ours and
 *                       nothing it does can reach this kernel
 *
 * Both are four-level. They are not interchangeable and the entries do
 * not have the same shape, which is the single most common way to get
 * this wrong.
 *
 * ---- what has run ----
 *
 * None of it, and that is checkable rather than a caveat to be
 * discovered. Asking the emulator this is developed against:
 *
 *     query-cpu-model-expansion max  ->  svm = True,  npt = True
 *                                        vmx = False, vmx-ept = False
 *
 * QEMU's TCG interpreter implements AMD's SVM and does not implement
 * VMX at all, so on this machine the AMD path in hyper.h is the one
 * that can be executed and this one cannot. It compiles, its structure
 * sizes and table arithmetic are checked on the host by
 * tools/vmx_test.c, and on real Intel silicon it would be the path
 * taken -- but no VMLAUNCH in this file has ever been executed, and a
 * control field in the wrong place fails in a way only hardware can
 * show.
 */

#ifndef VMX_HOST_TEST
/* Its own, rather than borrowing hyper.h's: that file is the AMD
 * path and this one must not depend on it having been included. */
static void vmx_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                      uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

#endif /* VMX_HOST_TEST */

/* ===== feature detection ===== */

#define IA32_FEATURE_CONTROL        0x3A
#define IA32_VMX_BASIC              0x480
#define IA32_VMX_PINBASED_CTLS      0x481
#define IA32_VMX_PROCBASED_CTLS     0x482
#define IA32_VMX_EXIT_CTLS          0x483
#define IA32_VMX_ENTRY_CTLS         0x484
#define IA32_VMX_MISC               0x485
#define IA32_VMX_CR0_FIXED0         0x486
#define IA32_VMX_CR0_FIXED1         0x487
#define IA32_VMX_CR4_FIXED0         0x488
#define IA32_VMX_CR4_FIXED1         0x489
#define IA32_VMX_PROCBASED_CTLS2    0x48B
#define IA32_VMX_EPT_VPID_CAP       0x48C
#define IA32_VMX_TRUE_PINBASED      0x48D
#define IA32_VMX_TRUE_PROCBASED     0x48E
#define IA32_VMX_TRUE_EXIT          0x48F
#define IA32_VMX_TRUE_ENTRY         0x490

#define IA32_EFER_MSR               0xC0000080
#define IA32_SYSENTER_CS            0x174
#define IA32_SYSENTER_ESP           0x175
#define IA32_SYSENTER_EIP           0x176

#define FEATURE_CONTROL_LOCK        (1ULL << 0)
#define FEATURE_CONTROL_VMX_SMX     (1ULL << 1)
#define FEATURE_CONTROL_VMX_NO_SMX  (1ULL << 2)

#define CR4_VMXE                    (1ULL << 13)
#define CR4_PAE                     (1ULL << 5)
#define CR0_PE                      (1ULL << 0)
#define CR0_PG                      (1ULL << 31)
#define CR0_NE                      (1ULL << 5)
#define EFER_LME                    (1ULL << 8)
#define EFER_LMA                    (1ULL << 10)

/* ===== VMCS field encodings (Intel SDM vol 3, appendix B) ===== */

/* 16-bit guest/host state */
#define VMCS_GUEST_ES_SEL           0x0800
#define VMCS_GUEST_CS_SEL           0x0802
#define VMCS_GUEST_SS_SEL           0x0804
#define VMCS_GUEST_DS_SEL           0x0806
#define VMCS_GUEST_FS_SEL           0x0808
#define VMCS_GUEST_GS_SEL           0x080A
#define VMCS_GUEST_LDTR_SEL         0x080C
#define VMCS_GUEST_TR_SEL           0x080E
#define VMCS_HOST_ES_SEL            0x0C00
#define VMCS_HOST_CS_SEL            0x0C02
#define VMCS_HOST_SS_SEL            0x0C04
#define VMCS_HOST_DS_SEL            0x0C06
#define VMCS_HOST_FS_SEL            0x0C08
#define VMCS_HOST_GS_SEL            0x0C0A
#define VMCS_HOST_TR_SEL            0x0C0C

/* 64-bit control fields */
#define VMCS_IO_BITMAP_A            0x2000
#define VMCS_IO_BITMAP_B            0x2002
#define VMCS_MSR_BITMAP             0x2004
#define VMCS_EPT_POINTER            0x201A

/* 64-bit read-only */
#define VMCS_GUEST_PHYS_ADDR        0x2400

/* 64-bit guest state */
#define VMCS_LINK_POINTER           0x2800
#define VMCS_GUEST_IA32_DEBUGCTL    0x2802
#define VMCS_GUEST_IA32_EFER        0x2806

/* 64-bit host state */
#define VMCS_HOST_IA32_EFER         0x2C02

/* 32-bit control fields */
#define VMCS_PIN_BASED_CTLS         0x4000
#define VMCS_PROC_BASED_CTLS        0x4002
#define VMCS_EXCEPTION_BITMAP       0x4004
#define VMCS_PF_ERROR_MASK          0x4006
#define VMCS_PF_ERROR_MATCH         0x4008
#define VMCS_CR3_TARGET_COUNT       0x400A
#define VMCS_EXIT_CTLS              0x400C
#define VMCS_EXIT_MSR_STORE_COUNT   0x400E
#define VMCS_EXIT_MSR_LOAD_COUNT    0x4010
#define VMCS_ENTRY_CTLS             0x4012
#define VMCS_ENTRY_MSR_LOAD_COUNT   0x4014
#define VMCS_ENTRY_INTR_INFO        0x4016
#define VMCS_PROC_BASED_CTLS2       0x401E

/* 32-bit read-only */
#define VMCS_VM_INSTR_ERROR         0x4400
#define VMCS_EXIT_REASON            0x4402
#define VMCS_EXIT_INTR_INFO         0x4404
#define VMCS_EXIT_INSTR_LEN         0x440C

/* 32-bit guest state */
#define VMCS_GUEST_ES_LIMIT         0x4800
#define VMCS_GUEST_CS_LIMIT         0x4802
#define VMCS_GUEST_SS_LIMIT         0x4804
#define VMCS_GUEST_DS_LIMIT         0x4806
#define VMCS_GUEST_FS_LIMIT         0x4808
#define VMCS_GUEST_GS_LIMIT         0x480A
#define VMCS_GUEST_LDTR_LIMIT       0x480C
#define VMCS_GUEST_TR_LIMIT         0x480E
#define VMCS_GUEST_GDTR_LIMIT       0x4810
#define VMCS_GUEST_IDTR_LIMIT       0x4812
#define VMCS_GUEST_ES_AR            0x4814
#define VMCS_GUEST_CS_AR            0x4816
#define VMCS_GUEST_SS_AR            0x4818
#define VMCS_GUEST_DS_AR            0x481A
#define VMCS_GUEST_FS_AR            0x481C
#define VMCS_GUEST_GS_AR            0x481E
#define VMCS_GUEST_LDTR_AR          0x4820
#define VMCS_GUEST_TR_AR            0x4822
#define VMCS_GUEST_INTERRUPTIBILITY 0x4824
#define VMCS_GUEST_ACTIVITY_STATE   0x4826
#define VMCS_GUEST_SYSENTER_CS      0x482A

/* 32-bit host state */
#define VMCS_HOST_SYSENTER_CS       0x4C00

/* natural-width control */
#define VMCS_CR0_GUEST_HOST_MASK    0x6000
#define VMCS_CR4_GUEST_HOST_MASK    0x6002
#define VMCS_CR0_READ_SHADOW        0x6004
#define VMCS_CR4_READ_SHADOW        0x6006

/* natural-width read-only */
#define VMCS_EXIT_QUALIFICATION     0x6400

/* natural-width guest state */
#define VMCS_GUEST_CR0              0x6800
#define VMCS_GUEST_CR3              0x6802
#define VMCS_GUEST_CR4              0x6804
#define VMCS_GUEST_ES_BASE          0x6806
#define VMCS_GUEST_CS_BASE          0x6808
#define VMCS_GUEST_SS_BASE          0x680A
#define VMCS_GUEST_DS_BASE          0x680C
#define VMCS_GUEST_FS_BASE          0x680E
#define VMCS_GUEST_GS_BASE          0x6810
#define VMCS_GUEST_LDTR_BASE        0x6812
#define VMCS_GUEST_TR_BASE          0x6814
#define VMCS_GUEST_GDTR_BASE        0x6816
#define VMCS_GUEST_IDTR_BASE        0x6818
#define VMCS_GUEST_RSP              0x681C
#define VMCS_GUEST_RIP              0x681E
#define VMCS_GUEST_RFLAGS           0x6820
#define VMCS_GUEST_SYSENTER_ESP     0x6824
#define VMCS_GUEST_SYSENTER_EIP     0x6826

/* natural-width host state */
#define VMCS_HOST_CR0               0x6C00
#define VMCS_HOST_CR3               0x6C02
#define VMCS_HOST_CR4               0x6C04
#define VMCS_HOST_FS_BASE           0x6C06
#define VMCS_HOST_GS_BASE           0x6C08
#define VMCS_HOST_TR_BASE           0x6C0A
#define VMCS_HOST_GDTR_BASE         0x6C0C
#define VMCS_HOST_IDTR_BASE         0x6C0E
#define VMCS_HOST_SYSENTER_ESP      0x6C10
#define VMCS_HOST_SYSENTER_EIP      0x6C12
#define VMCS_HOST_RSP               0x6C14
#define VMCS_HOST_RIP               0x6C16

/* control bits worth naming */
#define PINCTL_EXT_INTR             (1u << 0)
#define PINCTL_NMI                  (1u << 3)

#define PROCCTL_HLT_EXIT            (1u << 7)
#define PROCCTL_CR3_LOAD_EXIT       (1u << 15)
#define PROCCTL_CR3_STORE_EXIT      (1u << 16)
#define PROCCTL_UNCOND_IO_EXIT      (1u << 24)
#define PROCCTL_USE_MSR_BITMAPS     (1u << 28)
#define PROCCTL_SECONDARY           (1u << 31)

#define PROCCTL2_ENABLE_EPT         (1u << 1)
#define PROCCTL2_ENABLE_RDTSCP      (1u << 3)
#define PROCCTL2_UNRESTRICTED_GUEST (1u << 7)
#define PROCCTL2_ENABLE_INVPCID     (1u << 12)
#define PROCCTL2_ENABLE_XSAVES      (1u << 20)

#define EXITCTL_SAVE_DEBUG          (1u << 2)
#define EXITCTL_HOST_ADDR_SPACE_64  (1u << 9)
#define EXITCTL_ACK_INTR_ON_EXIT    (1u << 15)
#define EXITCTL_SAVE_IA32_EFER      (1u << 20)
#define EXITCTL_LOAD_IA32_EFER      (1u << 21)

#define ENTRYCTL_LOAD_DEBUG         (1u << 2)
#define ENTRYCTL_IA32E_MODE_GUEST   (1u << 9)
#define ENTRYCTL_LOAD_IA32_EFER     (1u << 15)

/* Exit reasons, VMX_ prefixed. AMD's VMEXIT codes in hyper.h use the
 * same obvious names for different values -- VMEXIT_CPUID is 0x72 there
 * and 10 here -- and both headers are in one translation unit, so the
 * unprefixed spelling silently resolved to whichever was included
 * first. */
#define VMX_EXIT_EXCEPTION_NMI          0
#define VMX_EXIT_EXTERNAL_INTERRUPT     1
#define VMX_EXIT_TRIPLE_FAULT           2
#define VMX_EXIT_CPUID                  10
#define VMX_EXIT_HLT                    12
#define VMX_EXIT_VMCALL                 18
#define VMX_EXIT_CR_ACCESS              28
#define VMX_EXIT_IO_INSTRUCTION         30
#define VMX_EXIT_RDMSR                  31
#define VMX_EXIT_WRMSR                  32
#define VMX_EXIT_ENTRY_FAIL_GUEST_STATE 33
#define VMX_EXIT_ENTRY_FAIL_MSR_LOAD    34
#define VMX_EXIT_EPT_VIOLATION          48
#define VMX_EXIT_EPT_MISCONFIG          49

/* ===========================================================
 * the guest
 * ===========================================================
 *
 * Sixteen megabytes, which is enough guest-physical space for a small
 * 64-bit kernel and its page tables, and small enough to be a static
 * kernel object rather than something the allocator has to find.
 */

#define VMX_GUEST_BYTES   (16u << 20)
#define VMX_GUEST_PAGES   (VMX_GUEST_BYTES / 4096)

/* Where things sit in guest-physical space. */
#define VMX_G_PML4        0x1000u      /* the guest's own page tables  */
#define VMX_G_PDPT        0x2000u
#define VMX_G_PD          0x3000u
#define VMX_G_STACK       0x9000u
#define VMX_G_ENTRY       0x100000u    /* 1 MB, where a kernel lands   */

static uint8_t vmx_guest_mem[VMX_GUEST_BYTES] __attribute__((aligned(4096)));

/* ===========================================================
 * EPT: guest-physical to host-physical
 * ===========================================================
 *
 * An EPT entry is *not* a page-table entry, and reusing the paging bits
 * is the mistake that makes a guest die with an EPT misconfiguration
 * before it executes an instruction. The formats do not overlap:
 *
 *   page table   bit 0 present, 1 write, 2 user, 3 PWT, 4 PCD ...
 *   EPT          bit 0 read,    1 write, 2 execute,
 *                bits 5:3 memory type (leaf entries only),
 *                bit 6 ignore-PAT (leaf entries only),
 *                bit 7 large page (PD/PDPT entries)
 *
 * There is no present bit: an entry is present if any of read, write or
 * execute is set, so a zero entry is "not present" and 0x7 is "all
 * three". And the memory type has to be write-back on the leaves --
 * leaving those bits zero selects uncacheable, which is architecturally
 * legal, boots, and runs the guest roughly two orders of magnitude
 * slower than it should.
 */

#define EPT_READ          (1ULL << 0)
#define EPT_WRITE         (1ULL << 1)
#define EPT_EXEC          (1ULL << 2)
#define EPT_MEMTYPE_WB    (6ULL << 3)
#define EPT_IGNORE_PAT    (1ULL << 6)
#define EPT_LARGE_PAGE    (1ULL << 7)

#define EPT_RWX           (EPT_READ | EPT_WRITE | EPT_EXEC)

/* A non-leaf entry: permissions only, no memory type. */
static inline uint64_t ept_table_entry(uint64_t phys) {
    return (phys & 0x000FFFFFFFFFF000ULL) | EPT_RWX;
}

/* A leaf entry: permissions, write-back, and ignore whatever the guest
 * PAT says -- the guest does not get to make our memory uncacheable. */
static inline uint64_t ept_page_entry(uint64_t phys) {
    return (phys & 0x000FFFFFFFFFF000ULL) | EPT_RWX | EPT_MEMTYPE_WB |
           EPT_IGNORE_PAT;
}

/* A 2 MB leaf, which is what maps sixteen megabytes in eight entries
 * rather than four thousand. */
static inline uint64_t ept_large_entry(uint64_t phys) {
    return (phys & 0x000FFFFFFFE00000ULL) | EPT_RWX | EPT_MEMTYPE_WB |
           EPT_IGNORE_PAT | EPT_LARGE_PAGE;
}

/*
 * The EPT pointer, which is a VMCS field rather than a register:
 *
 *   bits 2:0   memory type of the EPT structures themselves (WB = 6)
 *   bits 5:3   page-walk length minus one (4 levels -> 3)
 *   bit 6      accessed/dirty flags enabled
 *   bits 51:12 the PML4 address
 */
static inline uint64_t ept_pointer(uint64_t pml4_phys, int enable_ad) {
    return (pml4_phys & 0x000FFFFFFFFFF000ULL) | 6ULL | (3ULL << 3) |
           (enable_ad ? (1ULL << 6) : 0);
}

static uint64_t vmx_ept_pml4[512] __attribute__((aligned(4096)));
static uint64_t vmx_ept_pdpt[512] __attribute__((aligned(4096)));
static uint64_t vmx_ept_pd[512]   __attribute__((aligned(4096)));

/*
 * Map the guest's sixteen megabytes with 2 MB EPT leaves.
 *
 * Large leaves rather than 4 KB pages because the guest memory is one
 * contiguous static object, so every page is adjacent to the last and
 * there is nothing for a finer granularity to express -- and eight
 * entries fit in a cache line where four thousand do not.
 */
static void vmx_build_ept(void) {
    uint64_t base = kern_virt_to_phys(vmx_guest_mem);

    for (int i = 0; i < 512; i++) {
        vmx_ept_pml4[i] = 0;
        vmx_ept_pdpt[i] = 0;
        vmx_ept_pd[i] = 0;
    }

    vmx_ept_pml4[0] = ept_table_entry(kern_virt_to_phys(vmx_ept_pdpt));
    vmx_ept_pdpt[0] = ept_table_entry(kern_virt_to_phys(vmx_ept_pd));

    /* The guest memory has to start on a 2 MB boundary for large leaves
     * to describe it; the array is only 4 KB aligned, so fall back to
     * refusing rather than mapping something that is not there. */
    if (base & 0x1FFFFF) {
        for (uint32_t i = 0; i < VMX_GUEST_BYTES / 0x200000 + 1; i++)
            vmx_ept_pd[i] = 0;
        return;
    }

    for (uint32_t i = 0; i < VMX_GUEST_BYTES / 0x200000; i++)
        vmx_ept_pd[i] = ept_large_entry(base + (uint64_t)i * 0x200000);
}

/* ===========================================================
 * the guest's own page tables, for long mode
 * ===========================================================
 *
 * Long mode requires paging, so a 64-bit guest cannot run the way
 * hyper.h's 32-bit one does. These tables live *in guest memory* and
 * hold guest-physical addresses -- the guest's CR3 points at
 * VMX_G_PML4, which EPT then translates to somewhere in our array.
 *
 * They are ordinary page-table entries, not EPT entries. Two different
 * formats, two levels of translation, and the processor walks both.
 */
static void vmx_build_guest_tables(void) {
    uint64_t *pml4 = (uint64_t *)(vmx_guest_mem + VMX_G_PML4);
    uint64_t *pdpt = (uint64_t *)(vmx_guest_mem + VMX_G_PDPT);
    uint64_t *pd   = (uint64_t *)(vmx_guest_mem + VMX_G_PD);
    const uint64_t flags = 0x3;          /* present | write            */
    const uint64_t leaf  = 0x83;         /* present | write | 2 MB     */

    for (int i = 0; i < 512; i++) { pml4[i] = 0; pdpt[i] = 0; pd[i] = 0; }

    /* Guest-physical addresses, because that is what the guest's own
     * CR3 walk produces -- EPT does the rest. */
    pml4[0] = VMX_G_PDPT | flags;
    pdpt[0] = VMX_G_PD   | flags;

    /* Identity-map the whole sixteen megabytes with 2 MB pages, so a
     * kernel loaded at guest-physical 1 MB is also at virtual 1 MB. */
    for (uint32_t i = 0; i < VMX_GUEST_BYTES / 0x200000; i++)
        pd[i] = ((uint64_t)i * 0x200000) | leaf;
}

#ifndef VMX_HOST_TEST
/* ===========================================================
 * the VMX instructions
 * =========================================================== */

/* Every one of these reports through RFLAGS rather than a register:
 * CF set means the instruction failed with no current VMCS, ZF set
 * means it failed and VM_INSTRUCTION_ERROR says why. Returning 0 for
 * success matches the rest of this kernel. */
static inline int vmx_on(uint64_t phys) {
    uint8_t err;
    __asm__ volatile("vmxon %1; setna %0" : "=r"(err) : "m"(phys) : "cc", "memory");
    return err ? -1 : 0;
}

static inline void vmx_off(void) {
    __asm__ volatile("vmxoff" ::: "cc", "memory");
}

static inline int vmx_clear(uint64_t phys) {
    uint8_t err;
    __asm__ volatile("vmclear %1; setna %0" : "=r"(err) : "m"(phys) : "cc", "memory");
    return err ? -1 : 0;
}

static inline int vmx_ptrld(uint64_t phys) {
    uint8_t err;
    __asm__ volatile("vmptrld %1; setna %0" : "=r"(err) : "m"(phys) : "cc", "memory");
    return err ? -1 : 0;
}

static inline int vmx_write(uint64_t field, uint64_t value) {
    uint8_t err;
    __asm__ volatile("vmwrite %2, %1; setna %0"
                     : "=r"(err) : "r"(field), "r"(value) : "cc", "memory");
    return err ? -1 : 0;
}

static inline uint64_t vmx_read(uint64_t field) {
    uint64_t value = 0;
    __asm__ volatile("vmread %1, %0" : "=r"(value) : "r"(field) : "cc");
    return value;
}

#endif /* VMX_HOST_TEST: the instructions above are x86 asm */

/* ===========================================================
 * negotiating the control fields
 * ===========================================================
 *
 * This is the part with no AMD equivalent and the part that a first
 * VT-x implementation gets wrong.
 *
 * A control bit cannot simply be set. Each control register has a
 * capability MSR whose low half says which bits *must* be 1 and whose
 * high half says which bits *may* be 1. A bit that must be 1 and is
 * written 0 fails VM entry; so does a bit that may not be 1 and is
 * written 1. So the value actually written is the wanted value, forced
 * to include everything required and stripped of everything forbidden.
 *
 * The "true" MSRs (0x48D..0x490) report the same thing without the
 * default-1 bits that the original MSRs are obliged to claim, and are
 * available when IA32_VMX_BASIC bit 55 is set. Using them where they
 * exist is what allows a control like "HLT exiting" to be left off.
 */
static uint32_t vmx_adjust_ctls(uint32_t wanted, uint32_t cap_msr) {
    uint64_t cap = rdmsr(cap_msr);
    uint32_t must_be_1 = (uint32_t)(cap & 0xFFFFFFFFu);
    uint32_t may_be_1  = (uint32_t)(cap >> 32);

    wanted |= must_be_1;      /* everything the processor requires */
    wanted &= may_be_1;       /* nothing it does not support       */
    return wanted;
}

/* CR0 and CR4 have the same treatment: bits the processor fixes at 1
 * in VMX operation, and bits it fixes at 0. */
static uint64_t vmx_fix_cr(uint64_t value, uint32_t fixed0, uint32_t fixed1) {
    value |= rdmsr(fixed0);
    value &= rdmsr(fixed1);
    return value;
}

#ifndef VMX_HOST_TEST
/* ===========================================================
 * state
 * =========================================================== */

static struct {
    int      supported;         /* CPUID says VMX                     */
    int      enabled;           /* VMXON succeeded                    */
    int      ept;               /* EPT available and in use           */
    int      unrestricted;      /* unrestricted guest available       */
    int      long_mode_guest;   /* the guest was entered in IA-32e    */
    uint32_t revision;          /* VMCS revision id                   */
    uint32_t vmcs_size;
    uint64_t vmxon_phys;
    uint64_t vmcs_phys;
    uint32_t exits;
    uint32_t last_exit_reason;
    uint64_t last_qualification;
    const char *status;
} vmx = { .status = "not probed" };

static uint8_t vmx_vmxon_region[4096] __attribute__((aligned(4096)));
static uint8_t vmx_vmcs_region[4096]  __attribute__((aligned(4096)));
static uint8_t vmx_msr_bitmap[4096]   __attribute__((aligned(4096)));

static void vmx_log(const char *s) {
    serial_puts("[vmx] ");
    serial_puts(s);
    serial_putc('\n');
}

/* ===========================================================
 * bring-up
 * =========================================================== */

/*
 * Is there VT-x, and are we allowed to use it?
 *
 * Two separate questions. CPUID says the silicon has it;
 * IA32_FEATURE_CONTROL says the firmware permitted it, and that MSR is
 * write-once -- once the lock bit is set it cannot be changed until the
 * next reset. A machine whose BIOS locked VMX off cannot be talked
 * into it, and reporting that plainly is the only correct response.
 */
static int vmx_probe(void) {
    uint32_t a, b, c, d;
    uint64_t feat;

    vmx.supported = 0;
    vmx.enabled = 0;

    vmx_cpuid(1, &a, &b, &c, &d);
    if (!(c & (1u << 5))) {
        vmx.status = "no VT-x on this processor";
        return -1;
    }

    feat = rdmsr(IA32_FEATURE_CONTROL);
    if ((feat & FEATURE_CONTROL_LOCK) &&
        !(feat & FEATURE_CONTROL_VMX_NO_SMX)) {
        vmx.status = "VT-x present but disabled and locked by firmware";
        return -1;
    }
    if (!(feat & FEATURE_CONTROL_LOCK)) {
        /* Unlocked: opt in and lock it ourselves, which is what the
         * architecture expects the first software to touch it to do. */
        wrmsr(IA32_FEATURE_CONTROL,
              feat | FEATURE_CONTROL_LOCK | FEATURE_CONTROL_VMX_NO_SMX);
    }

    {
        uint64_t basic = rdmsr(IA32_VMX_BASIC);
        vmx.revision  = (uint32_t)(basic & 0x7FFFFFFFu);
        vmx.vmcs_size = (uint32_t)((basic >> 32) & 0x1FFFu);
        if (vmx.vmcs_size == 0 || vmx.vmcs_size > 4096) {
            vmx.status = "VMCS size out of range";
            return -1;
        }
    }

    /* EPT and unrestricted guest live in the secondary controls, which
     * only exist if the primary controls say they do. */
    {
        uint64_t proc = rdmsr(IA32_VMX_PROCBASED_CTLS);
        if ((uint32_t)(proc >> 32) & PROCCTL_SECONDARY) {
            uint64_t proc2 = rdmsr(IA32_VMX_PROCBASED_CTLS2);
            uint32_t allow = (uint32_t)(proc2 >> 32);
            vmx.ept          = (allow & PROCCTL2_ENABLE_EPT) ? 1 : 0;
            vmx.unrestricted = (allow & PROCCTL2_UNRESTRICTED_GUEST) ? 1 : 0;
        }
    }

    if (!vmx.ept) {
        /* Without EPT the guest's physical addresses would have to be
         * shadowed by hand, which is a different and much larger
         * hypervisor. Refusing is honest. */
        vmx.status = "VT-x without EPT: not supported by this hypervisor";
        return -1;
    }

    vmx.supported = 1;
    vmx.status = "VT-x with EPT available";
    return 0;
}

/*
 * Enter VMX operation.
 *
 * CR4.VMXE first, then CR0 and CR4 forced to the values the processor
 * fixes in VMX operation, then VMXON against a region stamped with the
 * revision id. A revision mismatch is the most common reason VMXON
 * fails on a machine that supports it.
 */
static int vmx_enable(void) {
    uint64_t cr0, cr4;

    if (!vmx.supported) return -1;

    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    cr4 |= CR4_VMXE;
    cr0 = vmx_fix_cr(cr0, IA32_VMX_CR0_FIXED0, IA32_VMX_CR0_FIXED1);
    cr4 = vmx_fix_cr(cr4, IA32_VMX_CR4_FIXED0, IA32_VMX_CR4_FIXED1);

    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0) : "memory");
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4) : "memory");

    for (int i = 0; i < 4096; i++) vmx_vmxon_region[i] = 0;
    *(uint32_t *)vmx_vmxon_region = vmx.revision;
    vmx.vmxon_phys = kern_virt_to_phys(vmx_vmxon_region);

    if (vmx_on(vmx.vmxon_phys) != 0) {
        vmx.status = "VMXON refused";
        return -1;
    }

    vmx.enabled = 1;
    vmx.status = "in VMX operation";
    return 0;
}

/*
 * Fill in the VMCS.
 *
 * Every field is written with VMWRITE; there is no struct to assign to.
 * The host half describes where the processor returns on a VM exit, and
 * getting it wrong does not fail entry -- it fails on the way *back*,
 * which is unrecoverable.
 */
static int vmx_build_vmcs(uint64_t guest_rip, uint64_t host_rip) {
    uint16_t host_cs, host_ss, host_tr;
    uint64_t host_cr0, host_cr3, host_cr4;
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr, idtr;

    for (int i = 0; i < 4096; i++) vmx_vmcs_region[i] = 0;
    *(uint32_t *)vmx_vmcs_region = vmx.revision;
    vmx.vmcs_phys = kern_virt_to_phys(vmx_vmcs_region);

    if (vmx_clear(vmx.vmcs_phys) != 0) { vmx.status = "VMCLEAR failed"; return -1; }
    if (vmx_ptrld(vmx.vmcs_phys) != 0) { vmx.status = "VMPTRLD failed"; return -1; }

    /* ---- controls, every one negotiated ---- */
    {
        uint64_t basic = rdmsr(IA32_VMX_BASIC);
        int have_true = (basic >> 55) & 1;

        uint32_t pin = vmx_adjust_ctls(0,
            have_true ? IA32_VMX_TRUE_PINBASED : IA32_VMX_PINBASED_CTLS);

        uint32_t proc = vmx_adjust_ctls(PROCCTL_HLT_EXIT |
                                        PROCCTL_UNCOND_IO_EXIT |
                                        PROCCTL_SECONDARY,
            have_true ? IA32_VMX_TRUE_PROCBASED : IA32_VMX_PROCBASED_CTLS);

        uint32_t proc2 = vmx_adjust_ctls(PROCCTL2_ENABLE_EPT |
                                         (vmx.unrestricted
                                            ? PROCCTL2_UNRESTRICTED_GUEST : 0),
                                         IA32_VMX_PROCBASED_CTLS2);

        /* The host is 64-bit, so the exit controls must say so --
         * without HOST_ADDR_SPACE_SIZE the processor returns from the
         * exit in compatibility mode and immediately triple-faults. */
        uint32_t exit_c = vmx_adjust_ctls(EXITCTL_HOST_ADDR_SPACE_64 |
                                          EXITCTL_SAVE_IA32_EFER |
                                          EXITCTL_LOAD_IA32_EFER,
            have_true ? IA32_VMX_TRUE_EXIT : IA32_VMX_EXIT_CTLS);

        /* And the guest is 64-bit, which is the whole point of this
         * path: IA32E_MODE_GUEST is what makes VM entry establish long
         * mode rather than protected mode. */
        uint32_t entry_c = vmx_adjust_ctls(ENTRYCTL_IA32E_MODE_GUEST |
                                           ENTRYCTL_LOAD_IA32_EFER,
            have_true ? IA32_VMX_TRUE_ENTRY : IA32_VMX_ENTRY_CTLS);

        vmx_write(VMCS_PIN_BASED_CTLS, pin);
        vmx_write(VMCS_PROC_BASED_CTLS, proc);
        vmx_write(VMCS_PROC_BASED_CTLS2, proc2);
        vmx_write(VMCS_EXIT_CTLS, exit_c);
        vmx_write(VMCS_ENTRY_CTLS, entry_c);

        vmx.long_mode_guest = (entry_c & ENTRYCTL_IA32E_MODE_GUEST) ? 1 : 0;
    }

    vmx_write(VMCS_EXCEPTION_BITMAP, 0);
    vmx_write(VMCS_PF_ERROR_MASK, 0);
    vmx_write(VMCS_PF_ERROR_MATCH, 0);
    vmx_write(VMCS_CR3_TARGET_COUNT, 0);
    vmx_write(VMCS_EXIT_MSR_STORE_COUNT, 0);
    vmx_write(VMCS_EXIT_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_ENTRY_MSR_LOAD_COUNT, 0);
    vmx_write(VMCS_ENTRY_INTR_INFO, 0);

    /* ---- EPT ---- */
    vmx_build_ept();
    vmx_write(VMCS_EPT_POINTER,
              ept_pointer(kern_virt_to_phys(vmx_ept_pml4), 0));

    /* The link pointer must be all ones when VMCS shadowing is not in
     * use; zero means "there is a shadow VMCS here" and fails entry. */
    vmx_write(VMCS_LINK_POINTER, ~0ULL);

    /* ---- guest state: long mode ---- */
    vmx_build_guest_tables();

    {
        uint64_t gcr0 = vmx_fix_cr(CR0_PE | CR0_PG | CR0_NE,
                                   IA32_VMX_CR0_FIXED0, IA32_VMX_CR0_FIXED1);
        uint64_t gcr4 = vmx_fix_cr(CR4_PAE,
                                   IA32_VMX_CR4_FIXED0, IA32_VMX_CR4_FIXED1);

        vmx_write(VMCS_GUEST_CR0, gcr0);
        vmx_write(VMCS_GUEST_CR3, VMX_G_PML4);   /* guest-physical */
        vmx_write(VMCS_GUEST_CR4, gcr4);
        vmx_write(VMCS_GUEST_IA32_EFER, EFER_LME | EFER_LMA);

        /* The guest sees its own CR0/CR4; nothing is hidden behind a
         * shadow, so the masks are empty and the shadows match. */
        vmx_write(VMCS_CR0_GUEST_HOST_MASK, 0);
        vmx_write(VMCS_CR4_GUEST_HOST_MASK, 0);
        vmx_write(VMCS_CR0_READ_SHADOW, gcr0);
        vmx_write(VMCS_CR4_READ_SHADOW, gcr4);
    }

    /*
     * Segments. In long mode the bases and limits are ignored for
     * everything but FS and GS, but the *access rights* are not -- an
     * unusable bit in the wrong place is a guest-state entry failure
     * with no further explanation.
     *
     * 0xA09B: present, code, execute/read, long mode, accessed.
     * 0xC093: present, data, read/write, 32-bit, granularity, accessed.
     */
    vmx_write(VMCS_GUEST_CS_SEL, 0x08);
    vmx_write(VMCS_GUEST_CS_BASE, 0);
    vmx_write(VMCS_GUEST_CS_LIMIT, 0xFFFFFFFF);
    vmx_write(VMCS_GUEST_CS_AR, 0xA09B);

    {
        const uint16_t data_sel = 0x10;
        const uint64_t data_ar  = 0xC093;
        vmx_write(VMCS_GUEST_SS_SEL, data_sel);
        vmx_write(VMCS_GUEST_SS_BASE, 0);
        vmx_write(VMCS_GUEST_SS_LIMIT, 0xFFFFFFFF);
        vmx_write(VMCS_GUEST_SS_AR, data_ar);
        vmx_write(VMCS_GUEST_DS_SEL, data_sel);
        vmx_write(VMCS_GUEST_DS_BASE, 0);
        vmx_write(VMCS_GUEST_DS_LIMIT, 0xFFFFFFFF);
        vmx_write(VMCS_GUEST_DS_AR, data_ar);
        vmx_write(VMCS_GUEST_ES_SEL, data_sel);
        vmx_write(VMCS_GUEST_ES_BASE, 0);
        vmx_write(VMCS_GUEST_ES_LIMIT, 0xFFFFFFFF);
        vmx_write(VMCS_GUEST_ES_AR, data_ar);
        vmx_write(VMCS_GUEST_FS_SEL, data_sel);
        vmx_write(VMCS_GUEST_FS_BASE, 0);
        vmx_write(VMCS_GUEST_FS_LIMIT, 0xFFFFFFFF);
        vmx_write(VMCS_GUEST_FS_AR, data_ar);
        vmx_write(VMCS_GUEST_GS_SEL, data_sel);
        vmx_write(VMCS_GUEST_GS_BASE, 0);
        vmx_write(VMCS_GUEST_GS_LIMIT, 0xFFFFFFFF);
        vmx_write(VMCS_GUEST_GS_AR, data_ar);
    }

    /* LDTR unusable (bit 16), TR busy-64-bit and present -- TR must be
     * usable even though the guest never task-switches. */
    vmx_write(VMCS_GUEST_LDTR_SEL, 0);
    vmx_write(VMCS_GUEST_LDTR_BASE, 0);
    vmx_write(VMCS_GUEST_LDTR_LIMIT, 0);
    vmx_write(VMCS_GUEST_LDTR_AR, 0x10000);
    vmx_write(VMCS_GUEST_TR_SEL, 0x18);
    vmx_write(VMCS_GUEST_TR_BASE, 0);
    vmx_write(VMCS_GUEST_TR_LIMIT, 0xFFFF);
    vmx_write(VMCS_GUEST_TR_AR, 0x8B);

    vmx_write(VMCS_GUEST_GDTR_BASE, 0);
    vmx_write(VMCS_GUEST_GDTR_LIMIT, 0xFFFF);
    vmx_write(VMCS_GUEST_IDTR_BASE, 0);
    vmx_write(VMCS_GUEST_IDTR_LIMIT, 0xFFFF);

    vmx_write(VMCS_GUEST_RSP, VMX_G_STACK);
    vmx_write(VMCS_GUEST_RIP, guest_rip);
    vmx_write(VMCS_GUEST_RFLAGS, 0x2);        /* bit 1 is always set */
    vmx_write(VMCS_GUEST_IA32_DEBUGCTL, 0);
    vmx_write(VMCS_GUEST_INTERRUPTIBILITY, 0);
    vmx_write(VMCS_GUEST_ACTIVITY_STATE, 0);  /* active */
    vmx_write(VMCS_GUEST_SYSENTER_CS, 0);
    vmx_write(VMCS_GUEST_SYSENTER_ESP, 0);
    vmx_write(VMCS_GUEST_SYSENTER_EIP, 0);

    /* ---- host state ---- */
    __asm__ volatile("mov %%cr0, %0" : "=r"(host_cr0));
    __asm__ volatile("mov %%cr3, %0" : "=r"(host_cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(host_cr4));
    __asm__ volatile("mov %%cs, %0"  : "=r"(host_cs));
    __asm__ volatile("mov %%ss, %0"  : "=r"(host_ss));
    __asm__ volatile("str %0"        : "=r"(host_tr));
    __asm__ volatile("sgdt %0"       : "=m"(gdtr));
    __asm__ volatile("sidt %0"       : "=m"(idtr));

    vmx_write(VMCS_HOST_CR0, host_cr0);
    vmx_write(VMCS_HOST_CR3, host_cr3);
    vmx_write(VMCS_HOST_CR4, host_cr4);

    /* Host selectors must have RPL and TI clear -- the processor loads
     * them directly and a stray low bit is a fault on the way out. */
    vmx_write(VMCS_HOST_CS_SEL, host_cs & 0xF8);
    vmx_write(VMCS_HOST_SS_SEL, host_ss & 0xF8);
    vmx_write(VMCS_HOST_DS_SEL, host_ss & 0xF8);
    vmx_write(VMCS_HOST_ES_SEL, host_ss & 0xF8);
    vmx_write(VMCS_HOST_FS_SEL, host_ss & 0xF8);
    vmx_write(VMCS_HOST_GS_SEL, host_ss & 0xF8);
    vmx_write(VMCS_HOST_TR_SEL, host_tr & 0xF8);

    vmx_write(VMCS_HOST_GDTR_BASE, gdtr.base);
    vmx_write(VMCS_HOST_IDTR_BASE, idtr.base);
    vmx_write(VMCS_HOST_TR_BASE, 0);
    vmx_write(VMCS_HOST_FS_BASE, rdmsr(0xC0000100));
    vmx_write(VMCS_HOST_GS_BASE, rdmsr(0xC0000101));
    vmx_write(VMCS_HOST_IA32_EFER, rdmsr(IA32_EFER_MSR));
    vmx_write(VMCS_HOST_SYSENTER_CS, 0);
    vmx_write(VMCS_HOST_SYSENTER_ESP, 0);
    vmx_write(VMCS_HOST_SYSENTER_EIP, 0);
    vmx_write(VMCS_HOST_RIP, host_rip);

    return 0;
}

/* ===========================================================
 * exits
 * =========================================================== */

static const char *vmx_exit_name(uint32_t reason) {
    switch (reason) {
    case VMX_EXIT_EXCEPTION_NMI:          return "exception or NMI";
    case VMX_EXIT_EXTERNAL_INTERRUPT:     return "external interrupt";
    case VMX_EXIT_TRIPLE_FAULT:           return "triple fault";
    case VMX_EXIT_CPUID:                  return "CPUID";
    case VMX_EXIT_HLT:                    return "HLT";
    case VMX_EXIT_VMCALL:                 return "VMCALL";
    case VMX_EXIT_CR_ACCESS:              return "control register access";
    case VMX_EXIT_IO_INSTRUCTION:         return "I/O instruction";
    case VMX_EXIT_RDMSR:                  return "RDMSR";
    case VMX_EXIT_WRMSR:                  return "WRMSR";
    case VMX_EXIT_ENTRY_FAIL_GUEST_STATE: return "VM entry failed: guest state";
    case VMX_EXIT_ENTRY_FAIL_MSR_LOAD:    return "VM entry failed: MSR load";
    case VMX_EXIT_EPT_VIOLATION:          return "EPT violation";
    case VMX_EXIT_EPT_MISCONFIG:          return "EPT misconfiguration";
    }
    return "unknown";
}

/*
 * Record why the guest came back.
 *
 * Bit 31 of the exit reason means VM entry itself failed rather than
 * the guest having run at all, and the two want completely different
 * responses: an entry failure is a bug in the VMCS above, a genuine
 * exit is the guest doing something intercepted.
 */
static void vmx_note_exit(void) {
    uint32_t reason = (uint32_t)vmx_read(VMCS_EXIT_REASON);

    vmx.exits++;
    vmx.last_exit_reason = reason & 0xFFFF;
    vmx.last_qualification = vmx_read(VMCS_EXIT_QUALIFICATION);

    if (reason & (1u << 31)) {
        serial_puts("[vmx] VM ENTRY FAILED: ");
        serial_puts(vmx_exit_name(reason & 0xFFFF));
        serial_puts(", instruction error ");
        serial_put_dec((uint32_t)vmx_read(VMCS_VM_INSTR_ERROR));
        serial_putc('\n');
    }
}

/* ===========================================================
 * the public face
 * =========================================================== */

/*
 * Bring VT-x up as far as it will go on this machine.
 *
 * Returns 0 when the processor is in VMX operation with a VMCS built
 * for a 64-bit guest. It deliberately stops short of VMLAUNCH: the
 * launch needs a host-resume stub in assembly that this kernel does not
 * have yet, and entering a guest with no way back is worse than not
 * entering one.
 */
static int vmx_init(void) {
    if (vmx_probe() != 0) {
        vmx_log(vmx.status);
        return -1;
    }

    serial_puts("[vmx] VT-x present: revision ");
    serial_put_dec(vmx.revision);
    serial_puts(", VMCS ");
    serial_put_dec(vmx.vmcs_size);
    serial_puts(" bytes, EPT yes, unrestricted guest ");
    serial_puts(vmx.unrestricted ? "yes\n" : "no\n");

    if (vmx_enable() != 0) {
        vmx_log(vmx.status);
        return -1;
    }
    vmx_log("in VMX operation");

    if (vmx_build_vmcs(VMX_G_ENTRY, 0) != 0) {
        vmx_log(vmx.status);
        vmx_off();
        vmx.enabled = 0;
        return -1;
    }

    serial_puts("[vmx] VMCS built for a ");
    serial_puts(vmx.long_mode_guest ? "64-bit" : "32-bit");
    serial_puts(" guest, ");
    serial_put_dec(VMX_GUEST_BYTES >> 20);
    serial_puts(" MB behind EPT\n");

    vmx.status = "ready (VMLAUNCH not issued: no resume stub)";
    return 0;
}

static void vmx_shutdown(void) {
    if (!vmx.enabled) return;
    vmx_clear(vmx.vmcs_phys);
    vmx_off();
    vmx.enabled = 0;
    vmx.status = "shut down";
}

static int vmx_available(void) { return vmx.supported; }
static const char *vmx_status(void) { return vmx.status; }

#endif /* VMX_HOST_TEST: everything above touches hardware */

#endif /* VEXTRO_HYPER_INTEL_H */
