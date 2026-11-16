/* x86-64 application-processor startup and per-CPU idle entry. */
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/AddressSpace.h>
#include <svindows/Rtl.h>
#include <svindows/Gfx.h>

#define AP_TRAMPOLINE_PHYS 0x8000u
#define AP_TRAMPOLINE_PAGE 8u

extern UINT8 ap_trampoline_start[];
extern UINT8 ap_trampoline_end[];
extern UINT32 ap_trampoline_cr3;
extern UINT32 ap_trampoline_processor;
extern UINT64 ap_trampoline_stack;
extern UINT8 boot_pml4[];
extern UINT8 boot_pdpt[];

static void smp_delay(ULONG iterations)
{
    while (iterations--)
        __asm__ volatile("pause");
}

static NORETURN void smp_idle(void)
{
    /* Each AP's idle thread is backed by this processor's bootstrap stack.
     * The local APIC wake vector enters the shared scheduler; when this idle
     * context is selected again, IRET returns here and waits for more work. */
    for (;;) __asm__ volatile("sti; hlt" : : : "memory");
}

static NORETURN void smp_halt_forever(void)
{
    for (;;) __asm__ volatile("cli; hlt" : : : "memory");
}

NORETURN void KiApplicationProcessorEntry(UINT32 processor_number)
{
    /* The SIPI tables map the kernel's higher-half window. Switch to the
     * runtime page tables before touching any post-MMIO mapping or allocator. */
    MmSwitchAddressSpace(MmGetKernelAddressSpace());

    PKPRCB processor = KeGetProcessorBlock(processor_number);
    if (!processor || processor_number == 0 ||
        processor_number >= HalGetProcessorCount())
        smp_halt_forever();

    KeInitializeGdtForProcessor(processor_number);
    KeLoadIdt();
    KiInitializeSystemCallsForProcessor(processor_number);
    GfxInitializeProcessor();
    if (processor->IdleThread) {
        processor->IdleThread->State = ThreadStateRunning;
        processor->CurrentThread = processor->IdleThread;
    }
    KeSetKernelStack(KeGetProcessorBootstrapStackTop(processor_number));

    if (!HalInitializeLocalApic()) {
        processor->StartupState = 3; /* failed to initialize this CPU's LAPIC */
        __sync_synchronize();
        smp_halt_forever();
    }

    __sync_synchronize();
    processor->StartupState = 2; /* per-CPU architectural state is online */
    __sync_synchronize();
    while (!__atomic_load_n(&processor->StartupRelease, __ATOMIC_ACQUIRE)) {
        /* STI;HLT is an atomic sleep pattern: a pending APIC interrupt wakes
         * this CPU even when StartupRelease was published just before HLT. */
        __asm__ volatile("sti; hlt" : : : "memory");
    }

    /* The scheduler, dispatcher lists, DPC queue and pool allocator use
     * inter-CPU spin locks. APs now join the global ready queue and run their
     * own idle context until an IPI or timer tick makes work runnable. */
    smp_idle();
}

