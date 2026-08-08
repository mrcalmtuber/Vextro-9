#ifndef VEXTRO_HYPER_H
#define VEXTRO_HYPER_H

#include "idt.h"    /* rdmsr / wrmsr */
#include "pci.h"    /* kern_virt_to_phys */

/*
 * src/hyper.h — a type-1 hypervisor, on AMD-V.
 *
 * This runs a guest operating environment on the real CPU, in hardware,
 * with its own address space: not an interpreter pretending to be a
 * processor. The guest's instructions execute at native speed and only
 * the operations the host chose to intercept -- CPUID, I/O, MSR access,
 * HLT, and the hypercall instruction -- trap back here.
 *
 * AMD-V rather than Intel VT-x, for a reason that is checkable: QEMU's
 * TCG interpreter implements SVM including nested paging, and does not
 * implement VMX at all. Asking the emulator directly:
 *
 *     query-cpu-model-expansion max  ->  svm = True, npt = True,
 *                                        vmx = False
 *
 * so on this machine the AMD path is the one that can actually be run
 * and tested, and the Intel path would be code no one could execute.
 * On real Intel silicon this reports "no SVM" and stays out of the way.
 *
 * The pieces:
 *
 *   1. feature probe   CPUID 8000_0001.ECX[2], then 8000_000A for
 *                      nested paging and the NRIP-save optimisation
 *   2. enable          EFER.SVME, after checking VM_CR.SVMDIS
 *   3. host save area  a page the processor parks host state in
 *   4. VMCB            control area: what to intercept, the ASID, the
 *                      nested CR3; state save area: the guest's
 *                      registers and segments
 *   5. nested paging   a four-level table mapping guest-physical to
 *                      host-physical, so the guest's idea of address
 *                      zero is a page of ours and nothing it does can
 *                      reach the kernel
 *   6. the run loop    VMRUN, decode the exit, service it, re-enter
 *
 * The guest is 32-bit protected mode with paging off. That is a
 * deliberate choice: with nested paging the processor translates
 * guest-physical addresses itself, so an unpaged guest needs no page
 * tables of its own, and a 32-bit flat model is the smallest thing that
 * is still a real protected-mode environment.
 */

/* ===== SVM constants ===== */

#define MSR_VM_CR          0xC0010114u
#define MSR_VM_HSAVE_PA    0xC0010117u
#define VM_CR_SVMDIS       (1u << 4)
#define EFER_SVME          (1ull << 12)

/* control area */
#define VMCB_INTERCEPT_CR       0x000
#define VMCB_INTERCEPT_EXC      0x008
#define VMCB_INTERCEPT_MISC1    0x00C
#define VMCB_INTERCEPT_MISC2    0x010
#define VMCB_IOPM_BASE          0x040
#define VMCB_MSRPM_BASE         0x048
#define VMCB_TSC_OFFSET         0x050
#define VMCB_GUEST_ASID         0x058
#define VMCB_TLB_CONTROL        0x05C
#define VMCB_EXITCODE           0x070
#define VMCB_EXITINFO1          0x078
#define VMCB_EXITINFO2          0x080
#define VMCB_EXITINTINFO        0x088
#define VMCB_NP_ENABLE          0x090
#define VMCB_EVENTINJ           0x0A8
#define VMCB_N_CR3              0x0B0
#define VMCB_CLEAN              0x0C0
#define VMCB_NRIP               0x0C8

/* MISC1 intercepts */
#define INTERCEPT_INTR     (1u << 0)
#define INTERCEPT_CPUID    (1u << 18)
#define INTERCEPT_HLT      (1u << 24)
#define INTERCEPT_IOIO     (1u << 27)
#define INTERCEPT_MSR      (1u << 28)
#define INTERCEPT_SHUTDOWN (1u << 31)

/* MISC2 intercepts */
#define INTERCEPT_VMRUN    (1u << 0)
#define INTERCEPT_VMMCALL  (1u << 1)

