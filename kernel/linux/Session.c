/* Session and process-group IDs for controlling pseudo terminals. */
#include "Internal.h"
#include <svindows/Ob.h>
#include <svindows/Ps.h>

static PKPROCESS linux_session_target(PKPROCESS process, UINT64 pid,
                                      POBJECT *reference)
{
    *reference = NULL;
    if (!pid || pid == process->ProcessId) return process;
    for (UINT32 i = 0; i < 32; i++) {
        if (process->LinuxContext->Children[i].Pid != pid ||
            !process->LinuxContext->Children[i].ProcessHandle)
            continue;
        NTSTATUS status = ObReferenceObjectByHandle(
            process->LinuxContext->Children[i].ProcessHandle, 0, NULL,
            reference);
        if (!NT_SUCCESS(status)) return NULL;
        PKPROCESS child = PsGetProcessFromObject(*reference);
        if (child && child->LinuxContext) return child;
        ObDereferenceObject(*reference);
        *reference = NULL;
        return NULL;
    }
    return NULL;
}

UINT64 LinuxSetSid(PKPROCESS process)
{
    LINUX_PROCESS_CONTEXT *context = process->LinuxContext;
    if (context->ProcessGroupId == process->ProcessId)
        return LinuxError(LINUX_EPERM);
    context->SessionId = process->ProcessId;
    context->ProcessGroupId = process->ProcessId;
    if (context->ControllingPty) {
        LinuxPtyControlClose(context->ControllingPty);
        context->ControllingPty = NULL;
    }
    return process->ProcessId;
}

UINT64 LinuxSetPgId(PKPROCESS process, UINT64 pid, UINT64 group)
{
    POBJECT reference;
    PKPROCESS target = linux_session_target(process, pid, &reference);
    if (!target) return LinuxError(LINUX_EPERM);
    if (!group) group = target->ProcessId;
    if (group > 0x7fffffff || target->LinuxContext->SessionId !=
                              process->LinuxContext->SessionId) {
        if (reference) ObDereferenceObject(reference);
        return LinuxError(LINUX_EPERM);
    }
    target->LinuxContext->ProcessGroupId = (UINT32)group;
    if (reference) ObDereferenceObject(reference);
    return 0;
}

UINT64 LinuxGetPgId(PKPROCESS process, UINT64 pid)
{
    POBJECT reference;
    PKPROCESS target = linux_session_target(process, pid, &reference);
    if (!target) return LinuxError(LINUX_EPERM);
    UINT64 result = target->LinuxContext->ProcessGroupId;
    if (reference) ObDereferenceObject(reference);
    return result;
}

UINT64 LinuxGetSid(PKPROCESS process, UINT64 pid)
{
    POBJECT reference;
    PKPROCESS target = linux_session_target(process, pid, &reference);
    if (!target) return LinuxError(LINUX_EPERM);
    UINT64 result = target->LinuxContext->SessionId;
    if (reference) ObDereferenceObject(reference);
    return result;
}
