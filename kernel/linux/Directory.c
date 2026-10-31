/* Linux directory descriptors and getdents64 over the FAT enumerator. */
#include "Internal.h"
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Rtl.h>

#define LINUX_O_CREAT 0x40u
#define LINUX_O_TRUNC 0x200u
#define LINUX_O_CLOEXEC 0x80000u
#define LINUX_DT_DIR 4u
#define LINUX_DT_REG 8u

typedef struct _LINUX_DIRENT64 {
    UINT64 Inode;
    INT64 NextOffset;
    UINT16 RecordLength;
    UINT8 Type;
    char Name[];
} LINUX_DIRENT64;

_Static_assert(__builtin_offsetof(LINUX_DIRENT64, Name) == 19,
               "Linux getdents64 name offset");

UINT64 LinuxOpenDirectory(PKPROCESS process, UINT64 slot,
                          const char *path, UINT64 flags)
{
    if ((flags & 3u) || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC)))
        return LinuxError(LINUX_EISDIR);
    SIZE_T length = strlen(path) + 1;
    char *copy = ExAllocatePoolWithTag(NonPagedPool, length, 'riDL');
    if (!copy) return LinuxError(LINUX_ENOMEM);
    memcpy(copy, path, length);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    entry->DirectoryPath = copy;
    entry->CloseOnExec = !!(flags & LINUX_O_CLOEXEC);
    entry->Kind = LINUX_FD_DIRECTORY;
    entry->Used = TRUE;
    return slot;
}

UINT64 LinuxDirectorySeek(LINUX_FD_ENTRY *entry, INT64 offset, UINT64 whence)
{
    UINT64 base;
    if (whence == 0) base = 0;
    else if (whence == 1) base = entry->Offset;
    else return LinuxError(LINUX_EINVAL);
    if (offset < 0) {
        UINT64 backwards = 0 - (UINT64)offset;
        if (backwards > base) return LinuxError(LINUX_EINVAL);
        base -= backwards;
    } else {
        if (base > 0xffffffffULL ||
            (UINT64)offset > 0xffffffffULL - base)
            return LinuxError(LINUX_EINVAL);
        base += (UINT64)offset;
    }
    entry->Offset = base;
    return base;
}

static UINT64 linux_dir_inode(const char *directory, const char *name)
{
    UINT64 hash = 14695981039346656037ULL;
    for (const char *p = directory; *p; p++) {
        UINT8 ch = (UINT8)*p;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        hash = (hash ^ ch) * 1099511628211ULL;
    }
    if (directory[strlen(directory) - 1] != '\\')
        hash = (hash ^ '\\') * 1099511628211ULL;
    for (const char *p = name; *p; p++) {
        UINT8 ch = (UINT8)*p;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        hash = (hash ^ ch) * 1099511628211ULL;
    }
    return hash ? hash : 1;
}

UINT64 LinuxGetdents64(PKPROCESS process, UINT64 fd,
                        UINT64 buffer, UINT64 length)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Kind != LINUX_FD_DIRECTORY)
        return LinuxError(LINUX_ENOTDIR);
    if (length < 24 || length > LINUX_IO_MAX)
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForWrite(buffer, length)) return LinuxError(LINUX_EFAULT);

    if (LinuxProcPathKind(entry->DirectoryPath) != LINUX_PROC_NONE)
        return LinuxProcGetdents(entry, buffer, length);

    UINT64 written = 0;
    while (entry->Offset <= 0xffffffffULL + 2ULL) {
        FAT_FIND_DATA file;
        const char *name;
        UINT8 type;
        if (entry->Offset < 2) {
            name = entry->Offset ? ".." : ".";
            type = LINUX_DT_DIR;
        } else {
            NTSTATUS status = FatEnumerateDirectory(entry->DirectoryPath,
                                         (UINT32)(entry->Offset - 2), &file);
            if (status == STATUS_OBJECT_NAME_NOT_FOUND) break;
            if (!NT_SUCCESS(status))
                return written ? written : LinuxError(LINUX_EIO);
            name = file.Name;
            type = (file.Attributes & 0x10u) ? LINUX_DT_DIR : LINUX_DT_REG;
            if ((name[0] == '.' && !name[1]) ||
                (name[0] == '.' && name[1] == '.' && !name[2])) {
                entry->Offset++;
                continue;
            }
        }
        SIZE_T name_length = strlen(name);
        UINT64 record_length = (19 + name_length + 1 + 7) & ~7ULL;
        if (record_length > length - written)
            return written ? written : LinuxError(LINUX_EINVAL);
        LINUX_DIRENT64 *record = (LINUX_DIRENT64 *)(ULONG_PTR)(buffer + written);
        memset(record, 0, (SIZE_T)record_length);
        record->Inode = linux_dir_inode(entry->DirectoryPath, name);
        record->NextOffset = (INT64)(entry->Offset + 1);
        record->RecordLength = (UINT16)record_length;
        record->Type = type;
        memcpy(record->Name, name, name_length + 1);
        written += record_length;
        entry->Offset++;
    }
    return written;
}