/* state save area */
#define VMCB_ES     0x400
#define VMCB_CS     0x410
#define VMCB_SS     0x420
#define VMCB_DS     0x430
#define VMCB_FS     0x440
#define VMCB_GS     0x450
#define VMCB_GDTR   0x460
#define VMCB_LDTR   0x470
#define VMCB_IDTR   0x480
#define VMCB_TR     0x490
#define VMCB_CPL    0x4CB
#define VMCB_EFER   0x4D0
#define VMCB_CR4    0x548
#define VMCB_CR3    0x550
#define VMCB_CR0    0x558
#define VMCB_DR7    0x560
#define VMCB_DR6    0x568
#define VMCB_RFLAGS 0x570
#define VMCB_RIP    0x578
#define VMCB_RSP    0x5D8
#define VMCB_RAX    0x5F8
#define VMCB_CR2    0x640
#define VMCB_G_PAT  0x668

/* exit codes */
#define EXIT_INTR      0x060
#define EXIT_CPUID     0x072
#define EXIT_HLT       0x078
#define EXIT_IOIO      0x07B
#define EXIT_MSR       0x07C
#define EXIT_VMMCALL   0x081
#define EXIT_NPF       0x400
#define EXIT_INVALID   0xFFFFFFFFFFFFFFFFull

/* ===== memory ===== */

/*
 * A megabyte of guest RAM, which is what makes the classic addresses
 * work: code at 0x1000, scratch at 0x2000, and a text screen at
 * 0xB8000 -- all inside one flat megabyte the guest owns and cannot
 * reach past. The nested page table maps exactly this and nothing else,
 * so a stray guest write faults instead of finding the kernel.
 */
#define HV_GUEST_BYTES  (1u << 20)
#define HV_GUEST_PAGES  (HV_GUEST_BYTES / 4096)

#define HV_ENTRY        0x1000u
#define HV_SCRATCH      0x2000u
#define HV_STACK        0x8000u
#define HV_SCREEN       0xB8000u
#define HV_SCREEN_COLS  80
#define HV_SCREEN_ROWS  25

static uint8_t hv_guest_mem[HV_GUEST_BYTES] __attribute__((aligned(4096)));
static uint8_t hv_vmcb[4096]   __attribute__((aligned(4096)));
static uint8_t hv_hsave[4096]  __attribute__((aligned(4096)));
static uint8_t hv_iopm[12288]  __attribute__((aligned(4096)));
static uint8_t hv_msrpm[8192]  __attribute__((aligned(4096)));

/* four levels, one page each: enough for the single megabyte above */
static uint64_t hv_npt_pml4[512] __attribute__((aligned(4096)));
static uint64_t hv_npt_pdpt[512] __attribute__((aligned(4096)));
static uint64_t hv_npt_pd[512]   __attribute__((aligned(4096)));
static uint64_t hv_npt_pt[512]   __attribute__((aligned(4096)));

/* ===== the guest =====
 *
 * Assembled from this source with the same cross toolchain the kernel
 * uses, and pasted in as bytes so the build needs no extra step:
 *
 *   .code32
 *   _start:
 *     movl  $0xB8000, %edi          # the text screen
 *     movl  $msg, %esi
 *     movb  $0x0A, %ah              # bright green on black
 *   1: lodsb
 *     testb %al, %al
 *     jz    2f
 *     movw  %ax, (%edi)
 *     addl  $2, %edi
 *     jmp   1b
 *   2: movl  $1, %eax               # hypercall 1: add to the argument
 *     movl  $0x1234, %ebx
 *     vmmcall
 *     movl  %eax, 0x2000            # store what the host answered
 *     movl  $0x40000000, %eax       # the hypervisor CPUID leaf
 *     cpuid
 *     movl  %eax, 0x2004
 *     movl  %ebx, 0x2008
 *     movl  %ecx, 0x200c
 *     movl  %edx, 0x2010
 *     movw  $0x3F8, %dx             # an I/O write the host intercepts
 *     movb  $0x56, %al
 *     outb  %al, %dx
 *     movl  $2, %eax                # hypercall 2: stop
 *     vmmcall
 *     hlt
 *   3: jmp 3b
 *   msg: .asciz "Vextro legacy guest: 32-bit, unpaged, under NPT"
 */
