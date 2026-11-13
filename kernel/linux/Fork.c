/* Linux process state copied on fork; native process objects stay in Ps. */
#include "Internal.h"
#include <svindows/Ex.h>
#include <svindows/Ob.h>
#include <svindows/Ps.h>
#include <svindows/Rtl.h>
#include <svindows/Hal.h>
#include <svindows/Ke.h>

BOOLEAN LinuxSharedPage(UINT64 address, PVOID opaque)
{
    const LINUX_MEMORY_CONTEXT *memory =
        &((PKPROCESS)opaque)->LinuxContext->Memory;
    for (UINT32 i = 0; i < LINUX_MAP_REGIONS; i++) {
        const LINUX_MAP_REGION *region = &memory->Regions[i];
        if (region->Kind == LINUX_MAP_FRAMEBUFFER && region->Base &&
            address >= region->Base && address - region->Base < region->Size)
            return TRUE;
    }
    return FALSE;
}

NTSTATUS LinuxCloneProcessContext(PKPROCESS parent, PKPROCESS child)
{
    LINUX_PROCESS_CONTEXT *source = parent->LinuxContext;
    LINUX_PROCESS_CONTEXT *target = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*target), 'corL');
    if (!target) return STATUS_NO_MEMORY;
    memset(target, 0, sizeof(*target));
    child->LinuxContext = target;
    target->Memory = source->Memory;
    target->SessionId = source->SessionId;
    target->ProcessGroupId = source->ProcessGroupId;
    target->ControllingPty = source->ControllingPty;
    if (target->ControllingPty)
        LinuxPtyControlInherit(target->ControllingPty);

    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        const LINUX_FD_ENTRY *entry = &source->Files[i];
        if (!entry->Used) continue;
        LINUX_FD_ENTRY *copy = &target->Files[i];
        *copy = *entry;
        copy->Used = FALSE; /* Cleanup sees only fully acquired entries. */
        if (entry->Kind == LINUX_FD_REGULAR ||
            entry->Kind == LINUX_FD_CONSOLE) {
            NTSTATUS status = ObDuplicateHandleBetweenTables(
                parent->HandleTable, entry->Handle, child->HandleTable,
                0, TRUE, FALSE, &copy->Handle);
            if (!NT_SUCCESS(status)) return status;
            copy->Owned = TRUE;
        } else if (entry->Kind == LINUX_FD_DIRECTORY) {
            SIZE_T length = strlen(entry->DirectoryPath) + 1;
            copy->DirectoryPath = ExAllocatePoolWithTag(
                NonPagedPool, length, 'riDL');
            if (!copy->DirectoryPath) return STATUS_NO_MEMORY;
            memcpy(copy->DirectoryPath, entry->DirectoryPath, length);
        } else if (entry->Kind == LINUX_FD_SOCKET) {
            LinuxSocketInherit(entry->Socket);
        } else if (entry->Kind == LINUX_FD_EPOLL) {
            LinuxEpollInherit(entry->Epoll);
        } else if (entry->Kind == LINUX_FD_EVENT) {
            if (entry->EventDevice < LINUX_EVENT_DEVICE_COUNT)
                LinuxEventSetOpen(entry->EventDevice, TRUE);
        } else if (entry->Kind == LINUX_FD_TTY) {
            LinuxVtInherit(entry->Terminal);
        } else if (entry->Kind == LINUX_FD_PTY) {
            LinuxPtyInherit(entry->Pty, entry->PtyMaster);
        } else if (entry->Kind != LINUX_FD_FRAMEBUFFER &&
                   entry->Kind != LINUX_FD_PROC) {
            KeLog("[lx]   fork rejected fd %u kind %u\n", i, entry->Kind);
            return STATUS_NOT_SUPPORTED;
        }
        copy->Used = TRUE;
    }
    child->StandardInput = target->Files[0].Handle;
    child->StandardOutput = target->Files[1].Handle;
    child->StandardError = target->Files[2].Handle;
    if (target->Memory.FramebufferOwner) HalConsoleAcquireFramebuffer();
    return STATUS_SUCCESS;
}

UINT64 LinuxFork(PKTHREAD parent)
{
    PKPROCESS process = parent->Process;
    UINT32 slot = 0;
    while (slot < 32 && process->LinuxContext->Children[slot].ProcessHandle)
        slot++;
    if (slot == 32) return LinuxError(LINUX_EAGAIN);
    HANDLE handle = NULL;
    ULONG pid = 0;
    NTSTATUS status = PsForkLinuxProcess(parent, &handle, &pid);
    if (!NT_SUCCESS(status)) {
        KeLog("[lx]   fork failed for pid %u: status 0x%08x\n",
              process->ProcessId, (unsigned)status);
        return LinuxError(status == STATUS_NO_MEMORY ? LINUX_ENOMEM :
                           status == STATUS_NOT_SUPPORTED ? LINUX_EOPNOTSUPP :
                                                            LINUX_EAGAIN);
    }
    process->LinuxContext->Children[slot].ProcessHandle = handle;
    process->LinuxContext->Children[slot].Pid = pid;
    return pid;
}

UINT64 LinuxWait4(PKPROCESS process, INT64 pid, UINT64 status_pointer,
                  UINT64 options, UINT64 usage)
{
    /* WNOHANG (1) and WUNTRACED (2) are accepted; job control never stops a
     * child here, so WUNTRACED is a no-op. The optional rusage block is
     * zeroed rather than rejected: callers pass one for accounting only. */
    if (options & ~3ULL) return LinuxError(LINUX_EINVAL);
    if (usage && !MmProbeForWrite(usage, 144))
        return LinuxError(LINUX_EFAULT);
    if (status_pointer && !MmProbeForWrite(status_pointer, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    if (pid != -1 && pid != 0 && pid <= 0) return LinuxError(LINUX_ECHILD);
    UINT32 slot = 0;
    while (slot < 32) {
        if (process->LinuxContext->Children[slot].ProcessHandle &&
            (pid == -1 || pid == 0 ||
             process->LinuxContext->Children[slot].Pid == pid))
            break;
        slot++;
    }
    if (slot == 32) return LinuxError(LINUX_ECHILD);
    HANDLE handle = process->LinuxContext->Children[slot].ProcessHandle;
    POBJECT object = NULL;
    NTSTATUS result = ObReferenceObjectByHandle(handle, 0, NULL, &object);
    if (!NT_SUCCESS(result)) return LinuxError(LINUX_ECHILD);
    PKPROCESS child = PsGetProcessFromObject(object);
    if (!child) {
        ObDereferenceObject(object);
        return LinuxError(LINUX_ECHILD);
    }
    if (!child->Exiting && (options & 1u)) {
        ObDereferenceObject(object);
        return 0;
    }
    if (!child->Exiting) result = KeWaitForSingleObject((PDISPATCHER_HEADER)object);
    if (!NT_SUCCESS(result)) {
        ObDereferenceObject(object);
        return LinuxError(LINUX_EIO);
    }
    if (usage)
        memset((void *)(ULONG_PTR)usage, 0, 144);
    if (status_pointer)
        *(UINT32 *)(ULONG_PTR)status_pointer = ((UINT32)child->ExitStatus & 0xffu) << 8;
    ULONG child_pid = child->ProcessId;
    process->LinuxContext->Children[slot].ProcessHandle = NULL;
    process->LinuxContext->Children[slot].Pid = 0;
    ObCloseHandle(handle);
    ObDereferenceObject(object);
    return child_pid;
}
