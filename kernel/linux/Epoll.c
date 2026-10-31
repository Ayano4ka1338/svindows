/* Linux epoll descriptors and level-triggered AF_UNIX readiness. */
#include "SocketPrivate.h"
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Ob.h>
#include <svindows/Rtl.h>

#define LINUX_EPOLL_CLOEXEC 0x80000u
#define LINUX_EPOLL_CTL_ADD 1u
#define LINUX_EPOLL_CTL_DEL 2u
#define LINUX_EPOLL_CTL_MOD 3u
#define LINUX_EPOLL_EVENT_SIZE 12u
#define LINUX_EPOLL_ET 0x80000000u
#define LINUX_EPOLL_ONESHOT 0x40000000u
#define LINUX_EPOLL_SUPPORTED (1u | 4u | 8u | 0x10u | 0x2000u | \
                               LINUX_EPOLL_ET | LINUX_EPOLL_ONESHOT)

typedef struct PACKED _LINUX_EPOLL_EVENT {
    UINT32 Events;
    UINT64 Data;
} LINUX_EPOLL_EVENT;

_Static_assert(sizeof(LINUX_EPOLL_EVENT) == 12, "Linux epoll event ABI");

UINT64 LinuxEpollCreate(PKPROCESS process, UINT64 flags)
{
    if (flags & ~(UINT64)LINUX_EPOLL_CLOEXEC)
        return LinuxError(LINUX_EINVAL);
    UINT64 slot = 0;
    while (slot < LINUX_MAX_FDS && process->LinuxContext->Files[slot].Used)
        slot++;
    if (slot == LINUX_MAX_FDS) return LinuxError(LINUX_EMFILE);
    LINUX_EPOLL_CONTEXT *epoll = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*epoll), 'lpoL');
    if (!epoll) return LinuxError(LINUX_ENOMEM);
    memset(epoll, 0, sizeof(*epoll));
    epoll->References = 1;
    epoll->DescriptorReferences = 1;
    KeInitializeEvent(&epoll->Event, TRUE, FALSE);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
    entry->Epoll = epoll;
    entry->CloseOnExec = !!(flags & LINUX_EPOLL_CLOEXEC);
    entry->Kind = LINUX_FD_EPOLL;
    entry->Used = TRUE;
    return slot;
}

