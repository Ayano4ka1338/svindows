/* AF_UNIX stream data movement and descriptor-facing operations. */
#include "SocketPrivate.h"
#include <svindows/Rtl.h>

#define LINUX_MSG_PEEK 0x2u
#define LINUX_MSG_DONTWAIT 0x40u
#define LINUX_MSG_NOSIGNAL 0x4000u
#define LINUX_SOL_SOCKET 1u
#define LINUX_SO_REUSEADDR 2u
#define LINUX_SO_TYPE 3u
#define LINUX_SO_ERROR 4u
#define LINUX_SO_SNDBUF 7u
#define LINUX_SO_RCVBUF 8u
#define LINUX_SO_KEEPALIVE 9u
#define LINUX_SO_PASSCRED 16u
#define LINUX_SO_PEERCRED 17u
#define LINUX_SO_DOMAIN 39u

UINT64 LinuxSocketPair(PKPROCESS process, UINT64 family, UINT64 type,
                       UINT64 protocol, UINT64 pair)
{
    if (family != LINUX_AF_UNIX) return LinuxError(LINUX_EAFNOSUPPORT);
    if (protocol || (type & ~(UINT64)(LINUX_SOCK_STREAM |
                  LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) ||
        (type & 15u) != LINUX_SOCK_STREAM)
        return LinuxError(LINUX_EPROTONOSUPPORT);
    if (!MmProbeForWrite(pair, 2 * sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKET *left = LinuxSocketAllocate();
    LINUX_SOCKET *right = LinuxSocketAllocate();
    if (!left || !right) {
        if (left) LinuxSocketRelease(left);
        if (right) LinuxSocketRelease(right);
        return LinuxError(LINUX_ENOMEM);
    }
    left->OwnerPid = right->OwnerPid = process->ProcessId;
    left->State = right->State = LINUX_SOCKET_CONNECTED;
    left->Peer = right;
    right->Peer = left;
    UINT64 first = LinuxSocketInstall(process, left, type);
    if ((INT64)first < 0) {
        LinuxSocketRelease(left);
        LinuxSocketRelease(right);
        return first;
    }
    UINT64 second = LinuxSocketInstall(process, right, type);
    if ((INT64)second < 0) {
        LinuxClose(process, first);
        LinuxSocketRelease(right);
        return second;
    }
    UINT32 result[2] = {(UINT32)first, (UINT32)second};
    memcpy((void *)(ULONG_PTR)pair, result, sizeof(result));
    return 0;
}

UINT64 LinuxSocketAccept(PKPROCESS process, UINT64 fd, UINT64 address,
                         UINT64 address_length, UINT64 flags)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetAccept(process, fd, address, address_length, flags);
    if (flags & ~(UINT64)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
        return LinuxError(LINUX_EINVAL);
    if (address && !MmProbeForWrite(address_length, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKET *listener = LinuxSocketReferenceFd(process, fd);
    if (!listener) return LinuxError(LINUX_ENOTSOCK);
    UINT64 result;
    for (;;) {
        UINT64 irq = KiIrqSave();
        if (listener->Closed) {
            result = LinuxError(LINUX_EBADF);
            KiIrqRestore(irq);
            break;
        }
        if (listener->State != LINUX_SOCKET_LISTENING) {
            result = LinuxError(LINUX_EINVAL);
            KiIrqRestore(irq);
            break;
        }
        if (listener->PendingHead) {
            UINT64 slot = 0;
            while (slot < LINUX_MAX_FDS && process->LinuxContext->Files[slot].Used)
                slot++;
            if (slot == LINUX_MAX_FDS) {
                result = LinuxError(LINUX_EMFILE);
                KiIrqRestore(irq);
                break;
            }
            LINUX_SOCKET *server = listener->PendingHead;
            listener->PendingHead = server->PendingNext;
            if (!listener->PendingHead) {
                listener->PendingTail = NULL;
                KeResetEvent(&listener->AcceptEvent);
            }
            server->PendingNext = NULL;
            listener->PendingCount--;
            LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
            entry->Socket = server;
            entry->Kind = LINUX_FD_SOCKET;
            entry->Used = TRUE;
            entry->Mode = 2;
            entry->CloseOnExec = !!(flags & LINUX_SOCK_CLOEXEC);
            entry->NonBlocking = !!(flags & LINUX_SOCK_NONBLOCK);
            server->NonBlocking = entry->NonBlocking;
            char peer_name[LINUX_SOCKET_NAME_MAX] = {0};
            UINT16 peer_length = 0;
            if (server->Peer) {
                peer_length = server->Peer->NameLength;
                memcpy(peer_name, server->Peer->Name, peer_length);
            }
            KiIrqRestore(irq);
            result = slot;
            if (address) {
                UINT64 error = LinuxSocketWriteAddress(address, address_length,
                                                        peer_name, peer_length);
                if (error) {
                    LinuxClose(process, slot);
                    result = error;
                }
            }
            break;
        }
        if (listener->NonBlocking) {
            result = LinuxError(LINUX_EAGAIN);
            KiIrqRestore(irq);
            break;
        }
        /* Never sleep with interrupts disabled.  The event is reset while
         * holding the IRQ lock; a connect racing after the restore sets it
         * and the wait below observes the signal instead of losing wakeup. */
        KeResetEvent(&listener->AcceptEvent);
        KiIrqRestore(irq);
        KeWaitForSingleObject(&listener->AcceptEvent.Header);
    }
    LinuxSocketRelease(listener);
    return result;
}

UINT64 LinuxSocketReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                            UINT64 buffer, UINT64 length, UINT64 flags)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetReadWrite(process, write, fd, buffer, length, flags);
    UINT64 accepted_flags = LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL |
                            (write ? 0 : LINUX_MSG_PEEK);
    if (flags & ~accepted_flags) return LinuxError(LINUX_EOPNOTSUPP);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (length && !(write ? MmProbeForRead(buffer, length) :
                           MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    UINT64 result = 0;
    while (length) {
        UINT64 irq = KiIrqSave();
        if (socket->Closed) {
            result = LinuxError(LINUX_EBADF);
            KiIrqRestore(irq);
            break;
        }
        if (socket->State != LINUX_SOCKET_CONNECTED) {
            result = LinuxError(LINUX_ENOTCONN);
            KiIrqRestore(irq);
            break;
        }
        if (write) {
            LINUX_SOCKET *peer = socket->Peer;
            if (!peer || socket->WriteShutdown || peer->ReadShutdown) {
                result = LinuxError(LINUX_EPIPE);
                KiIrqRestore(irq);
                break;
            }
            UINT32 available = LINUX_SOCKET_BUFFER - peer->Buffered;
            if (available) {
                UINT32 copied = length < available ? (UINT32)length : available;
                UINT32 end = (peer->ReadOffset + peer->Buffered) %
                             LINUX_SOCKET_BUFFER;
                UINT32 first = copied < LINUX_SOCKET_BUFFER - end ? copied :
                               LINUX_SOCKET_BUFFER - end;
                memcpy(peer->Data + end, (const void *)(ULONG_PTR)buffer, first);
                if (copied > first)
                    memcpy(peer->Data, (const void *)(ULONG_PTR)(buffer + first),
                           copied - first);
                peer->Buffered += copied;
                KeSetEvent(&peer->ReadEvent);
                if (peer->Buffered == LINUX_SOCKET_BUFFER)
                    KeResetEvent(&peer->SpaceEvent);
                result = copied;
                KiIrqRestore(irq);
                break;
            }
            if (socket->NonBlocking || (flags & LINUX_MSG_DONTWAIT)) {
                result = LinuxError(LINUX_EAGAIN);
                KiIrqRestore(irq);
                break;
            }
            peer->References++;
            KeResetEvent(&peer->SpaceEvent);
            KiIrqRestore(irq);
            /* Waiting with IF=0 can stop the timer/scheduler that must wake
             * this thread when the peer drains its receive buffer. */
            KeWaitForSingleObject(&peer->SpaceEvent.Header);
            LinuxSocketRelease(peer);
        } else {
            if (socket->ReadShutdown) {
                KiIrqRestore(irq);
                break;
            }
            if (socket->Buffered) {
                UINT32 copied = length < socket->Buffered ?
                                (UINT32)length : socket->Buffered;
                UINT32 first = copied < LINUX_SOCKET_BUFFER - socket->ReadOffset ?
                               copied : LINUX_SOCKET_BUFFER - socket->ReadOffset;
                memcpy((void *)(ULONG_PTR)buffer,
                       socket->Data + socket->ReadOffset, first);
                if (copied > first)
                    memcpy((void *)(ULONG_PTR)(buffer + first), socket->Data,
                           copied - first);
                if (!(flags & LINUX_MSG_PEEK)) {
                    socket->ReadOffset = (socket->ReadOffset + copied) %
                                         LINUX_SOCKET_BUFFER;
                    socket->Buffered -= copied;
                    KeSetEvent(&socket->SpaceEvent);
                    if (!socket->Buffered && !socket->PeerClosed)
                        KeResetEvent(&socket->ReadEvent);
                }
                result = copied;
                KiIrqRestore(irq);
                break;
            }
            if (socket->PeerClosed || !socket->Peer ||
                socket->Peer->WriteShutdown) {
                KiIrqRestore(irq);
                break;
            }
            if (socket->NonBlocking || (flags & LINUX_MSG_DONTWAIT)) {
                result = LinuxError(LINUX_EAGAIN);
                KiIrqRestore(irq);
                break;
            }
            KeResetEvent(&socket->ReadEvent);
            KiIrqRestore(irq);
            KeWaitForSingleObject(&socket->ReadEvent.Header);
        }
    }
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxSocketName(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 address_length, BOOLEAN peer)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetName(process, fd, address, address_length, peer);
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    char name[LINUX_SOCKET_NAME_MAX];
    UINT16 name_length;
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET *target = peer ? socket->Peer : socket;
    if (!target) {
        KiIrqRestore(irq);
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_ENOTCONN);
    }
    name_length = target->NameLength;
    memcpy(name, target->Name, name_length);
    KiIrqRestore(irq);
    UINT64 result = LinuxSocketWriteAddress(address, address_length,
                                            name, name_length);
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxSocketOption(PKPROCESS process, UINT64 fd, UINT64 level,
                         UINT64 option, UINT64 value, UINT64 length,
                         BOOLEAN get)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetOption(process, fd, level, option, value, length, get);
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    UINT64 result = 0;
    if (level != LINUX_SOL_SOCKET) {
        result = LinuxError(LINUX_EOPNOTSUPP);
    } else if (!get) {
        if (length < sizeof(UINT32) || !MmProbeForRead(value, sizeof(UINT32)))
            result = LinuxError(LINUX_EFAULT);
        else if (option != LINUX_SO_REUSEADDR && option != LINUX_SO_KEEPALIVE &&
                 option != LINUX_SO_SNDBUF && option != LINUX_SO_RCVBUF &&
                 option != LINUX_SO_PASSCRED)
            result = LinuxError(LINUX_EOPNOTSUPP);
    } else {
        if (!MmProbeForRead(length, sizeof(UINT32)) ||
            !MmProbeForWrite(length, sizeof(UINT32)))
            result = LinuxError(LINUX_EFAULT);
        else {
            UINT32 value_size = option == LINUX_SO_PEERCRED ? 12 : 4;
            if (*(UINT32 *)(ULONG_PTR)length < value_size ||
                !MmProbeForWrite(value, value_size))
                result = LinuxError(LINUX_EFAULT);
            else {
                UINT32 output[3] = {0, 0, 0};
                if (option == LINUX_SO_TYPE) output[0] = LINUX_SOCK_STREAM;
                else if (option == LINUX_SO_DOMAIN) output[0] = LINUX_AF_UNIX;
                else if (option == LINUX_SO_SNDBUF || option == LINUX_SO_RCVBUF)
                    output[0] = LINUX_SOCKET_BUFFER;
                else if (option == LINUX_SO_PEERCRED) {
                    UINT64 irq = KiIrqSave();
                    if (socket->Peer) output[0] = socket->Peer->OwnerPid;
                    KiIrqRestore(irq);
                } else if (option != LINUX_SO_ERROR &&
                           option != LINUX_SO_REUSEADDR &&
                           option != LINUX_SO_KEEPALIVE &&
                           option != LINUX_SO_PASSCRED)
                    result = LinuxError(LINUX_EOPNOTSUPP);
                if (!result) {
                    memcpy((void *)(ULONG_PTR)value, output, value_size);
                    *(UINT32 *)(ULONG_PTR)length = value_size;
                }
            }
        }
    }
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxSocketShutdown(PKPROCESS process, UINT64 fd, UINT64 how)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetShutdown(process, fd, how);
    if (how > 2) return LinuxError(LINUX_EINVAL);
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    UINT64 irq = KiIrqSave();
    UINT64 result = 0;
    if (socket->State != LINUX_SOCKET_CONNECTED)
        result = LinuxError(LINUX_ENOTCONN);
    else {
        if (how == 0 || how == 2) socket->ReadShutdown = TRUE;
        if (how == 1 || how == 2) socket->WriteShutdown = TRUE;
        KeSetEvent(&socket->ReadEvent);
        if (socket->Peer) {
            KeSetEvent(&socket->Peer->ReadEvent);
            KeSetEvent(&socket->Peer->SpaceEvent);
        }
    }
    KiIrqRestore(irq);
    LinuxSocketRelease(socket);
    return result;
}
