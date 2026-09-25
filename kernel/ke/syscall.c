/*
 * ke/syscall.c - the system-call boundary: MSR setup, the per-CPU block, the
 * service dispatcher, and the first handful of Nt* services.
 *
 * This is the interface native (ring 3) code will eventually reach through
 * ntdll. For now a tiny in-kernel user stub exercises it directly.
 */
#include <svindows/ke.h>
#include <svindows/trace.h>
#include <svindows/mm.h>
#include <svindows/io.h>
#include <svindows/ps.h>
#include <svindows/ldr.h>
#include <svindows/io.h>
#include <svindows/cm.h>
#include <svindows/gfx.h>
#include <svindows/win32k.h>
#include <svindows/rtl.h>
#include <nt/ntdef.h>
#include <nt/ntstatus.h>

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

/*
 * KPCR - per-CPU block reached via GS after swapgs. The assembly entry relies
 * on the exact offsets of the first two fields (0 and 8).
 */
typedef struct _KPCR {
    UINT64   UserRspScratch; /* +0: user RSP saved during syscall entry */
    UINT64   KernelRsp;      /* +8: kernel stack loaded on syscall entry */
    PKTHREAD CurrentThread;  /* +16 */
} KPCR;

static KPCR g_kpcr;

extern void KiSystemCallEntry(void); /* arch/x86_64/syscall_entry.asm */

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

/* The syscall entry stub reaches the current KTHREAD through KPCR+16 (GS
 * after swapgs), so the scheduler must publish it there on every switch. */
void KeSetCurrentThread(PKTHREAD thread)
{
    g_kpcr.CurrentThread = thread;
}

void KeSetKernelStack(UINT64 kernel_rsp)
{
    g_kpcr.KernelRsp = kernel_rsp;
    KeSetTssRsp0(kernel_rsp);
}

void KeSetUserGsBase(UINT64 teb)
{
    /* KERNEL_GS_BASE holds what swapgs will make the active GS on the next
     * kernel exit. For a user thread that is its TEB; for a kernel thread we
     * park the per-CPU block there. Kept correct on every context switch, so a
     * thread that terminates mid-syscall can't leave it stale. */
    wrmsr(MSR_KERNEL_GS_BASE, teb ? teb : (UINT64)&g_kpcr);
}

void KiInitializeSystemCalls(void)
{
    /* Invariant: in ring 0 the active GS base is the per-CPU block (KPCR). The
     * "other" base holds the value swapgs will restore on the next kernel exit;
     * it starts as the KPCR too and is repointed at each thread's TEB by the
     * scheduler (KeSetUserGsBase). */
    wrmsr(MSR_GS_BASE, (UINT64)&g_kpcr);
    wrmsr(MSR_KERNEL_GS_BASE, (UINT64)&g_kpcr);

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

/* Test-only private services used by ABITEST.EXE. Keeping the validation on
 * the kernel side proves that the syscall entry gathered all sixteen Win64
 * arguments from the correct register/stack slots. */
static UINT64 NtAbiProbe(UINT64 *a)
{
    UINT64 count = g_service_number - 0x180;
    if (count > 16)
        return 0;
    for (UINT64 i = 0; i < count; i++)
        if (a[i] != 0xA810000000000000ULL + i)
            return 0;
    return 0xA8100000ULL + count;
}

static UINT64 NtAbiShadowProbe(UINT64 *a)
{
    (void)a;
    if ((g_service_user_rsp & 15) != 8 ||
        !MmProbeForWrite(g_service_user_rsp + 8, 32))
        return 0;
    return 0xA8105A5AULL;
}

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
        const char *name = 0;
        UINT64 base = 0;
        if (value && LdrDescribeUserAddress(value, &name, &base))
            KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                  (void *)value, name, (unsigned long)(value - base));
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
            const char *name = 0;
            UINT64 base = 0;
            if (ret && LdrDescribeUserAddress(ret, &name, &base))
                KeLog("  #%d %s+0x%lx\n", level, name,
                      (unsigned long)(ret - base));
            if (!MmProbeForRead(rbp, 8))
                break;
            UINT64 next = *(volatile UINT64 *)rbp;
            if (next <= rbp || next >= stack_max)
                break;
            rbp = next;
        }
    }
}

