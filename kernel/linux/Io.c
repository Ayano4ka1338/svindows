/* Linux read/write/writev through the process's file-number table. */
#include "Internal.h"
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>

typedef struct _LINUX_IOVEC {
    UINT64 Base;
    UINT64 Length;
} LINUX_IOVEC;

UINT64 LinuxReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                      UINT64 buffer, UINT64 length)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_PROC) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
        return write ? LinuxError(LINUX_EBADF) :
            LinuxProcRead(process, entry, buffer, length, entry->Offset, TRUE);
    }
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET)
        return LinuxSocketReadWrite(process, write, fd, buffer, length, 0);
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_EVENT) {
        if (write) return LinuxError(LINUX_EBADF);
        return LinuxEventRead(process, fd, buffer, length);
    }
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_FRAMEBUFFER)
        return LinuxFramebufferReadWrite(process, write, fd, buffer, length);
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_TTY)
        return LinuxVtReadWrite(process, write, fd, buffer, length);
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_PTY)
        return LinuxPtyReadWrite(process, write, fd, buffer, length);
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_DIRECTORY)
        return LinuxError(LINUX_EISDIR);
    HANDLE handle;
    if (!LinuxFileHandle(process, fd, write, &handle))
        return LinuxError(LINUX_EBADF);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (!length) return 0;
    if (!(write ? MmProbeForRead(buffer, length) :
                  MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    if (write && !NT_SUCCESS(LinuxPrepareWrite(process, fd)))
        return LinuxError(LINUX_EIO);
    if (!write && process->LinuxContext->Files[fd].NonBlocking) {
        PVOID pipe_object = NULL;
        if (NT_SUCCESS(IoReferencePipeForPoll(handle, &pipe_object))) {
            SIZE_T completed = 0;
            NTSTATUS status = IoTryReadHandleKernel(handle,
                (void *)(ULONG_PTR)buffer, (SIZE_T)length, &completed);
            UINT32 ready = IoPollPipeObject(pipe_object);
            ObDereferenceObject(pipe_object);
            if (!NT_SUCCESS(status)) return LinuxError(LINUX_EIO);
            return completed ? completed :
                   (ready & IO_POLL_HANGUP) ? 0 : LinuxError(LINUX_EAGAIN);
        }
    }
    SIZE_T completed = 0;
    NTSTATUS status = write
        ? IoWriteHandleKernel(handle, (const void *)(ULONG_PTR)buffer,
                              (SIZE_T)length, &completed)
        : IoReadHandleKernel(handle, (void *)(ULONG_PTR)buffer,
                             (SIZE_T)length, &completed);
    return NT_SUCCESS(status) ? completed :
           completed ? completed : LinuxError(LINUX_EIO);
}

UINT64 LinuxPread(PKPROCESS process, UINT64 fd, UINT64 buffer,
                  UINT64 length, UINT64 offset)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_PROC) {
        if ((INT64)offset < 0) return LinuxError(LINUX_EINVAL);
        return LinuxProcRead(process, &process->LinuxContext->Files[fd],
                             buffer, length, offset, FALSE);
    }
    HANDLE handle;
    if (!LinuxFileHandle(process, fd, FALSE, &handle))
        return LinuxError(LINUX_EBADF);
    if (process->LinuxContext->Files[fd].Kind == LINUX_FD_PTY ||
        process->LinuxContext->Files[fd].Kind == LINUX_FD_CONSOLE)
        return LinuxError(LINUX_ESPIPE);
    if (length > LINUX_IO_MAX || (INT64)offset < 0)
        return LinuxError(LINUX_EINVAL);
    if (!length) return 0;
    if (!MmProbeForWrite(buffer, length)) return LinuxError(LINUX_EFAULT);
    POBJECT file_object;
    const UINT8 *data;
    SIZE_T size;
    NTSTATUS status = IoReferenceFileData(handle, &file_object, &data, &size);
    if (!NT_SUCCESS(status)) return LinuxError(LINUX_ESPIPE);
    UINT64 copied = 0;
    if (offset < size) {
        copied = length < size - offset ? length : size - offset;
        memcpy((void *)(ULONG_PTR)buffer, data + offset, (SIZE_T)copied);
    }
    ObDereferenceObject(file_object);
    return copied;
}

