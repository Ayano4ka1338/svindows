/* Synchronous Linux x86-64 signal delivery and the rt_* signal syscalls. */
#include "Internal.h"
#include <svindows/Rtl.h>

#define LINUX_SA_SIGINFO 4ULL
#define LINUX_SA_ONSTACK 0x08000000ULL
#define LINUX_SA_RESTORER 0x04000000ULL
#define LINUX_SS_ONSTACK 1
#define LINUX_SS_DISABLE 2
#define LINUX_SIG_DFL 0ULL
#define LINUX_SIG_IGN 1ULL

typedef struct {
    UINT32 Signo;
    INT32 Errno;
    INT32 Code;
    UINT32 Pad;
    UINT64 Address;
    UINT8 Rest[104];
} LINUX_SIGINFO;

/* Linux x86-64 ucontext_t/mcontext_t ABI. The gregs order is from
 * sys/ucontext.h and is also what HotSpot's implicit-exception handler edits. */
typedef struct {
    UINT64 Flags, Link;
    UINT64 StackPointer;
    INT32 StackFlags;
    UINT32 StackPad;
    UINT64 StackSize;
    UINT64 Gregs[23];
    UINT64 FpState;
    UINT64 Reserved[8];
    UINT64 SignalMask[16];
} LINUX_UCONTEXT;

typedef struct {
    UINT64 ReturnAddress;
    LINUX_SIGINFO Info;
    LINUX_UCONTEXT Context;
    UINT8 Trampoline[16];
    UINT8 FpPad[8];
    UINT8 FpState[512];
} LINUX_SIGNAL_FRAME;

enum { G_R8, G_R9, G_R10, G_R11, G_R12, G_R13, G_R14, G_R15,
       G_RDI, G_RSI, G_RBP, G_RBX, G_RDX, G_RAX, G_RCX, G_RSP,
       G_RIP, G_EFL, G_CSGSFS, G_ERR, G_TRAPNO, G_OLDMASK, G_CR2 };

UINT64 LinuxSignalAction(PKPROCESS process, const UINT64 args[6])
{
    UINT64 sig = args[0], action = args[1], old = args[2];
    if (!sig || sig > 64 || sig == 9 || sig == 19 || args[3] != 8)
        return LinuxError(LINUX_EINVAL);
    if ((action && !MmProbeForRead(action, 32)) ||
        (old && !MmProbeForWrite(old, 32))) return LinuxError(LINUX_EFAULT);
    typeof(process->LinuxContext->SignalActions[0]) next = {0};
    if (action) memcpy(&next, (void *)(ULONG_PTR)action, sizeof(next));
    if (old) memcpy((void *)(ULONG_PTR)old,
                    &process->LinuxContext->SignalActions[sig], sizeof(next));
    if (action) process->LinuxContext->SignalActions[sig] = next;
    return 0;
}

UINT64 LinuxSignalMask(PKTHREAD thread, const UINT64 args[6])
{
    UINT64 set = args[1], old = args[2], how = args[0], value;
    if (args[3] != 8) return LinuxError(LINUX_EINVAL);
    if ((set && !MmProbeForRead(set, 8)) ||
        (old && !MmProbeForWrite(old, 8))) return LinuxError(LINUX_EFAULT);
    value = thread->LinuxThread->SignalMask;
    if (old) *(UINT64 *)(ULONG_PTR)old = value;
    if (set) {
        UINT64 incoming = *(UINT64 *)(ULONG_PTR)set;
        if (how == 0) value |= incoming;
        else if (how == 1) value &= ~incoming;
        else if (how == 2) value = incoming;
        else return LinuxError(LINUX_EINVAL);
        value &= ~((1ULL << 8) | (1ULL << 18)); /* SIGKILL, SIGSTOP */
        thread->LinuxThread->SignalMask = value;
    }
    return 0;
}

