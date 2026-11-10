/*
 * ke/Syscall.c - the system-call boundary: MSR setup, the per-CPU block, the
 * service dispatcher, and the first handful of Nt* services.
 *
 * Native ring-3 code reaches this interface through ntdll.
 */
#include <svindows/Ke.h>
#include <svindows/Trace.h>
#include <svindows/Mm.h>
#include <svindows/Io.h>
#include <svindows/Ps.h>
#include <svindows/Ldr.h>
#include <svindows/Cm.h>
#include <svindows/Gfx.h>
#include <svindows/Hal.h>
#include <svindows/Input.h>
#include <svindows/Win32k.h>
#include <svindows/Rtl.h>
#include <svindows/Ob.h>
#include <svindows/Ex.h>
#include <svindows/Linux.h>
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
#include <nt/Ntobject.h>

#ifndef _WCHAR_DEFINED
#define WCHAR UINT16
#endif

/* --- Model-specific registers we program. --- */
#define MSR_EFER          0xC0000080 /* bit 0 = SCE (SYSCALL enable)      */
#define MSR_STAR          0xC0000081 /* segment selectors for syscall/ret */
#define MSR_LSTAR         0xC0000082 /* 64-bit SYSCALL entry RIP          */
#define MSR_SFMASK        0xC0000084 /* RFLAGS bits cleared on SYSCALL    */
#define MSR_GS_BASE       0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_FS_BASE       0xC0000100

/* Each CPU receives a private syscall/exception block. */
static KPRCB g_kpcr[KE_MAXIMUM_PROCESSORS] ALIGNED(64);

extern void KiSystemCallEntry(void); /* arch/x86_64/SyscallEntry.asm */

static ALWAYS_INLINE void wrmsr(UINT32 msr, UINT64 value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32)));
}

static ALWAYS_INLINE UINT64 rdmsr(UINT32 msr)
{
    UINT32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((UINT64)hi << 32) | lo;
}

PKPRCB KeGetCurrentProcessorBlock(void)
{
    PKPRCB processor;
    __asm__ volatile("movq %%gs:24, %0" : "=r"(processor) : : "memory");
    return processor;
}

PKPRCB KeGetProcessorBlock(ULONG processor_number)
{
    return processor_number < KE_MAXIMUM_PROCESSORS ?
        &g_kpcr[processor_number] : NULL;
}

/* The syscall entry stub reaches the current KTHREAD through KPCR+16 (GS
 * after swapgs), so the scheduler publishes it in this CPU's block. */
void KeSetCurrentThread(PKTHREAD thread)
{
    KeGetCurrentProcessorBlock()->CurrentThread = thread;
}

void KeSetKernelStack(UINT64 kernel_rsp)
{
    KeGetCurrentProcessorBlock()->KernelRsp = kernel_rsp;
    KeSetTssRsp0(kernel_rsp);
}

void KeSetUserGsBase(UINT64 teb)
{
    /* KERNEL_GS_BASE holds what swapgs will make the active GS on the next
     * kernel exit. For a user thread that is its TEB; for a kernel thread we
     * park the per-CPU block there. Kept correct on every context switch, so a
     * thread that terminates mid-syscall can't leave it stale. */
    PKPRCB processor = KeGetCurrentProcessorBlock();
    PKTHREAD thread = processor->CurrentThread;
    BOOLEAN linux_user = thread && thread->UserMode && thread->Process &&
                         thread->Process->LinuxContext;
    wrmsr(MSR_KERNEL_GS_BASE,
          (teb || linux_user) ? teb : (UINT64)processor);
}

void KeSetUserFsBase(UINT64 base)
{
    wrmsr(MSR_FS_BASE, base);
}

void KiInitializeSystemCallsForProcessor(ULONG processor_number)
{
    if (processor_number >= KE_MAXIMUM_PROCESSORS)
        KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED,
                   "invalid syscall processor index");
    KPRCB *processor = &g_kpcr[processor_number];
    processor->Self = processor;
    processor->ProcessorNumber = processor_number;
    processor->Irql = PASSIVE_LEVEL;

    /* Invariant: in ring 0 the active GS base is the per-CPU block (KPCR). The
     * "other" base holds the value swapgs will restore on the next kernel exit;
     * it starts as the KPCR too and is repointed at each thread's TEB by the
     * scheduler (KeSetUserGsBase). */
    wrmsr(MSR_GS_BASE, (UINT64)processor);
    wrmsr(MSR_KERNEL_GS_BASE, (UINT64)processor);
    KeSetUserFsBase(0);

    /* Enable the SYSCALL/SYSRET instructions. */
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1 | (1ULL << 11)); /* SCE + NXE */

    /*
     * STAR selectors:
     *   [47:32] = 0x08 -> SYSCALL sets CS=0x08 (kernel code), SS=0x10.
     *   [63:48] = 0x10 -> SYSRET sets SS=0x18 (user data|3), CS=0x20 (user code|3).
     */
    wrmsr(MSR_STAR, ((UINT64)0x10 << 48) | ((UINT64)0x08 << 32));
    wrmsr(MSR_LSTAR, (UINT64)&KiSystemCallEntry);

    /* Clear IF, DF, TF on entry: syscalls run non-preemptible for now. */
    wrmsr(MSR_SFMASK, 0x700);

}

void KiInitializeSystemCalls(void)
{
    KPRCB *processor = &g_kpcr[0];
    memset(processor, 0, sizeof(*processor));
    processor->ProcessorNumber = 0;
    UINT32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    processor->ApicId = ebx >> 24;
    KiInitializeSystemCallsForProcessor(0);
    KiInitializeServiceTable();

    KeLog("[ke]   syscall path armed (LSTAR=%p)\n", (void *)&KiSystemCallEntry);
}

/* ------------------------------------------------------------------ */
/* Nt* system services                                                */
/* ------------------------------------------------------------------ */

/* Services receive the full argument array (see the syscall entry), so those
 * with the real, up-to-11-argument NT signatures can read every parameter. */

/* NtDisplayString(PUNICODE-ish ptr) - print a NUL-terminated user string. */
static UINT64 NtDisplayString(UINT64 *a)
{
    /* Ring 0 may read the user page directly (same address space). A real
     * implementation would validate and capture the buffer first. */
    KeLog("[user] %s\n", (const char *)a[0]);
    return 0; /* STATUS_SUCCESS */
}

/* NtDisplayNumber(value) - print an integer argument. */
static UINT64 NtDisplayNumber(UINT64 *a)
{
    KeLog("[user] NtDisplayNumber: %lu (0x%lx)\n",
          (unsigned long)a[0], (unsigned long)a[0]);
    return 0;
}

/* User RSP of the syscall currently being serviced (set by the dispatcher
 * before the service runs; used for diagnostics such as stack walks). */
static UINT64 g_service_user_rsp;
static UINT64 g_service_number;

/* Walk the caller's user stack and annotate values that land inside loaded
 * module images (return addresses), giving a poor-man's exit backtrace. */
static void KiDumpUserStack(UINT64 rsp)
{
    KeLog("[user] user stack walk (rsp=%p, module-relative return sites):\n",
          (void *)rsp);
    for (UINT64 i = 0; i < 64; i++) {
        UINT64 va = rsp + i * 8;
        if (!MmProbeForRead(va, sizeof(UINT64)))
            break;
        UINT64 value = *(volatile UINT64 *)va;
        const char *name = 0, *symbol = 0;
        UINT64 base = 0, symbol_address = 0;
        if (value && LdrDescribeUserSymbol(value, &name, &base, &symbol,
                                           &symbol_address)) {
            if (symbol)
                KeLog("  [%02lu] %p  %s!%s+0x%lx\n", (unsigned long)i,
                      (void *)value, name, symbol,
                      (unsigned long)(value - symbol_address));
            else
                KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                      (void *)value, name, (unsigned long)(value - base));
        }
    }

    /* Frame-pointer chain: with the trap frame unavailable here, reconstruct
     * the first rbp by scanning for a slot whose value points further up the
     * same stack and whose own target again points upward; then walk [rbp] /
     * [rbp+8] pairs for the true caller chain. */
    UINT64 stack_max = rsp + 0x8000;
    UINT64 rbp = 0;
    for (UINT64 va = rsp + 8; va < rsp + 0x400; va += 8) {
        if (!MmProbeForRead(va, 8) || !MmProbeForRead(*(UINT64 *)va, 8))
            continue;
        UINT64 candidate = *(volatile UINT64 *)va;
        if (candidate <= va || candidate >= stack_max)
            continue;
        UINT64 next = *(volatile UINT64 *)candidate;
        if (next > candidate && next < stack_max) {
            rbp = candidate;
            break;
        }
    }
    if (rbp) {
        KeLog("[user] frame chain:\n");
        for (int level = 0; level < 12; level++) {
            if (!MmProbeForRead(rbp + 8, 8))
                break;
            UINT64 ret = *(volatile UINT64 *)(rbp + 8);
            const char *name = 0, *symbol = 0;
            UINT64 base = 0, symbol_address = 0;
            if (ret && LdrDescribeUserSymbol(ret, &name, &base, &symbol,
                                             &symbol_address)) {
                if (symbol)
                    KeLog("  #%d %s!%s+0x%lx\n", level, name, symbol,
                          (unsigned long)(ret - symbol_address));
                else
                    KeLog("  #%d %s+0x%lx\n", level, name,
                          (unsigned long)(ret - base));
            }
            if (!MmProbeForRead(rbp, 8))
                break;
            UINT64 next = *(volatile UINT64 *)rbp;
            if (next <= rbp || next >= stack_max)
                break;
            rbp = next;
        }
    }
}

