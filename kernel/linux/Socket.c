/* AF_UNIX stream endpoint lifetime, naming, and connection queues. */
#include "SocketPrivate.h"
#include <svindows/Ex.h>
#include <svindows/Io.h>
#include <svindows/Rtl.h>

static LINUX_SOCKET *g_bound_sockets;

LINUX_SOCKET *LinuxSocketAllocate(void)
{
    LINUX_SOCKET *socket = ExAllocatePoolWithTag(
        NonPagedPool, sizeof(*socket), 'kcoL');
    if (!socket) return NULL;
    memset(socket, 0, sizeof(*socket));
    socket->References = 1;
    socket->Family = LINUX_AF_UNIX;
    socket->LwipFd = -1;
    socket->DescriptorReferences = 1;
    KeInitializeEvent(&socket->ReadEvent, TRUE, FALSE);
    KeInitializeEvent(&socket->SpaceEvent, TRUE, TRUE);
    KeInitializeEvent(&socket->AcceptEvent, TRUE, FALSE);
    return socket;
}

void LinuxSocketRelease(LINUX_SOCKET *socket)
{
    UINT64 irq = KiIrqSave();
    BOOLEAN free_socket = --socket->References == 0;
    KiIrqRestore(irq);
    if (free_socket) ExFreePool(socket);
}

LINUX_SOCKET *LinuxSocketReferenceFd(PKPROCESS process, UINT64 fd)
{
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET *socket = NULL;
    if (fd < LINUX_MAX_FDS) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
        if (entry->Used && entry->Kind == LINUX_FD_SOCKET) {
            socket = entry->Socket;
            socket->References++;
        }
    }
    KiIrqRestore(irq);
    return socket;
}

UINT64 LinuxSocketInstall(PKPROCESS process, LINUX_SOCKET *socket,
                          UINT64 flags)
{
    UINT64 irq = KiIrqSave();
    UINT64 slot = 0;
    while (slot < LINUX_MAX_FDS && process->LinuxContext->Files[slot].Used)
        slot++;
    if (slot < LINUX_MAX_FDS) {
        LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[slot];
        entry->Socket = socket;
        entry->Kind = LINUX_FD_SOCKET;
        entry->Used = TRUE;
        entry->Mode = 2;
        entry->CloseOnExec = !!(flags & LINUX_SOCK_CLOEXEC);
        entry->NonBlocking = !!(flags & LINUX_SOCK_NONBLOCK);
        socket->NonBlocking = entry->NonBlocking;
    }
    KiIrqRestore(irq);
    return slot < LINUX_MAX_FDS ? slot : LinuxError(LINUX_EMFILE);
}

UINT64 LinuxSocketCopyAddress(UINT64 source, UINT64 length,
                              char name[LINUX_SOCKET_NAME_MAX],
                              UINT16 *name_length)
{
    if (length < 2 || length > LINUX_SOCKET_NAME_MAX + 2)
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForRead(source, length)) return LinuxError(LINUX_EFAULT);
    const UINT8 *address = (const void *)(ULONG_PTR)source;
    UINT16 family;
    memcpy(&family, address, sizeof(family));
    if (family != LINUX_AF_UNIX) return LinuxError(LINUX_EAFNOSUPPORT);
    UINT32 bytes = (UINT32)length - 2;
    if (!bytes) return LinuxError(LINUX_EINVAL);
    memcpy(name, address + 2, bytes);
    if (name[0]) {
        UINT32 n = 0;
        while (n < bytes && name[n]) n++;
        if (n == LINUX_SOCKET_NAME_MAX) return LinuxError(LINUX_ENAMETOOLONG);
        name[n] = 0;
        bytes = n + 1;
        if (name[0] != '/') return LinuxError(LINUX_EINVAL);
    }
    *name_length = (UINT16)bytes;
    return 0;
}

