/* Process-wide advisory flock locks for Linux regular-file descriptors. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Rtl.h>

#define LINUX_LOCK_SH 1u
#define LINUX_LOCK_EX 2u
#define LINUX_LOCK_NB 4u
#define LINUX_LOCK_UN 8u
#define LINUX_ENOLCK 37
#define LINUX_FLOCK_SLOTS 64u

typedef struct _LINUX_FLOCK_ENTRY {
    UINT64 FileId;
    UINT32 ProcessId;
    BOOLEAN Used;
} LINUX_FLOCK_ENTRY;

static LINUX_FLOCK_ENTRY g_flock_entries[LINUX_FLOCK_SLOTS];
static KSPIN_LOCK g_flock_lock;

UINT64 LinuxFlock(PKPROCESS process, UINT64 fd, UINT64 operation)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    LINUX_FD_ENTRY *descriptor = &process->LinuxContext->Files[fd];
    if (descriptor->Kind != LINUX_FD_REGULAR || !descriptor->Handle)
        return LinuxError(LINUX_EBADF);

    UINT32 command = (UINT32)operation;
    UINT32 lock_type = command & (LINUX_LOCK_SH | LINUX_LOCK_EX | LINUX_LOCK_UN);
    if ((command & ~(LINUX_LOCK_SH | LINUX_LOCK_EX | LINUX_LOCK_NB |
                     LINUX_LOCK_UN)) ||
        (lock_type != LINUX_LOCK_SH && lock_type != LINUX_LOCK_EX &&
         lock_type != LINUX_LOCK_UN))
        return LinuxError(LINUX_EINVAL);

    IO_KERNEL_FILE_STAT stat;
    NTSTATUS status = IoQueryFileStatKernel(descriptor->Handle, &stat);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EBADF);
    UINT64 file_id = stat.FileId ? stat.FileId : 1;

    if (lock_type == LINUX_LOCK_UN) {
        UINT64 irq = KiAcquireRawSpinLock(&g_flock_lock);
        for (UINT32 i = 0; i < LINUX_FLOCK_SLOTS; i++) {
            if (g_flock_entries[i].Used &&
                g_flock_entries[i].FileId == file_id &&
                g_flock_entries[i].ProcessId == process->ProcessId) {
                memset(&g_flock_entries[i], 0, sizeof(g_flock_entries[i]));
                break;
            }
        }
        KiReleaseRawSpinLock(&g_flock_lock, irq);
        return 0;
    }

    for (;;) {
        UINT32 free_slot = LINUX_FLOCK_SLOTS;
        BOOLEAN busy = FALSE;
        UINT64 irq = KiAcquireRawSpinLock(&g_flock_lock);
        for (UINT32 i = 0; i < LINUX_FLOCK_SLOTS; i++) {
            LINUX_FLOCK_ENTRY *entry = &g_flock_entries[i];
            if (!entry->Used) {
                if (free_slot == LINUX_FLOCK_SLOTS) free_slot = i;
                continue;
            }
            if (entry->FileId != file_id) continue;
            if (entry->ProcessId != process->ProcessId) {
                busy = TRUE;
                break;
            }
            /* Locks from multiple descriptors in the same process are
             * treated as one owner, which matches apk's database lock use. */
            KiReleaseRawSpinLock(&g_flock_lock, irq);
            return 0;
        }
        if (!busy && free_slot < LINUX_FLOCK_SLOTS) {
            g_flock_entries[free_slot].FileId = file_id;
            g_flock_entries[free_slot].ProcessId = process->ProcessId;
            g_flock_entries[free_slot].Used = TRUE;
            KiReleaseRawSpinLock(&g_flock_lock, irq);
            return 0;
        }
        KiReleaseRawSpinLock(&g_flock_lock, irq);
        if (!busy) return LinuxError(LINUX_ENOLCK);
        if (command & LINUX_LOCK_NB) return LinuxError(LINUX_EAGAIN);
        KeYield();
    }
}

void LinuxFlockReleaseProcess(PKPROCESS process)
{
    if (!process) return;
    UINT64 irq = KiAcquireRawSpinLock(&g_flock_lock);
    for (UINT32 i = 0; i < LINUX_FLOCK_SLOTS; i++) {
        if (g_flock_entries[i].Used &&
            g_flock_entries[i].ProcessId == process->ProcessId)
            memset(&g_flock_entries[i], 0, sizeof(g_flock_entries[i]));
    }
    KiReleaseRawSpinLock(&g_flock_lock, irq);
}
