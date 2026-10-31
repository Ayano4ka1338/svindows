/* Linux thread ABI over the kernel scheduler's shared-address-space threads. */
#include "Internal.h"
#include <svindows/Ex.h>
#include <svindows/Rtl.h>
#include <svindows/Ob.h>

#define LINUX_ARCH_SET_GS 0x1001u
#define LINUX_ARCH_SET_FS 0x1002u
#define LINUX_ARCH_GET_FS 0x1003u
#define LINUX_ARCH_GET_GS 0x1004u
#define LINUX_USER_ADDRESS_LIMIT 0x0000800000000000ULL

#define LINUX_CLONE_VM 0x00000100u
#define LINUX_CLONE_FS 0x00000200u
#define LINUX_CLONE_FILES 0x00000400u
#define LINUX_CLONE_SIGHAND 0x00000800u
#define LINUX_CLONE_THREAD 0x00010000u
#define LINUX_CLONE_SYSVSEM 0x00040000u
#define LINUX_CLONE_SETTLS 0x00080000u
#define LINUX_CLONE_PARENT_SETTID 0x00100000u
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000u
#define LINUX_CLONE_CHILD_SETTID 0x01000000u
#define LINUX_CLONE_DETACHED 0x00400000u /* Historical bit; Linux ignores it. */
#define LINUX_CLONE_REQUIRED (LINUX_CLONE_VM | LINUX_CLONE_FS | \
                              LINUX_CLONE_FILES | LINUX_CLONE_SIGHAND | \
                              LINUX_CLONE_THREAD)
#define LINUX_CLONE_ALLOWED (LINUX_CLONE_REQUIRED | LINUX_CLONE_SYSVSEM | \
                             LINUX_CLONE_SETTLS | LINUX_CLONE_PARENT_SETTID | \
                             LINUX_CLONE_CHILD_CLEARTID | \
                             LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_DETACHED)

_Static_assert(__builtin_offsetof(KTHREAD, LinuxThread) == 0x11a8,
               "syscall assembly Linux thread offset");
_Static_assert(__builtin_offsetof(LINUX_THREAD_CONTEXT, Registers) == 0,
               "clone capture register offset");
_Static_assert(sizeof(LINUX_CLONE_REGISTERS) == 96,
               "clone register layout");
_Static_assert(__builtin_offsetof(LINUX_THREAD_CONTEXT, ExecStart) == 113,
               "exec entry flag offset");
_Static_assert(__builtin_offsetof(LINUX_THREAD_CONTEXT, WindowsExec) == 114,
               "windows exec flag offset");
_Static_assert(__builtin_offsetof(LINUX_THREAD_CONTEXT, ExecArgument) == 120,
               "windows exec argument offset");

UINT64 LinuxArchPrctl(PKTHREAD thread, UINT64 command, UINT64 value)
{
    switch (command) {
    case LINUX_ARCH_SET_FS:
    case LINUX_ARCH_SET_GS:
        if (value >= LINUX_USER_ADDRESS_LIMIT)
            return LinuxError(LINUX_EINVAL);
        if (command == LINUX_ARCH_SET_FS) {
            thread->UserFsBase = value;
            KeSetUserFsBase(value);
        } else {
            thread->UserGsBase = value;
            KeSetUserGsBase(value);
        }
        return 0;
    case LINUX_ARCH_GET_FS:
    case LINUX_ARCH_GET_GS:
        if (!MmProbeForWrite(value, sizeof(UINT64)))
            return LinuxError(LINUX_EFAULT);
        *(UINT64 *)(ULONG_PTR)value = command == LINUX_ARCH_GET_FS ?
                                      thread->UserFsBase : thread->UserGsBase;
        return 0;
    default:
        return LinuxError(LINUX_EINVAL);
    }
}

UINT64 LinuxSetTidAddress(PKTHREAD thread, UINT64 address)
{
    if (address >= LINUX_USER_ADDRESS_LIMIT)
        return LinuxError(LINUX_EFAULT);
    thread->LinuxThread->ClearChildTid = address;
    return thread->LinuxThread->Tid;
}

void LinuxThreadBeginWindowsExec(PKTHREAD thread, UINT64 argument)
{
    thread->LinuxThread->WindowsExec = TRUE;
    thread->LinuxThread->ExecStart = TRUE;
    thread->LinuxThread->ExecArgument = argument;
}