/* NtTraceCall - loader trampoline notification (see svindows/Trace.h). Args keep
 * their positions (r10 holds the original RCX); the trampoline's tag sits at
 * SVINDOWS_TRACE_TAG_VA and the caller's return address at [user_rsp]. */
static UINT64 NtTraceCall(UINT64 *a)
{
    static const char *const names[] = {
        NULL,
        "RegisterClassW",
        "RegisterClassExW",
        "CreateWindowExW",
        "DestroyWindow",
        "PostThreadMessageW",
        "PostMessageW",
        "DefWindowProcW",
        "DispatchMessageW",
        "RegisterClassW@entry",
    };
    UINT32 tag = 0;
    if (MmProbeForRead(SVINDOWS_TRACE_TAG_VA, sizeof(UINT32)))
        tag = *(volatile UINT32 *)SVINDOWS_TRACE_TAG_VA;
    const char *name = tag < sizeof(names) / sizeof(names[0]) && names[tag]
                           ? names[tag]
                           : "?";
    UINT64 ret = 0;
    if (MmProbeForRead(g_service_user_rsp, sizeof(UINT64)))
        ret = *(volatile UINT64 *)g_service_user_rsp;
    const char *mod = 0, *symbol = 0;
    UINT64 base = 0, symbol_address = 0;
    if (ret && LdrDescribeUserSymbol(ret, &mod, &base, &symbol,
                                     &symbol_address)) {
        if (symbol)
            KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from "
                  "%s!%s+0x%lx\n", name, (void *)a[0], (void *)a[1],
                  (void *)a[2], (void *)a[3], mod, symbol,
                  (unsigned long)(ret - symbol_address));
        else
            KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from %s+0x%lx\n",
                  name, (void *)a[0], (void *)a[1], (void *)a[2],
                  (void *)a[3], mod, (unsigned long)(ret - base));
    }
    else
        KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from %p\n", name,
              (void *)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
              (void *)ret);
    /* For class registrations, dump the WNDCLASS/EX the caller built: the
     * class-name pointer (+0x40) is the value the whole client-side pipeline
     * hinges on. */
    if ((tag == 1 || tag == 2) && MmProbeForRead(a[0] + 0x40, 8)) {
        UINT64 cls = *(volatile UINT64 *)(a[0] + 0x40);
        UINT64 inst = 0, brush = 0;
        UINT32 style = 0;
        if (MmProbeForRead(a[0] + 0x18, 8))
            inst = *(volatile UINT64 *)(a[0] + 0x18);
        if (MmProbeForRead(a[0] + 0x30, 8))
            brush = *(volatile UINT64 *)(a[0] + 0x30);
        if (MmProbeForRead(a[0] + 0x4, 4))
            style = *(volatile UINT32 *)(a[0] + 0x4);
        KeLog("[trace]   wcx: style=%lx hInstance=%p hbrBackground=%p lpszClassName=%p\n",
              (unsigned long)style, (void *)inst, (void *)brush, (void *)cls);
        if (MmProbeForRead(a[0], 0x50)) {
            KeLog("[trace]   wcx raw:");
            for (int off = 0; off < 0x50; off += 8)
                KeLog(" +%x=%p", off,
                      (void *)*( (volatile UINT64 *)(a[0] + off)));
            KeLog("\n");
        }
        if (MmProbeForRead(cls, 24 * sizeof(WCHAR))) {
            WCHAR wname[24] = {0};
            for (int i = 0; i < 23; i++) {
                UINT16 c = *(volatile UINT16 *)(cls + i * 2);
                wname[i] = c;
                if (!c)
                    break;
            }
            wname[23] = 0;
            char ascii[24];
            int i = 0;
            for (; i < 23 && wname[i]; i++)
                ascii[i] = wname[i] < 128 ? (char)wname[i] : '?';
            ascii[i] = 0;
            KeLog("[trace]   class name: %s\n", ascii);
        }
    }
    return 0;
}

/* NtTerminateThread - terminate the referenced thread or the caller. */
static UINT64 NtTerminateThread(UINT64 *a)
{
    KeLog("[user] NtTerminateThread(handle=%p, status=0x%08lx); ending user thread\n",
          (void *)a[0], (unsigned long)(UINT32)a[1]);
    KiDumpUserStack(g_service_user_rsp);
    PKTHREAD thread = KeGetCurrentThread();
    if (thread && (!a[0] || a[0] == ~(UINT64)1)) {
        thread->ExitStatus = (NTSTATUS)a[1];
        /* KiUserExceptionDispatcher uses this service when no handler accepts
         * a first-chance exception. Windows terminates the process in that
         * case even when the fault happened on a worker thread. */
        if (thread->ExceptionActive) {
            KiDumpUserException(thread->ExceptionRecord,
                                thread->ExceptionContext);
            thread->ProcessMainThread = TRUE;
        }
    }
    if (!a[0] || a[0] == ~(UINT64)1) {
        KeTerminateThread();
        return 0; /* unreachable */
    }
    return (UINT64)PsTerminateThreadByHandle((HANDLE)(ULONG_PTR)a[0],
                                              (NTSTATUS)a[1]);
}

/* NtContinue's user-mode half restores the AMD64 register image after this
 * service validates the context and closes the active exception dispatch. */
static UINT64 NtContinue(UINT64 *a)
{
    if (!MmProbeForRead(a[0], 256))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    PKTHREAD thread = KeGetCurrentThread();
    if (thread) {
        thread->ExceptionActive = FALSE;
        thread->ExceptionRecord = 0;
        thread->ExceptionContext = 0;
    }
    return (UINT64)STATUS_SUCCESS;
}

#define MEM_COMMIT      0x1000u
#define MEM_RESERVE     0x2000u
#define MEM_DECOMMIT    0x4000u
#define MEM_RELEASE     0x8000u
#define MEM_FREE        0x10000u
#define MEM_PRIVATE     0x20000u
#define MEM_MAPPED      0x40000u
#define PAGE_NOACCESS   0x01u
#define PAGE_READONLY   0x02u
#define PAGE_READWRITE  0x04u
#define PAGE_GUARD      0x100u

static PMM_PROCESS_REGION MiFindRegion(PKPROCESS process, UINT64 base,
                                       UINT64 size)
{
    if (!process || base + size < base)
        return NULL;
    for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++) {
        PMM_PROCESS_REGION region = &process->VmRegions[i];
        if (region->InUse && base >= region->Base &&
            base + size <= region->Base + region->ReserveSize)
            return region;
    }
    return NULL;
}

static PMM_PROCESS_REGION MiAllocateRegionSlot(PKPROCESS process)
{
    for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++)
        if (!process->VmRegions[i].InUse)
            return &process->VmRegions[i];
    return NULL;
}

static UINT64 MiProtectionFlags(UINT32 protect)
{
    UINT32 base = protect & 0xff;
    UINT64 flags = 0;
    if (base != PAGE_NOACCESS && !(protect & PAGE_GUARD))
        flags |= PTE_USER;
    if (base == PAGE_READWRITE || base == 0x08 || base == 0x40 || base == 0x80)
        flags |= PTE_WRITE;
    if (!(base == 0x10 || base == 0x20 || base == 0x40 || base == 0x80))
        flags |= PTE_NX;
    return flags;
}

static BOOLEAN MiValidProtection(UINT32 protect)
{
    UINT32 base = protect & 0xff;
    return (BOOLEAN)(base == PAGE_NOACCESS || base == PAGE_READONLY ||
                     base == PAGE_READWRITE || base == 0x08 ||
                     base == 0x10 || base == 0x20 || base == 0x40 ||
                     base == 0x80);
}

/*
 * NtAllocateVirtualMemory(ProcessHandle, *BaseAddress, ZeroBits, *RegionSize,
 *                         AllocationType, Protect) -> NTSTATUS.
 * The real signature: the base and size are in/out pointers. We honor a
 * requested base of 0 (choose one) and update *BaseAddress; the region grows
 * from a bump pointer.
 */