UINT64 LinuxSignalAltStack(PKTHREAD thread, const UINT64 args[6])
{
    UINT64 next = args[0], old = args[1];
    if (next && !MmProbeForRead(next, 24)) return LinuxError(LINUX_EFAULT);
    if (old && !MmProbeForWrite(old, 24)) return LinuxError(LINUX_EFAULT);
    if (old) {
        UINT8 *p = (UINT8 *)(ULONG_PTR)old;
        *(UINT64 *)p = thread->LinuxThread->SignalAltStackBase;
        *(INT32 *)(p + 8) = thread->LinuxThread->SignalOnAltStack ?
                             LINUX_SS_ONSTACK :
                             (thread->LinuxThread->SignalAltStackSize ? 0 :
                              LINUX_SS_DISABLE);
        *(UINT32 *)(p + 12) = 0;
        *(UINT64 *)(p + 16) = thread->LinuxThread->SignalAltStackSize;
    }
    if (next) {
        UINT8 *p = (UINT8 *)(ULONG_PTR)next;
        UINT64 base = *(UINT64 *)p;
        INT32 flags = *(INT32 *)(p + 8);
        UINT64 size = *(UINT64 *)(p + 16);
        if (thread->LinuxThread->SignalOnAltStack) return LinuxError(LINUX_EPERM);
        if (flags & ~LINUX_SS_DISABLE) return LinuxError(LINUX_EINVAL);
        if (flags & LINUX_SS_DISABLE) {
            thread->LinuxThread->SignalAltStackBase = 0;
            thread->LinuxThread->SignalAltStackSize = 0;
        } else {
            if (size < 2048 || base + size < base ||
                !MmProbeForWrite(base, (SIZE_T)size))
                return LinuxError(LINUX_ENOMEM);
            thread->LinuxThread->SignalAltStackBase = base;
            thread->LinuxThread->SignalAltStackSize = size;
        }
    }
    return 0;
}

static void linux_frame_to_gregs(PKTRAP_FRAME f, UINT64 g[23], UINT64 addr)
{
    memset(g, 0, 23 * sizeof(UINT64));
    g[G_R8]=f->r8; g[G_R9]=f->r9; g[G_R10]=f->r10; g[G_R11]=f->r11;
    g[G_R12]=f->r12; g[G_R13]=f->r13; g[G_R14]=f->r14; g[G_R15]=f->r15;
    g[G_RDI]=f->rdi; g[G_RSI]=f->rsi; g[G_RBP]=f->rbp; g[G_RBX]=f->rbx;
    g[G_RDX]=f->rdx; g[G_RAX]=f->rax; g[G_RCX]=f->rcx; g[G_RSP]=f->rsp;
    g[G_RIP]=f->rip; g[G_EFL]=f->rflags; g[G_CSGSFS]=f->cs | ((UINT64)f->ss << 48);
    g[G_ERR]=f->error_code; g[G_TRAPNO]=f->vector; g[G_CR2]=addr;
}