static const uint8_t hv_guest_code[] = {
    0xBF, 0x00, 0x80, 0x0B, 0x00, 0xBE, 0x5B, 0x10, 0x00, 0x00, 0xB4, 0x0A,
    0xAC, 0x84, 0xC0, 0x74, 0x08, 0x66, 0x89, 0x07, 0x83, 0xC7, 0x02, 0xEB,
    0xF3, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x34, 0x12, 0x00, 0x00, 0x0F,
    0x01, 0xD9, 0xA3, 0x00, 0x20, 0x00, 0x00, 0xB8, 0x00, 0x00, 0x00, 0x40,
    0x0F, 0xA2, 0xA3, 0x04, 0x20, 0x00, 0x00, 0x89, 0x1D, 0x08, 0x20, 0x00,
    0x00, 0x89, 0x0D, 0x0C, 0x20, 0x00, 0x00, 0x89, 0x15, 0x10, 0x20, 0x00,
    0x00, 0x66, 0xBA, 0xF8, 0x03, 0xB0, 0x56, 0xEE, 0xB8, 0x02, 0x00, 0x00,
    0x00, 0x0F, 0x01, 0xD9, 0xF4, 0xEB, 0xFE, 0x56, 0x65, 0x78, 0x74, 0x72,
    0x6F, 0x20, 0x6C, 0x65, 0x67, 0x61, 0x63, 0x79, 0x20, 0x67, 0x75, 0x65,
    0x73, 0x74, 0x3A, 0x20, 0x33, 0x32, 0x2D, 0x62, 0x69, 0x74, 0x2C, 0x20,
    0x75, 0x6E, 0x70, 0x61, 0x67, 0x65, 0x64, 0x2C, 0x20, 0x75, 0x6E, 0x64,
    0x65, 0x72, 0x20, 0x4E, 0x50, 0x54, 0x00,
};

/* ===== guest general-purpose registers =====
 *
 * VMRUN saves and restores the host's control state and RAX, and
 * nothing else: RBX through R15 are simply whatever the guest left in
 * them when it exited. So they are stashed here the instant VMRUN
 * returns and reloaded just before the next one. Missing this is the
 * classic way to write a hypervisor that corrupts its own host.
 */
typedef struct {
    uint64_t rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
} hv_gpr_t;

static hv_gpr_t hv_gpr;

/*
 * The world switch.
 *
 * Written as a whole function in assembly because it has to control
 * every register across the transition, which no amount of inline-asm
 * constraints can express: the guest's registers are loaded, the guest
 * runs, and its registers are captured before the compiler is allowed
 * to touch anything.
 *
 * Interrupts are off around VMRUN (the caller clears IF). The guest's
 * own RFLAGS has IF clear too, so a host interrupt that arrives while
 * the guest is running stays pending -- and the INTR intercept turns it
 * into an ordinary exit rather than something delivered to the guest.
 */