UINT64 LinuxSocketWriteAddress(UINT64 address, UINT64 length_pointer,
                               const char *name, UINT16 name_length)
{
    if (!MmProbeForRead(length_pointer, sizeof(UINT32)) ||
        !MmProbeForWrite(length_pointer, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    UINT32 capacity = *(UINT32 *)(ULONG_PTR)length_pointer;
    UINT32 actual = name_length + 2;
    UINT32 copied = capacity < actual ? capacity : actual;
    if (copied && !MmProbeForWrite(address, copied))
        return LinuxError(LINUX_EFAULT);
    UINT8 bytes[LINUX_SOCKET_NAME_MAX + 2] = {0};
    bytes[0] = LINUX_AF_UNIX;
    if (name_length) memcpy(bytes + 2, name, name_length);
    if (copied) memcpy((void *)(ULONG_PTR)address, bytes, copied);
    *(UINT32 *)(ULONG_PTR)length_pointer = actual;
    return 0;
}

UINT64 LinuxSocketCreate(PKPROCESS process, UINT64 family, UINT64 type,
                         UINT64 protocol)
{
    if (family == LINUX_AF_INET)
        return LinuxInetSocketCreate(process, type, protocol);
    if (family != LINUX_AF_UNIX) return LinuxError(LINUX_EAFNOSUPPORT);
    if (protocol || (type & ~(UINT64)(LINUX_SOCK_STREAM |
                  LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) ||
        (type & 15u) != LINUX_SOCK_STREAM)
        return LinuxError(LINUX_EPROTONOSUPPORT);
    LINUX_SOCKET *socket = LinuxSocketAllocate();
    if (!socket) return LinuxError(LINUX_ENOMEM);
    socket->OwnerPid = process->ProcessId;
    socket->SocketType = LINUX_SOCK_STREAM;
    UINT64 result = LinuxSocketInstall(process, socket, type);
    if ((INT64)result < 0) LinuxSocketRelease(socket);
    return result;
}

static LINUX_SOCKET *linux_find_bound(const char *name, UINT16 length)
{
    for (LINUX_SOCKET *socket = g_bound_sockets; socket;
         socket = socket->BoundNext)
        if (!socket->Closed && socket->NameLength == length &&
            !memcmp(socket->Name, name, length))
            return socket;
    return NULL;
}

BOOLEAN LinuxLocalConnectable(const char *name, UINT16 length)
{
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET *listener = linux_find_bound(name, length);
    BOOLEAN ready = listener && !listener->Closed &&
                    listener->State == LINUX_SOCKET_LISTENING &&
                    listener->PendingCount < listener->Backlog;
    KiIrqRestore(irq);
    return ready;
}

static UINT64 linux_socket_dos_path(const char *name, char result[260])
{
    char path[260] = "C:";
    UINT32 i = 2;
    for (const char *part = name; *part && i + 1 < sizeof(path); part++)
        path[i++] = *part == '/' ? '\\' : *part;
    if (name[i - 2]) return LinuxError(LINUX_ENAMETOOLONG);
    path[i] = 0;
    if (!IoCanonicalizeDosPathKernel(path, result))
        return LinuxError(LINUX_EINVAL);
    return 0;
}

BOOLEAN LinuxSocketPathPresent(const char *dos_path)
{
    UINT64 irq = KiIrqSave();
    BOOLEAN present = FALSE;
    for (LINUX_SOCKET *socket = g_bound_sockets; socket;
         socket = socket->BoundNext)
        if (socket->DosPath[0] && !strcmp(socket->DosPath, dos_path)) {
            present = TRUE;
            break;
        }
    KiIrqRestore(irq);
    return present;
}

BOOLEAN LinuxSocketUnlinkPath(const char *dos_path)
{
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET **cursor = &g_bound_sockets;
    while (*cursor && strcmp((*cursor)->DosPath, dos_path))
        cursor = &(*cursor)->BoundNext;
    BOOLEAN removed = *cursor != NULL;
    if (removed) {
        LINUX_SOCKET *socket = *cursor;
        *cursor = socket->BoundNext;
        socket->BoundNext = NULL;
        socket->DosPath[0] = 0;
    }
    KiIrqRestore(irq);
    return removed;
}

UINT64 LinuxSocketBind(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 length)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetBind(process, fd, address, length);
    char name[LINUX_SOCKET_NAME_MAX] = {0};
    UINT16 name_length;
    UINT64 error = LinuxSocketCopyAddress(address, length, name, &name_length);
    if (error) return error;
    char dos_path[260] = {0};
    if (name[0]) {
        error = linux_socket_dos_path(name, dos_path);
        if (error) return error;
        if (LinuxProcPathKind(dos_path) != LINUX_PROC_NONE)
            return LinuxError(LINUX_EROFS);
        FAT_FIND_DATA entry;
        if (NT_SUCCESS(FatQueryPath(dos_path, &entry)))
            return LinuxError(LINUX_EADDRINUSE);
        char parent[260];
        memcpy(parent, dos_path, strlen(dos_path) + 1);
        char *separator = NULL;
        for (char *part = parent; *part; part++)
            if (*part == '\\') separator = part;
        if (!separator) return LinuxError(LINUX_EINVAL);
        separator[1] = 0;
        NTSTATUS status = FatQueryPath(parent, &entry);
        if (!NT_SUCCESS(status)) return LinuxError(LINUX_ENOENT);
        if (!(entry.Attributes & 0x10u)) return LinuxError(LINUX_ENOTDIR);
    }
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    UINT64 irq = KiIrqSave();
    if (socket->State != LINUX_SOCKET_CREATED || socket->NameLength)
        error = LinuxError(LINUX_EINVAL);
    else if (linux_find_bound(name, name_length))
        error = LinuxError(LINUX_EADDRINUSE);
    else {
        memcpy(socket->Name, name, name_length);
        socket->NameLength = name_length;
        memcpy(socket->DosPath, dos_path, strlen(dos_path) + 1);
        socket->BoundNext = g_bound_sockets;
        g_bound_sockets = socket;
        error = 0;
    }
    KiIrqRestore(irq);
    LinuxSocketRelease(socket);
    return error;
}

UINT64 LinuxSocketListen(PKPROCESS process, UINT64 fd, UINT64 backlog)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetListen(process, fd, backlog);
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    UINT64 irq = KiIrqSave();
    UINT64 result = 0;
    if (!socket->NameLength || socket->State == LINUX_SOCKET_CONNECTED)
        result = LinuxError(LINUX_EINVAL);
    else {
        socket->State = LINUX_SOCKET_LISTENING;
        socket->Backlog = backlog > 64 ? 64 : backlog ? (UINT32)backlog : 1;
    }
    KiIrqRestore(irq);
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxSocketConnect(PKPROCESS process, UINT64 fd, UINT64 address,
                          UINT64 length)
{
    if (fd < LINUX_MAX_FDS && process->LinuxContext->Files[fd].Used &&
        process->LinuxContext->Files[fd].Kind == LINUX_FD_SOCKET &&
        process->LinuxContext->Files[fd].Socket->Family == LINUX_AF_INET)
        return LinuxInetConnect(process, fd, address, length);
    char name[LINUX_SOCKET_NAME_MAX] = {0};
    UINT16 name_length;
    UINT64 error = LinuxSocketCopyAddress(address, length, name, &name_length);
    if (error) return error;
    LINUX_SOCKET *client = LinuxSocketReferenceFd(process, fd);
    if (!client) return LinuxError(LINUX_ENOTSOCK);
    LINUX_SOCKET *server = LinuxSocketAllocate();
    if (!server) {
        LinuxSocketRelease(client);
        return LinuxError(LINUX_ENOMEM);
    }
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET *listener = linux_find_bound(name, name_length);
    if (client->State == LINUX_SOCKET_CONNECTED)
        error = LinuxError(LINUX_EISCONN);
    else if (client->State != LINUX_SOCKET_CREATED)
        error = LinuxError(LINUX_EINVAL);
    else if (!listener || listener->State != LINUX_SOCKET_LISTENING ||
             listener->Closed)
        error = LinuxError(LINUX_ECONNREFUSED);
    else if (listener->PendingCount >= listener->Backlog)
        error = LinuxError(LINUX_EAGAIN);
    else {
        client->State = server->State = LINUX_SOCKET_CONNECTED;
        server->OwnerPid = listener->OwnerPid;
        memcpy(server->Name, listener->Name, listener->NameLength);
        server->NameLength = listener->NameLength;
        client->Peer = server;
        server->Peer = client;
        if (listener->PendingTail)
            listener->PendingTail->PendingNext = server;
        else
            listener->PendingHead = server;
        listener->PendingTail = server;
        if (!listener->PendingCount) listener->ReadSequence++;
        listener->PendingCount++;
        KeSetEvent(&listener->AcceptEvent);
        error = 0;
    }
    KiIrqRestore(irq);
    if (error) LinuxSocketRelease(server);
    LinuxSocketRelease(client);
    return error;
}

void LinuxSocketClose(LINUX_SOCKET *socket)
{
    UINT64 irq = KiIrqSave();
    if (--socket->DescriptorReferences) {
        KiIrqRestore(irq);
        LinuxSocketRelease(socket);
        return;
    }
    if (!socket->Closed && socket->Family == LINUX_AF_INET) {
        socket->Closed = TRUE;
        KiIrqRestore(irq);
        LinuxInetClose(socket);
        LinuxSocketRelease(socket);
        return;
    }
    if (!socket->Closed) {
        socket->Closed = TRUE;
        if (socket->NameLength) {
            LINUX_SOCKET **cursor = &g_bound_sockets;
            while (*cursor && *cursor != socket)
                cursor = &(*cursor)->BoundNext;
            if (*cursor == socket) *cursor = socket->BoundNext;
        }
        if (socket->Peer) {
            socket->Peer->Peer = NULL;
            socket->Peer->PeerClosed = TRUE;
            KeSetEvent(&socket->Peer->ReadEvent);
            KeSetEvent(&socket->Peer->SpaceEvent);
            socket->Peer = NULL;
        }
        KeSetEvent(&socket->ReadEvent);
        KeSetEvent(&socket->SpaceEvent);
        KeSetEvent(&socket->AcceptEvent);
    }
    LINUX_SOCKET *pending = socket->PendingHead;
    socket->PendingHead = socket->PendingTail = NULL;
    socket->PendingCount = 0;
    KiIrqRestore(irq);
    while (pending) {
        LINUX_SOCKET *next = pending->PendingNext;
        pending->PendingNext = NULL;
        LinuxSocketClose(pending);
        pending = next;
    }
    LinuxSocketRelease(socket);
}

void LinuxSocketInherit(struct _LINUX_SOCKET *socket)
{
    UINT64 irq = KiIrqSave();
    socket->References++;
    socket->DescriptorReferences++;
    KiIrqRestore(irq);
}

UINT32 LinuxSocketPoll(LINUX_SOCKET *socket)
{
    if (socket->Family == LINUX_AF_INET)
        return LinuxInetPoll(socket);
    UINT64 irq = KiIrqSave();
    UINT32 ready = 0;
    if (socket->State == LINUX_SOCKET_LISTENING) {
        if (socket->PendingCount) ready |= 1u; /* EPOLLIN */
    } else if (socket->State == LINUX_SOCKET_CONNECTED) {
        if (socket->Buffered || socket->PeerClosed || socket->ReadShutdown)
            ready |= 1u;
        if (socket->Peer && !socket->WriteShutdown &&
            !socket->Peer->ReadShutdown &&
            socket->Peer->Buffered < LINUX_SOCKET_BUFFER)
            ready |= 4u; /* EPOLLOUT */
        if (socket->PeerClosed) ready |= 0x10u; /* EPOLLHUP */
    }
    KiIrqRestore(irq);
    return ready;
}

/* Kernel clients use the same AF_UNIX queues without borrowing user FDs. */
LINUX_SOCKET *LinuxLocalConnect(const char *name, UINT16 length)
{
    LINUX_SOCKET *client = LinuxSocketAllocate(), *server = LinuxSocketAllocate();
    if (!client || !server) {
        if (client) LinuxSocketRelease(client);
        if (server) LinuxSocketRelease(server);
        return NULL;
    }
    UINT64 irq = KiIrqSave();
    LINUX_SOCKET *listener = linux_find_bound(name, length);
    if (!listener || listener->Closed || listener->State != LINUX_SOCKET_LISTENING ||
        listener->PendingCount >= listener->Backlog) {
        KiIrqRestore(irq);
        LinuxSocketRelease(client); LinuxSocketRelease(server);
        return NULL;
    }
    client->SocketType = server->SocketType = LINUX_SOCK_STREAM;
    client->State = server->State = LINUX_SOCKET_CONNECTED;
    server->OwnerPid = listener->OwnerPid;
    client->Peer = server; server->Peer = client;
    if (listener->PendingTail) listener->PendingTail->PendingNext = server;
    else listener->PendingHead = server;
    listener->PendingTail = server;
    if (!listener->PendingCount) listener->ReadSequence++;
    listener->PendingCount++;
    KeSetEvent(&listener->AcceptEvent);
    KiIrqRestore(irq);
    return client;
}