/* NtTraceCall - loader trampoline notification (see svindows/trace.h). Args keep
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
    const char *mod = 0;
    UINT64 base = 0;
    if (ret && LdrDescribeUserAddress(ret, &mod, &base))
        KeLog("[trace] %s(a1=%p, a2=%p, a3=%p, a4=%p) from %s+0x%lx\n",
              name, (void *)a[0], (void *)a[1], (void *)a[2], (void *)a[3],
              mod, (unsigned long)(ret - base));
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
        if (MmProbeForRead(cls, 16)) {
            WCHAR wname[24];
            for (int i = 0; i < 23; i++) {
                UINT16 c = *(volatile UINT16 *)(cls + i * 2);
                wname[i] = c;
                if (!c)
                    break;
            }
            wname[23] = 0;
            char ascii[24];
            for (int i = 0; i < 23; i++)
                ascii[i] = wname[i] && wname[i] < 128 ? (char)wname[i] : '?';
            ascii[wname[23] ? 23 : 0] = 0;
            KeLog("[trace]   class name: %s\n", ascii);
        }
    }
    return 0;
}

/* NtTerminateThread - end the calling thread; does not return. */
static UINT64 NtTerminateThread(UINT64 *a)
{
    KeLog("[user] NtTerminateThread(handle=%p, status=0x%08lx); ending user thread\n",
          (void *)a[0], (unsigned long)(UINT32)a[1]);
    KiDumpUserStack(g_service_user_rsp);
    PKTHREAD thread = KeGetCurrentThread();
    if (thread) {
        thread->ExitStatus = (NTSTATUS)a[1];
        /* KiUserExceptionDispatcher uses this service when no handler accepts
         * a first-chance exception. Windows terminates the process in that
         * case even when the fault happened on a worker thread. */
        if (thread->ExceptionActive)
            thread->ProcessMainThread = TRUE;
    }
    KeTerminateThread();
    return 0; /* unreachable */
}

/* NtContinue's user-mode half restores the AMD64 register image after this
 * service validates the context and closes the active exception dispatch. */
static UINT64 NtContinue(UINT64 *a)
{
    if (!MmProbeForRead(a[0], 256))
        return (UINT64)STATUS_ACCESS_VIOLATION;
    PKTHREAD thread = KeGetCurrentThread();
    if (thread)
        thread->ExceptionActive = FALSE;
    return (UINT64)STATUS_SUCCESS;
}

