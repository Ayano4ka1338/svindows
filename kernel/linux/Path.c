/* Linux path permission checks over the native FAT namespace. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Ex.h>
#include <svindows/Rtl.h>

#define LINUX_AT_FDCWD ((UINT64)(INT64)-100)
#define LINUX_PENDING_SYMLINK_COUNT 128

typedef struct _LINUX_PENDING_SYMLINK {
    BOOLEAN Used;
    char LinkPath[260];
    char TargetPath[260];
} LINUX_PENDING_SYMLINK;

/* FAT has no symlink entries. A package can list a symlink before its target,
 * so retain those links until the target is published by apk's rename. */
static LINUX_PENDING_SYMLINK g_linux_pending_symlinks[
    LINUX_PENDING_SYMLINK_COUNT];

static BOOLEAN linux_path_equal(const char *left, const char *right)
{
    while (*left && *right) {
        char a = *left++, b = *right++;
        if (a >= 'a' && a <= 'z') a -= 'a' - 'A';
        if (b >= 'a' && b <= 'z') b -= 'a' - 'A';
        if (a != b) return FALSE;
    }
    return *left == *right;
}

static void linux_materialize_pending_symlinks(const char *target_path)
{
    FAT_FIND_DATA info;
    NTSTATUS status = FatQueryPath(target_path, &info);
    if (!NT_SUCCESS(status) || (info.Attributes & 0x10u)) return;

    void *contents = NULL;
    SIZE_T size = 0;
    status = FatLoadFile(target_path, &contents, &size, NULL);
    if (!NT_SUCCESS(status)) return;
    for (UINT32 i = 0; i < LINUX_PENDING_SYMLINK_COUNT; i++) {
        LINUX_PENDING_SYMLINK *pending = &g_linux_pending_symlinks[i];
        if (!pending->Used || !linux_path_equal(pending->TargetPath,
                                                target_path))
            continue;
        status = FatStoreFile(pending->LinkPath, contents, size, TRUE);
        if (NT_SUCCESS(status)) {
            pending->Used = FALSE;
        } else {
            KeLog("[linux-fs] deferred symlink '%s' -> '%s' materialization "
                  "failed status=0x%08x\n", pending->LinkPath,
                  pending->TargetPath, (unsigned)status);
        }
    }
    ExFreePool(contents);
}

static UINT64 linux_capture_path(PKPROCESS process, UINT64 user,
                                 const char *relative_base, char result[260])
{
    char path[260];
    UINT32 length = 0;
    for (; length + 1 < sizeof(path); length++) {
        if (user + length < user || !MmProbeForRead(user + length, 1))
            return LinuxError(LINUX_EFAULT);
        char ch = *(const char *)(ULONG_PTR)(user + length);
        if (ch == '\\' || ch == ':') return LinuxError(LINUX_EINVAL);
        path[length] = ch;
        if (!ch) break;
    }
    if (length + 1 == sizeof(path))
        return LinuxError(LINUX_ENAMETOOLONG);
    if (!length)
        return LinuxError(LINUX_ENOENT);

    UINT32 out = 0;
    const char *prefix = path[0] == '/' ? "C:" : relative_base;
    while (prefix[out] && out + 1 < 260) {
        result[out] = prefix[out];
        out++;
    }
    if (prefix[out]) return LinuxError(LINUX_ENAMETOOLONG);
    if (out && result[out - 1] != '\\') {
        if (out + 1 >= 260) return LinuxError(LINUX_ENAMETOOLONG);
        result[out++] = '\\';
    }
    UINT32 in = path[0] == '/' ? 1 : 0;
    while (path[in]) {
        if (out + 1 >= 260) return LinuxError(LINUX_ENAMETOOLONG);
        result[out++] = path[in] == '/' ? '\\' : path[in];
        in++;
    }
    result[out] = 0;
    char canonical[260];
    if (!IoCanonicalizeDosPathKernel(result, canonical))
        return LinuxError(LINUX_ENAMETOOLONG);
    for (UINT32 i = 0; i < 260; i++) {
        result[i] = canonical[i];
        if (!canonical[i]) break;
    }
    return 0;
}

