/* Per-process Linux file numbers and FAT path translation. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>
#include <svindows/Ex.h>

#define LINUX_O_CREAT 0x40u
#define LINUX_O_EXCL 0x80u
#define LINUX_O_NOCTTY 0x100u
#define LINUX_O_TRUNC 0x200u
#define LINUX_O_APPEND 0x400u
#define LINUX_O_NONBLOCK 0x800u
#define LINUX_O_LARGEFILE 0x8000u
#define LINUX_O_DIRECTORY 0x10000u
#define LINUX_O_CLOEXEC 0x80000u
#define LINUX_OPEN_FLAGS (3u | LINUX_O_CREAT | LINUX_O_EXCL | LINUX_O_NOCTTY | \
                          LINUX_O_TRUNC | LINUX_O_APPEND | LINUX_O_NONBLOCK | \
                          LINUX_O_LARGEFILE | LINUX_O_DIRECTORY | LINUX_O_CLOEXEC)

/* Install one inherited standard descriptor. Char-device bridges are
 * recognized so Linux children of PE parents keep real PTY/VT semantics
 * (isatty, termios, line discipline) instead of plain file descriptors.
 * The fd entry takes its own device reference, matching the dup/fork rule;
 * the NT handle holds a separate reference and is closed with the table.
 * Handles are probed in the child's own table: this runs while the
 * creator's table is still current, and the same handle value can denote
 * a different object there. */
static void linux_install_std_handle(PKPROCESS process, UINT32 slot,
                                     HANDLE handle, UINT8 mode)
{
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    memset(entry, 0, sizeof(*entry));
    entry->Used = handle != NULL;
    entry->Owned = FALSE;
    entry->Mode = mode;
    if (!handle) return;
    UINT64 tag = 0;
    void *context = NULL;
    if (NT_SUCCESS(IoQueryCharDeviceFileInTable(process->HandleTable, handle,
                                                &tag, &context)) &&
        tag == IO_CHAR_DEVICE_TAG_PTY) {
        LINUX_PTY_BRIDGE *bridge = context;
        entry->Kind = LINUX_FD_PTY;
        entry->Pty = bridge->Pty;
        entry->PtyMaster = bridge->Master;
        LinuxPtyInherit(bridge->Pty, bridge->Master);
        return;
    }
    if (NT_SUCCESS(IoIsConsoleFileInTable(process->HandleTable, handle))) {
        entry->Kind = LINUX_FD_TTY; /* console-backed VT stream */
        return;
    }
    entry->Kind = LINUX_FD_REGULAR;
    entry->Handle = handle;
}

void LinuxInitializeFiles(PKPROCESS process)
{
    for (UINT32 slot = 0; slot < 3; slot++) {
        HANDLE handle = slot == 0 ? process->StandardInput :
                          slot == 1 ? process->StandardOutput :
                                      process->StandardError;
        linux_install_std_handle(process, slot, handle,
                                 slot == 0 ? 0 : 1);
    }
}

/* Convert a Linux descriptor into an NT handle a PE child can use as a
 * standard stream. Regular descriptors pass their underlying file handle;
 * PTY endpoints and VTs are published as char-device File objects, so both
 * personalities see the same bytes with their own native semantics. */
NTSTATUS LinuxStdHandleFromFd(PKPROCESS process, UINT64 fd, BOOLEAN write,
                              HANDLE *out)
{
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext ||
        !process->LinuxContext->Files[fd].Used)
        return STATUS_INVALID_HANDLE;
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (write ? entry->Mode == 0 : entry->Mode == 1)
        return STATUS_INVALID_HANDLE;
    if (entry->Kind == LINUX_FD_REGULAR)
        return entry->Handle ?
            ObDuplicateHandleToTable(entry->Handle, process->HandleTable, out) :
            STATUS_INVALID_HANDLE;
    if (entry->Kind == LINUX_FD_PTY)
        return LinuxPtyCreateBridgeFile(entry->Pty, entry->PtyMaster, out);
    if (entry->Kind == LINUX_FD_TTY)
        return IoOpenConsoleKernel(out);
    return STATUS_INVALID_HANDLE;
}

BOOLEAN LinuxFileHandle(PKPROCESS process, UINT64 fd, BOOLEAN write,
                        HANDLE *handle_out)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used ||
        (write ? process->LinuxContext->Files[fd].Mode == 0 :
                 process->LinuxContext->Files[fd].Mode == 1))
        return FALSE;
    *handle_out = process->LinuxContext->Files[fd].Handle;
    return TRUE;
}

NTSTATUS LinuxPrepareWrite(PKPROCESS process, UINT64 fd)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return STATUS_INVALID_HANDLE;
    if (!process->LinuxContext->Files[fd].Append) return STATUS_SUCCESS;
    UINT64 ignored;
    return IoSeekHandleKernel(process->LinuxContext->Files[fd].Handle, 0, 2, &ignored);
}

