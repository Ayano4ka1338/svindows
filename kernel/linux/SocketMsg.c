/* Linux x86-64 sendmsg/recvmsg for AF_UNIX and AF_INET sockets. */
#include "SocketPrivate.h"
#include <svindows/Ex.h>
#include <svindows/Rtl.h>

typedef struct _LINUX_IOVEC {
    UINT64 Base;
    UINT64 Length;
} LINUX_IOVEC;

typedef struct _LINUX_MSGHDR {
    UINT64 Name;
    UINT32 NameLength;
    UINT32 Padding;
    UINT64 Vectors;
    UINT64 VectorCount;
    UINT64 Control;
    UINT64 ControlLength;
    UINT32 Flags;
    UINT32 TailPadding;
} LINUX_MSGHDR;

_Static_assert(sizeof(LINUX_MSGHDR) == 56, "Linux x86-64 msghdr ABI");

#define LINUX_MSG_PEEK          0x00000002u
#define LINUX_MSG_DONTWAIT      0x00000040u
#define LINUX_MSG_NOSIGNAL      0x00004000u
#define LINUX_MSG_CMSG_CLOEXEC  0x40000000u

UINT64 LinuxSocketMessage(PKPROCESS process, BOOLEAN write, UINT64 fd,
                          UINT64 message, UINT64 flags)
{
    const UINT64 supported = write ?
        (UINT64)(LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL) :
        (UINT64)(LINUX_MSG_PEEK | LINUX_MSG_DONTWAIT |
                 LINUX_MSG_CMSG_CLOEXEC);
    if (flags & ~supported)
        return LinuxError(LINUX_EOPNOTSUPP);
    if (!(write ? MmProbeForRead(message, sizeof(LINUX_MSGHDR)) :
                  MmProbeForWrite(message, sizeof(LINUX_MSGHDR))))
        return LinuxError(LINUX_EFAULT);

    LINUX_MSGHDR header;
    memcpy(&header, (const void *)(ULONG_PTR)message, sizeof(header));
    if (header.VectorCount > 1024)
        return LinuxError(LINUX_EINVAL);
    if (header.VectorCount &&
        !MmProbeForRead(header.Vectors,
                        header.VectorCount * sizeof(LINUX_IOVEC)))
        return LinuxError(LINUX_EFAULT);

    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    BOOLEAN inet = socket->Family == LINUX_AF_INET;
    if (write && header.ControlLength) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_EOPNOTSUPP);
    }
    if (!inet && write && header.Name) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_EOPNOTSUPP);
    }
    if (!inet && !write && header.Name) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_EOPNOTSUPP);
    }

    LINUX_IOVEC *vectors = (void *)(ULONG_PTR)header.Vectors;
    UINT64 length = 0;
    for (UINT64 i = 0; i < header.VectorCount; i++) {
        if (vectors[i].Length > LINUX_IO_MAX - length) {
            LinuxSocketRelease(socket);
            return LinuxError(LINUX_EINVAL);
        }
        if (vectors[i].Length && !(write ?
            MmProbeForRead(vectors[i].Base, vectors[i].Length) :
            MmProbeForWrite(vectors[i].Base, vectors[i].Length))) {
            LinuxSocketRelease(socket);
            return LinuxError(LINUX_EFAULT);
        }
        length += vectors[i].Length;
    }

    if (inet) {
        UINT8 local_buffer[1];
        UINT8 *buffer = length ? ExAllocatePoolWithTag(
            NonPagedPool, (SIZE_T)length, 'gMsL') : local_buffer;
        if (length && !buffer) {
            LinuxSocketRelease(socket);
            return LinuxError(LINUX_ENOMEM);
        }
        if (write) {
            UINT64 offset = 0;
            for (UINT64 i = 0; i < header.VectorCount; i++) {
                if (vectors[i].Length)
                    memcpy(buffer + offset,
                           (const void *)(ULONG_PTR)vectors[i].Base,
                           (SIZE_T)vectors[i].Length);
                offset += vectors[i].Length;
            }
        }

        UINT64 transferred;
        if (write) {
            transferred = LinuxInetSendMessage(socket, buffer, length, flags,
                                                header.Name,
                                                header.NameLength);
        } else {
            transferred = LinuxInetReceiveMessage(socket, buffer, length,
                flags & (LINUX_MSG_PEEK | LINUX_MSG_DONTWAIT), header.Name,
                message + 8); /* msghdr.msg_namelen */
            if ((INT64)transferred >= 0) {
                UINT64 remaining = transferred;
                UINT64 offset = 0;
                for (UINT64 i = 0; i < header.VectorCount && remaining; i++) {
                    UINT64 copied = vectors[i].Length < remaining ?
                                    vectors[i].Length : remaining;
                    if (copied)
                        memcpy((void *)(ULONG_PTR)vectors[i].Base,
                               buffer + offset, (SIZE_T)copied);
                    offset += vectors[i].Length;
                    remaining -= copied;
                }
                LINUX_MSGHDR *output = (void *)(ULONG_PTR)message;
                output->ControlLength = 0;
                output->Flags = 0;
            }
        }
        if (length) ExFreePool(buffer);
        LinuxSocketRelease(socket);
        return transferred;
    }
    LinuxSocketRelease(socket);

    if (write) {
        UINT64 completed = 0;
        for (UINT64 i = 0; i < header.VectorCount; i++) {
            if (!vectors[i].Length) continue;
            UINT64 transferred = LinuxSocketReadWrite(process, TRUE, fd,
                vectors[i].Base, vectors[i].Length, flags);
            if ((INT64)transferred < 0)
                return completed ? completed : transferred;
            completed += transferred;
            if (transferred < vectors[i].Length) break;
        }
        return completed;
    }

    UINT64 completed = 0;
    for (UINT64 i = 0; i < header.VectorCount; i++) {
        if (!vectors[i].Length) continue;
        UINT64 transferred = LinuxSocketReadWrite(process, FALSE, fd,
            vectors[i].Base, vectors[i].Length,
            flags & (LINUX_MSG_PEEK | LINUX_MSG_DONTWAIT));
        if ((INT64)transferred < 0)
            return completed ? completed : transferred;
        completed += transferred;
        if (transferred < vectors[i].Length) break;
    }
    LINUX_MSGHDR *output = (void *)(ULONG_PTR)message;
    output->ControlLength = 0;
    output->Flags = 0;
    return completed;
}
