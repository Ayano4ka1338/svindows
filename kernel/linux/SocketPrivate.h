#ifndef SVINDOWS_LINUX_SOCKET_PRIVATE_H
#define SVINDOWS_LINUX_SOCKET_PRIVATE_H

#include "Internal.h"

#define LINUX_AF_UNIX 1u
#define LINUX_AF_INET 2u
#define LINUX_SOCK_STREAM 1u
#define LINUX_SOCK_NONBLOCK 0x800u
#define LINUX_SOCK_CLOEXEC 0x80000u
/* A 320x200 X11 ZPixmap is 256 KB. Small queues force the DOS sender
 * to yield dozens of times per frame while Xorg drains the stream. */
#define LINUX_SOCKET_BUFFER (256u * 1024u)
#define LINUX_SOCKET_NAME_MAX 108u

#define LINUX_SOCKET_CREATED 0u
#define LINUX_SOCKET_LISTENING 1u
#define LINUX_SOCKET_CONNECTED 2u

typedef struct _LINUX_SOCKET {
    UINT32 References;
    UINT16 Family;
    UINT16 SocketType;
    UINT32 Protocol;
    INT32 LwipFd;
    UINT32 DescriptorReferences;
    UINT8 State;
    BOOLEAN Closed;
    BOOLEAN PeerClosed;
    BOOLEAN ReadShutdown;
    BOOLEAN WriteShutdown;
    BOOLEAN NonBlocking;
    UINT16 NameLength;
    UINT32 OwnerPid;
    UINT32 Backlog;
    UINT32 PendingCount;
    UINT32 ReadOffset;
    UINT32 Buffered;
    UINT64 ReadSequence, WriteSequence; /* AF_UNIX readiness transitions */
    char Name[LINUX_SOCKET_NAME_MAX];
    char DosPath[260];
    struct _LINUX_SOCKET *Peer;
    struct _LINUX_SOCKET *BoundNext;
    struct _LINUX_SOCKET *PendingHead;
    struct _LINUX_SOCKET *PendingTail;
    struct _LINUX_SOCKET *PendingNext;
    KEVENT ReadEvent;
    KEVENT SpaceEvent;
    KEVENT AcceptEvent;
    UINT8 Data[LINUX_SOCKET_BUFFER];
} LINUX_SOCKET;

LINUX_SOCKET *LinuxSocketAllocate(void);
UINT64 LinuxSocketInstall(PKPROCESS process, LINUX_SOCKET *socket,
                          UINT64 flags);
UINT64 LinuxSocketCopyAddress(UINT64 source, UINT64 length,
                              char name[LINUX_SOCKET_NAME_MAX],
                              UINT16 *name_length);
UINT64 LinuxSocketWriteAddress(UINT64 address, UINT64 length_pointer,
                               const char *name, UINT16 name_length);

UINT64 LinuxInetSocketCreate(PKPROCESS process, UINT64 type, UINT64 protocol);
UINT64 LinuxInetBind(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 length);
UINT64 LinuxInetConnect(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 length);
UINT64 LinuxInetListen(PKPROCESS process, UINT64 fd, UINT64 backlog);
UINT64 LinuxInetAccept(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 address_length, UINT64 flags);
UINT64 LinuxInetReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd, UINT64 buffer, UINT64 length, UINT64 flags);
UINT64 LinuxInetSendToRecvFrom(PKPROCESS process, BOOLEAN write, UINT64 fd, UINT64 buffer, UINT64 length, UINT64 flags, UINT64 address, UINT64 address_length);
UINT64 LinuxInetName(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 address_length, BOOLEAN peer);
UINT64 LinuxInetOption(PKPROCESS process, UINT64 fd, UINT64 level, UINT64 option, UINT64 value, UINT64 length, BOOLEAN get);
UINT64 LinuxInetShutdown(PKPROCESS process, UINT64 fd, UINT64 how);
UINT64 LinuxInetSetNonBlocking(struct _LINUX_SOCKET *socket, BOOLEAN enabled);
UINT32 LinuxInetPoll(struct _LINUX_SOCKET *socket);
void LinuxInetClose(struct _LINUX_SOCKET *socket);
UINT64 LinuxInetSendMessage(struct _LINUX_SOCKET *socket, const void *buffer, UINT64 length, UINT64 flags, UINT64 address, UINT64 address_length);
UINT64 LinuxInetReceiveMessage(struct _LINUX_SOCKET *socket, void *buffer, UINT64 length, UINT64 flags, UINT64 address, UINT64 address_length);

LINUX_SOCKET *LinuxLocalConnect(const char *name, UINT16 length);
BOOLEAN LinuxLocalConnectable(const char *name, UINT16 length);
UINT64 LinuxLocalTransfer(LINUX_SOCKET *socket, BOOLEAN write, void *data, UINT64 length, UINT64 flags);
#endif