static UINT64 NtAllocateVirtualMemory(UINT64 *a)
{
    PVOID   *base_ptr = (PVOID *)a[1];
    SIZE_T  *size_ptr = (SIZE_T *)a[3];
    UINT32 allocation_type = (UINT32)a[4];
    UINT32 protect = (UINT32)a[5];
    if (!MmProbeForWrite((UINT64)base_ptr, sizeof(*base_ptr)) ||
        !MmProbeForWrite((UINT64)size_ptr, sizeof(*size_ptr)) ||
        *size_ptr == 0 || *size_ptr > MM_USER_MAX - PAGE_MASK ||
        !(allocation_type & (MEM_RESERVE | MEM_COMMIT)) ||
        (allocation_type & ~(MEM_RESERVE | MEM_COMMIT)) ||
        !MiValidProtection(protect))
        return (UINT64)STATUS_INVALID_PARAMETER;

    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;
    if (!process)
        return (UINT64)STATUS_INVALID_PARAMETER;

    UINT64 requested = (UINT64)*base_ptr;
    UINT64 base = requested ? PAGE_ALIGN(requested) : 0;
    /* The unaligned prefix belongs to the requested interval as well. */
    if (requested && *size_ptr > MM_USER_MAX - (requested & PAGE_MASK) - PAGE_MASK)
        return (UINT64)STATUS_INVALID_PARAMETER;
    UINT64 size = PAGE_ALIGN_UP(*size_ptr + (requested & PAGE_MASK));
    PMM_PROCESS_REGION region = NULL;
    if (allocation_type & MEM_RESERVE) {
        if (!base)
            base = (process->UserAllocationNext + 0xffff) & ~0xffffULL;
        if (base >= MM_USER_MAX || size > MM_USER_MAX - base)
            return (UINT64)STATUS_INVALID_PARAMETER;
        if (MiFindRegion(process, base, size))
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++) {
            PMM_PROCESS_REGION other = &process->VmRegions[i];
            if (other->InUse && base < other->Base + other->ReserveSize &&
                other->Base < base + size)
                return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        }
        /* Loader images, PEB/TEB and stacks are not yet VAD-backed. Their
         * mapped frames still must never be overwritten by a new reservation. */
        for (UINT64 va = base; va < base + size; va += PAGE_SIZE)
            if (MmGetPhysicalAddress(va) != MM_INVALID_PHYS)
                return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        region = MiAllocateRegionSlot(process);
        if (!region)
            return (UINT64)STATUS_NO_MEMORY;
        memset(region, 0, sizeof(*region));
        region->Base = base;
        region->ReserveSize = size;
        region->AllocationProtect = protect;
        region->Protect = protect;
        region->Type = MEM_PRIVATE;
        region->InUse = TRUE;
        if (base + size > process->UserAllocationNext)
            process->UserAllocationNext = base + size;
    } else {
        region = MiFindRegion(process, base, size);
        if (!region)
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
    }

    if (allocation_type & MEM_COMMIT) {
        UINT64 missing = 0;
        for (UINT64 va = base; va < base + size; va += PAGE_SIZE)
            if (MmGetPhysicalAddress(va) == MM_INVALID_PHYS)
                missing++;
        if (missing > MmFreePageCount())
            return (UINT64)STATUS_NO_MEMORY;
        for (UINT64 va = base; va < base + size; va += PAGE_SIZE) {
            if (MmGetPhysicalAddress(va) != MM_INVALID_PHYS)
                continue;
            UINT64 pa = MmAllocatePage();
            if (pa == MM_INVALID_PHYS)
                return (UINT64)STATUS_NO_MEMORY;
            if (!MmMapPage(va, pa, MiProtectionFlags(protect))) {
                MmFreePages(pa, 1);
                return (UINT64)STATUS_NO_MEMORY;
            }
            memset(MmPhysToVirt(pa), 0, PAGE_SIZE);
            region->CommittedPages++;
        }
        region->Protect = protect;
    }

    *base_ptr = (PVOID)base;
    *size_ptr = size;

    KeTrace("[user] NtAllocateVirtualMemory(%lu) -> %p\n",
          (unsigned long)size, (void *)base);
    return (UINT64)STATUS_SUCCESS;
}

static UINT64 NtFreeVirtualMemory(UINT64 *a)
{
    PVOID *base_ptr = (PVOID *)a[1];
    SIZE_T *size_ptr = (SIZE_T *)a[2];
    UINT32 free_type = (UINT32)a[3];
    if (!MmProbeForWrite((UINT64)base_ptr, sizeof(*base_ptr)) ||
        !MmProbeForWrite((UINT64)size_ptr, sizeof(*size_ptr)))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    PKPROCESS process = KeGetCurrentThread()->Process;
    UINT64 base = PAGE_ALIGN((UINT64)*base_ptr);
    UINT64 size = PAGE_ALIGN_UP(*size_ptr);
    PMM_PROCESS_REGION region = MiFindRegion(process, base,
        free_type == MEM_RELEASE ? 1 : size);
    if (!region)
        return (UINT64)STATUS_UNABLE_TO_FREE_VM;
    if (region->Section)
        return (UINT64)STATUS_UNABLE_TO_FREE_VM;
    if (free_type == MEM_RELEASE) {
        if (base != region->Base || *size_ptr != 0)
            return (UINT64)STATUS_INVALID_PARAMETER;
        base = region->Base;
        size = region->ReserveSize;
    } else if (free_type != MEM_DECOMMIT || !size) {
        return (UINT64)STATUS_INVALID_PARAMETER;
    }
    for (UINT64 va = base; va < base + size; va += PAGE_SIZE) {
        UINT64 pa = MmGetPhysicalAddress(va);
        if (pa == MM_INVALID_PHYS)
            continue;
        MmUnmapPage(va);
        MmFreePages(pa & ~PAGE_MASK, 1);
        if (region->CommittedPages)
            region->CommittedPages--;
    }
    if (free_type == MEM_RELEASE)
        memset(region, 0, sizeof(*region));
    *base_ptr = (PVOID)base;
    *size_ptr = size;
    return (UINT64)STATUS_SUCCESS;
}

/*
 * NtDelayExecution(BOOLEAN Alertable, PLARGE_INTEGER Interval) - block the
 * caller until the relative/absolute 100 ns deadline. Implemented as a
 * dispatcher timer wait (10 ms resolution). An alertable wait is interrupted
 * by a queued user APC; ntdll invokes the callback before returning to caller.
 */