#define MEM_COMMIT      0x1000u
#define MEM_RESERVE     0x2000u
#define MEM_DECOMMIT    0x4000u
#define MEM_RELEASE     0x8000u
#define MEM_FREE        0x10000u
#define MEM_PRIVATE     0x20000u
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
        *size_ptr == 0 || !(allocation_type & (MEM_RESERVE | MEM_COMMIT)) ||
        !MiValidProtection(protect))
        return (UINT64)STATUS_INVALID_PARAMETER;

    UINT64 size = PAGE_ALIGN_UP(*size_ptr);
    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;
    if (!process)
        return (UINT64)STATUS_INVALID_PARAMETER;

    UINT64 requested = (UINT64)*base_ptr;
    UINT64 base = requested ? PAGE_ALIGN(requested) : 0;
    PMM_PROCESS_REGION region = NULL;
    if (allocation_type & MEM_RESERVE) {
        if (!base)
            base = (process->UserAllocationNext + 0xffff) & ~0xffffULL;
        if (MiFindRegion(process, base, size))
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++) {
            PMM_PROCESS_REGION other = &process->VmRegions[i];
            if (other->InUse && base < other->Base + other->ReserveSize &&
                other->Base < base + size)
                return (UINT64)STATUS_CONFLICTING_ADDRESSES;
        }
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
            if (pa == MM_INVALID_PHYS ||
                !MmMapPage(va, pa, MiProtectionFlags(protect)))
                return (UINT64)STATUS_NO_MEMORY;
            memset(MmPhysToVirt(pa), 0, PAGE_SIZE);
            region->CommittedPages++;
        }
        region->Protect = protect;
    }

    *base_ptr = (PVOID)base;
    *size_ptr = size;

    KeLog("[user] NtAllocateVirtualMemory(%lu) -> %p\n",
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
 * caller for the (relative, negative, 100 ns units) interval. Implemented as a
 * cooperative yield loop against the tick count (10 ms resolution).
 */
static UINT64 NtDelayExecution(UINT64 *a)
{
    INT64 *interval = (INT64 *)a[1];
    if (!interval)
        return (UINT64)STATUS_SUCCESS;

    INT64 iv = *interval;
    UINT64 ticks_100ns = (iv < 0) ? (UINT64)(-iv) : 0; /* only relative delays */
    UINT64 wait = ticks_100ns / 100000ULL;             /* 100 ns -> 10 ms ticks */
    if (wait == 0)
        wait = 1;

    UINT64 target = KeGetTickCount() + wait;
    while (KeGetTickCount() < target)
        KeYield();
    return (UINT64)STATUS_SUCCESS;
}

/*
 * NtQuerySystemInformation(SystemInformationClass, SystemInformation,
 *                          SystemInformationLength, ReturnLength).
 *
 * Explorer asks for SystemPolicyInformation (class 134) while constructing
 * the desktop.  This is an opaque CLIP/WarBird licensing channel, not a plain
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
        !MiValidProtection(protect))
        return (UINT64)STATUS_ACCESS_VIOLATION;

    UINT64 base = PAGE_ALIGN((UINT64)*base_ptr);
    UINT64 size = PAGE_ALIGN_UP(*size_ptr);
    PKPROCESS process = KeGetCurrentThread()->Process;
    PMM_PROCESS_REGION region = MiFindRegion(process, base, size);
    if (!region)
        return (UINT64)STATUS_CONFLICTING_ADDRESSES;
    for (UINT64 va = base; va < base + size; va += PAGE_SIZE)
        if (MmGetPhysicalAddress(va) == MM_INVALID_PHYS)
            return (UINT64)STATUS_CONFLICTING_ADDRESSES;

    if (old_ptr && MmProbeForWrite((UINT64)old_ptr, sizeof(UINT32)))
        *old_ptr = region->Protect;
    MmProtectRangeFlags(base, size, MiProtectionFlags(protect));
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


#include "../win32k/user_messages.inc"
#include "../win32k/user_windows.inc"
#include "../win32k/user_callbacks.inc"

/*
 * The system service table, indexed by real Windows 7 SP1 x64 syscall numbers
 * (from the public NT syscall-number tables). Sparse: most slots are unused.
 * Our ntdll stubs issue these same numbers, and a service's slot holds the
 * routine implementing it. Aligning to a real build's numbering is what would
 * let a genuine ntdll drive this kernel; the exact values target Win7 SP1 x64.
 */
#define SVINDOWS_MAX_SYSCALL 0x2000
static KI_SERVICE KiServiceTable[SVINDOWS_MAX_SYSCALL];

/* Number assignments (Win7 SP1 x64). Kept together as the single source of
 * truth, mirrored by the ntdll stubs. */
#define SN_NtWaitForSingleObject   0x01
#define SN_NtWriteFile             0x05
#define SN_NtReadFile              0x03
#define SN_NtClose                 0x0C
#define SN_NtOpenKey               0x0F
#define SN_NtAllocateVirtualMemory 0x15
#define SN_NtFreeVirtualMemory     0x1B
#define SN_NtQueryVirtualMemory    0x20
#define SN_NtQueryValueKey         0x17
#define SN_NtCreateKey             0x1A
#define SN_NtSetEvent              0x02
#define SN_NtCreateEvent           0x48
#define SN_NtCreateThreadEx        0xA5
#define SN_NtCreateFile            0x52
#define SN_NtSetValueKey           0x5D
#define SN_NtTerminateThread       0x50
#define SN_NtContinue              0x194
#define SN_NtDelayExecution        0x31
#define SN_NtQuerySystemInformation 0x33
#define SN_NtProtectVirtualMemory  0x4D
/* SVINDOWS-private services (no Windows equivalent) live above the real range. */
#define SN_NtDisplayString         0xF0
#define SN_NtDisplayNumber         0xF1
#define SN_NtLoadLibrary           0xF2
#define SN_NtEnumerateRootFiles    0xF3
#define SN_NtQueryFileInfo         0xF4
#define SN_NtSetFilePosition       0xF5
#define SN_NtWaitForMultipleObjects 0xF6
#define SN_NtResetEvent             0xF7
#define SN_NtCreateSemaphore        0xF8
#define SN_NtReleaseSemaphore       0xF9
#define SN_NtQueryInformationProcess 0xFA
#define SN_NtTraceCall           0xFB
#define SN_NtCreateUserProcessSimple 0xFD
#define SN_NtFileSystemControlSimple 0xFE
#define SN_NtSetFileSizeSimple       0xFF
#define SN_NtCreatePipeSimple        0x100
#define SN_NtConsoleControlSimple     0x101
#define SN_NtEnumerateDirectorySimple 0x102
#define SN_NtAbiProbeFirst             0x180
#define SN_NtAbiProbeLast              0x190
#define SN_NtAbiShadowProbe            0x191
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
#define SN_NtGdiCreateRectRgn             0x1086
#define SN_NtGdiSelectBrush               0x12F6
#define SN_NtGdiSelectPen                 0x12F8
#define SN_NtGdiSelectFont                0x103C
#define SN_NtGdiSelectBitmap              0x100E
#define SN_NtGdiDeleteObjectApp           0x1026
#define SN_NtGdiPatBlt                    0x105C
#define SN_NtGdiBitBlt                    0x100B
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
    KiServiceTable[SN_NtClose]                 = NtClose;
    KiServiceTable[SN_NtOpenKey]               = NtOpenKey;
    KiServiceTable[SN_NtAllocateVirtualMemory] = NtAllocateVirtualMemory;
    KiServiceTable[SN_NtFreeVirtualMemory]     = NtFreeVirtualMemory;
    KiServiceTable[SN_NtQueryVirtualMemory]    = NtQueryVirtualMemory;
    KiServiceTable[SN_NtQueryValueKey]         = NtQueryValueKey;
    KiServiceTable[SN_NtCreateKey]             = NtCreateKey;
    KiServiceTable[SN_NtSetEvent]              = NtSetEvent;
    KiServiceTable[SN_NtCreateEvent]           = NtCreateEvent;
    KiServiceTable[SN_NtCreateThreadEx]        = NtCreateThreadEx;
    KiServiceTable[SN_NtCreateFile]            = NtCreateFile;
    KiServiceTable[SN_NtSetValueKey]           = NtSetValueKey;
    KiServiceTable[SN_NtTerminateThread]       = NtTerminateThread;
    KiServiceTable[SN_NtContinue]              = NtContinue;
    KiServiceTable[SN_NtDelayExecution]        = NtDelayExecution;
    KiServiceTable[SN_NtQuerySystemInformation] = NtQuerySystemInformation;
    KiServiceTable[SN_NtProtectVirtualMemory]  = NtProtectVirtualMemory;
    KiServiceTable[SN_NtDisplayString]         = NtDisplayString;
    KiServiceTable[SN_NtDisplayNumber]         = NtDisplayNumber;
    KiServiceTable[SN_NtLoadLibrary]           = NtLoadLibrary;
    KiServiceTable[SN_NtEnumerateRootFiles]    = NtEnumerateRootFiles;
    KiServiceTable[SN_NtQueryFileInfo]         = NtQueryFileInfo;
    KiServiceTable[SN_NtSetFilePosition]       = NtSetFilePosition;
    KiServiceTable[SN_NtWaitForMultipleObjects] = NtWaitForMultipleObjects;
    KiServiceTable[SN_NtResetEvent]             = NtResetEvent;
    KiServiceTable[SN_NtCreateSemaphore]        = NtCreateSemaphore;
    KiServiceTable[SN_NtReleaseSemaphore]       = NtReleaseSemaphore;
    KiServiceTable[SN_NtQueryInformationProcess] = NtQueryInformationProcess;
    KiServiceTable[SN_NtTraceCall] = NtTraceCall;
    KiServiceTable[SN_NtCreateUserProcessSimple] = NtCreateUserProcessSimple;
    KiServiceTable[SN_NtFileSystemControlSimple] = NtFileSystemControlSimple;
    KiServiceTable[SN_NtSetFileSizeSimple] = NtSetFileSizeSimple;
    KiServiceTable[SN_NtCreatePipeSimple] = NtCreatePipeSimple;
    KiServiceTable[SN_NtConsoleControlSimple] = NtConsoleControlSimple;
    KiServiceTable[SN_NtEnumerateDirectorySimple] = NtEnumerateDirectorySimple;
    for (UINT64 number = SN_NtAbiProbeFirst;
         number <= SN_NtAbiProbeLast; number++)
        KiServiceTable[number] = NtAbiProbe;
    KiServiceTable[SN_NtAbiShadowProbe] = NtAbiShadowProbe;
    KiServiceTable[SN_NtCreateMutant] = NtCreateMutant;
    KiServiceTable[SN_NtReleaseMutant] = NtReleaseMutant;
    KiServiceTable[SN_NtCallbackReturn] = NtCallbackReturn;
    KiServiceTable[SN_NtUserGetThreadState] = NtUserGetThreadState;
    KiServiceTable[SN_NtUserPeekMessage] = NtUserPeekMessage;
    KiServiceTable[SN_NtUserCallOneParam] = NtUserCallOneParam;
    KiServiceTable[SN_NtUserGetMessage] = NtUserGetMessage;
    KiServiceTable[SN_NtUserGetDC] = NtUserGetDC;
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
    KiServiceTable[SN_NtGdiCreateRectRgn] = NtGdiCreateRectRgn;
    KiServiceTable[SN_NtGdiSelectBrush] = NtGdiSelectBrush;
    KiServiceTable[SN_NtGdiSelectPen] = NtGdiSelectPen;
    KiServiceTable[SN_NtGdiSelectFont] = NtGdiSelectFont;
    KiServiceTable[SN_NtGdiSelectBitmap] = NtGdiSelectBitmap;
    KiServiceTable[SN_NtGdiDeleteObjectApp] = NtGdiDeleteObjectApp;
    KiServiceTable[SN_NtGdiPatBlt] = NtGdiPatBlt;
    KiServiceTable[SN_NtGdiBitBlt] = NtGdiBitBlt;
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

UINT64 KiSystemServiceDispatch(UINT64 number, UINT64 *reg_args, UINT64 user_rsp)
{
    g_service_user_rsp = user_rsp;
    g_service_number = number;
    static UINT8 seen_services[SVINDOWS_MAX_SYSCALL];
    if (number < SVINDOWS_MAX_SYSCALL && number >= 0x1000 &&
        !seen_services[number]) {
        seen_services[number] = 1;
        PKTHREAD caller = KeGetCurrentThread();
        KeLog("[user] first win32u service 0x%lx (tid %lu)\n",
              (unsigned long)number,
              caller ? (unsigned long)caller->ThreadId : 0ul);
        /* The first win32k-range call means USER32 finished its process
         * attach, so its `gpsi` is set. Report where it points: SERVERINFO is
         * the page GetSystemMetrics and GetSysColor answer out of without ever
         * entering the kernel, so a wrong pointer here is invisible in every
         * other log. */
        static UINT8 gpsi_reported;
        if (!gpsi_reported) {
            gpsi_reported = 1;
            UINT64 user32 = LdrGetModuleBase("user32.dll");
            if (user32 && MmProbeForRead(user32 + USER32_GPSI_OFFSET, 8)) {
                UINT64 gpsi = *(volatile UINT64 *)(user32 +
                                                   USER32_GPSI_OFFSET);
                KeLog("[user] USER32 gpsi = %p (SERVERINFO is %p) -> %s\n",
                      (void *)gpsi, (void *)PROCESS_SERVER_INFO_VA,
                      gpsi == PROCESS_SERVER_INFO_VA ? "connected"
                                                     : "NOT ours");
                if (gpsi == PROCESS_SERVER_INFO_VA)
                    KeLog("[user]   screen from gpsi: %ldx%ld\n",
                          (long)*(volatile INT32 *)(gpsi + 0x758),
                          (long)*(volatile INT32 *)(gpsi + 0x75C));
            }
            /* GDI32 owns the client side of the handle table: it caches the
             * table pointer and the handle count in its own data, and every
             * handle check goes through them. If its initialization did not
             * publish them, nothing GDI does can succeed, so fill them in. */
            UINT64 gdi32 = LdrGetModuleBase("gdi32.dll");
            if (gdi32) {
                UINT64 table = LdrGetProcAddress(gdi32,
                                                 "pGdiSharedHandleTable");
                UINT64 count = LdrGetProcAddress(gdi32,
                                                 "gMaxGdiHandleCount");
                if (table && MmProbeForWrite(table, sizeof(UINT64))) {
                    if (*(volatile UINT64 *)table != PROCESS_GDI_SHARED_TABLE_VA)
                        *(UINT64 *)table = PROCESS_GDI_SHARED_TABLE_VA;
                }
                if (count && MmProbeForWrite(count, sizeof(UINT32))) {
                    if (*(volatile UINT32 *)count == 0)
                        *(UINT32 *)count = 0x10000;
                }
                /* Every GDI handle check ends with
                 *   (cell.ProcessId & ~1) == *(DWORD *)(gdi32 + 0x2E04C)
                 * -- gdi32's cached process id, read out of GetDeviceCaps. The
                 * cells win32k publishes carry this process's client id, so the
                 * cache has to agree or every DC is rejected before any syscall
                 * happens, which looks exactly like "GDI does nothing".
                 */
                UINT64 pid_cache = gdi32 + 0x2E04C;
                if (MmProbeForWrite(pid_cache, sizeof(UINT32))) {
                    UINT32 cached = *(volatile UINT32 *)pid_cache;
                    if (cached != PROCESS_CLIENT_ID) {
                        *(UINT32 *)pid_cache = PROCESS_CLIENT_ID;
                        KeLog("[user] GDI32 cached process id %lu -> %u\n",
                              (unsigned long)cached,
                              (unsigned)PROCESS_CLIENT_ID);
                    }
                }
                KeLog("[user] GDI32 pGdiSharedHandleTable=%p "
                      "gMaxGdiHandleCount=%lu\n",
                      table ? (void *)*(volatile UINT64 *)table : 0,
                      count ? (unsigned long)*(volatile UINT32 *)count : 0ul);
            }
        }
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

    return KiServiceTable[number](args);
}
