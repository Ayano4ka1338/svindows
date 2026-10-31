/* AF_INET socket bridge: Linux x86-64 socket syscalls backed by lwIP's
 * BSD socket API.  The Linux descriptor table owns these wrappers; lwIP
 * owns protocol state, packet queues, retransmission and timers. */
#include "SocketInet.h"
#include <stddef.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <svindows/Ex.h>
#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include <svindows/Net.h>

#define LWIP_O_NONBLOCK 0x01
#define LINUX_F_SETFL 4
#define LINUX_MSG_PEEK 0x2u
#define LINUX_MSG_DONTWAIT 0x40u
#define LINUX_MSG_NOSIGNAL 0x4000u
#define LINUX_SOL_SOCKET 1u
#define LINUX_SO_TYPE 3u
#define LINUX_SO_ERROR 4u
#define LINUX_SO_DOMAIN 39u
#define LINUX_SO_PROTOCOL 38u
#define LINUX_POLLIN 0x0001u
#define LINUX_POLLOUT 0x0004u
#define LINUX_POLLERR 0x0008u
#define LINUX_POLLHUP 0x0010u
#define LINUX_POLLNVAL 0x0020u

/* lwIP 2.2.1 uses a different poll bit ABI than Linux:
 *   lwIP: POLLIN=0x01, POLLOUT=0x02, POLLERR=0x04, POLLNVAL=0x08
 *   Linux: POLLIN=0x01, POLLOUT=0x04, POLLERR=0x08, POLLHUP=0x10
 * Never pass Linux event masks directly to lwip_poll or return lwIP's
 * revents unchanged to Linux poll/epoll users. */
#define LWIP_POLLIN  0x0001u
#define LWIP_POLLOUT 0x0002u
#define LWIP_POLLERR 0x0004u
#define LWIP_POLLNVAL 0x0008u
#define LWIP_POLLHUP 0x0200u

extern int errno;

typedef struct PACKED _LINUX_SOCKADDR_IN {
    UINT16 Family;
    UINT16 Port;
    UINT32 Address;
    UINT8 Zero[8];
} LINUX_SOCKADDR_IN;

_Static_assert(sizeof(LINUX_SOCKADDR_IN) == 16, "Linux sockaddr_in ABI");

static UINT64 inet_error(void)
{
    switch (errno) {
    case 0: return 0;
    case 1: return LinuxError(LINUX_EPERM);
    case 2: return LinuxError(LINUX_ENOENT);
    case 4: return LinuxError(4); /* EINTR */
    case 5: return LinuxError(LINUX_EIO);
    case 9: return LinuxError(LINUX_EBADF);
    case 11: return LinuxError(LINUX_EAGAIN);
    case 12: return LinuxError(LINUX_ENOMEM);
    case 13: return LinuxError(LINUX_EACCES);
    case 14: return LinuxError(LINUX_EFAULT);
    case 22: return LinuxError(LINUX_EINVAL);
    case 23: return LinuxError(23); /* ENFILE */
    case 24: return LinuxError(LINUX_EMFILE);
    case 32: return LinuxError(LINUX_EPIPE);
    case 88: return LinuxError(LINUX_ENOTSOCK);
    case 89: return LinuxError(89); /* EDESTADDRREQ */
    case 90: return LinuxError(90); /* EMSGSIZE */
    case 92: return LinuxError(92); /* ENOPROTOOPT */
    case 93: return LinuxError(93); /* EPROTONOSUPPORT */
    case 95: return LinuxError(LINUX_EOPNOTSUPP);
    case 97: return LinuxError(LINUX_EAFNOSUPPORT);
    case 98: return LinuxError(LINUX_EADDRINUSE);
    case 99: return LinuxError(99); /* EADDRNOTAVAIL */
    case 100: return LinuxError(100); /* ENETDOWN */
    case 101: return LinuxError(101); /* ENETUNREACH */
    case 103: return LinuxError(103); /* ECONNABORTED */
    case 104: return LinuxError(104); /* ECONNRESET */
    case 105: return LinuxError(105); /* ENOBUFS */
    case 106: return LinuxError(LINUX_EISCONN);
    case 107: return LinuxError(LINUX_ENOTCONN);
    case 110: return LinuxError(LINUX_ETIMEDOUT);
    case 111: return LinuxError(LINUX_ECONNREFUSED);
    case 113: return LinuxError(113); /* EHOSTUNREACH */
    case 114: return LinuxError(114); /* EALREADY */
    case 115: return LinuxError(115); /* EINPROGRESS */
    default:
        /* arch/cc.h exposes Linux/POSIX errno numbers, so preserve any
           additional lwIP socket errno instead of collapsing it to EIO. */
        if (errno > 0 && errno <= 4095)
            return LinuxError((UINT32)errno);
        return LinuxError(LINUX_EIO);
    }
}