__asm__(
    ".text\n"
    ".globl hv_world_switch\n"
    ".type hv_world_switch, @function\n"
    "hv_world_switch:\n"
    "    push %rbx\n"
    "    push %rbp\n"
    "    push %r12\n"
    "    push %r13\n"
    "    push %r14\n"
    "    push %r15\n"
    "    mov  %rdi, %rax\n"              /* VMRUN takes the VMCB in RAX */
    "    mov  hv_gpr+0(%rip),   %rbx\n"
    "    mov  hv_gpr+8(%rip),   %rcx\n"
    "    mov  hv_gpr+16(%rip),  %rdx\n"
    "    mov  hv_gpr+24(%rip),  %rbp\n"
    "    mov  hv_gpr+48(%rip),  %r8\n"
    "    mov  hv_gpr+56(%rip),  %r9\n"
    "    mov  hv_gpr+64(%rip),  %r10\n"
    "    mov  hv_gpr+72(%rip),  %r11\n"
    "    mov  hv_gpr+80(%rip),  %r12\n"
    "    mov  hv_gpr+88(%rip),  %r13\n"
    "    mov  hv_gpr+96(%rip),  %r14\n"
    "    mov  hv_gpr+104(%rip), %r15\n"
    "    mov  hv_gpr+40(%rip),  %rdi\n"  /* RDI and RSI last: they were */
    "    mov  hv_gpr+32(%rip),  %rsi\n"  /* holding our own arguments   */
    "    vmrun\n"
    "    mov  %rbx, hv_gpr+0(%rip)\n"
    "    mov  %rcx, hv_gpr+8(%rip)\n"
    "    mov  %rdx, hv_gpr+16(%rip)\n"
    "    mov  %rbp, hv_gpr+24(%rip)\n"
    "    mov  %rsi, hv_gpr+32(%rip)\n"
    "    mov  %rdi, hv_gpr+40(%rip)\n"
    "    mov  %r8,  hv_gpr+48(%rip)\n"
    "    mov  %r9,  hv_gpr+56(%rip)\n"
    "    mov  %r10, hv_gpr+64(%rip)\n"
    "    mov  %r11, hv_gpr+72(%rip)\n"
    "    mov  %r12, hv_gpr+80(%rip)\n"
    "    mov  %r13, hv_gpr+88(%rip)\n"
    "    mov  %r14, hv_gpr+96(%rip)\n"
    "    mov  %r15, hv_gpr+104(%rip)\n"
    "    pop  %r15\n"
    "    pop  %r14\n"
    "    pop  %r13\n"
    "    pop  %r12\n"
    "    pop  %rbp\n"
    "    pop  %rbx\n"
    "    ret\n"
    ".size hv_world_switch, .-hv_world_switch\n"
);

extern void hv_world_switch(uint64_t vmcb_phys);

/* ===== state ===== */

#define HV_LOG_MAX 64

typedef struct {
    uint64_t code;
    uint64_t info1;
    uint64_t rip;
} hv_exit_t;

static struct {
    int      supported;      /* CPUID says SVM                    */
    int      npt;            /* ...and nested paging              */
    int      nrip;           /* ...and NRIP save                  */
    int      enabled;        /* EFER.SVME set, VMCB built         */
    int      running;
    int      finished;
    uint32_t asid_max;
    uint32_t revision;
    const char *status;

    uint64_t vmcb_phys, npt_phys;

    uint32_t vmruns;
    uint32_t n_cpuid, n_io, n_msr, n_hlt, n_hypercall, n_npf, n_intr, n_other;
    uint64_t last_code;
    uint32_t io_port, io_value;
    uint32_t hypercall_arg, hypercall_ret;

    hv_exit_t log[HV_LOG_MAX];
    int       log_n;
    int       log_head;
} hv = { .status = "not probed" };

static void hv_log(uint64_t code, uint64_t info1, uint64_t rip) {
    hv.log[hv.log_head].code = code;
    hv.log[hv.log_head].info1 = info1;
    hv.log[hv.log_head].rip = rip;
    hv.log_head = (hv.log_head + 1) % HV_LOG_MAX;
    if (hv.log_n < HV_LOG_MAX) hv.log_n++;
}

static const char *hv_exit_name(uint64_t code) {
    switch (code) {
    case EXIT_INTR:    return "INTR";
    case EXIT_CPUID:   return "CPUID";
    case EXIT_HLT:     return "HLT";
    case EXIT_IOIO:    return "IOIO";
    case EXIT_MSR:     return "MSR";
    case EXIT_VMMCALL: return "VMMCALL";
    case EXIT_NPF:     return "NPF";
    case EXIT_INVALID: return "INVALID";
    default:           return "other";
    }
}

/* ===== VMCB accessors ===== */

static inline void hv_w8(uint32_t off, uint8_t v)   { hv_vmcb[off] = v; }
static inline void hv_w16(uint32_t off, uint16_t v) {
    *(volatile uint16_t *)(hv_vmcb + off) = v;
}
static inline void hv_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(hv_vmcb + off) = v;
}
static inline void hv_w64(uint32_t off, uint64_t v) {
    *(volatile uint64_t *)(hv_vmcb + off) = v;
}
static inline uint32_t hv_r32(uint32_t off) {
    return *(volatile uint32_t *)(hv_vmcb + off);
}
static inline uint64_t hv_r64(uint32_t off) {
    return *(volatile uint64_t *)(hv_vmcb + off);
}