static UINT64 NtDelayExecution(UINT64 *a)
{
    INT64 *interval = (INT64 *)a[1];
    if (!MmProbeForRead((UINT64)interval, sizeof(*interval)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    INT64 iv;
    memcpy(&iv, interval, sizeof(iv));
    if (a[0] && KeHasUserApc(KeGetCurrentThread()))
        return (UINT64)STATUS_USER_APC;
    if (KeDueTimeToTicks(iv) == 0) {
        KeYield();
        return (UINT64)STATUS_SUCCESS;
    }
    KTIMER timer;
    KeInitializeTimerEx(&timer, FALSE);
    timer.StackOwner = KeGetCurrentThread();
    KeSetTimerEx(&timer, iv, 0, NULL);
    NTSTATUS status = KeWaitForSingleObjectTimeoutAlertable(
        &timer.Header, ~(UINT64)0, (BOOLEAN)(a[0] != 0));
    KeCancelTimer(&timer);
    return (UINT64)status;
}

/*
 * NtQuerySystemInformation(SystemInformationClass, SystemInformation,
 *                          SystemInformationLength, ReturnLength).
 *
 * SystemPolicyInformation (class 134) is an opaque CLIP/WarBird licensing
 * channel, not a plain
 * list of policy values: both its request and response contain encrypted and
 * checksummed blobs.  Expose the native entry point and validate its public
 * descriptor, but report that the licensing provider is absent rather than
 * returning a fabricated blob that user mode would reject as corrupt.
 */
static UINT64 NtQuerySystemInformation(UINT64 *a)
{
    typedef struct _SYSTEM_BASIC_INFORMATION_LOCAL {
        UINT32 Reserved;
        UINT32 TimerResolution;
        UINT32 PageSize;
        UINT32 NumberOfPhysicalPages;
        UINT32 LowestPhysicalPageNumber;
        UINT32 HighestPhysicalPageNumber;
        UINT32 AllocationGranularity;
        UINT32 Padding;
        UINT64 MinimumUserModeAddress;
        UINT64 MaximumUserModeAddress;
        UINT64 ActiveProcessorsAffinityMask;
        UINT8  NumberOfProcessors;
        UINT8  Reserved2[7];
    } SYSTEM_BASIC_INFORMATION_LOCAL;

    const UINT32 SystemBasicInformation = 0;
    const UINT32 SystemPolicyInformation = 134;
    const UINT32 policy_info_size = 32;
    UINT32 info_class = (UINT32)a[0];
    UINT64 buffer = a[1];
    UINT32 length = (UINT32)a[2];
    UINT32 *return_length = (UINT32 *)a[3];

    if (info_class == 5) /* SystemProcessInformation */
        return (UINT64)PsQuerySystemProcessInformation(
            (void *)buffer, length, return_length);

    if (info_class == SystemBasicInformation) {
        UINT32 required = sizeof(SYSTEM_BASIC_INFORMATION_LOCAL);
        if (return_length &&
            MmProbeForWrite((UINT64)return_length, sizeof(*return_length)))
            *return_length = required;
        if (length < required)
            return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
        if (!MmProbeForWrite(buffer, required))
            return (UINT64)STATUS_ACCESS_VIOLATION;
        SYSTEM_BASIC_INFORMATION_LOCAL *info =
            (SYSTEM_BASIC_INFORMATION_LOCAL *)buffer;
        memset(info, 0, sizeof(*info));
        info->TimerResolution = 100000; /* 10 ms in 100 ns units */
        info->PageSize = PAGE_SIZE;
        info->NumberOfPhysicalPages = (UINT32)MmTotalPages();
        info->HighestPhysicalPageNumber = info->NumberOfPhysicalPages - 1;
        info->AllocationGranularity = 0x10000;
        info->MinimumUserModeAddress = 0x10000;
        info->MaximumUserModeAddress = 0x00007FFFFFFEFFFFULL;
        info->ActiveProcessorsAffinityMask = 1;
        info->NumberOfProcessors = 1;
        return (UINT64)STATUS_SUCCESS;
    }

    if (info_class != SystemPolicyInformation)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (return_length && MmProbeForWrite((UINT64)return_length,
                                         sizeof(*return_length)))
        *return_length = policy_info_size;
    if (length < policy_info_size)
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite(buffer, policy_info_size))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT32 *descriptor_status = (UINT32 *)(buffer + 0x1c);
    if (!MmProbeForWrite((UINT64)descriptor_status, sizeof(*descriptor_status)))
        return (UINT64)STATUS_INVALID_PARAMETER;
    *descriptor_status = (UINT32)STATUS_NOT_SUPPORTED;
    KeLog("[user] NtQuerySystemInformation(SystemPolicyInformation): no licensing provider\n");
    return (UINT64)STATUS_NOT_SUPPORTED;
}

/*
 * NtProtectVirtualMemory(ProcessHandle, *BaseAddress, *RegionSize, NewProtect,
 *                        *OldProtect) - change page protection on a range.
 */
static UINT64 NtProtectVirtualMemory(UINT64 *a)
{
    PVOID  *base_ptr = (PVOID *)a[1];
    SIZE_T *size_ptr = (SIZE_T *)a[2];
    UINT32  protect  = (UINT32)a[3];
    UINT32 *old_ptr  = (UINT32 *)a[4];

    if (!MmProbeForWrite((UINT64)base_ptr, sizeof(PVOID)) ||
        !MmProbeForWrite((UINT64)size_ptr, sizeof(SIZE_T)) ||
        !MmProbeForWrite((UINT64)old_ptr, sizeof(*old_ptr)) ||
        !MiValidProtection(protect))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT64 base = PAGE_ALIGN((UINT64)*base_ptr);
    if (!*size_ptr || *size_ptr > MM_USER_MAX - PAGE_MASK ||
        base >= MM_USER_MAX || *size_ptr > MM_USER_MAX - base)
        return (UINT64)STATUS_INVALID_PARAMETER;
    UINT64 size = PAGE_ALIGN_UP(*size_ptr);
    PKPROCESS process = KeGetCurrentThread()->Process;
    PMM_PROCESS_REGION region = MiFindRegion(process, base, size);
    if (!region) {
        /* Images are mapped page-by-page by the PE loader and are not always
         * represented as a private VM reservation. Permit protection changes
         * over a fully resident image range while still rejecting holes. */
        UINT64 image_base = 0, section_base = 0, section_size = 0;
        if (!LdrQueryImageSection(base, &image_base, &section_base,
                                 &section_size, NULL) ||
            base < section_base || base + size > section_base + section_size)
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        for (UINT64 va = base; va < base + size; va += PAGE_SIZE) {
            if (MmGetPhysicalAddress(va) == MM_INVALID_PHYS)
                return (UINT64)STATUS_CONFLICTING_ADDRESSES;
            UINT64 flags = MmGetPageFlags(va);
            *old_ptr = (flags & PTE_NX) ? ((flags & PTE_WRITE) ? 0x04 : 0x02)
                                        : ((flags & PTE_WRITE) ? 0x40 : 0x20);
        }
        MmProtectRangeFlags(base, size, MiProtectionFlags(protect));
        *base_ptr = (PVOID)base;
        *size_ptr = size;
        return (UINT64)STATUS_SUCCESS;
    }
    for (UINT64 va = base; va < base + size; va += PAGE_SIZE)
        if (MmGetPhysicalAddress(va) == MM_INVALID_PHYS)
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;

    *old_ptr = region->Protect;
    MmProtectRangeFlags(base, size, MiProtectionFlags(protect));
    /* Image and data-section mappings share an allocation region while each
     * mapped page can have a different initial protection. VirtualProtect
     * updates only the requested PTE range; do not replace the whole section
     * region's allocation protection metadata. */
    if (!region->Section)
        region->Protect = protect;
    *base_ptr = (PVOID)base;
    *size_ptr = size;
    return (UINT64)STATUS_SUCCESS;
}

static UINT64 NtQueryVirtualMemory(UINT64 *a)
{
    typedef struct _MEMORY_BASIC_INFORMATION_LOCAL {
        PVOID BaseAddress;
        PVOID AllocationBase;
        UINT32 AllocationProtect;
        UINT32 PartitionId;
        SIZE_T RegionSize;
        UINT32 State;
        UINT32 Protect;
        UINT32 Type;
        UINT32 Reserved;
    } MEMORY_BASIC_INFORMATION_LOCAL;

    UINT64 address = PAGE_ALIGN(a[1]);
    UINT32 info_class = (UINT32)a[2];
    MEMORY_BASIC_INFORMATION_LOCAL *info =
        (MEMORY_BASIC_INFORMATION_LOCAL *)a[3];
    SIZE_T length = (SIZE_T)a[4];
    SIZE_T *return_length = (SIZE_T *)a[5];
    if (info_class != 0)
        return (UINT64)STATUS_INVALID_INFO_CLASS;
    if (return_length && MmProbeForWrite((UINT64)return_length,
                                         sizeof(*return_length)))
        *return_length = sizeof(*info);
    if (length < sizeof(*info))
        return (UINT64)STATUS_INFO_LENGTH_MISMATCH;
    if (!MmProbeForWrite((UINT64)info, sizeof(*info)))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    PKPROCESS process = KeGetCurrentThread()->Process;
    PMM_PROCESS_REGION region = MiFindRegion(process, address, 1);
    memset(info, 0, sizeof(*info));
    info->BaseAddress = (PVOID)address;
    if (!region) {
        UINT64 allocation_base = 0, section_base = 0, section_size = 0;
        UINT32 initial_protect = 0;
        if (LdrQueryImageSection(address, &allocation_base, &section_base,
                                 &section_size, &initial_protect)) {
            memset(info, 0, sizeof(*info));
            info->BaseAddress = (PVOID)section_base;
            info->AllocationBase = (PVOID)allocation_base;
            info->AllocationProtect = initial_protect;
            info->RegionSize = section_base + section_size - address;
            info->State = MEM_COMMIT;
            UINT64 flags = MmGetPageFlags(address);
            info->Protect = (flags & PTE_NX)
                ? ((flags & PTE_WRITE) ? 0x04 : 0x02)
                : ((flags & PTE_WRITE) ? 0x40 : 0x20);
            info->Type = 0x01000000; /* MEM_IMAGE */
            return (UINT64)STATUS_SUCCESS;
        }
        UINT64 next = process->UserAllocationNext;
        for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++)
            if (process->VmRegions[i].InUse &&
                process->VmRegions[i].Base > address &&
                process->VmRegions[i].Base < next)
                next = process->VmRegions[i].Base;
        info->RegionSize = next > address ? next - address : PAGE_SIZE;
        info->State = MEM_FREE;
        info->Protect = PAGE_NOACCESS;
        return (UINT64)STATUS_SUCCESS;
    }
    info->AllocationBase = (PVOID)region->Base;
    info->AllocationProtect = region->AllocationProtect;
    info->RegionSize = region->Base + region->ReserveSize - address;
    info->State = MmGetPhysicalAddress(address) == MM_INVALID_PHYS
                      ? MEM_RESERVE : MEM_COMMIT;
    info->Protect = info->State == MEM_COMMIT ? region->Protect : 0;
    info->Type = region->Type;
    return (UINT64)STATUS_SUCCESS;
}

