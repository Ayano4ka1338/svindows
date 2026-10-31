/* x86-64 Linux stat ABI over native file objects and FAT metadata. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Rtl.h>

#define LINUX_AT_FDCWD ((UINT64)(INT64)-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100u
#define LINUX_S_IFIFO 0010000u
#define LINUX_S_IFCHR 0020000u
#define LINUX_S_IFDIR 0040000u
#define LINUX_S_IFREG 0100000u
#define LINUX_S_IFSOCK 0140000u
#define LINUX_S_IFLNK 0120000u

typedef struct _LINUX_STAT {
    UINT64 Dev, Ino, Nlink;
    UINT32 Mode, Uid, Gid, Pad0;
    UINT64 Rdev;
    INT64 Size, BlockSize, Blocks;
    INT64 AccessTime;
    UINT64 AccessNanoseconds;
    INT64 ModifyTime;
    UINT64 ModifyNanoseconds;
    INT64 ChangeTime;
    UINT64 ChangeNanoseconds;
    INT64 Unused[3];
} LINUX_STAT;

_Static_assert(sizeof(LINUX_STAT) == 144, "x86-64 Linux stat size");
_Static_assert(__builtin_offsetof(LINUX_STAT, Mode) == 24,
               "x86-64 Linux stat mode offset");
_Static_assert(__builtin_offsetof(LINUX_STAT, Size) == 48,
               "x86-64 Linux stat size offset");

static BOOLEAN linux_leap(UINT32 year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static INT64 linux_fat_seconds(UINT16 date, UINT16 time)
{
    if (!date) return 0;
    UINT32 year = 1980 + (date >> 9);
    UINT32 month = (date >> 5) & 15;
    UINT32 day = date & 31;
    static const UINT8 days_per_month[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month < 1 || month > 12) return 0;
    UINT32 month_days = days_per_month[month - 1] +
                        (month == 2 && linux_leap(year));
    if (!day || day > month_days) return 0;
    UINT64 days = 0;
    for (UINT32 y = 1970; y < year; y++)
        days += linux_leap(y) ? 366 : 365;
    for (UINT32 m = 1; m < month; m++)
        days += days_per_month[m - 1] + (m == 2 && linux_leap(year));
    days += day - 1;
    return (INT64)(days * 86400 + ((time >> 11) & 31) * 3600 +
                   ((time >> 5) & 63) * 60 + (time & 31) * 2);
}

static UINT64 linux_path_inode(const char *path)
{
    UINT64 hash = 14695981039346656037ULL;
    for (; *path; path++) {
        UINT8 ch = (UINT8)*path;
        if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return hash ? hash : 1;
}

static UINT64 linux_emit_stat(UINT64 destination,
                              const IO_KERNEL_FILE_STAT *info)
{
    if (!MmProbeForWrite(destination, sizeof(LINUX_STAT)))
        return LinuxError(LINUX_EFAULT);
    LINUX_STAT result;
    memset(&result, 0, sizeof(result));
    result.Dev = info->Kind == 0 || info->Kind == 3 ? 1 : 0;
    result.Ino = info->FileId ? info->FileId : 1;
    result.Nlink = 1;
    if (info->Kind == 1 || info->Kind == 4) {
        result.Mode = LINUX_S_IFCHR | 0666u;
        if (info->Kind == 4)
            result.Rdev = 29u << 8; /* framebuffer major */
        else if (info->FileId >= 0x200u &&
                 info->FileId < 0x200u + LINUX_EVENT_DEVICE_COUNT)
            result.Rdev = (13u << 8) | (64u + info->FileId - 0x200u);
        else if (info->FileId >= 0x300u && info->FileId <= 0x30cu)
            result.Rdev = (4u << 8) | (info->FileId - 0x300u);
        else if (info->FileId == 0x500u)
            result.Rdev = (5u << 8) | 2u; /* /dev/ptmx */
        else if (info->FileId >= 0x600u && info->FileId < 0x610u)
            result.Rdev = (136u << 8) | (info->FileId - 0x600u);
        else
            result.Rdev = 0x501;
    } else if (info->Kind == 2) {
        result.Mode = LINUX_S_IFIFO | 0600u;
    } else if (info->Kind == 3 || (info->Attributes & 0x10u)) {
        result.Mode = LINUX_S_IFDIR | 0755u;
    } else {
        result.Mode = LINUX_S_IFREG |
                      ((info->Attributes & 1u) ? 0444u : 0666u);
        result.Size = (INT64)info->Size;
    }
    UINT32 sectors_per_cluster, free_clusters, bytes_per_sector, total_clusters;
    UINT64 block_size = 4096;
    if (NT_SUCCESS(FatQueryDiskSpace(&sectors_per_cluster, &free_clusters,
                                     &bytes_per_sector, &total_clusters)) &&
        sectors_per_cluster && bytes_per_sector)
        block_size = (UINT64)sectors_per_cluster * bytes_per_sector;
    result.BlockSize = (INT64)block_size;
    if (result.Size > 0)
        result.Blocks = (INT64)(((UINT64)result.Size + block_size - 1) /
                                block_size * (block_size / 512));
    result.ModifyTime = linux_fat_seconds(info->FatWriteDate,
                                          info->FatWriteTime);
    result.ChangeTime = result.ModifyTime;
    memcpy((void *)(ULONG_PTR)destination, &result, sizeof(result));
    return 0;
}