UINT64 LinuxCapturePath(PKPROCESS process, UINT64 user, char result[260])
{
    return linux_capture_path(process, user, process->CurrentDirectory, result);
}

UINT64 LinuxCapturePathAt(PKPROCESS process, UINT64 directory_fd,
                          UINT64 user, char result[260])
{
    if (!MmProbeForRead(user, 1)) return LinuxError(LINUX_EFAULT);
    if (*(const char *)(ULONG_PTR)user == '/' ||
        directory_fd == LINUX_AT_FDCWD)
        return LinuxCapturePath(process, user, result);
    if (directory_fd >= LINUX_MAX_FDS ||
        !process->LinuxContext->Files[directory_fd].Used)
        return LinuxError(LINUX_EBADF);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[directory_fd];
    if (entry->Kind != LINUX_FD_DIRECTORY)
        return LinuxError(LINUX_ENOTDIR);
    return linux_capture_path(process, user, entry->DirectoryPath, result);
}


UINT64 LinuxAccess(PKPROCESS process, UINT64 path, UINT64 mode)
{
    if (mode & ~7ULL) return LinuxError(LINUX_EINVAL);
    char dos_path[260];
    UINT64 error = LinuxCapturePath(process, path, dos_path);
    if (error) return error;
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc == LINUX_PROC_CWD) {
        memcpy(dos_path, process->CurrentDirectory, sizeof(dos_path));
        proc = LinuxProcPathKind(dos_path);
    }
    if (proc != LINUX_PROC_NONE) {
        if (proc == LINUX_PROC_UNKNOWN) return LinuxError(LINUX_ENOENT);
        if (mode & 2u) return LinuxError(LINUX_EROFS);
        if ((mode & 1u) && proc != LINUX_PROC_ROOT &&
            proc != LINUX_PROC_SELF && proc != LINUX_PROC_EXE &&
            proc != LINUX_PROC_CWD)
            return LinuxError(LINUX_EACCES);
        return 0;
    }
    if (LinuxFramebufferPath(dos_path))
        return LinuxFramebufferSize() ? 0 : LinuxError(LINUX_ENOENT);
    UINT32 pty_number;
    BOOLEAN pty_master;
    if (LinuxPtyPath(dos_path, &pty_number, &pty_master))
        return pty_master || LinuxPtyExists(pty_number) ? 0 :
               LinuxError(LINUX_ENOENT);
    UINT8 terminal;
    if (LinuxVtPath(dos_path, &terminal)) return 0;
    UINT32 event_index;
    if (LinuxEventPath(dos_path, &event_index)) {
        /* An evdev node is read-only: a W_OK probe is refused, matching a
         * real kernel where the device mode is 0444/0600 and not writable. */
        return (mode & 2u) ? LinuxError(LINUX_EACCES) : 0;
    }
    if (LinuxSocketPathPresent(dos_path)) return 0;
    FAT_FIND_DATA entry;
    NTSTATUS status = FatQueryPath(dos_path, &entry);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    if ((mode & 2u) && (entry.Attributes & 1u))
        return LinuxError(LINUX_EACCES);
    return 0;
}

UINT64 LinuxGetcwd(PKPROCESS process, UINT64 buffer, UINT64 size)
{
    const char *dos_path = process->CurrentDirectory;
    if (dos_path[0] != 'C' && dos_path[0] != 'c')
        return LinuxError(LINUX_ENOENT);
    if (dos_path[1] != ':' || dos_path[2] != '\\')
        return LinuxError(LINUX_ENOENT);
    char linux_path[260];
    UINT32 length = 0;
    const char *part = dos_path + 2;
    for (; *part && length + 1 < sizeof(linux_path); part++)
        linux_path[length++] = *part == '\\' ? '/' : *part;
    if (!length || *part)
        return LinuxError(LINUX_ERANGE);
    linux_path[length] = 0;
    if (size < length + 1) return LinuxError(LINUX_ERANGE);
    if (!MmProbeForWrite(buffer, length + 1)) return LinuxError(LINUX_EFAULT);
    memcpy((void *)(ULONG_PTR)buffer, linux_path, length + 1);
    return length + 1;
}