#include "../mm/Sections.inc"

/* NtLoadLibrary(name) - load a DLL by name at runtime; returns its base or 0.
 * (Not a real NT service name; our loader hook for kernel32's LoadLibraryA.) */
static UINT64 NtLoadLibrary(UINT64 *a)
{
    if (a[0] == 0)
        return 0;
    UINT64 base = LdrLoadLibrary((const char *)a[0]);
    KeLog("[user] NtLoadLibrary('%s') -> %p\n", (const char *)a[0], (void *)base);
    return base;
}

typedef UINT64 (*KI_SERVICE)(UINT64 *args);


#include "../win32k/UserMessages.inc"
#include "../win32k/UserWindows.inc"
#include "../win32k/UserCallbacks.inc"
/* Last: the window manager needs the window records, the message queue and the
 * callback machinery of the parts above. */
#include "../win32k/UserScreen.inc"
#include "../win32k/XorgMirror.inc"

/*
 * The system service table, indexed by NT x64 syscall numbers
 * (from the public NT syscall-number tables). Sparse: most slots are unused.
 * Our ntdll stubs issue these same numbers, and a service's slot holds the
 * routine implementing it. Aligning to a real build's numbering is what would
 * let an ABI-compatible ntdll drive this kernel.
 */
#define SVINDOWS_MAX_SYSCALL 0x2000
static KI_SERVICE KiServiceTable[SVINDOWS_MAX_SYSCALL];

/* Number assignments for the supported NT x64 ABI. Kept together as the single source of
 * truth, mirrored by the ntdll stubs. */
#define SN_NtWaitForSingleObject   0x01
#define SN_NtWriteFile             0x05
#define SN_NtReadFile              0x03
#define SN_NtQueryInformationFile  0x06
#define SN_NtClose                 0x0C
#define SN_NtOpenKey               0x0F
#define SN_NtAllocateVirtualMemory 0x15
#define SN_NtFreeVirtualMemory     0x1B
#define SN_NtQueryVirtualMemory    0x20
#define SN_NtSetInformationFile    0x24
#define SN_NtQueryValueKey         0x17
#define SN_NtCreateKey             0x1A
#define SN_NtSetEvent              0x02
#define SN_NtCreateEvent           0x45
#define SN_NtFlushBuffersFile      0x48
#define SN_NtCreateThreadEx        0xA5
#define SN_NtCreateFile            0x52
#define SN_NtSetValueKey           0x5D
#define SN_NtTerminateThread       0x50
#define SN_NtContinue              0x194
#define SN_NtDelayExecution        0x31
#define SN_NtCreateTimer           0xA6
#define SN_NtSetTimer              0x5F
#define SN_NtCancelTimer           0x5E
#define SN_NtQueryTimer            0x35
#define SN_NtQueryDirectoryFile    0x32
#define SN_NtQuerySystemInformation 0x33
#define SN_NtProtectVirtualMemory  0x4D
/* SVINDOWS-private service numbers live above the verified native range. Some
 * names have Windows equivalents but use private numbers until ABI-checked. */
#define SN_NtDisplayString         0xF0
#define SN_NtDisplayNumber         0xF1
#define SN_NtLoadLibrary           0xF2
#define SN_NtQueryFileInfo         0xF4
#define SN_NtWaitForMultipleObjects 0xF6
#define SN_NtResetEvent             0xF7
#define SN_NtCreateSemaphore        0xF8
#define SN_NtReleaseSemaphore       0xF9
#define SN_NtQueryInformationProcess 0xFA
#define SN_NtOpenThread             0x10E
#define SN_NtOpenEvent              0x10F
#define SN_NtCreateJobObject        0x110
#define SN_NtAssignProcessToJobObject 0x111
#define SN_NtSetInformationJobObject 0x112
#define SN_NtQueryInformationJobObject 0x113
#define SN_NtSuspendThread 0x114
#define SN_NtResumeThread 0x115
#define SN_NtSetInformationProcess 0x116
#define SN_NtQueryThreadPriority 0x117
#define SN_NtSetThreadPriority 0x118
#define SN_NtCreateIoCompletion 0x119
#define SN_NtSetIoCompletion 0x11A
#define SN_NtRemoveIoCompletion 0x11B
#define SN_NtAssociateIoCompletionFile 0x11C
#define SN_NtDuplicateObject 0x11D
#define SN_NtOpenProcess 0x11E
#define SN_NtCreateSection 0x11F
#define SN_NtMapViewOfSection 0x120
#define SN_NtUnmapViewOfSection 0x121
#define SN_NtOpenFile 0x122

/*
 * Window-manager services this kernel needs and the numbering above does not
 * cover. They are Svindows-defined: the numbers sit in the unused part of the
 * USER range, and user/w/ntdll/Ntdll.asm issues exactly these.
 */
#define SN_NtUserGetWindowRect          0x1031
#define SN_NtUserGetClientRect          0x1032
#define SN_NtUserSetFocus               0x1033
#define SN_NtUserGetFocus               0x1034
#define SN_NtUserSetWindowText          0x1035
#define SN_NtUserGetWindowText          0x1036
#define SN_NtUserWindowFromPoint        0x1037
#define SN_NtUserGetSystemMetrics       0x1050
#define SN_NtUserGetSysColor            0x1051
#define SN_NtTraceCall           0xFB
#define SN_NtCreateUserProcessSimple 0xFD
#define SN_NtFileSystemControlSimple 0xFE
#define SN_NtSetFileSizeSimple       0xFF
#define SN_NtCreatePipeSimple        0x100
#define SN_NtConsoleControlSimple     0x101
#define SN_NtQueueApcThreadSimple      0x103
#define SN_NtDequeueUserApc            0x104
#define SN_NtTestAlertSimple           0x105
#define SN_NtCreateMutant              0x192
#define SN_NtReleaseMutant             0x193

