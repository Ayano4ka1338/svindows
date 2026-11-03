/* x86-64 application-processor startup and the early parked-AP loop. */
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/AddressSpace.h>
#include <svindows/Rtl.h>

#define AP_TRAMPOLINE_PHYS 0x8000u
#define AP_TRAMPOLINE_PAGE 8u

extern UINT8 ap_trampoline_start[];
extern UINT8 ap_trampoline_end[];
extern UINT32 ap_trampoline_cr3;
extern UINT32 ap_trampoline_processor;
extern UINT64 ap_trampoline_stack;
extern UINT8 boot_pml4[];

static void smp_delay(ULONG iterations)
{
    while (iterations--)
        __asm__ volatile("pause");
}

static NORETURN void smp_park(void)
{
    for (;;) {
        /* STI's interrupt shadow makes the following HLT an atomic wait for
         * the wake IPI. The IPI handler EOIs and returns to this loop. */
        __asm__ volatile("sti; hlt; cli" : : : "memory");
    }
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
    while (!processor->StartupRelease)
        __asm__ volatile("sti; hlt; cli" : : : "memory");

    /* Shared scheduling remains intentionally gated until its global queues,
     * dispatcher objects, timers and allocators have SMP-safe locking. */
    smp_park();
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
        KeLog("[smp]  CPU %u (APIC %u) online and parked\n", i, info->ApicId);
    }
}