UINT64 LinuxWritev(PKPROCESS process, UINT64 fd, UINT64 vectors, UINT64 count)
{
    BOOLEAN socket = fd < LINUX_MAX_FDS &&
                     process->LinuxContext->Files[fd].Used &&
                     process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET;
    BOOLEAN terminal = fd < LINUX_MAX_FDS &&
                       process->LinuxContext->Files[fd].Used &&
                       process->LinuxContext->Files[fd].Kind == LINUX_FD_TTY;
    BOOLEAN pty = fd < LINUX_MAX_FDS &&
                  process->LinuxContext->Files[fd].Used &&
                  process->LinuxContext->Files[fd].Kind == LINUX_FD_PTY;
    HANDLE handle;
    if (!socket && !terminal && !pty &&
        !LinuxFileHandle(process, fd, TRUE, &handle))
        return LinuxError(LINUX_EBADF);
    if (count > 1024) return LinuxError(LINUX_EINVAL);
    if (!count) return 0;
    if (!MmProbeForRead(vectors, count * sizeof(LINUX_IOVEC)))
        return LinuxError(LINUX_EFAULT);

    const LINUX_IOVEC *iov = (const LINUX_IOVEC *)(ULONG_PTR)vectors;
    UINT64 total = 0;
    for (UINT64 i = 0; i < count; i++) {
        if (iov[i].Length > LINUX_IO_MAX - total)
            return LinuxError(LINUX_EINVAL);
        if (iov[i].Length && !MmProbeForRead(iov[i].Base, iov[i].Length))
            return LinuxError(LINUX_EFAULT);
        total += iov[i].Length;
    }

    UINT64 completed = 0;
    for (UINT64 i = 0; i < count; i++) {
        if (!iov[i].Length) continue;
        if (socket || terminal || pty) {
            UINT64 written = pty ?
                LinuxPtyReadWrite(process, TRUE, fd, iov[i].Base, iov[i].Length) :
                terminal ?
                LinuxVtReadWrite(process, TRUE, fd, iov[i].Base, iov[i].Length) :
                LinuxSocketReadWrite(process, TRUE, fd,
                                     iov[i].Base, iov[i].Length, 0);
            if ((INT64)written < 0) return completed ? completed : written;
            completed += written;
            if (written < iov[i].Length) break;
            continue;
        }
        if (!NT_SUCCESS(LinuxPrepareWrite(process, fd)))
            return completed ? completed : LinuxError(LINUX_EIO);
        SIZE_T written = 0;
        NTSTATUS status = IoWriteHandleKernel(handle,
            (const void *)(ULONG_PTR)iov[i].Base, (SIZE_T)iov[i].Length,
            &written);
        completed += written;
        if (!NT_SUCCESS(status))
            return completed ? completed : LinuxError(LINUX_EIO);
        if (written < iov[i].Length) break;
    }
    return completed;
}

UINT64 LinuxReadv(PKPROCESS process, UINT64 fd, UINT64 vectors, UINT64 count)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    if (count > 1024) return LinuxError(LINUX_EINVAL);
    if (!count) return 0;
    if (!MmProbeForRead(vectors, count * sizeof(LINUX_IOVEC)))
        return LinuxError(LINUX_EFAULT);
    const LINUX_IOVEC *iov = (const LINUX_IOVEC *)(ULONG_PTR)vectors;
    UINT64 total = 0;
    for (UINT64 i = 0; i < count; i++) {
        if (iov[i].Length > LINUX_IO_MAX - total)
            return LinuxError(LINUX_EINVAL);
        if (iov[i].Length && !MmProbeForWrite(iov[i].Base, iov[i].Length))
            return LinuxError(LINUX_EFAULT);
        total += iov[i].Length;
    }
    UINT64 completed = 0;
    for (UINT64 i = 0; i < count; i++) {
        if (!iov[i].Length) continue;
        UINT64 read = LinuxReadWrite(process, FALSE, fd,
                                     iov[i].Base, iov[i].Length);
        if ((INT64)read < 0) return completed ? completed : read;
        completed += read;
        if (read < iov[i].Length) break;
    }
    return completed;
}