/* win32u.dll service numbers from the matching Windows 10 user-mode build. */
#define SN_NtUserGetThreadState         0x1003
#define SN_NtUserPeekMessage            0x1004
#define SN_NtUserCallOneParam           0x1005
#define SN_NtUserGetMessage             0x1009
#define SN_NtUserGetDC                  0x100D
#define SN_NtUserPostThreadMessage      0x1061
#define SN_NtUserGetCaretBlinkTime      0x10F5
#define SN_NtUserGetAtomName            0x10AB
#define SN_NtUserRegisterWindowMessage  0x103A
#define SN_NtUserSystemParametersInfo   0x1045
#define SN_NtUserFindWindowEx           0x1070
#define SN_NtUserCreateWindowEx         0x1078
#define SN_NtUserDestroyWindow          0x109F
#define SN_NtUserDdeInitialize          0x110E
#define SN_NtUserSetWinEventHook        0x1105
#define SN_NtUserUnhookWinEvent         0x1106
#define SN_NtUserEnableMouseInPointer   0x1372
#define SN_NtUserSetProcessUIAccessZorder 0x143C
/* Service numbers extracted from the matching win32u.dll build. */
#define SN_NtUserCallNoParam            0x1008
#define SN_NtUserMessageCall            0x100A
#define SN_NtUserPostMessage            0x1012
#define SN_NtUserQueryWindow            0x1013
#define SN_NtUserTranslateAccelerator   0x1014
#define SN_NtUserBeginPaint             0x101A
#define SN_NtUserSetTimer               0x101B
#define SN_NtUserEndPaint               0x101C
#define SN_NtUserKillTimer              0x101E
#define SN_NtUserSetWindowPos           0x1027
#define SN_NtUserDispatchMessage        0x1039
#define SN_NtUserGetForegroundWindow    0x103F
#define SN_NtUserSetCapture             0x104C
#define SN_NtUserEnumDisplayMonitors    0x104D
#define SN_NtUserSetProp                0x104F
#define SN_NtUserShowWindow             0x105A
#define SN_NtUserMoveWindow             0x1060
#define SN_NtUserInvalidateRect         0x1007
#define SN_NtUserUpdateWindow           0x10E9
#define SN_NtUserGetKeyState            0x1F00 /* Svindows private USER ABI */
#define SN_NtUserTrackMouseEvent        0x1F01
#define SN_NtUserGetAncestor            0x10B4
#define SN_NtUserRegisterClassExWOW     0x10B2
#define SN_NtUserUnregisterClass        0x10BD
#define SN_NtUserGetClassInfoEx         0x10BB
#define SN_NtUserRemoveProp             0x1049
#define SN_NtUserGetProp                0x1011
#define SN_NtUserWaitMessage            0x100F
#define SN_NtUserTranslateMessage       0x1010
#define SN_NtUserSetActiveWindow        0x10E1
#define SN_NtUserReleaseCapture         0x1017
#define SN_NtUserGetProcessWindowStation 0x1025
#define SN_NtUserGetThreadDesktop       0x1085
#define SN_NtUserSetThreadDesktop       0x1093
#define SN_NtUserOpenInputDesktop       0x13F0
#define SN_NtUserCloseDesktop           0x10A8
#define SN_NtUserOpenDesktop            0x10A9
#define SN_NtUserOpenWindowStation      0x10A2
#define SN_NtUserSetProcessWindowStation 0x10AA
#define SN_NtUserCloseWindowStation     0x10B7
#define SN_NtUserNotifyWinEvent         0x1030
#define SN_NtUserCallHwnd               0x110D
#define SN_NtUserCallHwndLock           0x1024
#define SN_NtUserCallHwndParam          0x10A0
#define SN_NtUserCallTwoParam           0x102D
#define SN_NtUserSetWindowLong          0x105E
#define SN_NtUserSetWindowLongPtr       0x1471
#define SN_NtUserChangeWindowMessageFilterEx 0x134A
#define SN_NtUserGetObjectInformation     0x106E
#define SN_NtUserRegisterHotKey           0x1403
#define SN_NtUserValidateRect             0x10CF
#define SN_NtUserRedrawWindow             0x1016
#define SN_NtUserGetUpdateRect            0x1056
#define SN_NtUserFindExistingCursorIcon   0x1041
#define SN_NtUserCreateAcceleratorTable   0x10F2
#define SN_NtUserDestroyAcceleratorTable  0x10FF
#define SN_NtUserDestroyCursor            0x109E
#define SN_NtUserSetCursor                0x101D
#define SN_NtGdiGetEntry                  0x12AE
#define SN_NtGdiCreateSolidBrush          0x10BA
#define SN_NtGdiCreatePen                 0x1059
#define SN_NtGdiHfontCreate               0x105F
#define SN_NtGdiStretchBlt                0x1062
#define SN_NtGdiCreateRectRgn             0x1086
#define SN_NtGdiSelectBrush               0x12F6
#define SN_NtGdiSelectPen                 0x12F8
#define SN_NtGdiSelectFont                0x103C
#define SN_NtGdiSelectBitmap              0x100E
#define SN_NtGdiDeleteObjectApp           0x1026
#define SN_NtGdiPatBlt                    0x105C
#define SN_NtGdiBitBlt                    0x100B
#define SN_NtGdiBlitUserBuffer            0x1052
#define SN_NtGdiExtTextOutW               0x103B
#define SN_NtGdiGetTextMetricsW           0x1077
#define SN_NtGdiGetTextExtent             0x1096
#define SN_NtGdiGetTextExtentExW          0x12CB
#define SN_NtGdiSetPixel                  0x10B1
#define SN_NtGdiRectangle                 0x1092
#define SN_NtGdiMoveTo                    0x12DC
#define SN_NtGdiLineTo                    0x1044
#define SN_NtGdiCreateCompatibleDC        0x1057
#define SN_NtGdiCreateCompatibleBitmap    0x104E
#define SN_NtGdiCreateBitmap              0x106F
#define SN_NtGdiGetDCObject               0x1038
#define SN_NtGdiFlush                     0x1015
#define SN_NtGdiGetDeviceCaps             0x12A6
#define SN_NtGdiGetCurrentDpiInfo         0x12A5
#define SN_NtUserGetDCEx                  0x1094
#define SN_NtUserGetWindowDC              0x1066