UINT64 LinuxClone(PKTHREAD parent, const UINT64 args[6])
{
    UINT64 flags = args[0], stack = args[1];
    UINT64 parent_tid = args[2], child_tid = args[3], tls = args[4];
    if ((flags & LINUX_CLONE_REQUIRED) != LINUX_CLONE_REQUIRED ||
        (flags & ~(UINT64)LINUX_CLONE_ALLOWED) ||
        !stack || stack >= LINUX_USER_ADDRESS_LIMIT ||
        !MmProbeForWrite(stack - 8, 8) ||
        ((flags & LINUX_CLONE_SETTLS) && tls >= LINUX_USER_ADDRESS_LIMIT))
        return LinuxError(LINUX_EINVAL);
    if ((flags & LINUX_CLONE_PARENT_SETTID) &&
        !MmProbeForWrite(parent_tid, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    if ((flags & (LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_CHILD_CLEARTID)) &&
        !MmProbeForWrite(child_tid, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);

    LINUX_THREAD_CONTEXT *context = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*context), 'rhtL');
    if (!context) return LinuxError(LINUX_ENOMEM);
    memset(context, 0, sizeof(*context));
    context->Registers = parent->LinuxThread->Registers;
    context->CloneStart = TRUE;
    KeInitializeEvent(&context->FutexEvent, FALSE, FALSE);
    context->ClearChildTid = (flags & LINUX_CLONE_CHILD_CLEARTID) ?
                             child_tid : 0;

    PKTHREAD child = KeCreateUserThreadForProcess(parent->Process,
                    parent->Name, parent->UserRip, stack, 0, 0,
                    parent->Priority);
    if (!child) {
        ExFreePool(context);
        return LinuxError(LINUX_ENOMEM);
    }
    context->Tid = child->ThreadId;
    child->LinuxThread = context;
    child->UserRflags = (parent->UserRflags & 0x240fd7ULL) | 0x202;
    child->UserFsBase = (flags & LINUX_CLONE_SETTLS) ? tls :
                        parent->UserFsBase;
    child->UserGsBase = parent->UserGsBase;
    if (flags & LINUX_CLONE_PARENT_SETTID)
        *(UINT32 *)(ULONG_PTR)parent_tid = (UINT32)context->Tid;
    if (flags & LINUX_CLONE_CHILD_SETTID)
        *(UINT32 *)(ULONG_PTR)child_tid = (UINT32)context->Tid;
    return context->Tid;
}

NTSTATUS LinuxCreateForkThread(PKTHREAD parent, PKPROCESS process,
                               PDISPATCHER_HEADER termination,
                               PKTHREAD *out)
{
    LINUX_THREAD_CONTEXT *context = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*context), 'rhtL');
    if (!context) return STATUS_NO_MEMORY;
    memset(context, 0, sizeof(*context));
    context->Registers = parent->LinuxThread->Registers;
    context->CloneStart = TRUE;
    context->Tid = process->ProcessId;
    KeInitializeEvent(&context->FutexEvent, FALSE, FALSE);
    UINT64 irq = KiIrqSave();
    PKTHREAD child = KeCreateUserThreadForProcess(
        process, process->Name, parent->UserRip, parent->UserRsp,
        0, 0, process->BasePriority);
    if (child) {
        child->LinuxThread = context;
        child->UserFsBase = parent->UserFsBase;
        child->UserGsBase = parent->UserGsBase;
        child->UserRflags = (parent->UserRflags & 0x240fd7ULL) | 0x202;
        child->ProcessMainThread = TRUE;
        child->TerminationObject = termination;
        ObReferenceObject((POBJECT)termination);
    }
    KiIrqRestore(irq);
    if (!child) {
        ExFreePool(context);
        return STATUS_NO_MEMORY;
    }
    *out = child;
    return STATUS_SUCCESS;
}

void LinuxThreadExited(PKTHREAD thread)
{
    if (!thread->LinuxThread) return;
    UINT64 address = thread->LinuxThread->ClearChildTid;
    if (address && MmProbeForWrite(address, sizeof(UINT32)))
        *(UINT32 *)(ULONG_PTR)address = 0;
    if (address) LinuxFutexWake(thread->Process, address, 1);
}

NORETURN void LinuxExitGroup(PKTHREAD thread, NTSTATUS exit_status)
{
    PKPROCESS process = thread->Process;
    for (PLIST_ENTRY entry = process->ThreadListHead.Flink;
         entry != &process->ThreadListHead; entry = entry->Flink) {
        PKTHREAD member = CONTAINING_RECORD(entry, KTHREAD, ProcessEntry);
        if (member->ProcessMainThread) {
            KeTerminateThreadByPointer(member, exit_status);
            break;
        }
    }
    thread->ExitStatus = exit_status;
    KeTerminateThread();
}

void LinuxDestroyThread(PKTHREAD thread)
{
    if (!thread->LinuxThread) return;
    ExFreePool(thread->LinuxThread);
    thread->LinuxThread = NULL;
}

BOOLEAN LinuxIsCloneThread(PKTHREAD thread)
{
    return thread->LinuxThread && thread->LinuxThread->CloneStart;
}

const void *LinuxCloneRegisters(PKTHREAD thread)
{
    return &thread->LinuxThread->Registers;
}
