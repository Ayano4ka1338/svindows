#ifndef SVINDOWS_LINUX_SOCKET_INET_H
#define SVINDOWS_LINUX_SOCKET_INET_H

#include "SocketPrivate.h"

#define LINUX_SOCK_DGRAM 2u
#define LINUX_SOCK_RAW 3u
#define LINUX_IPPROTO_ICMP 1u
#define LINUX_IPPROTO_TCP 6u
#define LINUX_IPPROTO_UDP 17u

UINT64 LinuxInetSocketCreate(PKPROCESS process, UINT64 type, UINT64 protocol);
UINT64 LinuxInetBind(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 length);
UINT64 LinuxInetConnect(PKPROCESS process, UINT64 fd, UINT64 address, UINT64 length);
UINT64 LinuxInetListen(PKPROCESS process, UINT64 fd, UINT64 backlog);
UINT64 LinuxInetAccept(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 address_length, UINT64 flags);
UINT64 LinuxInetReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                          UINT64 buffer, UINT64 length, UINT64 flags);
UINT64 LinuxInetSendToRecvFrom(PKPROCESS process, BOOLEAN write, UINT64 fd,
                               UINT64 buffer, UINT64 length, UINT64 flags,
                               UINT64 address, UINT64 address_length);
UINT64 LinuxInetName(PKPROCESS process, UINT64 fd, UINT64 address,
                     UINT64 address_length, BOOLEAN peer);
UINT64 LinuxInetOption(PKPROCESS process, UINT64 fd, UINT64 level,
                       UINT64 option, UINT64 value, UINT64 length, BOOLEAN get);
UINT64 LinuxInetShutdown(PKPROCESS process, UINT64 fd, UINT64 how);
UINT64 LinuxInetSetNonBlocking(struct _LINUX_SOCKET *socket, BOOLEAN enabled);
UINT32 LinuxInetPoll(struct _LINUX_SOCKET *socket);
void LinuxInetClose(struct _LINUX_SOCKET *socket);
UINT64 LinuxInetSendMessage(struct _LINUX_SOCKET *socket,
                            const void *buffer, UINT64 length, UINT64 flags,
                            UINT64 address, UINT64 address_length);
UINT64 LinuxInetReceiveMessage(struct _LINUX_SOCKET *socket,
                               void *buffer, UINT64 length, UINT64 flags,
                               UINT64 address, UINT64 address_length);

#endif