static UINT64 linux_proc_stat(PKPROCESS process, UINT8 node,
                              UINT64 destination, BOOLEAN nofollow)
{
    if (node == LINUX_PROC_UNKNOWN) return LinuxError(LINUX_ENOENT);
    if ((node == LINUX_PROC_EXE || node == LINUX_PROC_CWD) && !nofollow) {
        const char *target = node == LINUX_PROC_EXE ? process->ImagePath :
                             process->CurrentDirectory;
        UINT8 target_node = LinuxProcPathKind(target);
        return target_node != LINUX_PROC_NONE ?
            linux_proc_stat(process, target_node, destination, TRUE) :
            LinuxStatDosPath(process, target, destination);
    }
    if (!MmProbeForWrite(destination, sizeof(LINUX_STAT)))
        return LinuxError(LINUX_EFAULT);
    LINUX_STAT info = {0};
    info.Dev = 2;
    info.Ino = 0x7000u + node;
    info.Nlink = 1;
    info.BlockSize = 4096;
    if (node == LINUX_PROC_ROOT || node == LINUX_PROC_SELF)
        info.Mode = LINUX_S_IFDIR | 0555u;
    else if (node == LINUX_PROC_EXE || node == LINUX_PROC_CWD) {
        info.Mode = LINUX_S_IFLNK | 0777u;
        const char *target = node == LINUX_PROC_EXE ? process->ImagePath :
                             process->CurrentDirectory;
        info.Size = strlen(target) >= 2 ? (INT64)strlen(target) - 2 : 0;
    } else {
        info.Mode = LINUX_S_IFREG | 0444u;
        info.Size = (INT64)LinuxProcSize(process, node);
    }
    memcpy((void *)(ULONG_PTR)destination, &info, sizeof(info));
    return 0;
}

UINT64 LinuxFstat(PKPROCESS process, UINT64 fd, UINT64 destination)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_EVENT) {
        IO_KERNEL_FILE_STAT event_device = {0};
        event_device.Kind = 1; /* character device */
        event_device.FileId = 0x200 + process->LinuxContext->Files[fd].EventDevice;
        return linux_emit_stat(destination, &event_device);
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_TTY) {
        IO_KERNEL_FILE_STAT terminal = {0};
        terminal.Kind = 1;
        terminal.FileId = 0x300 + process->LinuxContext->Files[fd].Terminal;
        return linux_emit_stat(destination, &terminal);
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_PTY) {
        IO_KERNEL_FILE_STAT terminal = {0};
        terminal.Kind = 1;
        terminal.FileId = process->LinuxContext->Files[fd].PtyMaster ?
            0x500u : 0x600u + process->LinuxContext->Files[fd].Terminal;
        return linux_emit_stat(destination, &terminal);
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_FRAMEBUFFER) {
        IO_KERNEL_FILE_STAT framebuffer = {0};
        framebuffer.Kind = 4;
        framebuffer.FileId = 1;
        return linux_emit_stat(destination, &framebuffer);
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET) {
        if (!MmProbeForWrite(destination, sizeof(LINUX_STAT)))
            return LinuxError(LINUX_EFAULT);
        LINUX_STAT info = {0};
        info.Mode = LINUX_S_IFSOCK | 0777u;
        info.Nlink = 1;
        info.Ino = fd + 1;
        info.BlockSize = 4096;
        memcpy((void *)(ULONG_PTR)destination, &info, sizeof(info));
        return 0;
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_DIRECTORY)
        return LinuxStatDosPath(process,
            process->LinuxContext->Files[fd].DirectoryPath, destination);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_PROC)
        return linux_proc_stat(process,
            process->LinuxContext->Files[fd].ProcNode, destination, TRUE);
    IO_KERNEL_FILE_STAT info;
    NTSTATUS status = IoQueryFileStatKernel(process->LinuxContext->Files[fd].Handle,
                                            &info);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EBADF);
    return linux_emit_stat(destination, &info);
}