UINT64 LinuxSyncFile(PKPROCESS process, UINT64 fd)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Kind == LINUX_FD_SOCKET || entry->Kind == LINUX_FD_PTY ||
        entry->Kind == LINUX_FD_TTY || entry->Kind == LINUX_FD_EVENT ||
        entry->Kind == LINUX_FD_FRAMEBUFFER)
        return LinuxError(LINUX_EINVAL);
    return NT_SUCCESS(FatFlush()) ? 0 : LinuxError(LINUX_EIO);
}

static UINT64 linux_file_error(NTSTATUS status)
{
    if (status == STATUS_OBJECT_NAME_NOT_FOUND ||
        status == STATUS_OBJECT_PATH_NOT_FOUND || status == STATUS_NO_SUCH_FILE)
        return LinuxError(LINUX_ENOENT);
    if (status == STATUS_OBJECT_NAME_COLLISION)
        return LinuxError(LINUX_EEXIST);
    if (status == STATUS_ACCESS_DENIED || status == STATUS_SHARING_VIOLATION)
        return LinuxError(LINUX_EACCES);
    if (status == STATUS_NO_MEMORY) return LinuxError(LINUX_ENOMEM);
    if (status == STATUS_DISK_FULL) return LinuxError(LINUX_ENOSPC);
    return LinuxError(LINUX_EIO);
}

static BOOLEAN linux_is_imlib_loader_path(const char *path)
{
    static const char token[] = "\\imlib2\\loaders\\";
    for (const char *p = path; *p; p++) {
        UINT32 i = 0;
        while (token[i] && p[i]) {
            char a = p[i], b = token[i];
            if (a >= 'A' && a <= 'Z') a += 'a' - 'A';
            if (b >= 'A' && b <= 'Z') b += 'a' - 'A';
            if (a != b) break;
            i++;
        }
        if (!token[i]) return TRUE;
    }
    return FALSE;
}

UINT64 LinuxOpenAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                   UINT64 flags)
{
    if (flags & ~(UINT64)LINUX_OPEN_FLAGS || (flags & 3u) == 3u ||
        ((flags & LINUX_O_TRUNC) && !(flags & 3u)))
        return LinuxError(LINUX_EINVAL);
    UINT64 slot = 0;
    while (slot < LINUX_MAX_FDS && process->LinuxContext->Files[slot].Used) slot++;
    if (slot == LINUX_MAX_FDS) return LinuxError(LINUX_EMFILE);
    char dos_path[260];
    UINT64 error = LinuxCapturePathAt(process, directory_fd, path, dos_path);
    if (error) return error;
    UINT8 proc = LinuxProcPathKind(dos_path);
    if (proc == LINUX_PROC_EXE) {
        memcpy(dos_path, process->ImagePath, sizeof(dos_path));
        proc = LinuxProcPathKind(dos_path);
    } else if (proc == LINUX_PROC_CWD) {
        memcpy(dos_path, process->CurrentDirectory, sizeof(dos_path));
        proc = LinuxProcPathKind(dos_path);
    }
    if (proc != LINUX_PROC_NONE)
        return LinuxProcOpen(process, slot, dos_path, flags);
    if (LinuxFramebufferPath(dos_path))
        return LinuxFramebufferOpen(process, slot, flags);

    UINT32 pty_number;
    BOOLEAN pty_master;
    if (LinuxPtyPath(dos_path, &pty_number, &pty_master))
        return LinuxPtyOpen(process, slot, pty_number, pty_master, flags);
    if (LinuxPtyControllingPath(dos_path) &&
        process->LinuxContext->ControllingPty)
        return LinuxPtyOpenControlling(process, slot, flags);

    UINT8 terminal;
    if (LinuxVtPath(dos_path, &terminal))
        return LinuxVtOpen(process, slot, terminal, flags);

    UINT32 event_index;
    if (LinuxEventPath(dos_path, &event_index))
        return LinuxEventOpen(process, slot, event_index, flags);

    FAT_FIND_DATA info;
    NTSTATUS queried = FatQueryPath(dos_path, &info);
    BOOLEAN imlib_loader = linux_is_imlib_loader_path(dos_path);
    if (imlib_loader) {
        KeLog("[linux-fs] Imlib2 loader lookup '%s' status=0x%08x size=%lu\n",
              dos_path, (unsigned)queried,
              (unsigned long)(NT_SUCCESS(queried) ? info.Size : 0));
    }
    if (NT_SUCCESS(queried) && (info.Attributes & 0x10u))
        return LinuxOpenDirectory(process, slot, dos_path, flags);
    if (flags & LINUX_O_DIRECTORY)
        return NT_SUCCESS(queried) ? LinuxError(LINUX_ENOTDIR) :
                                     linux_file_error(queried);

    ULONG access = (flags & 3u) == 0 ? GENERIC_READ :
                   (flags & 3u) == 1 ? GENERIC_WRITE :
                                         GENERIC_READ | GENERIC_WRITE;
    HANDLE handle;
    NTSTATUS status = IoOpenFileKernel(dos_path, access,
        !!(flags & LINUX_O_CREAT), !!(flags & LINUX_O_EXCL),
        !!(flags & LINUX_O_TRUNC), &handle);
    if (imlib_loader) {
        KeLog("[linux-fs] Imlib2 loader open '%s' status=0x%08x\n",
              dos_path, (unsigned)status);
    }
    if (!NT_SUCCESS(status)) return linux_file_error(status);
    process->LinuxContext->Files[slot].Handle = handle;
    process->LinuxContext->Files[slot].Mode = (UINT8)(flags & 3u);
    process->LinuxContext->Files[slot].Append = !!(flags & LINUX_O_APPEND);
    process->LinuxContext->Files[slot].NonBlocking = !!(flags & LINUX_O_NONBLOCK);
    process->LinuxContext->Files[slot].CloseOnExec = !!(flags & LINUX_O_CLOEXEC);
    process->LinuxContext->Files[slot].Owned = TRUE;
    process->LinuxContext->Files[slot].Used = TRUE;
    process->LinuxContext->Files[slot].Kind = LINUX_FD_REGULAR;
    return slot;
}