UINT64 LinuxEpollCtl(PKPROCESS process, UINT64 epfd, UINT64 operation,
                     UINT64 target_fd, UINT64 event)
{
    if (operation != LINUX_EPOLL_CTL_ADD &&
        operation != LINUX_EPOLL_CTL_DEL &&
        operation != LINUX_EPOLL_CTL_MOD)
        return LinuxError(LINUX_EINVAL);
    LINUX_EPOLL_EVENT requested = {0};
    if (operation != LINUX_EPOLL_CTL_DEL) {
        if (!MmProbeForRead(event, sizeof(requested)))
            return LinuxError(LINUX_EFAULT);
        memcpy(&requested, (const void *)(ULONG_PTR)event, sizeof(requested));
        if (requested.Events & ~LINUX_EPOLL_SUPPORTED)
            return LinuxError(LINUX_EOPNOTSUPP);
    }
    UINT64 irq = KiIrqSave();
    if (epfd >= LINUX_MAX_FDS || !process->LinuxContext->Files[epfd].Used ||
        process->LinuxContext->Files[epfd].Kind != LINUX_FD_EPOLL ||
        target_fd >= LINUX_MAX_FDS ||
        !process->LinuxContext->Files[target_fd].Used) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EBADF);
    }
    if (target_fd == epfd) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EINVAL);
    }
    LINUX_FD_ENTRY *target = &process->LinuxContext->Files[target_fd];
    if (target->Kind != LINUX_FD_SOCKET &&
        target->Kind != LINUX_FD_REGULAR &&
        target->Kind != LINUX_FD_EVENT &&
        target->Kind != LINUX_FD_PTY) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EPERM);
    }
    PVOID new_pipe = NULL;
    if (operation == LINUX_EPOLL_CTL_ADD &&
        target->Kind == LINUX_FD_REGULAR &&
        !NT_SUCCESS(IoReferencePipeForPoll(target->Handle, &new_pipe))) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EPERM);
    }
    LINUX_EPOLL_CONTEXT *epoll = process->LinuxContext->Files[epfd].Epoll;
    UINT32 found = LINUX_MAX_FDS, free_slot = LINUX_MAX_FDS;
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        if (epoll->Watches[i].Used &&
            epoll->Watches[i].Descriptor == target_fd)
            found = i;
        if (!epoll->Watches[i].Used && free_slot == LINUX_MAX_FDS)
            free_slot = i;
    }
    UINT64 result = 0;
    struct _LINUX_SOCKET *removed = NULL;
    struct _LINUX_PTY *removed_pty = NULL;
    PVOID removed_pipe = NULL;
    if (operation == LINUX_EPOLL_CTL_ADD) {
        if (found != LINUX_MAX_FDS) result = LinuxError(LINUX_EEXIST);
        else if (free_slot == LINUX_MAX_FDS) result = LinuxError(LINUX_ENOSPC);
        else {
            if (target->Kind == LINUX_FD_SOCKET) {
                epoll->Watches[free_slot].Socket = target->Socket;
                target->Socket->References++;
            } else if (target->Kind == LINUX_FD_REGULAR) {
                epoll->Watches[free_slot].PipeObject = new_pipe;
                new_pipe = NULL;
            } else if (target->Kind == LINUX_FD_PTY) {
                epoll->Watches[free_slot].PtyObject = target->Pty;
                epoll->Watches[free_slot].PtyMaster = target->PtyMaster;
                epoll->Watches[free_slot].Mode = target->Mode;
                LinuxPtyControlInherit(target->Pty);
            }
            epoll->Watches[free_slot].Kind = target->Kind;
            epoll->Watches[free_slot].Descriptor = (UINT32)target_fd;
            epoll->Watches[free_slot].Events = requested.Events;
            epoll->Watches[free_slot].Data = requested.Data;
            epoll->Watches[free_slot].Used = TRUE;
        }
    } else if (found == LINUX_MAX_FDS) {
        result = LinuxError(LINUX_ENOENT);
    } else if (operation == LINUX_EPOLL_CTL_DEL) {
        removed = epoll->Watches[found].Socket;
        removed_pty = epoll->Watches[found].PtyObject;
        removed_pipe = epoll->Watches[found].PipeObject;
        memset(&epoll->Watches[found], 0, sizeof(epoll->Watches[found]));
    } else {
        epoll->Watches[found].Events = requested.Events;
        epoll->Watches[found].Data = requested.Data;
        epoll->Watches[found].LastReady = 0;
        epoll->Watches[found].Disabled = FALSE;
    }
    KiIrqRestore(irq);
    if (removed) LinuxSocketRelease(removed);
    if (removed_pty) LinuxPtyControlClose(removed_pty);
    if (removed_pipe) ObDereferenceObject(removed_pipe);
    if (new_pipe) ObDereferenceObject(new_pipe);
    return result;
}

