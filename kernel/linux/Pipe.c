/* Linux pipe descriptors backed by the native kernel pipe object. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>

#define LINUX_O_NONBLOCK 0x800u
#define LINUX_O_CLOEXEC 0x80000u

UINT64 LinuxPipe(PKPROCESS process, UINT64 pair, UINT64 flags)
{
    if (flags & ~(UINT64)(LINUX_O_NONBLOCK | LINUX_O_CLOEXEC))
        return LinuxError(LINUX_EINVAL);
    if (flags & LINUX_O_NONBLOCK)
        return LinuxError(LINUX_EOPNOTSUPP);
    if (!MmProbeForWrite(pair, 2 * sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);

    HANDLE reader = NULL, writer = NULL;
    NTSTATUS status = IoCreatePipeKernel(&reader, &writer);
    if (!NT_SUCCESS(status))
        return LinuxError(status == STATUS_NO_MEMORY ? LINUX_ENOMEM :
                                                  LINUX_EIO);

    UINT32 slots[2] = {LINUX_MAX_FDS, LINUX_MAX_FDS};
    UINT64 irq = KiIrqSave();
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        if (process->LinuxContext->Files[i].Used) continue;
        if (slots[0] == LINUX_MAX_FDS) slots[0] = i;
        else { slots[1] = i; break; }
    }
    if (slots[1] != LINUX_MAX_FDS) {
        HANDLE handles[2] = {reader, writer};
        for (UINT32 i = 0; i < 2; i++) {
            LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slots[i]];
            memset(entry, 0, sizeof(*entry));
            entry->Handle = handles[i];
            entry->Mode = (UINT8)i;
            entry->Kind = LINUX_FD_REGULAR;
            entry->Owned = TRUE;
            entry->Used = TRUE;
            entry->CloseOnExec = !!(flags & LINUX_O_CLOEXEC);
        }
    }
    KiIrqRestore(irq);
    if (slots[1] == LINUX_MAX_FDS) {
        ObCloseHandle(reader);
        ObCloseHandle(writer);
        return LinuxError(LINUX_EMFILE);
    }
    memcpy((void *)(ULONG_PTR)pair, slots, sizeof(slots));
    return 0;
}