UINT64 LinuxClose(PKPROCESS process, UINT64 fd)
{
    UINT64 irq = KiIrqSave();
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EBADF);
    }
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    HANDLE handle = entry->Handle;
    BOOLEAN owned = entry->Owned;
    UINT8 kind = entry->Kind;
    UINT8 terminal = entry->Terminal;
    struct _LINUX_PTY *pty = entry->Pty;
    BOOLEAN pty_master = entry->PtyMaster;
    if (kind == LINUX_FD_EVENT && entry->EventDevice < LINUX_EVENT_DEVICE_COUNT)
        LinuxEventSetOpen(entry->EventDevice, FALSE);
    char *directory_path = entry->DirectoryPath;
    LINUX_EPOLL_CONTEXT *epoll = entry->Epoll;
    struct _LINUX_SOCKET *socket = entry->Socket;
    memset(entry, 0, sizeof(*entry));
    KiIrqRestore(irq);
    if (kind == LINUX_FD_DIRECTORY) {
        ExFreePool(directory_path);
        return 0;
    }
    if (kind == LINUX_FD_EPOLL) {
        LinuxEpollClose(epoll);
        return 0;
    }
    if (kind == LINUX_FD_SOCKET) {
        LinuxSocketClose(socket);
        return 0;
    }
    if (kind == LINUX_FD_TTY) {
        LinuxVtClose(process, terminal);
        return 0;
    }
    if (kind == LINUX_FD_PTY) {
        LinuxPtyClose(pty, pty_master);
        return 0;
    }
    if (kind == LINUX_FD_FRAMEBUFFER) return 0;
    if (kind == LINUX_FD_EVENT) return 0;
    if (kind == LINUX_FD_PROC) return 0;
    if (!owned) return 0; /* inherited standard handles remain process-owned */
    NTSTATUS status = ObCloseHandle(handle);
    return NT_SUCCESS(status) ? 0 : linux_file_error(status);
}

UINT64 LinuxSeek(PKPROCESS process, UINT64 fd, INT64 offset, UINT64 whence)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_FRAMEBUFFER)
        return LinuxFramebufferSeek(process, fd, offset, whence);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_PROC) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
        UINT64 base = whence == 0 ? 0 : whence == 1 ? entry->Offset :
                      whence == 2 ? LinuxProcSize(process, entry->ProcNode) :
                                    ~(UINT64)0;
        if (base > 0x7fffffffffffffffULL)
            return LinuxError(LINUX_EINVAL);
        if (offset < 0) {
            UINT64 backward = 0 - (UINT64)offset;
            if (backward > base) return LinuxError(LINUX_EINVAL);
            entry->Offset = base - backward;
        } else {
            if ((UINT64)offset > 0x7fffffffffffffffULL - base)
                return LinuxError(LINUX_EINVAL);
            entry->Offset = base + (UINT64)offset;
        }
        return entry->Offset;
    }
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_PTY)
        return LinuxError(LINUX_ESPIPE);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_DIRECTORY)
        return LinuxDirectorySeek(&process->LinuxContext->Files[fd],
                                  offset, whence);
    if (!process->LinuxContext->Files[fd].Owned) return LinuxError(LINUX_ESPIPE);
    if (whence > 2) return LinuxError(LINUX_EINVAL);
    UINT64 position = 0;
    NTSTATUS status = IoSeekHandleKernel(process->LinuxContext->Files[fd].Handle,
                                         offset, (UINT32)whence, &position);
    return NT_SUCCESS(status) ? position : LinuxError(LINUX_EINVAL);
}
