/*
 * arch/x86_64/Gdt.c - the runtime GDT and TSS.
 *
 * Replaces the minimal boot GDT with a full one: null, kernel code/data, user
 * code/data, and a 64-bit Task State Segment. The TSS carries the stack the CPU
 * switches to for ring-0 entry (RSP0) and a dedicated interrupt stack (IST1)
 * used by the most dangerous faults, so a corrupted kernel stack can't take the
 * fault handler down with it.
 */
#include <nt/Ntdef.h>
#include <svindows/Ke.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>

/*
 * Segment selectors.
 *
 * The user descriptors are ordered (data at index 3, code at index 4) to match
 * what SYSRET expects: with IA32_STAR[63:48] = 0x10, SYSRET loads SS from
 * 0x10+8 = 0x18 (user data) and CS from 0x10+16 = 0x20 (user code).
 */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x1B /* index 3, RPL 3 */
#define SEL_UCODE 0x23 /* index 4, RPL 3 */
#define SEL_TSS   0x28 /* index 5 (spans indices 5 and 6) */

/* 64-bit Task State Segment. */
typedef struct PACKED _TSS64 {
    UINT32 reserved0;
    UINT64 rsp0;
    UINT64 rsp1;
    UINT64 rsp2;
    UINT64 reserved1;
    UINT64 ist[7];      /* ist[0] == IST1 */
    UINT64 reserved2;
    UINT16 reserved3;
    UINT16 iomap_base;
} TSS64;

/* GDTR/IDTR image handed to lgdt/lidt. */
typedef struct PACKED _DTR {
    UINT16 limit;
    UINT64 base;
} DTR;

/* The GDT: 5 code/data descriptors + a 16-byte (two-slot) TSS descriptor. */
static UINT64 g_gdt[KE_MAXIMUM_PROCESSORS][7] ALIGNED(16);
static TSS64  g_tss[KE_MAXIMUM_PROCESSORS] ALIGNED(16);

/* The BSP needs its architectural stacks before the pool is initialized.
 * AP stacks are allocated only for CPUs firmware actually reports, rather
 * than reserving two 16 KiB stacks for every possible CPU in the kernel BSS. */
static UINT8 g_ist1_boot_stack[0x4000] ALIGNED(16);
static UINT8 g_rsp0_boot_stack[0x4000] ALIGNED(16);
static UINT8 *g_ist1_stack[KE_MAXIMUM_PROCESSORS] = { g_ist1_boot_stack };
static UINT8 *g_rsp0_stack[KE_MAXIMUM_PROCESSORS] = { g_rsp0_boot_stack };

static BOOLEAN gdt_ensure_processor_stacks(ULONG processor_number)
{
    if (processor_number >= KE_MAXIMUM_PROCESSORS) return FALSE;
    if (g_ist1_stack[processor_number] && g_rsp0_stack[processor_number])
        return TRUE;
    if (processor_number == 0) return FALSE;

    UINT8 *ist = ExAllocatePoolWithTag(NonPagedPool, 0x4000, 'tSIK');
    if (!ist) return FALSE;
    UINT8 *rsp = ExAllocatePoolWithTag(NonPagedPool, 0x4000, 'pSRK');
    if (!rsp) {
        ExFreePool(ist);
        return FALSE;
    }
    memset(ist, 0, 0x4000);
    memset(rsp, 0, 0x4000);
    g_ist1_stack[processor_number] = ist;
    g_rsp0_stack[processor_number] = rsp;
    return TRUE;
}

extern void KiLoadGdtr(const void *gdtr);
extern void KiLoadTr(UINT16 selector);

static void gdt_set_tss(UINT64 *gdt, int slot, UINT64 base, UINT32 limit)
{
    UINT64 low = 0;
    low |= (limit & 0xFFFFULL);
    low |= (base & 0xFFFFFFULL) << 16;
    low |= 0x89ULL << 40;                    /* present, type = available 64-bit TSS */
    low |= ((UINT64)((limit >> 16) & 0xF)) << 48;
    low |= ((base >> 24) & 0xFFULL) << 56;
    gdt[slot] = low;
    gdt[slot + 1] = (base >> 32) & 0xFFFFFFFFULL; /* upper 32 bits of base */
}

void KeInitializeGdtForProcessor(ULONG processor_number)
{
    if (processor_number >= KE_MAXIMUM_PROCESSORS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, "invalid GDT processor index");
    if (!gdt_ensure_processor_stacks(processor_number))
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "cannot allocate processor interrupt stacks");
    UINT64 *gdt = g_gdt[processor_number];
    TSS64 *tss = &g_tss[processor_number];
    gdt[0] = 0;                       /* null */
    gdt[1] = 0x00AF9A000000FFFFULL;   /* kernel code: P, S, exec/read, L=1 */
    gdt[2] = 0x00AF92000000FFFFULL;   /* kernel data: P, S, read/write     */
    gdt[3] = 0x00AFF2000000FFFFULL;   /* user data:   DPL 3, read/write    */
    gdt[4] = 0x00AFFA000000FFFFULL;   /* user code:   DPL 3, exec/read, L=1 */

    /* Point the TSS's stacks at the top of their (downward-growing) buffers. */
    for (int i = 0; i < 7; i++)
        tss->ist[i] = 0;
    tss->rsp0 = (UINT64)(g_rsp0_stack[processor_number] + 0x4000);
    tss->rsp1 = 0;
    tss->rsp2 = 0;
    tss->ist[0] = (UINT64)(g_ist1_stack[processor_number] + 0x4000);
    tss->iomap_base = sizeof(TSS64);   /* no I/O permission bitmap */

    gdt_set_tss(gdt, 5, (UINT64)tss,
                sizeof(TSS64) - 1);

    DTR gdtr = { .limit = sizeof(g_gdt[processor_number]) - 1,
                 .base = (UINT64)gdt };
    KiLoadGdtr(&gdtr);
    KiLoadTr(SEL_TSS);
}

void KeInitializeGdt(void)
{
    KeInitializeGdtForProcessor(0);
}

UINT64 KeGetProcessorBootstrapStackTop(ULONG processor_number)
{
    if (!gdt_ensure_processor_stacks(processor_number)) return 0;
    return (UINT64)(g_rsp0_stack[processor_number] + 0x4000);
}

/* Update the stack the CPU switches to on a ring 3 -> ring 0 transition (e.g. a
 * hardware interrupt taken while user code runs). The scheduler points this at
 * the current thread's kernel stack. */
void KeSetTssRsp0(UINT64 rsp0)
{
    ULONG processor_number = KeGetCurrentProcessorBlock()->ProcessorNumber;
    if (processor_number < KE_MAXIMUM_PROCESSORS)
        g_tss[processor_number].rsp0 = rsp0;
}