/* lwIP's socket/netconn send path may defer payload consumption to
 * tcpip_thread.  A Linux user pointer is valid only in the calling process'
 * address space, while tcpip_thread runs in the System process.  Always stage
 * outbound payloads in nonpaged kernel memory before entering lwIP. */
static void *inet_stage_send_buffer(UINT64 user_buffer, UINT64 length)
{
    if (!length) return NULL;
    void *kernel_buffer = ExAllocatePoolWithTag(
        NonPagedPool, (SIZE_T)length, 'sNtL');
    if (!kernel_buffer) return NULL;
    memcpy(kernel_buffer, (const void *)(ULONG_PTR)user_buffer,
           (SIZE_T)length);
    return kernel_buffer;
}


static void inet_log_ipv4(const char *label, const struct sockaddr_in *address,
                          UINT16 port)
{
    const UINT8 *octet = (const UINT8 *)&address->sin_addr.s_addr;
    KeLog("[linux-net] %s %u.%u.%u.%u:%u\n", label,
          (unsigned)octet[0], (unsigned)octet[1],
          (unsigned)octet[2], (unsigned)octet[3],
          (unsigned)lwip_ntohs(port));
}

static UINT64 inet_socket_ref(PKPROCESS process, UINT64 fd,
                              LINUX_SOCKET **out)
{
    LINUX_SOCKET *socket = LinuxSocketReferenceFd(process, fd);
    if (!socket) return LinuxError(LINUX_ENOTSOCK);
    if (socket->Family != LINUX_AF_INET || socket->LwipFd < 0) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_EAFNOSUPPORT);
    }
    *out = socket;
    return 0;
}