/* selector, attributes, limit, base -- the four fields of a VMCB
 * segment, in that order. */
static void hv_seg(uint32_t off, uint16_t sel, uint16_t attrib,
                   uint32_t limit, uint64_t base) {
    hv_w16(off + 0, sel);
    hv_w16(off + 2, attrib);
    hv_w32(off + 4, limit);
    hv_w64(off + 8, base);
}

static void hv_cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                     uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

/* ===== probe ===== */

static void hv_probe(void) {
    uint32_t a, b, c, d;

    hv_cpuid(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u) { hv.status = "no extended CPUID leaves"; return; }

    hv_cpuid(0x80000001u, &a, &b, &c, &d);
    if (!(c & (1u << 2))) {
        hv.status = "this processor does not implement AMD-V";
        return;
    }
    hv.supported = 1;

    if (a >= 0x8000000Au || 1) {
        hv_cpuid(0x8000000Au, &a, &b, &c, &d);
        hv.revision = a & 0xFFu;
        hv.asid_max = b;
        hv.npt = (d & (1u << 0)) ? 1 : 0;
        hv.nrip = (d & (1u << 3)) ? 1 : 0;
    }

    /* A processor can support SVM and still have it locked off in
     * firmware; saying which of the two it is saves a lot of guessing. */
    const uint64_t vm_cr = rdmsr(MSR_VM_CR);
    if (vm_cr & VM_CR_SVMDIS) {
        hv.supported = 0;
        hv.status = "AMD-V is present but disabled in firmware";
        return;
    }

    if (!hv.npt) {
        hv.supported = 0;
        hv.status = "no nested paging; this hypervisor requires it";
        return;
    }
    if (hv.asid_max < 2) {
        hv.supported = 0;
        hv.status = "no address space identifiers available";
        return;
    }
    hv.status = "AMD-V with nested paging available";
}

/* ===== nested page tables =====
 *
 * Guest-physical to host-physical, four levels, identity over the first
 * megabyte. Every entry is present, writable and user: nested walks are
 * performed with user privilege, so an entry without the user bit would
 * fault every guest access and the symptom would be an endless stream of
 * nested page faults rather than anything that names the cause.
 */
static void hv_build_npt(void) {
    for (int i = 0; i < 512; i++) {
        hv_npt_pml4[i] = 0;
        hv_npt_pdpt[i] = 0;
        hv_npt_pd[i] = 0;
        hv_npt_pt[i] = 0;
    }
    const uint64_t flags = 0x7;      /* present | write | user */

    hv_npt_pml4[0] = kern_virt_to_phys(hv_npt_pdpt) | flags;
    hv_npt_pdpt[0] = kern_virt_to_phys(hv_npt_pd) | flags;
    hv_npt_pd[0]   = kern_virt_to_phys(hv_npt_pt) | flags;

    for (uint32_t p = 0; p < HV_GUEST_PAGES && p < 512; p++)
        hv_npt_pt[p] = kern_virt_to_phys(hv_guest_mem + p * 4096) | flags;

    hv.npt_phys = kern_virt_to_phys(hv_npt_pml4);
}

/* ===== VMCB ===== */

