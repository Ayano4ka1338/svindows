/* Linux poll(2) readiness for guest file descriptors. */
#include "Internal.h"
#include "SocketPrivate.h"
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>

#define LINUX_POLLIN       0x0001u
#define LINUX_POLLPRI      0x0002u
#define LINUX_POLLOUT      0x0004u
#define LINUX_POLLERR      0x0008u
#define LINUX_POLLHUP      0x0010u
#define LINUX_POLLNVAL     0x0020u
#define LINUX_POLLRDHUP    0x2000u
#define LINUX_POLL_MAX_FDS 1024u

typedef struct PACKED _LINUX_POLLFD {
    INT32 Fd;
    INT16 Events;
    INT16 ReturnedEvents;
} LINUX_POLLFD;

_Static_assert(sizeof(LINUX_POLLFD) == 8, "Linux pollfd ABI");

UINT32 LinuxPollDescriptor(PKPROCESS process, UINT32 fd)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LINUX_POLLNVAL;

    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (entry->Kind == LINUX_FD_SOCKET)
        return LinuxSocketPoll(entry->Socket);
    if (entry->Kind == LINUX_FD_EVENT)
        return LinuxEventPoll(process, fd);
    if (entry->Kind == LINUX_FD_TTY)
        return LinuxVtPoll(process, fd);
    if (entry->Kind == LINUX_FD_PTY)
        return LinuxPtyPoll(process, fd);
    if (entry->Kind == LINUX_FD_CONSOLE) {
        BOOLEAN available = FALSE;
        UINT32 ready = entry->Mode != 0 ? LINUX_POLLOUT : 0;
        if (entry->Mode != 1 &&
            NT_SUCCESS(IoHandleInputAvailableKernel(entry->Handle, &available)) &&
            available)
            ready |= LINUX_POLLIN;
        return ready;
    }
    if (entry->Kind == LINUX_FD_FRAMEBUFFER)
        return LINUX_POLLIN | LINUX_POLLOUT;
    if (entry->Kind == LINUX_FD_DIRECTORY)
        return LINUX_POLLIN;
    if (entry->Kind == LINUX_FD_PROC)
        return LINUX_POLLIN;
    if (entry->Kind == LINUX_FD_EPOLL)
        return LinuxEpollPoll(process, fd) ? LINUX_POLLIN : 0;

    PVOID pipe_object = NULL;
    if (entry->Handle && NT_SUCCESS(IoReferencePipeForPoll(
            entry->Handle, &pipe_object))) {
        UINT32 state = IoPollPipeObject(pipe_object);
        ObDereferenceObject(pipe_object);
        UINT32 ready = 0;
        if (state & IO_POLL_READABLE) ready |= LINUX_POLLIN;
        if (state & IO_POLL_WRITABLE) ready |= LINUX_POLLOUT;
        if (state & IO_POLL_ERROR) ready |= LINUX_POLLERR;
        if (state & IO_POLL_HANGUP) ready |= LINUX_POLLHUP;
        return ready;
    }

    UINT32 ready = 0;
    if ((entry->Mode & 3u) != 1u) ready |= LINUX_POLLIN;
    if ((entry->Mode & 3u) != 0u) ready |= LINUX_POLLOUT;
    return ready;
}

static UINT64 linux_poll_scan(PKPROCESS process, UINT64 address,
                              UINT64 count)
{
    LINUX_POLLFD *descriptors = (void *)(ULONG_PTR)address;
    UINT64 ready_count = 0;
    for (UINT64 i = 0; i < count; i++) {
        LINUX_POLLFD descriptor;
        memcpy(&descriptor, &descriptors[i], sizeof(descriptor));
        UINT32 returned = 0;
        if (descriptor.Fd >= 0) {
            UINT32 available = LinuxPollDescriptor(
                process, (UINT32)descriptor.Fd);
            UINT32 requested = (UINT16)descriptor.Events;
            returned = (available & requested &
                        (LINUX_POLLIN | LINUX_POLLPRI | LINUX_POLLOUT |
                         LINUX_POLLRDHUP)) |
                       (available & (LINUX_POLLERR | LINUX_POLLHUP |
                                     LINUX_POLLNVAL));
        }
        descriptor.ReturnedEvents = (INT16)returned;
        memcpy(&descriptors[i], &descriptor, sizeof(descriptor));
        if (returned) ready_count++;
    }
    return ready_count;
}

UINT64 LinuxPoll(PKPROCESS process, UINT64 address, UINT64 count,
                 INT64 timeout_ms)
{
    if (count > LINUX_POLL_MAX_FDS || timeout_ms < -1)
        return LinuxError(LINUX_EINVAL);
    if (count && (!MmProbeForRead(address, count * sizeof(LINUX_POLLFD)) ||
                  !MmProbeForWrite(address, count * sizeof(LINUX_POLLFD))))
        return LinuxError(LINUX_EFAULT);

    UINT64 start = KeGetTickCount();
    UINT64 timeout_ticks = timeout_ms < 0 ? ~(UINT64)0 :
                          ((UINT64)timeout_ms + 9) / 10;
    for (;;) {
        UINT64 ready = linux_poll_scan(process, address, count);
        if (ready || !timeout_ticks) return ready;

        UINT64 elapsed = KeGetTickCount() - start;
        if (timeout_ticks != ~(UINT64)0 && elapsed >= timeout_ticks)
            return 0;
        UINT64 wait_ticks = 1;
        if (timeout_ticks != ~(UINT64)0 &&
            wait_ticks > timeout_ticks - elapsed)
            wait_ticks = timeout_ticks - elapsed;
        KEVENT event;
        KeInitializeEvent(&event, FALSE, FALSE);
        KeWaitForSingleObjectTimeout(&event.Header, wait_ticks);
    }
}