BOOLEAN LinuxDeliverSyncSignal(PKTRAP_FRAME f, UINT32 sig, INT32 code,
                               UINT64 address)
{
    PKTHREAD t = KeGetCurrentThread();
    PKPROCESS p = t ? t->Process : NULL;
    if (!t || !t->LinuxThread || !p || !p->LinuxContext || !sig || sig > 64)
        return FALSE;
    UINT64 bit = 1ULL << (sig - 1);
    if (t->LinuxThread->SignalDepth >= 8) return FALSE;
    if (t->LinuxThread->SignalMask & bit) return FALSE;
    typeof(p->LinuxContext->SignalActions[0]) *a =
        &p->LinuxContext->SignalActions[sig];
    if (a->Handler == LINUX_SIG_DFL || a->Handler == LINUX_SIG_IGN ||
        !MmIsUserAddress(a->Handler)) return FALSE;

    LINUX_SIGNAL_FRAME sf;
    memset(&sf, 0, sizeof(sf));
    sf.Info.Signo = sig; sf.Info.Code = code; sf.Info.Address = address;
    sf.Context.StackPointer = t->LinuxThread->SignalAltStackBase;
    sf.Context.StackSize = t->LinuxThread->SignalAltStackSize;
    sf.Context.StackFlags = t->LinuxThread->SignalOnAltStack ?
                            LINUX_SS_ONSTACK :
                            (sf.Context.StackSize ? 0 : LINUX_SS_DISABLE);
    linux_frame_to_gregs(f, sf.Context.Gregs, address);
    sf.Context.SignalMask[0] = t->LinuxThread->SignalMask;

    BOOLEAN was_on_altstack = t->LinuxThread->SignalOnAltStack;
    UINT64 stack_top = f->rsp;
    if (!was_on_altstack && (a->Flags & LINUX_SA_ONSTACK) &&
        t->LinuxThread->SignalAltStackSize) {
        stack_top = t->LinuxThread->SignalAltStackBase +
                    t->LinuxThread->SignalAltStackSize;
        t->LinuxThread->SignalOnAltStack = TRUE;
    }
    if (stack_top < sizeof(sf) + 16) {
        t->LinuxThread->SignalOnAltStack = was_on_altstack;
        return FALSE;
    }
    UINT64 base = ((stack_top - sizeof(sf) - 8) & ~15ULL) + 8;
    UINT64 restorer = a->Restorer;
    if (!(a->Flags & LINUX_SA_RESTORER)) {
        restorer = base + __builtin_offsetof(LINUX_SIGNAL_FRAME, Trampoline);
        const UINT8 codebytes[] = {0xb8,15,0,0,0,0x0f,0x05,0x0f,0x0b};
        memcpy(sf.Trampoline, codebytes, sizeof(codebytes));
    }
    sf.ReturnAddress = restorer;
    sf.Context.FpState = base + __builtin_offsetof(LINUX_SIGNAL_FRAME, FpState);
    if (!MmProbeForWrite(base, sizeof(sf))) return FALSE;
    memcpy((void *)(ULONG_PTR)base, &sf, sizeof(sf));
    __asm__ volatile("fxsave64 (%0)" :: "r"(base +
        __builtin_offsetof(LINUX_SIGNAL_FRAME, FpState)) : "memory");

    t->LinuxThread->SignalFrameBase[t->LinuxThread->SignalDepth] = base;
    t->LinuxThread->SignalPreviousAltStack[t->LinuxThread->SignalDepth] =
        was_on_altstack;
    t->LinuxThread->SignalDepth++;
    t->LinuxThread->SignalMask |= a->Mask | bit;
    f->rip = a->Handler;
    f->rsp = base;
    f->rdi = sig;
    if (a->Flags & LINUX_SA_SIGINFO) {
        f->rsi = base + __builtin_offsetof(LINUX_SIGNAL_FRAME, Info);
        f->rdx = base + __builtin_offsetof(LINUX_SIGNAL_FRAME, Context);
    }
    f->rflags |= 0x202;
    return TRUE;
}

UINT64 LinuxSignalReturn(PKTHREAD t)
{
    if (!t || !t->LinuxThread || !t->LinuxThread->SignalDepth)
        return LinuxError(LINUX_EFAULT);
    UINT64 base = t->LinuxThread->SignalFrameBase[t->LinuxThread->SignalDepth - 1];
    UINT64 uc = base + __builtin_offsetof(LINUX_SIGNAL_FRAME, Context);
    if (!MmProbeForRead(uc, sizeof(LINUX_UCONTEXT)))
        return LinuxError(LINUX_EFAULT);
    LINUX_UCONTEXT *c = (LINUX_UCONTEXT *)(ULONG_PTR)uc;
    if (c->FpState < base || c->FpState + 512 < c->FpState ||
        c->FpState + 512 > base + sizeof(LINUX_SIGNAL_FRAME) ||
        (c->FpState & 15)) return LinuxError(LINUX_EFAULT);
    memcpy(t->LinuxThread->SignalReturn, c->Gregs, sizeof(c->Gregs));
    __asm__ volatile("fxrstor64 (%0)" :: "r"(c->FpState) : "memory");
    t->LinuxThread->SignalMask = c->SignalMask[0] &
        ~((1ULL << 8) | (1ULL << 18));
    t->LinuxThread->SignalOnAltStack =
        t->LinuxThread->SignalPreviousAltStack[t->LinuxThread->SignalDepth - 1];
    t->LinuxThread->SignalDepth--;
    t->LinuxThread->SignalReturnPending = 1;
    return c->Gregs[G_RAX];
}

UINT64 *LinuxConsumeSignalReturn(PKTHREAD t)
{
    if (!t || !t->LinuxThread || !t->LinuxThread->SignalReturnPending)
        return NULL;
    t->LinuxThread->SignalReturnPending = 0;
    return t->LinuxThread->SignalReturn;
}