UINT32 LinuxEpollPoll(PKPROCESS process, UINT64 fd)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used ||
        process->LinuxContext->Files[fd].Kind != LINUX_FD_EPOLL)
        return 0x10u;
    UINT64 irq = KiIrqSave();
    LINUX_EPOLL_CONTEXT *epoll = process->LinuxContext->Files[fd].Epoll;
    if (epoll->Closed) {
        KiIrqRestore(irq);
        return 0x10u;
    }
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        if (!epoll->Watches[i].Used) continue;
        if (epoll->Watches[i].Disabled) continue;
        UINT32 ready;
        if (epoll->Watches[i].Kind == LINUX_FD_EVENT) {
            ready = LinuxEventPoll(process, epoll->Watches[i].Descriptor);
        } else if (epoll->Watches[i].Socket) {
            ready = LinuxSocketPoll(epoll->Watches[i].Socket);
        } else if (epoll->Watches[i].PtyObject) {
            ready = LinuxPtyPollObject(epoll->Watches[i].PtyObject,
                epoll->Watches[i].PtyMaster, epoll->Watches[i].Mode);
        } else if (epoll->Watches[i].PipeObject) {
            UINT32 state = IoPollPipeObject(epoll->Watches[i].PipeObject);
            ready = ((state & IO_POLL_READABLE) ? 1u : 0u) |
                    ((state & IO_POLL_WRITABLE) ? 4u : 0u) |
                    ((state & IO_POLL_HANGUP) ? 0x10u : 0u) |
                    ((state & IO_POLL_ERROR) ? 8u : 0u);
        } else {
            ready = 0;
        }
        UINT32 current = ready & (1u | 4u | 8u | 0x10u | 0x2000u);
        if (epoll->Watches[i].Events & LINUX_EPOLL_ET) {
            /* poll(epollfd) must re-arm an edge after observing the source
             * become not-ready, but must not consume a pending edge. */
            if (!current) epoll->Watches[i].LastReady = 0;
            else if (!(current & ~epoll->Watches[i].LastReady))
                continue;
        }
        if (ready & (epoll->Watches[i].Events | 8u | 0x10u)) {
            KiIrqRestore(irq);
            return 1u;
        }
    }
    KiIrqRestore(irq);
    return 0;
}

UINT64 LinuxEpollWait(PKPROCESS process, UINT64 epfd, UINT64 events,
                      UINT64 maximum, INT64 timeout_ms)
{
    if (!maximum || maximum > 4096 || timeout_ms < -1)
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForWrite(events, maximum * LINUX_EPOLL_EVENT_SIZE))
        return LinuxError(LINUX_EFAULT);
    UINT64 irq = KiIrqSave();
    if (epfd >= LINUX_MAX_FDS || !process->LinuxContext->Files[epfd].Used ||
        process->LinuxContext->Files[epfd].Kind != LINUX_FD_EPOLL) {
        KiIrqRestore(irq);
        return LinuxError(LINUX_EBADF);
    }
    LINUX_EPOLL_CONTEXT *epoll = process->LinuxContext->Files[epfd].Epoll;
    epoll->References++;
    KiIrqRestore(irq);
    UINT64 remaining = timeout_ms < 0 ? ~(UINT64)0 :
                       ((UINT64)timeout_ms + 9) / 10;
    UINT64 result = 0;
    for (;;) {
        irq = KiIrqSave();
        if (epoll->Closed) {
            result = LinuxError(LINUX_EBADF);
            KiIrqRestore(irq);
            break;
        }
        UINT32 count = 0;
        LINUX_EPOLL_EVENT *output = (void *)(ULONG_PTR)events;
        for (UINT32 i = 0; i < LINUX_MAX_FDS && count < maximum; i++) {
            if (!epoll->Watches[i].Used)
                continue;
            if (epoll->Watches[i].Disabled)
                continue;
            UINT32 ready;
            if (epoll->Watches[i].Kind == LINUX_FD_EVENT) {
                ready = LinuxEventPoll(process, epoll->Watches[i].Descriptor);
            } else if (epoll->Watches[i].Socket) {
                ready = LinuxSocketPoll(epoll->Watches[i].Socket);
            } else if (epoll->Watches[i].PtyObject) {
                ready = LinuxPtyPollObject(epoll->Watches[i].PtyObject,
                    epoll->Watches[i].PtyMaster, epoll->Watches[i].Mode);
            } else if (epoll->Watches[i].PipeObject) {
                UINT32 state = IoPollPipeObject(epoll->Watches[i].PipeObject);
                ready = ((state & IO_POLL_READABLE) ? 1u : 0u) |
                        ((state & IO_POLL_WRITABLE) ? 4u : 0u) |
                        ((state & IO_POLL_HANGUP) ? 0x10u : 0u) |
                        ((state & IO_POLL_ERROR) ? 8u : 0u);
            } else {
                ready = 0;
            }
            UINT32 current = ready & (1u | 4u | 8u | 0x10u | 0x2000u);
            if (epoll->Watches[i].Events & LINUX_EPOLL_ET) {
                /* EPOLLET is a transition, not a one-time notification.
                 * Record both the asserted and cleared states on every
                 * wait scan.  Previously LastReady was only updated when
                 * an event was returned, so after the first accept/read
                 * drained a listener/socket, LastReady stayed set forever
                 * and later connections/data never generated another edge. */
                UINT32 previous = epoll->Watches[i].LastReady;
                epoll->Watches[i].LastReady = current;
                ready &= ~previous;
                ready &= epoll->Watches[i].Events | 8u | 0x10u;
            } else {
                ready &= epoll->Watches[i].Events | 8u | 0x10u;
            }
            if (!ready) continue;
            output[count].Events = ready;
            output[count].Data = epoll->Watches[i].Data;
            if (epoll->Watches[i].Events & LINUX_EPOLL_ONESHOT)
                epoll->Watches[i].Disabled = TRUE;
            count++;
        }
        KiIrqRestore(irq);
        if (count) {
            result = count;
            break;
        }
        if (!remaining) break;
        NTSTATUS status = KeWaitForSingleObjectTimeout(&epoll->Event.Header, 1);
        if (status != STATUS_TIMEOUT && !NT_SUCCESS(status)) {
            result = LinuxError(LINUX_EINVAL);
            break;
        }
        if (remaining != ~(UINT64)0) remaining--;
    }
    irq = KiIrqSave();
    BOOLEAN free_context = --epoll->References == 0;
    KiIrqRestore(irq);
    if (free_context) ExFreePool(epoll);
    return result;
}