void KiStartApplicationProcessors(void)
{
    UINT32 count = HalGetProcessorCount();
    if (count <= 1) return;

    UINT64 trampoline_size = (UINT64)(ap_trampoline_end - ap_trampoline_start);
    if (trampoline_size != PAGE_SIZE) {
        KeLog("[smp]  AP trampoline size invalid (%lu bytes); APs left offline\n",
              (unsigned long)trampoline_size);
        return;
    }

    /* The kernel image is loaded above 1 MiB. Copy its SIPI blob into the
     * reserved low page only after the BSP has switched to the direct map. */
    memcpy(MmPhysToVirt(AP_TRAMPOLINE_PHYS), ap_trampoline_start,
           (SIZE_T)trampoline_size);

    /* The APs enter with boot_pml4 active. Their bootstrap stacks are in the
     * kernel heap (-1 GiB window), which the old trampoline tables did not
     * map; the first C call therefore faulted before the AP could report in.
     * Allocate every stack first, then mirror the runtime heap PDPT entry into
     * the boot tables so the AP can safely switch CR3 while using that stack. */
    for (UINT32 i = 1; i < count; ++i) {
        if (!KeGetProcessorBootstrapStackTop(i)) {
            KeLog("[smp]  cannot allocate bootstrap stack for CPU %u; APs left offline\n",
                  i);
            return;
        }
    }
    UINT64 *kernel_pml4 = (UINT64 *)MmPhysToVirt(MmGetKernelAddressSpace());
    if (!(kernel_pml4[511] & PTE_PRESENT)) {
        KeLog("[smp]  kernel high-half page tables unavailable; APs left offline\n");
        return;
    }
    UINT64 *kernel_pdpt = (UINT64 *)MmPhysToVirt(kernel_pml4[511] & PTE_ADDR_MASK);
    UINT64 *bootstrap_pdpt = (UINT64 *)MmPhysToVirt((UINT64)(ULONG_PTR)boot_pdpt);
    if (!(kernel_pdpt[511] & PTE_PRESENT)) {
        KeLog("[smp]  kernel heap is not mapped; APs left offline\n");
        return;
    }
    bootstrap_pdpt[511] = kernel_pdpt[511];
    __asm__ volatile("mfence" ::: "memory");

    /* Patch the low page through the high direct map because the BSP dropped
     * identity mappings when it installed the runtime page tables. */
    volatile UINT32 *tr_cr3 = MmPhysToVirt(
        AP_TRAMPOLINE_PHYS + ((UINT64)(ULONG_PTR)&ap_trampoline_cr3 -
                             (UINT64)ap_trampoline_start));
    volatile UINT32 *tr_processor = MmPhysToVirt(
        AP_TRAMPOLINE_PHYS + ((UINT64)(ULONG_PTR)&ap_trampoline_processor -
                             (UINT64)ap_trampoline_start));
    volatile UINT64 *tr_stack = MmPhysToVirt(
        AP_TRAMPOLINE_PHYS + ((UINT64)(ULONG_PTR)&ap_trampoline_stack -
                             (UINT64)ap_trampoline_start));
    *tr_cr3 = (UINT32)(UINT64)(ULONG_PTR)boot_pml4;

    KeLog("[smp]  starting %u application processor(s)\n", count - 1);
    for (UINT32 i = 1; i < count; ++i) {
        const HAL_PROCESSOR_INFO *info = HalGetProcessorInfo(i);
        PKPRCB processor = KeGetProcessorBlock(i);
        UINT64 stack = KeGetProcessorBootstrapStackTop(i);
        if (!info || !processor || !stack) break;

        *tr_processor = i;
        *tr_stack = stack;
        processor->KernelRsp = stack;
        processor->StartupRelease = 0;
        processor->StartupState = 1;
        __sync_synchronize();

        if (!HalSendInitIpi(info->ApicId, TRUE)) {
            KeLog("[smp]  CPU %u (APIC %u): INIT delivery unsupported; stopping\n",
                  i, info->ApicId);
            break;
        }
        smp_delay(1000000);
        if (!HalSendInitIpi(info->ApicId, FALSE)) break;
        smp_delay(1000000);

        if (!HalSendStartupIpi(info->ApicId, AP_TRAMPOLINE_PAGE)) break;
        smp_delay(200000);
        if (processor->StartupState != 2)
            (void)HalSendStartupIpi(info->ApicId, AP_TRAMPOLINE_PAGE);

        ULONG wait = 20000000;
        while (processor->StartupState == 1 && wait--)
            __asm__ volatile("pause");
        if (processor->StartupState != 2) {
            KeLog("[smp]  CPU %u (APIC %u) did not reach online state (%u); "
                  "remaining APs left offline\n", i, info->ApicId,
                  processor->StartupState);
            /* The delayed AP could still be reading the shared trampoline
             * parameters, so do not patch them for another startup attempt. */
            break;
        }
        KeLog("[smp]  CPU %u (APIC %u) initialized\n", i, info->ApicId);
    }

    /* Publish every successfully initialized AP only after the startup loop
     * has finished using the shared low-memory trampoline parameters. Do not
     * send an immediate wake IPI here: the first BSP clock-tick IPI wakes each
     * AP after the BSP has left early boot and entered its normal idle loop. */
    for (UINT32 i = 1; i < count; ++i) {
        const HAL_PROCESSOR_INFO *info = HalGetProcessorInfo(i);
        PKPRCB processor = KeGetProcessorBlock(i);
        if (!info || !processor || processor->StartupState != 2)
            continue;
        __atomic_store_n(&processor->StartupRelease, 1, __ATOMIC_RELEASE);
        KeLog("[smp]  CPU %u (APIC %u): scheduler admission published\n",
              i, info->ApicId);
    }
}