static void hv_build_vmcb(void) {
    for (int i = 0; i < 4096; i++) hv_vmcb[i] = 0;

    /* Intercept everything this guest can do that touches the world.
     * VMRUN itself must be intercepted or the processor refuses to
     * enter the guest at all -- it is how nested virtualisation is kept
     * from happening by accident. */
    hv_w32(VMCB_INTERCEPT_MISC1,
           INTERCEPT_INTR | INTERCEPT_CPUID | INTERCEPT_HLT |
           INTERCEPT_IOIO | INTERCEPT_MSR | INTERCEPT_SHUTDOWN);
    hv_w32(VMCB_INTERCEPT_MISC2, INTERCEPT_VMRUN | INTERCEPT_VMMCALL);

    /* All ones: every I/O port and every MSR traps. The guest gets to
     * touch no real device. */
    for (int i = 0; i < (int)sizeof(hv_iopm); i++) hv_iopm[i] = 0xFF;
    for (int i = 0; i < (int)sizeof(hv_msrpm); i++) hv_msrpm[i] = 0xFF;
    hv_w64(VMCB_IOPM_BASE, kern_virt_to_phys(hv_iopm));
    hv_w64(VMCB_MSRPM_BASE, kern_virt_to_phys(hv_msrpm));

    hv_w32(VMCB_GUEST_ASID, 1);          /* zero is illegal */
    hv_w8(VMCB_TLB_CONTROL, 1);          /* flush this guest's TLB */

    hv_w64(VMCB_NP_ENABLE, 1);
    hv_w64(VMCB_N_CR3, hv.npt_phys);
    hv_w32(VMCB_CLEAN, 0);               /* nothing cached yet */

    /*
     * 32-bit protected mode, flat, paging off. The segment attribute is
     * the twelve bits the VMCB packs a descriptor into: type, S, DPL,
     * P, AVL, L, D/B, G. 0xC9B is a present, 4 GB, 32-bit code segment;
     * 0xC93 the matching data segment.
     */
    hv_seg(VMCB_CS, 0x08, 0xC9B, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_DS, 0x10, 0xC93, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_ES, 0x10, 0xC93, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_FS, 0x10, 0xC93, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_GS, 0x10, 0xC93, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_SS, 0x10, 0xC93, 0xFFFFFFFFu, 0);
    hv_seg(VMCB_GDTR, 0, 0, 0xFFFF, 0);
    hv_seg(VMCB_IDTR, 0, 0, 0xFFFF, 0);
    hv_seg(VMCB_LDTR, 0, 0, 0, 0);
    hv_seg(VMCB_TR, 0, 0x8B, 0xFFFF, 0);

    hv_w8(VMCB_CPL, 0);
    hv_w64(VMCB_CR0, 0x11);              /* PE | ET, no paging */
    hv_w64(VMCB_CR3, 0);
    hv_w64(VMCB_CR4, 0);
    hv_w64(VMCB_DR6, 0xFFFF0FF0ull);
    hv_w64(VMCB_DR7, 0x400);
    hv_w64(VMCB_RFLAGS, 0x2);            /* bit 1 is reserved-one, IF off */
    hv_w64(VMCB_RIP, HV_ENTRY);
    hv_w64(VMCB_RSP, HV_STACK);
    hv_w64(VMCB_RAX, 0);

    /* The guest's EFER must have SVME set or VMRUN fails a consistency
     * check -- one of the few requirements that is not about the guest
     * at all, but about the state being a legal one to resume. */
    hv_w64(VMCB_EFER, EFER_SVME);

    /* Nested paging uses the guest PAT, so it has to be a legal one. */
    hv_w64(VMCB_G_PAT, 0x0007040600070406ull);

    hv.vmcb_phys = kern_virt_to_phys(hv_vmcb);
}

static void hv_load_guest(void) {
    for (uint32_t i = 0; i < HV_GUEST_BYTES; i++) hv_guest_mem[i] = 0;
    for (uint32_t i = 0; i < sizeof(hv_guest_code); i++)
        hv_guest_mem[HV_ENTRY + i] = hv_guest_code[i];

    for (int i = 0; i < 14; i++) ((uint64_t *)&hv_gpr)[i] = 0;
}

/* ===== bring-up ===== */

static void hv_init(void) {
    hv_probe();
    if (!hv.supported) return;

    /* The host save area has to exist before SVME is set: the processor
     * writes host state there on every VMRUN. */
    for (int i = 0; i < 4096; i++) hv_hsave[i] = 0;
    wrmsr(MSR_VM_HSAVE_PA, kern_virt_to_phys(hv_hsave));
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SVME);

    if (!(rdmsr(MSR_EFER) & EFER_SVME)) {
        hv.status = "EFER.SVME would not stay set";
        hv.supported = 0;
        return;
    }

    hv_build_npt();
    hv_build_vmcb();
    hv_load_guest();

    hv.enabled = 1;
    hv.status = "ready";
}

