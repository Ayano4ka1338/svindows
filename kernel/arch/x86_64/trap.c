/*
 * arch/x86_64/trap.c - the C trap dispatcher.
 *
 * Called by the common assembly stub with a fully populated KTRAP_FRAME. For
 * Kernel-mode CPU exceptions remain fatal. A fault whose saved CS is ring 3,
 * however, belongs to the current process: dump it for diagnostics and
 * terminate only that user thread, just as NT turns an unhandled user exception
 * into process/thread teardown rather than a kernel bugcheck.
 */
#include <nt/ntdef.h>
#include <nt/ntstatus.h>
#include <nt/exception.h>
#include <svindows/ke.h>
#include <svindows/hal.h>
#include <svindows/mm.h>
#include <svindows/ldr.h>
#include <svindows/rtl.h>

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
    f->rip = dispatcher;
    f->rsp = dispatch_rsp;
    f->rcx = record_va;
    f->rdx = context_va;
    KeLog("[seh] dispatch code=0x%08lx address=%p tid=%lu\n",
          (unsigned long)(UINT32)r->ExceptionCode,
          (void *)r->ExceptionAddress,
          (unsigned long)thread->ThreadId);
    return TRUE;
}

static void dump_frame(PKTRAP_FRAME f)
{
    const char *name = f->vector < 32 ? g_exception_names[f->vector]
                                      : "(external interrupt)";
    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;

    KeLog("\n=== TRAP: vector %lu (%s), error_code=0x%lx ===\n",
          (unsigned long)f->vector, name, (unsigned long)f->error_code);

    KeLog("RIP=%p  CS=0x%lx  RFLAGS=0x%lx\n",
          (void *)f->rip, (unsigned long)f->cs, (unsigned long)f->rflags);
    KeLog("RSP=%p  SS=0x%lx\n", (void *)f->rsp, (unsigned long)f->ss);

    KeLog("process=%s pid=%lu thread=%s tid=%lu\n",
          process && process->Name ? process->Name : "?",
          process ? (unsigned long)process->ProcessId : 0ul,
          thread && thread->Name ? thread->Name : "?",
          thread ? (unsigned long)thread->ThreadId : 0ul);

    if ((f->cs & 3) == 3) {
        const char *module = NULL;
        UINT64 base = 0;
        if (LdrDescribeUserAddress(f->rip, &module, &base))
            KeLog("location=%s+0x%lx\n", module,
                  (unsigned long)(f->rip - base));
    }

    if (f->vector == 14) { /* #PF: CR2 holds the faulting linear address */
        UINT64 error = f->error_code;
        KeLog("CR2=%p page-fault=%s %s %s%s%s%s%s\n",
              (void *)read_cr2(),
              error & 1 ? "protection" : "not-present",
              error & 2 ? "write" : "read",
              error & 4 ? "user" : "supervisor",
              error & 8 ? " reserved-bit" : "",
              error & 16 ? " instruction-fetch" : "",
              error & 32 ? " protection-key" : "",
              error & 64 ? " shadow-stack" : "");
    }

    KeLog("RAX=%p RBX=%p RCX=%p RDX=%p\n",
          (void *)f->rax, (void *)f->rbx, (void *)f->rcx, (void *)f->rdx);
    KeLog("RSI=%p RDI=%p RBP=%p\n",
          (void *)f->rsi, (void *)f->rdi, (void *)f->rbp);
    KeLog("R8 =%p R9 =%p R10=%p R11=%p\n",
          (void *)f->r8, (void *)f->r9, (void *)f->r10, (void *)f->r11);
    KeLog("R12=%p R13=%p R14=%p R15=%p\n",
          (void *)f->r12, (void *)f->r13, (void *)f->r14, (void *)f->r15);

    /* Debug aid: raw bytes at the faulting RIP and the top of the stack. */
    if (f->rip && MmIsUserAddress(f->rip) && MmProbeForRead(f->rip, 16)) {
        const volatile UINT8 *ip = (const volatile UINT8 *)f->rip;
        KeLog("code@RIP:");
        for (int i = 0; i < 16; i++)
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
            const char *name = NULL;
            UINT64 base = 0;
            if (value && LdrDescribeUserAddress(value, &name, &base))
                KeLog("  [%02lu] %p  %s+0x%lx\n", (unsigned long)i,
                      (void *)value, name,
                      (unsigned long)(value - base));
        }
    }
}

void KiDispatchTrap(PKTRAP_FRAME frame)
{
    /* Hardware IRQs (remapped to vectors 32..47) go to the HAL, which EOIs the
     * PIC and runs the registered handler (e.g. the scheduler tick). */
    if (frame->vector >= IRQ_BASE_VECTOR && frame->vector < IRQ_BASE_VECTOR + 16) {
        HalDispatchIrq((UINT8)(frame->vector - IRQ_BASE_VECTOR));
        return;
    }

    /* Deliver first-chance ring-3 exceptions through ntdll. */
    if (frame->vector < 32) {
        if ((frame->cs & 3) == 3 && dispatch_user_exception(frame))
            return;
        dump_frame(frame);
        if ((frame->cs & 3) == 3) {
            const UINT64 cfg_trace_va = 0x0000000000088ff8ULL;
            if (MmProbeForRead(cfg_trace_va, sizeof(UINT64)))
                KeLog("[user] last CFG indirect target: %p\n",
                      (void *)*(volatile UINT64 *)cfg_trace_va);
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
    dump_frame(frame);
    KeLog("Ignoring unexpected interrupt %lu\n", (unsigned long)frame->vector);
}
