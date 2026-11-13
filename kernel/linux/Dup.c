/* Linux descriptor duplication over the native handle table. */
#include "Internal.h"
#include <svindows/Ob.h>

#define LINUX_O_CLOEXEC 0x80000u

UINT64 LinuxDup(PKPROCESS process, UINT64 source, UINT64 target,
                UINT64 flags, BOOLEAN exact, BOOLEAN dup3)
{
    if (source >= LINUX_MAX_FDS ||
        !process->LinuxContext->Files[source].Used)
        return LinuxError(LINUX_EBADF);
    if (flags & ~LINUX_O_CLOEXEC)
        return LinuxError(LINUX_EINVAL);
    if (dup3 && source == target)
        return LinuxError(LINUX_EINVAL);
    if (exact && target >= LINUX_MAX_FDS)
        return LinuxError(LINUX_EBADF);
    if (exact && source == target) return target;

    LINUX_FD_ENTRY *original = &process->LinuxContext->Files[source];
    /* Other descriptor kinds need their own shared lifetime and offset rules. */
    if (original->Kind != LINUX_FD_REGULAR &&
        original->Kind != LINUX_FD_CONSOLE &&
        original->Kind != LINUX_FD_TTY &&
        original->Kind != LINUX_FD_PTY &&
        original->Kind != LINUX_FD_SOCKET &&
        original->Kind != LINUX_FD_EPOLL)
        return LinuxError(LINUX_EOPNOTSUPP);

    if (!exact) {
        if (target >= LINUX_MAX_FDS) return LinuxError(LINUX_EINVAL);
        for (; target < LINUX_MAX_FDS; target++)
            if (!process->LinuxContext->Files[target].Used) break;
        if (target == LINUX_MAX_FDS) return LinuxError(LINUX_EMFILE);
    }

    HANDLE handle = NULL;
    if (original->Kind == LINUX_FD_REGULAR ||
        original->Kind == LINUX_FD_CONSOLE) {
        NTSTATUS status = ObDuplicateHandleToTable(original->Handle,
                                                   process->HandleTable, &handle);
        if (!NT_SUCCESS(status))
            return LinuxError(status == STATUS_NO_MEMORY ? LINUX_ENOMEM :
                                                           LINUX_EBADF);
    } else if (original->Kind == LINUX_FD_TTY) {
        LinuxVtInherit(original->Terminal);
    } else if (original->Kind == LINUX_FD_PTY) {
        LinuxPtyInherit(original->Pty, original->PtyMaster);
    } else if (original->Kind == LINUX_FD_SOCKET) {
        LinuxSocketInherit(original->Socket);
    } else {
        LinuxEpollInherit(original->Epoll);
    }
    LINUX_FD_ENTRY copy = *original;
    copy.Handle = handle;
    copy.Owned = copy.Kind == LINUX_FD_REGULAR ||
                 copy.Kind == LINUX_FD_CONSOLE;
    copy.CloseOnExec = !!(flags & LINUX_O_CLOEXEC);
    if (exact && process->LinuxContext->Files[target].Used)
        LinuxClose(process, target);
    process->LinuxContext->Files[target] = copy;
    return target;
}