void LinuxEpollClose(LINUX_EPOLL_CONTEXT *epoll)
{
    struct _LINUX_SOCKET *sources[LINUX_MAX_FDS];
    struct _LINUX_PTY *ptys[LINUX_MAX_FDS];
    PVOID pipes[LINUX_MAX_FDS];
    UINT32 count = 0, pty_count = 0, pipe_count = 0;
    UINT64 irq = KiIrqSave();
    if (--epoll->DescriptorReferences) {
        BOOLEAN free_context = --epoll->References == 0;
        KiIrqRestore(irq);
        if (free_context) ExFreePool(epoll);
        return;
    }
    epoll->Closed = TRUE;
    KeSetEvent(&epoll->Event);
    for (UINT32 i = 0; i < LINUX_MAX_FDS; i++) {
        if (epoll->Watches[i].Socket) {
            sources[count++] = epoll->Watches[i].Socket;
            epoll->Watches[i].Socket = NULL;
        }
        if (epoll->Watches[i].PtyObject) {
            ptys[pty_count++] = epoll->Watches[i].PtyObject;
            epoll->Watches[i].PtyObject = NULL;
        }
        if (epoll->Watches[i].PipeObject) {
            pipes[pipe_count++] = epoll->Watches[i].PipeObject;
            epoll->Watches[i].PipeObject = NULL;
        }
    }
    BOOLEAN free_context = --epoll->References == 0;
    KiIrqRestore(irq);
    for (UINT32 i = 0; i < count; i++) LinuxSocketRelease(sources[i]);
    for (UINT32 i = 0; i < pty_count; i++) LinuxPtyControlClose(ptys[i]);
    for (UINT32 i = 0; i < pipe_count; i++) ObDereferenceObject(pipes[i]);
    if (free_context) ExFreePool(epoll);
}

void LinuxEpollInherit(LINUX_EPOLL_CONTEXT *epoll)
{
    UINT64 irq = KiIrqSave();
    epoll->References++;
    epoll->DescriptorReferences++;
    KiIrqRestore(irq);
}