static void hv_reset(void) {
    if (!hv.supported) return;
    hv_build_npt();
    hv_build_vmcb();
    hv_load_guest();
    hv.vmruns = 0;
    hv.n_cpuid = hv.n_io = hv.n_msr = hv.n_hlt = 0;
    hv.n_hypercall = hv.n_npf = hv.n_intr = hv.n_other = 0;
    hv.log_n = hv.log_head = 0;
    hv.finished = 0;
    hv.running = 0;
    hv.last_code = 0;
    hv.hypercall_ret = 0;
    hv.io_port = hv.io_value = 0;
    hv.enabled = 1;
    hv.status = "ready";
}

/* ===== the run loop ===== */

/*
 * Advance past the instruction that caused the exit.
 *
 * The processor can report the next instruction pointer directly, but
 * only where NRIP-save exists -- and it does not under QEMU's
 * interpreter, which is where this is developed. So the lengths of the
 * two instructions that can trap here are applied by hand: VMMCALL is
 * three bytes (0F 01 D9) and CPUID is two (0F A2). An I/O exit needs
 * neither, because the exit information already carries the address of
 * the instruction after it.
 */
static void hv_skip(uint32_t len) {
    if (hv.nrip) {
        const uint64_t nrip = hv_r64(VMCB_NRIP);
        if (nrip) { hv_w64(VMCB_RIP, nrip); return; }
    }
    hv_w64(VMCB_RIP, hv_r64(VMCB_RIP) + len);
}

/* One entry into the guest and one exit out of it. Returns 0 while the
 * guest is still runnable. */