void KiInitializeServiceTable(void)
{
    KiServiceTable[SN_NtWaitForSingleObject]   = NtWaitForSingleObject;
    KiServiceTable[SN_NtWriteFile]             = NtWriteFile;
    KiServiceTable[SN_NtReadFile]              = NtReadFile;
    KiServiceTable[SN_NtQueryInformationFile] = NtQueryInformationFile;
    KiServiceTable[SN_NtClose]                 = NtClose;
    KiServiceTable[SN_NtOpenKey]               = NtOpenKey;
    KiServiceTable[SN_NtAllocateVirtualMemory] = NtAllocateVirtualMemory;
    KiServiceTable[SN_NtFreeVirtualMemory]     = NtFreeVirtualMemory;
    KiServiceTable[SN_NtQueryVirtualMemory]    = NtQueryVirtualMemory;
    KiServiceTable[SN_NtSetInformationFile]    = NtSetInformationFile;
    KiServiceTable[SN_NtQueryValueKey]         = NtQueryValueKey;
    KiServiceTable[SN_NtCreateKey]             = NtCreateKey;
    KiServiceTable[SN_NtSetEvent]              = NtSetEvent;
    KiServiceTable[SN_NtCreateEvent]           = NtCreateEvent;
    KiServiceTable[SN_NtFlushBuffersFile]      = NtFlushBuffersFile;
    KiServiceTable[SN_NtCreateThreadEx]        = NtCreateThreadEx;
    KiServiceTable[SN_NtCreateFile]            = NtCreateFile;
    KiServiceTable[SN_NtSetValueKey]           = NtSetValueKey;
    KiServiceTable[SN_NtTerminateThread]       = NtTerminateThread;
    KiServiceTable[SN_NtContinue]              = NtContinue;
    KiServiceTable[SN_NtDelayExecution]        = NtDelayExecution;
    KiServiceTable[SN_NtCreateTimer] = NtCreateTimer;
    KiServiceTable[SN_NtSetTimer] = NtSetTimer;
    KiServiceTable[SN_NtCancelTimer] = NtCancelTimer;
    KiServiceTable[SN_NtQueryTimer] = NtQueryTimer;
    KiServiceTable[SN_NtQueryDirectoryFile]    = NtQueryDirectoryFile;
    KiServiceTable[SN_NtQuerySystemInformation] = NtQuerySystemInformation;
    KiServiceTable[SN_NtProtectVirtualMemory]  = NtProtectVirtualMemory;
    KiServiceTable[SN_NtDisplayString]         = NtDisplayString;
    KiServiceTable[SN_NtDisplayNumber]         = NtDisplayNumber;
    KiServiceTable[SN_NtLoadLibrary]           = NtLoadLibrary;
    KiServiceTable[SN_NtQueryFileInfo]         = NtQueryFileInfo;
    KiServiceTable[SN_NtWaitForMultipleObjects] = NtWaitForMultipleObjects;
    KiServiceTable[SN_NtResetEvent]             = NtResetEvent;
    KiServiceTable[SN_NtCreateSemaphore]        = NtCreateSemaphore;
    KiServiceTable[SN_NtReleaseSemaphore]       = NtReleaseSemaphore;
    KiServiceTable[SN_NtQueryInformationProcess] = NtQueryInformationProcess;
    KiServiceTable[SN_NtOpenThread] = NtOpenThread;
    KiServiceTable[SN_NtOpenEvent] = NtOpenEvent;
    KiServiceTable[SN_NtCreateJobObject] = NtCreateJobObject;
    KiServiceTable[SN_NtAssignProcessToJobObject] = NtAssignProcessToJobObject;
    KiServiceTable[SN_NtSetInformationJobObject] = NtSetInformationJobObject;
    KiServiceTable[SN_NtQueryInformationJobObject] = NtQueryInformationJobObject;
    KiServiceTable[SN_NtSuspendThread] = NtSuspendThread;
    KiServiceTable[SN_NtResumeThread] = NtResumeThread;
    KiServiceTable[SN_NtSetInformationProcess] = NtSetInformationProcess;
    KiServiceTable[SN_NtQueryThreadPriority] = NtQueryThreadPriority;
    KiServiceTable[SN_NtSetThreadPriority] = NtSetThreadPriority;
    KiServiceTable[SN_NtCreateIoCompletion] = NtCreateIoCompletion;
    KiServiceTable[SN_NtSetIoCompletion] = NtSetIoCompletion;
    KiServiceTable[SN_NtRemoveIoCompletion] = NtRemoveIoCompletion;
    KiServiceTable[SN_NtAssociateIoCompletionFile] = NtAssociateIoCompletionFile;
    KiServiceTable[SN_NtDuplicateObject] = NtDuplicateObject;
    KiServiceTable[SN_NtOpenProcess] = NtOpenProcess;
    KiServiceTable[SN_NtCreateSection] = NtCreateSection;
    KiServiceTable[SN_NtMapViewOfSection] = NtMapViewOfSection;
    KiServiceTable[SN_NtUnmapViewOfSection] = NtUnmapViewOfSection;
    KiServiceTable[SN_NtOpenFile] = NtOpenFile;
    KiServiceTable[SN_NtTraceCall] = NtTraceCall;
    KiServiceTable[SN_NtCreateUserProcessSimple] = NtCreateUserProcessSimple;
    KiServiceTable[SN_NtFileSystemControlSimple] = NtFileSystemControlSimple;
    KiServiceTable[SN_NtSetFileSizeSimple] = NtSetFileSizeSimple;
    KiServiceTable[SN_NtCreatePipeSimple] = NtCreatePipeSimple;
    KiServiceTable[SN_NtConsoleControlSimple] = NtConsoleControlSimple;
    KiServiceTable[SN_NtQueueApcThreadSimple] = NtQueueApcThread;
    KiServiceTable[SN_NtDequeueUserApc] = NtDequeueUserApc;
    KiServiceTable[SN_NtTestAlertSimple] = NtTestAlert;
    KiServiceTable[0x106] = NtOpenProcessToken;
    KiServiceTable[0x107] = NtOpenThreadToken;
    KiServiceTable[0x108] = NtQueryInformationToken;
    KiServiceTable[0x109] = NtDeleteKey;
    KiServiceTable[0x10A] = NtDeleteValueKey;
    KiServiceTable[0x10B] = NtEnumerateKey;
    KiServiceTable[0x10C] = NtEnumerateValueKey;
    KiServiceTable[0x10D] = NtQueryKey;
    KiServiceTable[SN_NtCreateMutant] = NtCreateMutant;
    KiServiceTable[SN_NtReleaseMutant] = NtReleaseMutant;
    KiServiceTable[SN_NtCallbackReturn] = NtCallbackReturn;
    KiServiceTable[SN_NtUserGetThreadState] = NtUserGetThreadState;
    KiServiceTable[SN_NtUserPeekMessage] = NtUserPeekMessage;
    KiServiceTable[SN_NtUserCallOneParam] = NtUserCallOneParam;
    KiServiceTable[SN_NtUserGetMessage] = NtUserGetMessage;
    KiServiceTable[SN_NtUserGetDC] = NtUserGetDC;
    /* Svindows-defined window-manager services (see the SN_ block above). */
    KiServiceTable[SN_NtUserGetWindowRect] = NtUserGetWindowRect;
    KiServiceTable[SN_NtUserGetClientRect] = NtUserGetClientRect;
    KiServiceTable[SN_NtUserSetFocus] = NtUserSetFocus;
    KiServiceTable[SN_NtUserGetFocus] = NtUserGetFocus;
    KiServiceTable[SN_NtUserSetWindowText] = NtUserSetWindowText;
    KiServiceTable[SN_NtUserGetWindowText] = NtUserGetWindowText;
    KiServiceTable[SN_NtUserWindowFromPoint] = NtUserWindowFromPoint;
    KiServiceTable[SN_NtUserGetSystemMetrics] = NtUserGetSystemMetrics;
    KiServiceTable[SN_NtUserGetSysColor] = NtUserGetSysColor;
    KiServiceTable[SN_NtUserPostThreadMessage] = NtUserPostThreadMessage;
    KiServiceTable[SN_NtUserGetCaretBlinkTime] = NtUserGetCaretBlinkTime;
    KiServiceTable[SN_NtUserGetAtomName] = NtUserGetAtomName;
    KiServiceTable[SN_NtUserRegisterWindowMessage] = NtUserRegisterWindowMessage;
    KiServiceTable[SN_NtUserSystemParametersInfo] = NtUserSystemParametersInfo;
    KiServiceTable[SN_NtUserFindWindowEx] = NtUserFindWindowEx;
    KiServiceTable[SN_NtUserCreateWindowEx] = NtUserCreateWindowEx;
    KiServiceTable[SN_NtUserDestroyWindow] = NtUserDestroyWindow;
    KiServiceTable[SN_NtUserDdeInitialize] = NtUserDdeInitialize;
    KiServiceTable[SN_NtUserSetWinEventHook] = NtUserSetWinEventHook;
    KiServiceTable[SN_NtUserUnhookWinEvent] = NtUserUnhookWinEvent;
    KiServiceTable[SN_NtUserEnableMouseInPointer] = NtUserEnableMouseInPointer;
    KiServiceTable[SN_NtUserSetProcessUIAccessZorder] =
        NtUserSetProcessUIAccessZorder;
    KiServiceTable[SN_NtUserCallNoParam] = NtUserCallNoParam;
    KiServiceTable[SN_NtUserMessageCall] = NtUserMessageCall;
    KiServiceTable[SN_NtUserPostMessage] = NtUserPostMessage;
    KiServiceTable[SN_NtUserQueryWindow] = NtUserQueryWindow;
    KiServiceTable[SN_NtUserTranslateAccelerator] = NtUserTranslateAccelerator;
    KiServiceTable[SN_NtUserBeginPaint] = NtUserBeginPaint;
    KiServiceTable[SN_NtUserSetTimer] = NtUserSetTimer;
    KiServiceTable[SN_NtUserEndPaint] = NtUserEndPaint;
    KiServiceTable[SN_NtUserKillTimer] = NtUserKillTimer;
    KiServiceTable[SN_NtUserSetWindowPos] = NtUserSetWindowPos;
    KiServiceTable[SN_NtUserDispatchMessage] = NtUserDispatchMessage;
    KiServiceTable[SN_NtUserGetForegroundWindow] = NtUserGetForegroundWindow;
    KiServiceTable[SN_NtUserSetCapture] = NtUserSetCapture;
    KiServiceTable[SN_NtUserEnumDisplayMonitors] = NtUserEnumDisplayMonitors;
    KiServiceTable[SN_NtUserSetProp] = NtUserSetProp;
    KiServiceTable[SN_NtUserShowWindow] = NtUserShowWindow;
    KiServiceTable[SN_NtUserMoveWindow] = NtUserMoveWindow;
    KiServiceTable[SN_NtUserInvalidateRect] = NtUserInvalidateRect;
    KiServiceTable[SN_NtUserUpdateWindow] = NtUserUpdateWindow;
    KiServiceTable[SN_NtUserGetKeyState] = NtUserGetKeyState;
    KiServiceTable[SN_NtUserTrackMouseEvent] = NtUserTrackMouseEvent;
    KiServiceTable[SN_NtUserGetAncestor] = NtUserGetAncestor;
    KiServiceTable[SN_NtUserRegisterClassExWOW] = NtUserRegisterClassExWOW;
    KiServiceTable[SN_NtUserUnregisterClass] = NtUserUnregisterClass;
    KiServiceTable[SN_NtUserGetClassInfoEx] = NtUserGetClassInfoEx;
    KiServiceTable[SN_NtUserRemoveProp] = NtUserRemoveProp;
    KiServiceTable[SN_NtUserGetProp] = NtUserGetProp;
    KiServiceTable[SN_NtUserWaitMessage] = NtUserWaitMessage;
    KiServiceTable[SN_NtUserTranslateMessage] = NtUserTranslateMessage;
    KiServiceTable[SN_NtUserSetActiveWindow] = NtUserSetActiveWindow;
    KiServiceTable[SN_NtUserReleaseCapture] = NtUserReleaseCapture;
    KiServiceTable[SN_NtUserGetProcessWindowStation] =
        NtUserGetProcessWindowStation;
    KiServiceTable[SN_NtUserGetThreadDesktop] = NtUserGetThreadDesktop;
    KiServiceTable[SN_NtUserSetThreadDesktop] = NtUserSetThreadDesktop;
    KiServiceTable[SN_NtUserOpenInputDesktop] = NtUserOpenInputDesktop;
    KiServiceTable[SN_NtUserCloseDesktop] = NtUserCloseDesktop;
    KiServiceTable[SN_NtUserOpenDesktop] = NtUserOpenDesktop;
    KiServiceTable[SN_NtUserOpenWindowStation] = NtUserOpenWindowStation;
    KiServiceTable[SN_NtUserSetProcessWindowStation] =
        NtUserSetProcessWindowStation;
    KiServiceTable[SN_NtUserCloseWindowStation] = NtUserCloseWindowStation;
    KiServiceTable[SN_NtUserNotifyWinEvent] = NtUserNotifyWinEvent;
    KiServiceTable[SN_NtUserCallHwnd] = NtUserCallHwnd;
    KiServiceTable[SN_NtUserCallHwndLock] = NtUserCallHwndLock;
    KiServiceTable[SN_NtUserCallHwndParam] = NtUserCallHwndParam;
    KiServiceTable[SN_NtUserGetObjectInformation] = NtUserGetObjectInformation;
    KiServiceTable[SN_NtUserRegisterHotKey] = NtUserRegisterHotKey;
    KiServiceTable[SN_NtUserCallTwoParam] = NtUserCallTwoParam;
    KiServiceTable[0x1F02] = NtUserClassLong;
    KiServiceTable[0x1F03] = NtUserMapWindowPoints;
    KiServiceTable[0x1F04] = NtGdiReadUserBuffer;
    KiServiceTable[0x1F05] = NtUserMenuBar;
    KiServiceTable[0x1F06] = NtUserWindowRelation;
    KiServiceTable[0x1F07] = NtGdiCreatePatternBrush;
    KiServiceTable[0x1F08] = NtGdiClearWindowBackground;
    KiServiceTable[0x1F09] = NtRenameKey;
    KiServiceTable[0x1F0A] = NtAdjustPrivilegesToken;
    KiServiceTable[0x1F0B] = SvNtGdiCreateDisplayDC;
    KiServiceTable[0x1F0C] = SvNtUserGetCursorPos;
    KiServiceTable[0x1F0D] = SvNtUserChildWindowFromPoint;
    KiServiceTable[0x1F0E] = SvNtGdiViewportOrigin;
    KiServiceTable[0x1F0F] = SvNtGdiClipRect;
    KiServiceTable[0x1F10] = SvNtUserIsIconic;
    KiServiceTable[0x1F11] = SvNtUserCreateIconHandle;
    KiServiceTable[0x1F12] = SvNtUserDestroyIcon;
    KiServiceTable[0x1F13] = SvNtUserGetDesktopWindow;
    KiServiceTable[0x1F14] = SvNtUserSetCursorPos;
    KiServiceTable[0x1F15] = SvNtUserGetMessagePos;
    KiServiceTable[0x1F16] = SvNtUserQueryWindowState;
    KiServiceTable[0x1F19] = SvNtQueryThreadExitStatus;
    KiServiceTable[0x1F1A] = SvNtReadProcessMemory;
    KiServiceTable[SN_NtUserSetWindowLong] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserSetWindowLongPtr] = NtUserSetWindowLong;
    KiServiceTable[SN_NtUserChangeWindowMessageFilterEx] =
        NtUserChangeWindowMessageFilterEx;
    KiServiceTable[SN_NtUserValidateRect] = NtUserValidateRect;
    KiServiceTable[SN_NtUserRedrawWindow] = NtUserRedrawWindow;
    KiServiceTable[SN_NtUserGetUpdateRect] = NtUserGetUpdateRect;
    KiServiceTable[SN_NtUserFindExistingCursorIcon] =
        NtUserFindExistingCursorIcon;
    KiServiceTable[SN_NtUserCreateAcceleratorTable] =
        NtUserCreateAcceleratorTable;
    KiServiceTable[SN_NtUserDestroyAcceleratorTable] =
        NtUserDestroyAcceleratorTable;
    KiServiceTable[SN_NtUserDestroyCursor] = NtUserDestroyCursor;
    KiServiceTable[SN_NtUserSetCursor] = NtUserSetCursor;
    KiServiceTable[SN_NtGdiGetEntry] = NtGdiGetEntry;
    KiServiceTable[SN_NtGdiCreateSolidBrush] = NtGdiCreateSolidBrush;
    KiServiceTable[SN_NtGdiCreatePen] = NtGdiCreatePen;
    KiServiceTable[SN_NtGdiHfontCreate] = NtGdiHfontCreate;
    KiServiceTable[SN_NtGdiStretchBlt] = NtGdiStretchBlt;
    KiServiceTable[SN_NtGdiCreateRectRgn] = NtGdiCreateRectRgn;
    KiServiceTable[SN_NtGdiSelectBrush] = NtGdiSelectBrush;
    KiServiceTable[SN_NtGdiSelectPen] = NtGdiSelectPen;
    KiServiceTable[SN_NtGdiSelectFont] = NtGdiSelectFont;
    KiServiceTable[SN_NtGdiSelectBitmap] = NtGdiSelectBitmap;
    KiServiceTable[SN_NtGdiDeleteObjectApp] = NtGdiDeleteObjectApp;
    KiServiceTable[SN_NtGdiPatBlt] = NtGdiPatBlt;
    KiServiceTable[SN_NtGdiBitBlt] = NtGdiBitBlt;
    KiServiceTable[SN_NtGdiBlitUserBuffer] = NtGdiBlitUserBuffer;
    KiServiceTable[SN_NtGdiExtTextOutW] = NtGdiExtTextOutW;
    KiServiceTable[SN_NtGdiGetTextMetricsW] = NtGdiGetTextMetricsW;
    KiServiceTable[SN_NtGdiGetTextExtent] = NtGdiGetTextExtent;
    KiServiceTable[SN_NtGdiGetTextExtentExW] = NtGdiGetTextExtentExW;
    KiServiceTable[SN_NtGdiSetPixel] = NtGdiSetPixel;
    KiServiceTable[SN_NtGdiRectangle] = NtGdiRectangle;
    KiServiceTable[SN_NtGdiMoveTo] = NtGdiMoveTo;
    KiServiceTable[SN_NtGdiLineTo] = NtGdiLineTo;
    KiServiceTable[SN_NtGdiCreateCompatibleDC] = NtGdiCreateCompatibleDC;
    KiServiceTable[SN_NtGdiCreateCompatibleBitmap] = NtGdiCreateCompatibleBitmap;
    KiServiceTable[SN_NtGdiCreateBitmap] = NtGdiCreateBitmap;
    KiServiceTable[SN_NtGdiGetDCObject] = NtGdiGetDCObject;
    KiServiceTable[SN_NtGdiFlush] = NtGdiFlush;
    KiServiceTable[SN_NtGdiGetDeviceCaps] = NtGdiGetDeviceCaps;
    KiServiceTable[SN_NtGdiGetCurrentDpiInfo] = NtGdiGetCurrentDpiInfo;
    KiServiceTable[SN_NtUserGetDCEx] = NtUserGetDCEx;
    KiServiceTable[SN_NtUserGetWindowDC] = NtUserGetDCEx;
}