UINT64 LinuxChdir(PKPROCESS process, UINT64 path)
{
    char dos_path[260];
    UINT64 error = LinuxCapturePath(process, path, dos_path);
    if (error) return error;
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc == LINUX_PROC_CWD) {
        memcpy(dos_path, process->CurrentDirectory, sizeof(dos_path));
        proc = LinuxProcPathKind(dos_path);
    }
    if (proc != LINUX_PROC_NONE) {
        if (proc != LINUX_PROC_ROOT && proc != LINUX_PROC_SELF)
            return proc == LINUX_PROC_UNKNOWN ? LinuxError(LINUX_ENOENT) :
                                                LinuxError(LINUX_ENOTDIR);
        memcpy(process->CurrentDirectory, dos_path, strlen(dos_path) + 1);
        return 0;
    }
    FAT_FIND_DATA entry;
    NTSTATUS status = FatQueryPath(dos_path, &entry);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    if (!(entry.Attributes & 0x10u)) return LinuxError(LINUX_ENOTDIR);
    SIZE_T length = strlen(dos_path);
    if (length + 1 > sizeof(process->CurrentDirectory))
        return LinuxError(LINUX_ENAMETOOLONG);
    memcpy(process->CurrentDirectory, dos_path, length + 1);
    return 0;
}

UINT64 LinuxMkdir(PKPROCESS process, UINT64 path, UINT64 mode)
{
    (void)mode; /* FAT cannot store Unix permission bits. */
    char dos_path[260];
    UINT64 error = LinuxCapturePath(process, path, dos_path);
    if (error) return error;
    if (LinuxProcPathKind(dos_path) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    NTSTATUS status = FatCreateDirectory(dos_path);
    if (status == STATUS_OBJECT_NAME_COLLISION)
        return LinuxError(LINUX_EEXIST);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (status == STATUS_NO_MEMORY) return LinuxError(LINUX_ENOMEM);
    if (status == STATUS_DISK_FULL) return LinuxError(LINUX_ENOSPC);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}

UINT64 LinuxMkdirAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                    UINT64 mode)
{
    (void)mode; /* FAT cannot store Unix permission bits. */
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
    if (error) return error;
    if (LinuxProcPathKind(dos_path) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    NTSTATUS status = FatCreateDirectory(dos_path);
    if (status == STATUS_OBJECT_NAME_COLLISION)
        return LinuxError(LINUX_EEXIST);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (status == STATUS_NO_MEMORY) return LinuxError(LINUX_ENOMEM);
    if (status == STATUS_DISK_FULL) return LinuxError(LINUX_ENOSPC);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}

UINT64 LinuxUnlink(PKPROCESS process, UINT64 path)
{
    return LinuxUnlinkAt(process, LINUX_AT_FDCWD, path, 0);
}

UINT64 LinuxUnlinkAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                     UINT64 flags)
{
    const UINT64 AT_REMOVEDIR = 0x200u;
    if (flags & ~AT_REMOVEDIR) return LinuxError(LINUX_EINVAL);
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
    if (error) return error;
    if (LinuxProcPathKind(dos_path) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    if (LinuxSocketUnlinkPath(dos_path)) return 0;
    FAT_FIND_DATA entry;
    NTSTATUS status = FatQueryPath(dos_path, &entry);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    if (entry.Attributes & 0x10u) {
        if (!(flags & AT_REMOVEDIR)) return LinuxError(LINUX_EISDIR);
        status = FatRemoveDirectory(dos_path);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
            status == STATUS_OBJECT_PATH_NOT_FOUND ||
            status == STATUS_NO_SUCH_FILE)
            return LinuxError(LINUX_ENOENT);
        return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
    }
    if (flags & AT_REMOVEDIR) return LinuxError(LINUX_ENOTDIR);
    status = FatDeleteFile(dos_path);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}