static int hv_step(void) {
    if (!hv.enabled || hv.finished) return -1;

    /* The clean-bits field tells the processor which parts of the VMCB
     * it may keep cached. Zero means "assume nothing", which is correct
     * whenever the host has just edited the guest's state. */
    hv_w32(VMCB_CLEAN, 0);

    __asm__ volatile("cli");
    hv_world_switch(hv.vmcb_phys);
    __asm__ volatile("sti");

    hv.vmruns++;

    const uint64_t code = hv_r64(VMCB_EXITCODE);
    const uint64_t info1 = hv_r64(VMCB_EXITINFO1);
    const uint64_t info2 = hv_r64(VMCB_EXITINFO2);
    const uint64_t rip = hv_r64(VMCB_RIP);
    hv.last_code = code;
    hv_log(code, info1, rip);

    switch (code) {
    case EXIT_VMMCALL: {
        /*
         * The hypercall. RAX selects, RBX carries an argument, and the
         * answer goes back in RAX -- a calling convention invented here
         * and known to both sides, which is all a hypercall interface
         * ever is.
         */
        hv.n_hypercall++;
        const uint32_t fn = (uint32_t)hv_r64(VMCB_RAX);
        const uint32_t arg = (uint32_t)hv_gpr.rbx;
        if (fn == 1) {
            /* Only function 1 takes an argument. Recording RBX on every
             * hypercall would show whatever the last CPUID left there. */
            hv.hypercall_arg = arg;
            hv.hypercall_ret = arg + 0x1111u;
            hv_w64(VMCB_RAX, hv.hypercall_ret);
        } else if (fn == 2) {
            hv_skip(3);
            hv.finished = 1;
            hv.running = 0;
            hv.status = "guest asked to stop";
            return -1;
        } else {
            hv_w64(VMCB_RAX, 0xFFFFFFFFu);
        }
        hv_skip(3);
        return 0;
    }

    case EXIT_CPUID: {
        /*
         * The guest asks what it is running on. Leaf 0x40000000 is the
         * conventional place a hypervisor identifies itself, and
         * answering it is how a guest can discover it is virtualised
         * without any other cooperation.
         */
        hv.n_cpuid++;
        const uint32_t leaf = (uint32_t)hv_r64(VMCB_RAX);
        if (leaf == 0x40000000u) {
            hv_w64(VMCB_RAX, 0x40000001u);
            hv_gpr.rbx = 0x74786556u;      /* "Vext" */
            hv_gpr.rcx = 0x48206F72u;      /* "ro H" */
            hv_gpr.rdx = 0x00007079u;      /* "yp"   */
        } else {
            hv_w64(VMCB_RAX, 0);
            hv_gpr.rbx = hv_gpr.rcx = hv_gpr.rdx = 0;
        }
        hv_skip(2);
        return 0;
    }

    case EXIT_IOIO: {
        /*
         * EXITINFO1 describes the access: the port in its top sixteen
         * bits, the direction in bit 0, the width in bits 4 to 6.
         * EXITINFO2 is the address of the instruction after it, so the
         * host does not have to know how long an OUT was.
         */
        hv.n_io++;
        hv.io_port = (uint32_t)(info1 >> 16);
        if (!(info1 & 1))                        /* an OUT */
            hv.io_value = (uint32_t)hv_r64(VMCB_RAX) & 0xFF;
        else
            hv_w64(VMCB_RAX, 0);                 /* an IN reads nothing */
        hv_w64(VMCB_RIP, info2);
        return 0;
    }

    case EXIT_MSR:
        hv.n_msr++;
        if (info1)  hv_skip(2);                  /* WRMSR, ignored */
        else      { hv_w64(VMCB_RAX, 0); hv_gpr.rdx = 0; hv_skip(2); }
        return 0;

    case EXIT_HLT:
        hv.n_hlt++;
        hv.finished = 1;
        hv.running = 0;
        hv.status = "guest halted";
        return -1;

    case EXIT_INTR:
        /* A host interrupt was pending. Nothing to service here -- the
         * exit itself was the point, and the next entry resumes the
         * guest exactly where it was. */
        hv.n_intr++;
        return 0;

    case EXIT_NPF:
        hv.n_npf++;
        hv.finished = 1;
        hv.running = 0;
        hv.status = "guest touched memory it does not have";
        return -1;

    case EXIT_INVALID:
        hv.finished = 1;
        hv.running = 0;
        hv.enabled = 0;
        hv.status = "the processor rejected the guest state";
        return -1;

    default:
        hv.n_other++;
        hv.finished = 1;
        hv.running = 0;
        hv.status = "unhandled exit";
        return -1;
    }
}

/* Run to completion, bounded so a guest that never stops cannot take
 * the machine with it. */
static void hv_run(int budget) {
    if (!hv.enabled || hv.finished) return;
    hv.running = 1;
    for (int i = 0; i < budget; i++)
        if (hv_step() != 0) return;
    hv.running = 0;
    hv.status = "budget exhausted";
}

/*
 * Hex into a caller's buffer. The tree has serial_put_hex32 and
 * term_print_hex32, but both write to a stream; a window needs the
 * digits as a string it can measure and lay out.
 */
static void hv_hex(uint64_t v, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    int hi = 15;
    while (hi > 0 && ((v >> (hi * 4)) & 0xF) == 0) hi--;
    int n = 0;
    for (int i = hi; i >= 0; i--) out[n++] = digits[(v >> (i * 4)) & 0xF];
    out[n] = '\0';
}

/* What the guest wrote to its text screen, one row at a time. */
static int hv_screen_row(int row, char *out, int max) {
    int n = 0;
    if (row < 0 || row >= HV_SCREEN_ROWS) { out[0] = '\0'; return 0; }
    const uint8_t *p = hv_guest_mem + HV_SCREEN + row * HV_SCREEN_COLS * 2;
    for (int c = 0; c < HV_SCREEN_COLS && n < max - 1; c++) {
        const uint8_t ch = p[c * 2];
        out[n++] = (ch >= 32 && ch < 127) ? (char)ch : ' ';
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return n;
}

static uint32_t hv_scratch32(uint32_t off) {
    const uint8_t *p = hv_guest_mem + HV_SCRATCH + off;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif /* VEXTRO_HYPER_H */