void KiRejectUserReturn(UINT64 rip, UINT64 rsp)
{
    PKTHREAD thread = KeGetCurrentThread();
    KeLog("[ke]   rejected unsafe user return: RIP=%p RSP=%p\n",
          (void *)rip, (void *)rsp);
    if (thread)
        thread->ExitStatus = STATUS_ACCESS_VIOLATION;
    KeTerminateThread();
}

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *reg_args, UINT64 user_rsp)
{
    PKTHREAD current = KeGetCurrentThread();
    if (current && current->Process && current->Process->LinuxContext) {
        const UINT64 linux_args[6] = {
            reg_args[4], reg_args[5], reg_args[1],
            reg_args[0], reg_args[2], reg_args[3]
        };
        return LinuxSystemCallDispatch(number, linux_args);
    }
    g_service_user_rsp = user_rsp;
    g_service_number = number;
    static UINT8 seen_services[SVINDOWS_MAX_SYSCALL];
    if (number < SVINDOWS_MAX_SYSCALL && number >= 0x1000 &&
        !seen_services[number]) {
        seen_services[number] = 1;
        PKTHREAD caller = KeGetCurrentThread();
        KeTrace("[user] first win32u service 0x%lx (tid %lu)\n",
              (unsigned long)number,
              caller ? (unsigned long)caller->ThreadId : 0ul);
    }
    if (number >= SVINDOWS_MAX_SYSCALL || KiServiceTable[number] == NULL) {
        /* Win32k-range services (win32u) that we do not implement yet return
         * the neutral failure value 0 (NULL handle / FALSE) instead of an
         * NTSTATUS, mirroring the loader's return-0 import stubs: real USER32
         * code paths treat an NTSTATUS here as a valid kernel GDI handle. */
        if (number >= 0x1000 && number < SVINDOWS_MAX_SYSCALL) {
            static UINT8 seen_missing[SVINDOWS_MAX_SYSCALL];
            if (!seen_missing[number]) {
                seen_missing[number] = 1;
                KeLog("[user] win32u service 0x%lx unimplemented -> 0\n",
                      (unsigned long)number);
            }
            return 0;
        }
        KeLog("[ke]   invalid system service 0x%lx\n", (unsigned long)number);
        return (UINT64)STATUS_NOT_IMPLEMENTED;
    }

    /* Build the full argument array: the four register arguments, then up to
     * twelve stack arguments read from the user stack above the ntdll stub's
     * return address (+0x28). NtUserCreateWindowEx uses fifteen arguments on
     * current x64 Windows. Each stack slot is probed, so a call that passes
     * fewer arguments (leaving RSP near the top of the stack) yields zeros
     * instead of faulting the kernel on an unmapped read. */
    UINT64 args[16];
    args[0] = reg_args[0];
    args[1] = reg_args[1];
    args[2] = reg_args[2];
    args[3] = reg_args[3];
    for (int i = 0; i < 12; i++) {
        UINT64 slot = user_rsp + 0x28 + (UINT64)i * 8;
        args[4 + i] = MmProbeForRead(slot, 8) ? *(volatile UINT64 *)slot : 0;
    }

    UINT64 result = KiServiceTable[number](args);
    KiDeliverKernelApcs();
    return result;
}
