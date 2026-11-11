/*
 * arch/x86_64/Trap.c - the C trap dispatcher.
 *
 * Called by the common assembly stub with a fully populated KTRAP_FRAME. For
 * Kernel-mode CPU exceptions remain fatal. A fault whose saved CS is ring 3,
 * however, belongs to the current process: dump it for diagnostics and
 * terminate only that user thread, just as NT turns an unhandled user exception
 * into process/thread teardown rather than a kernel bugcheck.
 */
#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
#include <nt/Exception.h>
#include <svindows/Ke.h>
#include <svindows/Hal.h>
#include <svindows/Mm.h>
#include <svindows/Ldr.h>
#include <svindows/Rtl.h>

static const char *const g_exception_names[32] = {
    "#DE Divide-by-Zero",
    "#DB Debug",
    "NMI",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR BOUND Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "(reserved 15)",
    "#MF x87 Floating-Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point",
    "#VE Virtualization",
    "#CP Control Protection",
    "(reserved 22)", "(reserved 23)", "(reserved 24)", "(reserved 25)",
    "(reserved 26)", "(reserved 27)", "(reserved 28)", "(reserved 29)",
    "#SX Security", "(reserved 31)",
};

static UINT64 read_cr2(void)
{
    UINT64 v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

static NTSTATUS exception_status(UINT64 vector)
{
    switch (vector) {
    case 0:  return STATUS_INTEGER_DIVIDE_BY_ZERO;
    case 4:  return STATUS_INTEGER_OVERFLOW;
    case 6:  return STATUS_ILLEGAL_INSTRUCTION;
    case 13:
    case 14: return STATUS_ACCESS_VIOLATION;
    default: return STATUS_UNSUCCESSFUL;
    }
}

static void dump_user_location(const char *label, UINT64 address)
{
    const char *module = NULL;
    const char *symbol = NULL;
    UINT64 module_base = 0;
    UINT64 symbol_address = 0;
    if (!LdrDescribeUserSymbol(address, &module, &module_base, &symbol,
                               &symbol_address))
        return;
    if (symbol)
        KeLog("%s%s!%s+0x%lx\n", label, module, symbol,
              (unsigned long)(address - symbol_address));
    else
        KeLog("%s%s+0x%lx\n", label, module,
              (unsigned long)(address - module_base));
}

static const char *vm_protection_name(ULONG protection)
{
    switch (protection & 0xff) {
    case 0x01: return "NOACCESS";
    case 0x02: return "READONLY";
    case 0x04: return "READWRITE";
    case 0x08: return "WRITECOPY";
    case 0x10: return "EXECUTE";
    case 0x20: return "EXECUTE_READ";
    case 0x40: return "EXECUTE_READWRITE";
    case 0x80: return "EXECUTE_WRITECOPY";
    default:   return "?";
    }
}

static void dump_page_mapping(PKPROCESS process, UINT64 address)
{
    UINT64 pte = MmGetPageFlags(address);
    if (pte) {
        KeLog("PTE=0x%lx [%s %s %s %s accessed=%u dirty=%u]\n",
              (unsigned long)pte,
              pte & PTE_PRESENT ? "present" : "not-present",
              pte & PTE_WRITE ? "write" : "read-only",
              pte & PTE_USER ? "user" : "supervisor",
              pte & PTE_NX ? "NX" : "executable",
              (unsigned)((pte & PTE_ACCESSED) != 0),
              (unsigned)((pte & PTE_DIRTY) != 0));
    } else {
        KeLog("PTE=unmapped\n");
    }

    if (!process)
        return;
    for (ULONG i = 0; i < MM_MAX_PROCESS_REGIONS; i++) {
        PMM_PROCESS_REGION region = &process->VmRegions[i];
        if (!region->InUse || address < region->Base ||
            address - region->Base >= region->ReserveSize)
            continue;
        KeLog("VM region=[%p, %p) reserve=0x%lx committed=0x%lx "
              "protect=%s%s (0x%lx) allocation=0x%lx type=0x%lx\n",
              (void *)region->Base,
              (void *)(region->Base + region->ReserveSize),
              (unsigned long)region->ReserveSize,
              (unsigned long)(region->CommittedPages * PAGE_SIZE),
              vm_protection_name(region->Protect),
              region->Protect & 0x100 ? "|GUARD" : "",
              (unsigned long)region->Protect,
              (unsigned long)region->AllocationProtect,
              (unsigned long)region->Type);
        return;
    }
    KeLog("VM region=none\n");
}

void KiDumpUserException(UINT64 record_va, UINT64 context_va)
{
    if (!MmProbeForRead(record_va, sizeof(EXCEPTION_RECORD64)) ||
        !MmProbeForRead(context_va, sizeof(CONTEXT_AMD64))) {
        KeLog("[user] saved exception record/context is no longer readable\n");
        return;
    }

    PEXCEPTION_RECORD64 record = (PEXCEPTION_RECORD64)record_va;
    PCONTEXT_AMD64 context = (PCONTEXT_AMD64)context_va;
    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;
    KeLog("\n=== UNHANDLED USER EXCEPTION 0x%08lx ===\n",
          (unsigned long)(UINT32)record->ExceptionCode);
    KeLog("process=%s pid=%lu thread=%s tid=%lu\n",
          process && process->Name ? process->Name : "?",
          process ? (unsigned long)process->ProcessId : 0ul,
          thread && thread->Name ? thread->Name : "?",
          thread ? (unsigned long)thread->ThreadId : 0ul);
    dump_user_location("location=", context->Rip);
    KeLog("RIP=%p RSP=%p RFLAGS=0x%lx\n", (void *)context->Rip,
          (void *)context->Rsp, (unsigned long)context->EFlags);
    KeLog("RAX=%p RBX=%p RCX=%p RDX=%p\n", (void *)context->Rax,
          (void *)context->Rbx, (void *)context->Rcx, (void *)context->Rdx);
    KeLog("RSI=%p RDI=%p RBP=%p\n", (void *)context->Rsi,
          (void *)context->Rdi, (void *)context->Rbp);
    KeLog("R8 =%p R9 =%p R10=%p R11=%p\n", (void *)context->R8,
          (void *)context->R9, (void *)context->R10, (void *)context->R11);
    KeLog("R12=%p R13=%p R14=%p R15=%p\n", (void *)context->R12,
          (void *)context->R13, (void *)context->R14, (void *)context->R15);

    if ((UINT32)record->ExceptionCode ==
            (UINT32)STATUS_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        UINT64 access = record->ExceptionInformation[0];
        UINT64 address = record->ExceptionInformation[1];
        KeLog("fault-address=%p access=%s\n", (void *)address,
              access == 8 ? "execute" : (access == 1 ? "write" : "read"));
        dump_page_mapping(process, address);
    }
}

/* Turn a ring-3 CPU trap into a normal user-mode exception-dispatch call. */
static BOOLEAN dispatch_user_exception(PKTRAP_FRAME f)
{
    PKTHREAD thread = KeGetCurrentThread();
    if (!thread || thread->ExceptionActive)
        return FALSE;

    UINT64 ntdll = LdrGetModuleBase("ntdll.dll");
    UINT64 dispatcher = ntdll ?
        LdrGetProcAddress(ntdll, "KiUserExceptionDispatcher") : 0;
    if (!dispatcher || !MmIsUserAddress(dispatcher))
        return FALSE;

    UINT64 context_va = (f->rsp - sizeof(CONTEXT_AMD64)) & ~15ULL;
    UINT64 record_va = (context_va - sizeof(EXCEPTION_RECORD64)) & ~15ULL;
    UINT64 dispatch_rsp = (record_va - 0x30) & ~15ULL;
    dispatch_rsp -= 8; /* Win64 function-entry alignment. */
    if (!MmProbeForWrite(dispatch_rsp, context_va + sizeof(CONTEXT_AMD64) -
                                      dispatch_rsp))
        return FALSE;

    PCONTEXT_AMD64 c = (PCONTEXT_AMD64)context_va;
    PEXCEPTION_RECORD64 r = (PEXCEPTION_RECORD64)record_va;
    memset(c, 0, sizeof(*c));
    memset(r, 0, sizeof(*r));

    c->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    c->SegCs = (UINT16)f->cs; c->SegSs = (UINT16)f->ss;
    c->EFlags = (UINT32)f->rflags;
    c->Rax = f->rax; c->Rcx = f->rcx; c->Rdx = f->rdx; c->Rbx = f->rbx;
    c->Rsp = f->rsp; c->Rbp = f->rbp; c->Rsi = f->rsi; c->Rdi = f->rdi;
    c->R8 = f->r8; c->R9 = f->r9; c->R10 = f->r10; c->R11 = f->r11;
    c->R12 = f->r12; c->R13 = f->r13; c->R14 = f->r14; c->R15 = f->r15;
    c->Rip = f->rip;

    r->ExceptionCode = exception_status(f->vector);
    r->ExceptionAddress = f->rip;
    if (f->vector == 14) {
        r->NumberParameters = 2;
        r->ExceptionInformation[0] = (f->error_code & 16) ? 8 :
                                     ((f->error_code & 2) ? 1 : 0);
        r->ExceptionInformation[1] = read_cr2();
    }

    *(UINT64 *)dispatch_rsp = 0;
    thread->ExceptionActive = TRUE;
    thread->ExceptionRecord = record_va;
    thread->ExceptionContext = context_va;
    f->rip = dispatcher;
    f->rsp = dispatch_rsp;
    f->rcx = record_va;
    f->rdx = context_va;
    return TRUE;
}

static void dump_frame(PKTRAP_FRAME f, UINT64 fault_address)
{
    const char *name = f->vector < 32 ? g_exception_names[f->vector]
                                      : "(external interrupt)";
    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;

    KeLog("\n=== TRAP: vector %lu (%s), error_code=0x%lx ===\n",
          (unsigned long)f->vector, name, (unsigned long)f->error_code);

    KeLog("frame=%p RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
          (void *)f, (void *)f->rip, (unsigned long)f->cs,
          (unsigned long)f->rflags);
    /* On a same-CPL exception the CPU does not push RSP/SS.  Those two
       trailing KTRAP_FRAME fields therefore do not exist in memory; recover
       the interrupted RSP from the known hardware/stub frame geometry. */
    UINT64 interrupted_rsp = (f->cs & 3u) == 3u ? f->rsp :
                             (UINT64)(ULONG_PTR)f + 20u * sizeof(UINT64);
    if ((f->cs & 3u) == 3u)
        KeLog("RSP=%p SS=0x%lx (CPU-saved)\n", (void *)interrupted_rsp,
              (unsigned long)f->ss);
    else
        KeLog("RSP=%p (reconstructed kernel RSP)\n",
              (void *)interrupted_rsp);

    KeLog("process=%s pid=%lu thread=%s tid=%lu\n",
          process && process->Name ? process->Name : "?",
          process ? (unsigned long)process->ProcessId : 0ul,
          thread && thread->Name ? thread->Name : "?",
          thread ? (unsigned long)thread->ThreadId : 0ul);

    if ((f->cs & 3) == 3) {
        dump_user_location("location=", f->rip);
    }

    if (f->vector == 14) { /* #PF: CR2 holds the faulting linear address */
        UINT64 error = f->error_code;
        KeLog("CR2=%p page-fault=%s %s %s%s%s%s%s\n",
              (void *)fault_address,
              error & 1 ? "protection" : "not-present",
              error & 2 ? "write" : "read",
              error & 4 ? "user" : "supervisor",
              error & 8 ? " reserved-bit" : "",
              error & 16 ? " instruction-fetch" : "",
              error & 32 ? " protection-key" : "",
              error & 64 ? " shadow-stack" : "");
        dump_page_mapping(process, fault_address);
    }

    KeLog("RAX=%p RBX=%p RCX=%p RDX=%p\n",
          (void *)f->rax, (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
    KeLog("RSI=%p RDI=%p RBP=%p\n",
          (void *)f->rsi, (void *)f->rdi, (void *)f->rbp);
    KeLog("R8 =%p R9 =%p R10=%p R11=%p\n",
          (void *)f->r8, (void *)f->r9, (void *)f->r10, (void *)f->r11);
    KeLog("R12=%p R13=%p R14=%p R15=%p\n",
          (void *)f->r12, (void *)f->r13, (void *)f->r14, (void *)f->r15);

    /* The instruction bytes are taken from the actual executing image, not
       from source/debug line mappings which may be stale after ZIP updates. */
    if (f->rip && (((f->cs & 3u) == 0u) ||
                   (MmIsUserAddress(f->rip) && MmProbeForRead(f->rip, 4)))) {
        const volatile UINT8 *ip = (const volatile UINT8 *)f->rip;
        UINT32 count = (f->cs & 3u) == 3u ? 4u :
                       PAGE_SIZE - (UINT32)(f->rip & (PAGE_SIZE - 1u));
        if (count > 8u) count = 8u;
        KeLog("code@RIP (live):");
        for (UINT32 i = 0; i < count; i++)
            KeLog(" %02x", ip[i]);
        KeLog("\n");
    }
    if (f->rsp && MmIsUserAddress(f->rsp) && MmProbeForRead(f->rsp, 32)) {
        const volatile UINT64 *sp = (const volatile UINT64 *)f->rsp;
        KeLog("stack@RSP: %p %p %p %p\n",
              (void *)sp[0], (void *)sp[1], (void *)sp[2], (void *)sp[3]);
    }

    /* Annotate the user stack: values landing inside loaded module images are
     * return addresses, giving an exit/fault backtrace without full unwind
     * support. */
    if ((f->cs & 3) == 3 && f->rsp && MmIsUserAddress(f->rsp)) {
        KeLog("[user] stack walk:\n");
        for (UINT64 i = 0; i < 64; i++) {
            UINT64 va = f->rsp + i * 8;
            if (!MmProbeForRead(va, sizeof(UINT64)))
                break;
            UINT64 value = *(volatile UINT64 *)va;
            const char *module = NULL, *symbol = NULL;
            UINT64 module_base = 0, symbol_address = 0;
            if (!value ||
                !LdrDescribeUserSymbol(value, &module, &module_base, &symbol,
                                       &symbol_address))
                continue;
            if (symbol)
                KeLog("  [%02lu] %p  %s!%s+0x%lx\n", (unsigned long)i,
                      (void *)value, module, symbol,
                      (unsigned long)(value - symbol_address));
            else
                KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                      (void *)value, module,
                      (unsigned long)(value - module_base));
        }
    }
}

void KiDispatchTrap(PKTRAP_FRAME frame)
{
    /* CR2 is volatile across page faults. Snapshot it before any diagnostic
       helper or exception dispatch can touch pageable/user memory. */
    UINT64 fault_address = frame && frame->vector == 14 ? read_cr2() : 0;
    if(frame&&frame->vector==14)
        __atomic_add_fetch(&KeGetCurrentProcessorBlock()->PageFaults,1,__ATOMIC_RELAXED);
    /* Private SMP wake/IPI vector. The parked AP will recheck its release
     * state after iret; no scheduler work is done on this early path. */
    if (frame->vector == HAL_SMP_WAKE_VECTOR) {
        HalLocalApicEoi();
        return;
    }
    /* Hardware IRQs (remapped to vectors 32..47) go to the HAL, which EOIs the
     * PIC and runs the registered handler (e.g. the scheduler tick). */
    if (frame->vector >= IRQ_BASE_VECTOR && frame->vector < IRQ_BASE_VECTOR + 16) {
        KIRQL old_irql;
        KeRaiseIrql(HIGH_LEVEL, &old_irql);
        __atomic_add_fetch(&KeGetCurrentProcessorBlock()->InterruptCount,1,__ATOMIC_RELAXED);
        if(frame->vector==IRQ_BASE_VECTOR)
            KeAccountClockTick((frame->cs&3)==3);
        HalDispatchIrq((UINT8)(frame->vector - IRQ_BASE_VECTOR));
        KeLowerIrql(old_irql);
        KiScheduleAfterInterrupt();
        return;
    }

    /* Deliver first-chance ring-3 exceptions through ntdll. */
    if (frame->vector < 32) {
        if ((frame->cs & 3) == 3 && dispatch_user_exception(frame))
            return;
        dump_frame(frame, fault_address);
        if ((frame->cs & 3) == 3) {
            if (MmProbeForRead(LDR_CFG_TRACE_VA, sizeof(UINT64)))
                KeLog("[user] last CFG indirect target: %p\n",
                      (void *)*(volatile UINT64 *)LDR_CFG_TRACE_VA);
            PKTHREAD thread = KeGetCurrentThread();
            KeLog("[user] unhandled exception in thread '%s' (id %u); "
                  "terminating thread only\n",
                  thread && thread->Name ? thread->Name : "?",
                  thread ? thread->ThreadId : 0);
            KeTerminateThread();
        }
        KeBugCheck(KE_UNEXPECTED_KERNEL_MODE_TRAP,
                   "Unhandled CPU exception in kernel mode");
    }

    /* Any other vector is unexpected. */
    dump_frame(frame, fault_address);
    KeLog("Ignoring unexpected interrupt %lu\n", (unsigned long)frame->vector);
}
