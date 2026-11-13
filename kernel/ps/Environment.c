/* Immutable process environments: replacement and inheritance retain complete
 * blocks; a failed exec restores the referenced previous block. */
#include <svindows/Ps.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>
#include <nt/Ntstatus.h>

typedef struct _PS_ENVIRONMENT_STORAGE {
    volatile LONG References;
    SIZE_T Bytes;
    char Data[];
} PS_ENVIRONMENT_STORAGE;
static KSPIN_LOCK g_environment_lock;

static void PspReleaseEnvironment(PS_ENVIRONMENT_STORAGE *storage)
{
    if (storage && !InterlockedDecrement(&storage->References)) ExFreePool(storage);
}

const char *PsProcessEnvironmentData(PKPROCESS process, SIZE_T *bytes)
{
    PS_ENVIRONMENT_STORAGE *storage = process ? process->EnvironmentStorage : NULL;
    if (storage) { if (bytes) *bytes = storage->Bytes; return storage->Data; }
    if (!process) { if (bytes) *bytes = 2; return "\0"; }
    SIZE_T count = 0;
    while (count + 1 < sizeof(process->Environment)) {
        if (!process->Environment[count] && !process->Environment[count + 1]) { count += 2; break; }
        count++;
    }
    if (bytes) *bytes = count;
    return process->Environment;
}

NTSTATUS PsSetProcessEnvironmentBlock(PKPROCESS process, const char *block, SIZE_T bytes)
{
    if (!process || !block || bytes < 2 || block[bytes - 1] || block[bytes - 2] ||
        bytes > ~(SIZE_T)0 - sizeof(PS_ENVIRONMENT_STORAGE)) return STATUS_INVALID_PARAMETER;
    PS_ENVIRONMENT_STORAGE *storage = ExAllocatePoolWithTag(NonPagedPool,
                        sizeof(*storage) + bytes, 'vnEP');
    if (!storage) return STATUS_NO_MEMORY;
    storage->References = 1; storage->Bytes = bytes;
    memcpy(storage->Data, block, bytes);
    UINT64 flags = KiAcquireRawSpinLock(&g_environment_lock);
    PS_ENVIRONMENT_STORAGE *previous = process->EnvironmentStorage;
    process->EnvironmentStorage = storage;
    memset(process->Environment, 0, sizeof(process->Environment));
    if (bytes <= sizeof(process->Environment)) memcpy(process->Environment, block, bytes);
    KiReleaseRawSpinLock(&g_environment_lock, flags);
    PspReleaseEnvironment(previous); return STATUS_SUCCESS;
}

NTSTATUS PsSetProcessEnvironment(PKPROCESS process, const char *const *entries, UINT32 count)
{
    if (!process || (count && !entries)) return STATUS_INVALID_PARAMETER;
    SIZE_T bytes = count ? 1 : 2;
    for (UINT32 i = 0; i < count; i++) {
        if (!entries[i]) return STATUS_INVALID_PARAMETER;
        SIZE_T length = strlen(entries[i]) + 1;
        if (length > ~(SIZE_T)0 - bytes) return STATUS_NO_MEMORY;
        bytes += length;
    }
    char *block = ExAllocatePoolWithTag(NonPagedPool, bytes, 'vnEP');
    if (!block) return STATUS_NO_MEMORY;
    SIZE_T used = 0;
    for (UINT32 i = 0; i < count; i++) {
        SIZE_T length = strlen(entries[i]) + 1;
        memcpy(block + used, entries[i], length); used += length;
    }
    block[used++] = 0; if (!count) block[used] = 0;
    NTSTATUS status = PsSetProcessEnvironmentBlock(process, block, bytes);
    ExFreePool(block); return status;
}

void PsCaptureProcessEnvironment(PKPROCESS process, PS_ENVIRONMENT_SNAPSHOT *snapshot)
{
    UINT64 flags = KiAcquireRawSpinLock(&g_environment_lock);
    snapshot->Storage = process->EnvironmentStorage;
    if (snapshot->Storage) InterlockedIncrement(&((PS_ENVIRONMENT_STORAGE *)snapshot->Storage)->References);
    memcpy(snapshot->Inline, process->Environment, sizeof(snapshot->Inline));
    KiReleaseRawSpinLock(&g_environment_lock, flags);
}

void PsRestoreProcessEnvironment(PKPROCESS process, PS_ENVIRONMENT_SNAPSHOT *snapshot)
{
    UINT64 flags = KiAcquireRawSpinLock(&g_environment_lock);
    PS_ENVIRONMENT_STORAGE *previous = process->EnvironmentStorage;
    process->EnvironmentStorage = snapshot->Storage; snapshot->Storage = NULL;
    memcpy(process->Environment, snapshot->Inline, sizeof(process->Environment));
    KiReleaseRawSpinLock(&g_environment_lock, flags); PspReleaseEnvironment(previous);
}
void PsReleaseEnvironmentSnapshot(PS_ENVIRONMENT_SNAPSHOT *snapshot)
{ PspReleaseEnvironment(snapshot->Storage); snapshot->Storage = NULL; }
void PsCloneProcessEnvironment(PKPROCESS target, PKPROCESS source)
{
    PS_ENVIRONMENT_SNAPSHOT snapshot;
    PsCaptureProcessEnvironment(source, &snapshot);
    PsRestoreProcessEnvironment(target, &snapshot);
}
void PsCleanupProcessEnvironment(PKPROCESS process)
{
    PS_ENVIRONMENT_STORAGE *storage = process->EnvironmentStorage;
    process->EnvironmentStorage = NULL; PspReleaseEnvironment(storage);
}

NTSTATUS PsEnvironmentToWide(const char *block, SIZE_T bytes,
    UINT16 *output, SIZE_T capacity, SIZE_T *characters)
{
    if (!block || !bytes || !characters) return STATUS_INVALID_PARAMETER;
    SIZE_T used = 0;
    for (SIZE_T i = 0; i < bytes; ) {
        UINT8 first = (UINT8)block[i++]; UINT32 cp = first;
        UINT32 continuation = 0, minimum = 0;
        if (first >= 0xC2 && first <= 0xDF) { cp = first & 31; continuation = 1; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { cp = first & 15; continuation = 2; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { cp = first & 7; continuation = 3; minimum = 0x10000; }
        else if (first >= 0x80) return STATUS_INVALID_PARAMETER;
        if (continuation > bytes - i) return STATUS_INVALID_PARAMETER;
        for (UINT32 j = 0; j < continuation; j++) {
            UINT8 next = (UINT8)block[i++];
            if ((next & 0xC0) != 0x80) return STATUS_INVALID_PARAMETER;
            cp = (cp << 6) | (next & 63);
        }
        /* WTF-8 surrogate code points preserve otherwise-unpaired UTF-16
         * environment units; Windows treats environment values as UTF-16. */
        if (cp < minimum || cp > 0x10FFFF) return STATUS_INVALID_PARAMETER;
        SIZE_T units = cp >= 0x10000 ? 2 : 1;
        if (output && units > capacity - (used < capacity ? used : capacity)) return STATUS_BUFFER_TOO_SMALL;
        if (output) {
            if (units == 1) output[used] = (UINT16)cp;
            else { cp -= 0x10000; output[used] = 0xD800 | (cp >> 10); output[used + 1] = 0xDC00 | (cp & 1023); }
        }
        used += units;
    }
    *characters = used; return STATUS_SUCCESS;
}