UINT64 LinuxStatDosPath(PKPROCESS process, const char *dos_path,
                        UINT64 destination)
{
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc != LINUX_PROC_NONE)
        return linux_proc_stat(process, proc, destination, TRUE);
    if (LinuxSocketPathPresent(dos_path)) {
        if (!MmProbeForWrite(destination, sizeof(LINUX_STAT)))
            return LinuxError(LINUX_EFAULT);
        LINUX_STAT info = {0};
        info.Mode = LINUX_S_IFSOCK | 0777u;
        info.Nlink = 1;
        info.Ino = linux_path_inode(dos_path);
        info.BlockSize = 4096;
        memcpy((void *)(ULONG_PTR)destination, &info, sizeof(info));
        return 0;
    }
    FAT_FIND_DATA entry;
    NTSTATUS status = FatQueryPath(dos_path, &entry);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    IO_KERNEL_FILE_STAT info;
    memset(&info, 0, sizeof(info));
    info.Size = entry.Size;
    info.FileId = linux_path_inode(dos_path);
    info.Attributes = entry.Attributes;
    info.Kind = (entry.Attributes & 0x10u) ? 3 : 0;
    info.FatWriteDate = entry.WriteDate;
    info.FatWriteTime = entry.WriteTime;
    return linux_emit_stat(destination, &info);
}

UINT64 LinuxStatPath(PKPROCESS process, UINT64 dirfd, UINT64 path,
                     UINT64 destination, UINT64 flags)
{
    if (flags & ~(UINT64)LINUX_AT_SYMLINK_NOFOLLOW)
        return LinuxError(LINUX_EINVAL);
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, dirfd, path, dos_path);
    if (error) return error;
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc != LINUX_PROC_NONE)
        return linux_proc_stat(process, proc, destination,
                               !!(flags & LINUX_AT_SYMLINK_NOFOLLOW));
    if (LinuxFramebufferPath(dos_path)) {
        if (!LinuxFramebufferSize()) return LinuxError(LINUX_ENOENT);
        IO_KERNEL_FILE_STAT framebuffer = {0};
        framebuffer.Kind = 4;
        framebuffer.FileId = 1;
        return linux_emit_stat(destination, &framebuffer);
    }
    UINT32 pty_number;
    BOOLEAN pty_master;
    if (LinuxPtyPath(dos_path, &pty_number, &pty_master)) {
        if (!pty_master && !LinuxPtyExists(pty_number))
            return LinuxError(LINUX_ENOENT);
        IO_KERNEL_FILE_STAT terminal = {0};
        terminal.Kind = 1;
        terminal.FileId = pty_master ? 0x500u : 0x600u + pty_number;
        return linux_emit_stat(destination, &terminal);
    }
    UINT8 terminal;
    if (LinuxVtPath(dos_path, &terminal)) {
        IO_KERNEL_FILE_STAT device = {0};
        device.Kind = 1;
        device.FileId = 0x300 + terminal;
        return linux_emit_stat(destination, &device);
    }
    UINT32 event_index;
    if (LinuxEventPath(dos_path, &event_index)) {
        IO_KERNEL_FILE_STAT event_device = {0};
        event_device.Kind = 1;
        event_device.FileId = 0x200 + event_index;
        return linux_emit_stat(destination, &event_device);
    }
    return LinuxStatDosPath(process, dos_path, destination);
}