static UINT64 inet_read_address(UINT64 source, UINT64 length,
                                struct sockaddr_in *destination)
{
    if (length < sizeof(LINUX_SOCKADDR_IN))
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForRead(source, sizeof(LINUX_SOCKADDR_IN)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKADDR_IN input;
    memcpy(&input, (const void *)(ULONG_PTR)source, sizeof(input));
    if (input.Family != LINUX_AF_INET)
        return LinuxError(LINUX_EAFNOSUPPORT);
    memset(destination, 0, sizeof(*destination));
    /* lwIP 2.2.1 enables BSD sa_len fields by default.  Linux user memory
       has no sin_len byte, so populate it after translating the ABI. */
    destination->sin_len = (u8_t)sizeof(*destination);
    destination->sin_family = AF_INET;
    destination->sin_port = input.Port;
    destination->sin_addr.s_addr = input.Address;
    return 0;
}

static UINT64 inet_write_address(UINT64 address, UINT64 length_pointer,
                                 const struct sockaddr_in *source,
                                 socklen_t actual_length)
{
    if (!address) return 0;
    if (!MmProbeForRead(length_pointer, sizeof(UINT32)) ||
        !MmProbeForWrite(length_pointer, sizeof(UINT32)))
        return LinuxError(LINUX_EFAULT);
    UINT32 capacity = *(UINT32 *)(ULONG_PTR)length_pointer;
    UINT32 actual = sizeof(LINUX_SOCKADDR_IN);
    UINT32 copied = capacity < actual ? capacity : actual;
    if (copied && !MmProbeForWrite(address, copied))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKADDR_IN output;
    memset(&output, 0, sizeof(output));
    output.Family = LINUX_AF_INET;
    output.Port = source->sin_port;
    output.Address = source->sin_addr.s_addr;
    if (copied) memcpy((void *)(ULONG_PTR)address, &output, copied);
    *(UINT32 *)(ULONG_PTR)length_pointer = actual_length < actual ?
        (UINT32)actual_length : actual;
    return 0;
}

UINT64 LinuxInetSocketCreate(PKPROCESS process, UINT64 type, UINT64 protocol)
{
    /* Do not call lwIP before tcpip_init/netif registration. In that state
       lwIP's uninitialized pools report ENOBUFS, hiding the actual link error. */
    if (!NetIsStackReady())
        return LinuxError(LINUX_ENETDOWN);

    UINT64 flags = type & (LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC);
    UINT64 base_type = type & 15u;
    if (type & ~(UINT64)(15u | LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
        return LinuxError(LINUX_EINVAL);
    if (base_type != LINUX_SOCK_STREAM && base_type != LINUX_SOCK_DGRAM &&
        base_type != LINUX_SOCK_RAW)
        return LinuxError(LINUX_EPROTONOSUPPORT);
    if (base_type == LINUX_SOCK_STREAM && protocol != 0 &&
        protocol != LINUX_IPPROTO_TCP)
        return LinuxError(LINUX_EPROTONOSUPPORT);
    if (base_type == LINUX_SOCK_DGRAM && protocol != 0 &&
        protocol != LINUX_IPPROTO_UDP)
        return LinuxError(LINUX_EPROTONOSUPPORT);
    if (base_type == LINUX_SOCK_RAW && protocol == 0)
        return LinuxError(LINUX_EPROTONOSUPPORT);

    int lwip_fd = lwip_socket(AF_INET, (int)base_type, (int)protocol);
    if (lwip_fd < 0) {
        KeLog("[linux-net] lwIP socket create failed: family=AF_INET type=%u protocol=%u errno=%d\n",
              (unsigned)base_type, (unsigned)protocol, errno);
        return inet_error();
    }
    LINUX_SOCKET *socket = LinuxSocketAllocate();
    if (!socket) {
        (void)lwip_close(lwip_fd);
        return LinuxError(LINUX_ENOMEM);
    }
    socket->Family = LINUX_AF_INET;
    socket->SocketType = (UINT16)base_type;
    socket->Protocol = (UINT32)protocol;
    socket->LwipFd = lwip_fd;
    socket->OwnerPid = process->ProcessId;
    socket->NonBlocking = !!(flags & LINUX_SOCK_NONBLOCK);
    if (socket->NonBlocking && lwip_fcntl(lwip_fd, LINUX_F_SETFL,
                                          LWIP_O_NONBLOCK) < 0) {
        (void)lwip_close(lwip_fd);
        socket->LwipFd = -1;
        LinuxSocketRelease(socket);
        return inet_error();
    }
    UINT64 result = LinuxSocketInstall(process, socket, flags);
    if ((INT64)result < 0) {
        (void)lwip_close(lwip_fd);
        socket->LwipFd = -1;
        LinuxSocketRelease(socket);
    }
    return result;
}

UINT64 LinuxInetBind(PKPROCESS process, UINT64 fd, UINT64 address,
                     UINT64 length)
{
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    struct sockaddr_in target;
    result = inet_read_address(address, length, &target);
    if (!result && lwip_bind(socket->LwipFd,
            (const struct sockaddr *)&target, sizeof(target)) < 0)
        result = inet_error();
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetConnect(PKPROCESS process, UINT64 fd, UINT64 address,
                        UINT64 length)
{
    static volatile UINT32 trace_count;
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    struct sockaddr_in target;
    result = inet_read_address(address, length, &target);
    if (!result) {
        UINT32 trace = __sync_fetch_and_add(&trace_count, 1u);
        if (trace < 48u)
            KeLog("[linux-net] connect begin fd=%d addr=%08x port=%u\n",
                  socket->LwipFd, (unsigned)target.sin_addr.s_addr,
                  (unsigned)lwip_ntohs(target.sin_port));
        if (lwip_connect(socket->LwipFd,
                (const struct sockaddr *)&target, sizeof(target)) < 0) {
            int saved_errno = errno;
            result = inet_error();
            if (trace < 48u)
                KeLog("[linux-net] connect end fd=%d result=%08x errno=%d\n",
                      socket->LwipFd, (unsigned)result, saved_errno);
        } else if (trace < 48u) {
            KeLog("[linux-net] connect end fd=%d result=0\n", socket->LwipFd);
        }
    }
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetListen(PKPROCESS process, UINT64 fd, UINT64 backlog)
{
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    if (socket->SocketType != LINUX_SOCK_STREAM)
        result = LinuxError(LINUX_EOPNOTSUPP);
    else if (lwip_listen(socket->LwipFd,
                         backlog > 128 ? 128 : (int)backlog) < 0)
        result = inet_error();
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetAccept(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 address_length, UINT64 flags)
{
    if (flags & ~(UINT64)(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC))
        return LinuxError(LINUX_EINVAL);
    LINUX_SOCKET *listener;
    UINT64 result = inet_socket_ref(process, fd, &listener);
    if (result) return result;
    struct sockaddr_in peer;
    socklen_t peer_length = sizeof(peer);
    int accepted = lwip_accept(listener->LwipFd,
        address ? (struct sockaddr *)&peer : NULL,
        address ? &peer_length : NULL);
    if (accepted < 0) {
        result = inet_error();
        LinuxSocketRelease(listener);
        return result;
    }
    LINUX_SOCKET *socket = LinuxSocketAllocate();
    if (!socket) {
        (void)lwip_close(accepted);
        LinuxSocketRelease(listener);
        return LinuxError(LINUX_ENOMEM);
    }
    socket->Family = LINUX_AF_INET;
    socket->SocketType = LINUX_SOCK_STREAM;
    socket->Protocol = LINUX_IPPROTO_TCP;
    socket->LwipFd = accepted;
    socket->OwnerPid = process->ProcessId;
    socket->NonBlocking = !!(flags & LINUX_SOCK_NONBLOCK);
    if (socket->NonBlocking && lwip_fcntl(accepted, LINUX_F_SETFL,
                                          LWIP_O_NONBLOCK) < 0) {
        (void)lwip_close(accepted);
        socket->LwipFd = -1;
        LinuxSocketRelease(socket);
        LinuxSocketRelease(listener);
        return inet_error();
    }
    result = LinuxSocketInstall(process, socket, flags);
    if ((INT64)result < 0) {
        (void)lwip_close(accepted);
        socket->LwipFd = -1;
        LinuxSocketRelease(socket);
    } else if (address) {
        UINT64 address_result = inet_write_address(address, address_length,
                                                    &peer, peer_length);
        if (address_result) {
            LinuxClose(process, result);
            result = address_result;
        }
    }
    LinuxSocketRelease(listener);
    return result;
}

static int inet_translate_flags(UINT64 flags)
{
    int translated = 0;
    if (flags & LINUX_MSG_PEEK) translated |= MSG_PEEK;
    if (flags & LINUX_MSG_DONTWAIT) translated |= MSG_DONTWAIT;
#ifdef MSG_NOSIGNAL
    if (flags & LINUX_MSG_NOSIGNAL) translated |= MSG_NOSIGNAL;
#endif
    return translated;
}

/* Linux and lwIP intentionally use different SOL_SOCKET option numbers.
 * Translate the Linux ABI rather than leaking lwIP's private values to apps. */
static int inet_socket_level(int linux_level)
{
    return linux_level == LINUX_SOL_SOCKET ? SOL_SOCKET : linux_level;
}

static int inet_socket_option(int linux_level, int linux_option)
{
    /* Linux's TCP option numbers are not the lwIP BSD-socket values.
       In lwIP 2.2.1: KEEPALIVE=2, KEEPIDLE=3, KEEPINTVL=4, KEEPCNT=5;
       Linux uses MAXSEG=2, KEEPIDLE=4, KEEPINTVL=5, KEEPCNT=6. */
    if (linux_level == LINUX_IPPROTO_TCP) {
        switch (linux_option) {
#ifdef TCP_NODELAY
        case 1: return TCP_NODELAY;
#endif
#ifdef TCP_KEEPIDLE
        case 4: return TCP_KEEPIDLE;
#endif
#ifdef TCP_KEEPINTVL
        case 5: return TCP_KEEPINTVL;
#endif
#ifdef TCP_KEEPCNT
        case 6: return TCP_KEEPCNT;
#endif
        default: return -1;
        }
    }
    if (linux_level != LINUX_SOL_SOCKET) return linux_option;
    switch (linux_option) {
#ifdef SO_REUSEADDR
    case 2: return SO_REUSEADDR;
#endif
#ifdef SO_TYPE
    case 3: return SO_TYPE;
#endif
#ifdef SO_ACCEPTCONN
    case 30: return SO_ACCEPTCONN;
#endif
#ifdef SO_ERROR
    case 4: return SO_ERROR;
#endif
#ifdef SO_BROADCAST
    case 6: return SO_BROADCAST;
#endif
#ifdef SO_SNDBUF
    case 7: return SO_SNDBUF;
#endif
#ifdef SO_RCVBUF
    case 8: return SO_RCVBUF;
#endif
#ifdef SO_RCVLOWAT
    case 18: return SO_RCVLOWAT;
#endif
#ifdef SO_SNDLOWAT
    case 19: return SO_SNDLOWAT;
#endif
#ifdef SO_KEEPALIVE
    case 9: return SO_KEEPALIVE;
#endif
#ifdef SO_LINGER
    case 13: return SO_LINGER;
#endif
#ifdef SO_RCVTIMEO
    case 20: return SO_RCVTIMEO;
#endif
#ifdef SO_SNDTIMEO
    case 21: return SO_SNDTIMEO;
#endif
    default: return -1;
    }
}

UINT64 LinuxInetReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                          UINT64 buffer, UINT64 length, UINT64 flags)
{
    static volatile UINT32 io_trace_count;
    UINT64 supported = LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL |
                       (write ? 0 : LINUX_MSG_PEEK);
    if (flags & ~supported) return LinuxError(LINUX_EOPNOTSUPP);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (length && !(write ? MmProbeForRead(buffer, length) :
                           MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    void *staged = write ? inet_stage_send_buffer(buffer, length) : NULL;
    if (write && length && !staged) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_ENOMEM);
    }
    UINT32 trace = __sync_fetch_and_add(&io_trace_count, 1u);
    if (trace < 96u)
        KeLog("[linux-net] %s begin fd=%d length=%u flags=0x%llx\n",
              write ? "send" : "recv", socket->LwipFd,
              (unsigned)length, (unsigned long long)flags);
    int transferred = write ?
        lwip_send(socket->LwipFd, staged, (size_t)length,
                  inet_translate_flags(flags)) :
        lwip_recv(socket->LwipFd, (void *)(ULONG_PTR)buffer,
                  (size_t)length, inet_translate_flags(flags));
    int saved_errno = errno;
    result = transferred < 0 ? inet_error() : (UINT64)transferred;
    if (trace < 96u)
        KeLog("[linux-net] %s end fd=%d transferred=%d errno=%d result=0x%llx\n",
              write ? "send" : "recv", socket->LwipFd, transferred,
              saved_errno, (unsigned long long)result);
    if (staged) ExFreePool(staged);
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetSendToRecvFrom(PKPROCESS process, BOOLEAN write, UINT64 fd,
                               UINT64 buffer, UINT64 length, UINT64 flags,
                               UINT64 address, UINT64 address_length)
{
    UINT64 supported = LINUX_MSG_DONTWAIT | LINUX_MSG_NOSIGNAL |
                       (write ? 0 : LINUX_MSG_PEEK);
    if (flags & ~supported) return LinuxError(LINUX_EOPNOTSUPP);
    if (length > LINUX_IO_MAX) return LinuxError(LINUX_EINVAL);
    if (length && !(write ? MmProbeForRead(buffer, length) :
                           MmProbeForWrite(buffer, length)))
        return LinuxError(LINUX_EFAULT);
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    struct sockaddr_in target, source;
    socklen_t source_length = sizeof(source);
    struct sockaddr *destination = NULL;
    socklen_t destination_length = 0;
    if (write && address) {
        result = inet_read_address(address, address_length, &target);
        if (result) { LinuxSocketRelease(socket); return result; }
        destination = (struct sockaddr *)&target;
        destination_length = sizeof(target);
    }
    int transferred;
    void *staged = write ? inet_stage_send_buffer(buffer, length) : NULL;
    if (write && length && !staged) {
        LinuxSocketRelease(socket);
        return LinuxError(LINUX_ENOMEM);
    }
    static volatile UINT32 datagram_trace_count;
    UINT32 trace = __sync_fetch_and_add(&datagram_trace_count, 1u);
    if (trace < 64u) {
        if (write && address) {
            KeLog("[linux-net] sendto begin fd=%d length=%u flags=0x%llx\n",
                  socket->LwipFd, (unsigned)length,
                  (unsigned long long)flags);
            inet_log_ipv4("sendto dst", &target, target.sin_port);
        }
        else
            KeLog("[linux-net] %s begin fd=%d length=%u flags=0x%llx\n",
                  write ? "send" : "recvfrom", socket->LwipFd,
                  (unsigned)length, (unsigned long long)flags);
    }
    if (write) {
        transferred = address ?
            lwip_sendto(socket->LwipFd, staged,
                        (size_t)length, inet_translate_flags(flags),
                        destination, destination_length) :
            lwip_send(socket->LwipFd, staged, (size_t)length,
                      inet_translate_flags(flags));
    } else {
        transferred = lwip_recvfrom(socket->LwipFd,
            (void *)(ULONG_PTR)buffer, (size_t)length,
            inet_translate_flags(flags), address ? (struct sockaddr *)&source : NULL,
            address ? &source_length : NULL);
        if (transferred >= 0 && address)
            result = inet_write_address(address, address_length, &source,
                                         source_length);
    }
    int saved_errno = errno;
    if (transferred < 0) result = inet_error();
    else if (!result) result = (UINT64)transferred;
    if (trace < 64u) {
        KeLog("[linux-net] %s end fd=%d transferred=%d errno=%d result=0x%llx\n",
              write ? (address ? "sendto" : "send") : "recvfrom",
              socket->LwipFd, transferred, saved_errno,
              (unsigned long long)result);
        if (!write && transferred >= 0 && address)
            inet_log_ipv4("recvfrom src", &source, source.sin_port);
    }
    if (staged) ExFreePool(staged);
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetName(PKPROCESS process, UINT64 fd, UINT64 address,
                     UINT64 address_length, BOOLEAN peer)
{
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    struct sockaddr_in output;
    socklen_t actual = sizeof(output);
    int status = peer ? lwip_getpeername(socket->LwipFd,
                      (struct sockaddr *)&output, &actual) :
                        lwip_getsockname(socket->LwipFd,
                      (struct sockaddr *)&output, &actual);
    if (status < 0) result = inet_error();
    else result = inet_write_address(address, address_length, &output, actual);
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetOption(PKPROCESS process, UINT64 fd, UINT64 level,
                       UINT64 option, UINT64 value, UINT64 length, BOOLEAN get)
{
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    int lwip_level = inet_socket_level((int)level);
    int lwip_option = inet_socket_option((int)level, (int)option);
    if (get) {
        if (!MmProbeForRead(length, sizeof(UINT32)) ||
            !MmProbeForWrite(length, sizeof(UINT32)))
            result = LinuxError(LINUX_EFAULT);
        else {
            socklen_t option_length = *(UINT32 *)(ULONG_PTR)length;
            if (option_length > 4096 ||
                (option_length && !MmProbeForWrite(value, option_length)))
                result = LinuxError(LINUX_EFAULT);
            else if (level == LINUX_SOL_SOCKET &&
                     (option == LINUX_SO_DOMAIN ||
                      option == LINUX_SO_PROTOCOL)) {
                if (option_length < sizeof(INT32)) result = LinuxError(LINUX_EINVAL);
                else {
                    *(INT32 *)(ULONG_PTR)value = option == LINUX_SO_DOMAIN ?
                        (INT32)LINUX_AF_INET : (INT32)socket->Protocol;
                    *(UINT32 *)(ULONG_PTR)length = sizeof(INT32);
                }
            } else if (lwip_option < 0)
                result = LinuxError(LINUX_EOPNOTSUPP);
            else if (lwip_getsockopt(socket->LwipFd, lwip_level, lwip_option,
                         (void *)(ULONG_PTR)value, &option_length) < 0)
                result = inet_error();
            else
                *(UINT32 *)(ULONG_PTR)length = option_length;
        }
    } else {
        if (length > 4096 || (length && !MmProbeForRead(value, length)))
            result = LinuxError(LINUX_EFAULT);
        else if (lwip_option < 0)
            result = LinuxError(LINUX_EOPNOTSUPP);
        else if (lwip_setsockopt(socket->LwipFd, lwip_level, lwip_option,
                                 (const void *)(ULONG_PTR)value,
                                 (socklen_t)length) < 0)
            result = inet_error();
    }
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetShutdown(PKPROCESS process, UINT64 fd, UINT64 how)
{
    if (how > 2) return LinuxError(LINUX_EINVAL);
    LINUX_SOCKET *socket;
    UINT64 result = inet_socket_ref(process, fd, &socket);
    if (result) return result;
    result = lwip_shutdown(socket->LwipFd, (int)how) < 0 ? inet_error() : 0;
    LinuxSocketRelease(socket);
    return result;
}

UINT64 LinuxInetSetNonBlocking(struct _LINUX_SOCKET *socket, BOOLEAN enabled)
{
    if (!socket || socket->Family != LINUX_AF_INET || socket->LwipFd < 0)
        return LinuxError(LINUX_EBADF);
    if (lwip_fcntl(socket->LwipFd, LINUX_F_SETFL,
                   enabled ? LWIP_O_NONBLOCK : 0) < 0)
        return inet_error();
    socket->NonBlocking = enabled;
    return 0;
}

UINT32 LinuxInetPoll(struct _LINUX_SOCKET *socket)
{
#if LWIP_SOCKET_POLL
    struct pollfd descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd = socket->LwipFd;
    /* lwIP POLLOUT is 0x02; Linux POLLOUT is 0x04. */
    descriptor.events = (short)(LWIP_POLLIN | LWIP_POLLOUT);
    int result = lwip_poll(&descriptor, 1, 0);
    if (result <= 0) {
        static volatile UINT32 empty_poll_trace_count;
        UINT32 trace_index = __sync_fetch_and_add(&empty_poll_trace_count, 1u);
        if (trace_index < 32u)
            KeLog("[linux-net] lwip poll fd=%d result=%d revents=0x%04x errno=%d\n",
                  socket->LwipFd, result, (unsigned)(UINT16)descriptor.revents,
                  errno);
        return 0;
    }

    UINT16 events = (UINT16)descriptor.revents;
    UINT32 ready = 0;
    if (events & LWIP_POLLIN) ready |= LINUX_POLLIN;
    if (events & LWIP_POLLOUT) ready |= LINUX_POLLOUT;
    if (events & LWIP_POLLERR) ready |= LINUX_POLLERR;
    if (events & LWIP_POLLNVAL) ready |= LINUX_POLLNVAL;
    if (events & LWIP_POLLHUP) ready |= LINUX_POLLHUP;

    /* Keep the connect-completion decision visible without flooding the log. */
    {
        static volatile UINT32 poll_trace_count;
        UINT32 trace_index = __sync_fetch_and_add(&poll_trace_count, 1u);
        if (trace_index < 24u)
            KeLog("[linux-net] lwip poll fd=%d revents=0x%04x linux=0x%04x\n",
                  socket->LwipFd, (unsigned)events, (unsigned)ready);
    }
    return ready;
#else
    (void)socket;
    return 0;
#endif
}

void LinuxInetClose(struct _LINUX_SOCKET *socket)
{
    if (!socket || socket->LwipFd < 0) return;
    int fd = socket->LwipFd;
    socket->LwipFd = -1;
    (void)lwip_close(fd);
}

UINT64 LinuxInetSendMessage(struct _LINUX_SOCKET *socket,
                            const void *buffer, UINT64 length, UINT64 flags,
                            UINT64 address, UINT64 address_length)
{
    if (!socket || socket->Family != LINUX_AF_INET || length > LINUX_IO_MAX)
        return LinuxError(LINUX_EINVAL);
    struct sockaddr_in target;
    if (address) {
        UINT64 status = inet_read_address(address, address_length, &target);
        if (status) return status;
        int sent = lwip_sendto(socket->LwipFd, buffer, (size_t)length,
                              inet_translate_flags(flags),
                              (const struct sockaddr *)&target, sizeof(target));
        return sent < 0 ? inet_error() : (UINT64)sent;
    }
    int sent = lwip_send(socket->LwipFd, buffer, (size_t)length,
                         inet_translate_flags(flags));
    return sent < 0 ? inet_error() : (UINT64)sent;
}

UINT64 LinuxInetReceiveMessage(struct _LINUX_SOCKET *socket,
                               void *buffer, UINT64 length, UINT64 flags,
                               UINT64 address, UINT64 address_length)
{
    if (!socket || socket->Family != LINUX_AF_INET || length > LINUX_IO_MAX)
        return LinuxError(LINUX_EINVAL);
    struct sockaddr_in source;
    socklen_t source_length = sizeof(source);
    int received = lwip_recvfrom(socket->LwipFd, buffer, (size_t)length,
        inet_translate_flags(flags), address ? (struct sockaddr *)&source : NULL,
        address ? &source_length : NULL);
    if (received < 0) return inet_error();
    if (address) {
        UINT64 status = inet_write_address(address, address_length, &source,
                                             source_length);
        if (status) return status;
    }
    return (UINT64)received;
}