/* FAT cannot represent a hard link. A copy at the requested target path
 * would report success with different inode and lifetime semantics. */
UINT64 LinuxLink(PKPROCESS process, UINT64 old_path, UINT64 new_path)
{
    char source[260];
    UINT64 error = LinuxCapturePath(process, old_path, source);
    if (error) return error;
    char target[260];
    error = LinuxCapturePath(process, new_path, target);
    if (error) return error;
    if (LinuxProcPathKind(source) != LINUX_PROC_NONE ||
        LinuxProcPathKind(target) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    return LinuxError(LINUX_EOPNOTSUPP);
}

UINT64 LinuxRename(PKPROCESS process, UINT64 old_path, UINT64 new_path)
{
    return LinuxRenameAt(process, LINUX_AT_FDCWD, old_path,
                         LINUX_AT_FDCWD, new_path);
}

UINT64 LinuxRenameAt(PKPROCESS process, UINT64 old_directory_fd,
                     UINT64 old_path, UINT64 new_directory_fd,
                     UINT64 new_path)
{
    char source[260];
    UINT64 error = LinuxCapturePathAt(process, old_directory_fd,
                                      old_path, source);
    if (error) return error;
    char target[260];
    error = LinuxCapturePathAt(process, new_directory_fd, new_path, target);
    if (error) return error;
    if (LinuxProcPathKind(source) != LINUX_PROC_NONE ||
        LinuxProcPathKind(target) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    if (LinuxSocketPathPresent(source) || LinuxFramebufferPath(source))
        return LinuxError(LINUX_EPERM);
    NTSTATUS status = FatRenamePath(source, target);
    if (NT_SUCCESS(status)) {
        for (UINT32 i = 0; i < LINUX_PENDING_SYMLINK_COUNT; i++) {
            LINUX_PENDING_SYMLINK *pending = &g_linux_pending_symlinks[i];
            if (!pending->Used) continue;
            if (linux_path_equal(pending->LinkPath, source))
                memcpy(pending->LinkPath, target, strlen(target) + 1);
            if (linux_path_equal(pending->TargetPath, source))
                memcpy(pending->TargetPath, target, strlen(target) + 1);
        }
        linux_materialize_pending_symlinks(target);
    }
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (status == STATUS_OBJECT_NAME_COLLISION) return LinuxError(LINUX_EEXIST);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}

/* apk-tools uses RENAME_NOREPLACE when publishing a freshly extracted file.
 * FAT's rename helper normally replaces a regular destination, so implement
 * that flag with an existence check and keep rejecting the stronger modes
 * that this filesystem cannot provide. */
UINT64 LinuxRenameAt2(PKPROCESS process, UINT64 old_directory_fd,
                      UINT64 old_path, UINT64 new_directory_fd,
                      UINT64 new_path, UINT64 flags)
{
    const UINT64 RENAME_NOREPLACE = 1u;
    if (flags & ~RENAME_NOREPLACE)
        return LinuxError(LINUX_EOPNOTSUPP);
    if (flags & RENAME_NOREPLACE) {
        char target[260];
        UINT64 error = LinuxCapturePathAt(process, new_directory_fd,
                                          new_path, target);
        if (error) return error;
        if (LinuxProcPathKind(target) != LINUX_PROC_NONE)
            return LinuxError(LINUX_EROFS);
        FAT_FIND_DATA info;
        NTSTATUS status = FatQueryPath(target, &info);
        if (NT_SUCCESS(status)) return LinuxError(LINUX_EEXIST);
        if (status != STATUS_OBJECT_NAME_NOT_FOUND &&
            status != STATUS_OBJECT_PATH_NOT_FOUND &&
            status != STATUS_NO_SUCH_FILE)
            return LinuxError(LINUX_EIO);
    }
    return LinuxRenameAt(process, old_directory_fd, old_path,
                         new_directory_fd, new_path);
}

UINT64 LinuxSymlink(PKPROCESS process, UINT64 target, UINT64 link_path)
{
    return LinuxSymlinkAt(process, target, LINUX_AT_FDCWD, link_path);
}

/* FAT has no directory-entry type for symbolic links. Materialize links to
 * existing regular files as copies. When apk lists a link before its target,
 * create a temporary empty file and finish materializing it when the target
 * is renamed into place. */
UINT64 LinuxSymlinkAt(PKPROCESS process, UINT64 target, UINT64 directory_fd,
                      UINT64 link_path)
{
    char link_dos[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, link_path,
                                      link_dos);
    if (error) return error;
    if (LinuxProcPathKind(link_dos) != LINUX_PROC_NONE)
        return LinuxError(LINUX_EROFS);
    FAT_FIND_DATA existing;
    NTSTATUS status = FatQueryPath(link_dos, &existing);
    if (NT_SUCCESS(status)) return LinuxError(LINUX_EEXIST);
    if (status != STATUS_OBJECT_NAME_NOT_FOUND &&
        status != STATUS_OBJECT_PATH_NOT_FOUND && status != STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_EIO);

    char target_dos[260];
    if (!MmProbeForRead(target, 1)) return LinuxError(LINUX_EFAULT);
    if (*(const char *)(ULONG_PTR)target == '/') {
        error = LinuxCapturePath(process, target, target_dos);
    } else {
        char parent[260];
        SIZE_T length = strlen(link_dos);
        while (length && link_dos[length - 1] != '\\') length--;
        if (!length) return LinuxError(LINUX_EINVAL);
        memcpy(parent, link_dos, length);
        parent[length] = 0;
        error = linux_capture_path(process, target, parent, target_dos);
    }
    if (error) return error;
    status = FatQueryPath(target_dos, &existing);
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE) {
        UINT32 slot = 0;
        while (slot < LINUX_PENDING_SYMLINK_COUNT &&
               g_linux_pending_symlinks[slot].Used)
            slot++;
        if (slot == LINUX_PENDING_SYMLINK_COUNT)
            return LinuxError(LINUX_ENOSPC);
        status = FatStoreFile(link_dos, NULL, 0, TRUE);
        if (!NT_SUCCESS(status)) {
            if (status == STATUS_DISK_FULL)
                return LinuxError(LINUX_ENOSPC);
            if (status == STATUS_NO_MEMORY)
                return LinuxError(LINUX_ENOMEM);
            return LinuxError(LINUX_EIO);
        }
        LINUX_PENDING_SYMLINK *pending = &g_linux_pending_symlinks[slot];
        memcpy(pending->LinkPath, link_dos, strlen(link_dos) + 1);
        memcpy(pending->TargetPath, target_dos, strlen(target_dos) + 1);
        pending->Used = TRUE;
        return 0;
    }
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    if (existing.Attributes & 0x10u) return LinuxError(LINUX_EOPNOTSUPP);

    void *contents = NULL;
    SIZE_T size = 0;
    status = FatLoadFile(target_dos, &contents, &size, NULL);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
    status = FatStoreFile(link_dos, contents, size, TRUE);
    ExFreePool(contents);
    if (status == STATUS_DISK_FULL) return LinuxError(LINUX_ENOSPC);
    if (status == STATUS_NO_MEMORY) return LinuxError(LINUX_ENOMEM);
    return NT_SUCCESS(status) ? 0 : LinuxError(LINUX_EIO);
}
