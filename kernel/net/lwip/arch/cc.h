#ifndef SVINDOWS_LWIP_CC_H
#define SVINDOWS_LWIP_CC_H

#include <svindows/Ke.h>
#include <svindows/Rtl.h>
#include <svindows/Ex.h>

#include <stddef.h>
#include <stdint.h>
/* Use the compiler target's fd_set/timeval declarations before lwIP's
 * sockets.h is parsed. netdb.c includes hosted stdlib headers later. */
#include <sys/time.h>

/* lwIP's <lwip/arch.h> defines u8_t..u64_t and mem_ptr_t from
 * <stdint.h> when LWIP_NO_STDINT_H is 0.  Do not redeclare them here:
 * on x86_64 uint64_t is unsigned long, not unsigned long long. */

#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PLATFORM_DIAG(x) do { KeLog x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) KeBugCheck(KE_PHASE0_INITIALIZATION_FAILED, (x))
#define LWIP_NO_STDINT_H 0
#define LWIP_NO_INTTYPES_H 0
#define LWIP_NO_LIMITS_H 0
#define LWIP_TIMEVAL_PRIVATE 0

/* lwIP's freestanding errno port reads these from arch/cc.h when
 * LWIP_ERRNO_STDINCLUDE is disabled. Keep the conventional Linux/POSIX
 * numeric values because the socket API exposes these to applications. */
#ifndef EPERM
#define EPERM           1
#endif
#ifndef ENOENT
#define ENOENT          2
#endif
#ifndef EIO
#define EIO             5
#endif
#ifndef ENXIO
#define ENXIO           6
#endif
#ifndef ERANGE
#define ERANGE          34
#endif
#ifndef ENOMEM
#define ENOMEM          12
#endif
#ifndef EINVAL
#define EINVAL          22
#endif
#ifndef EWOULDBLOCK
#define EWOULDBLOCK     11
#endif
#ifndef EAGAIN
#define EAGAIN          EWOULDBLOCK
#endif
#ifndef EINPROGRESS
#define EINPROGRESS     115
#endif
#ifndef EALREADY
#define EALREADY        114
#endif
#ifndef ENOBUFS
#define ENOBUFS         105
#endif
#ifndef EISCONN
#define EISCONN         106
#endif
#ifndef ENOTCONN
#define ENOTCONN        107
#endif
#ifndef ECONNABORTED
#define ECONNABORTED    103
#endif
#ifndef ECONNRESET
#define ECONNRESET      104
#endif
#ifndef EADDRINUSE
#define EADDRINUSE      98
#endif
#ifndef EHOSTUNREACH
#define EHOSTUNREACH    113
#endif

/* Additional errno values referenced by lwIP 2.2.1 socket/netdb API. */
#ifndef E2BIG
#define E2BIG           7
#endif
#ifndef ENOEXEC
#define ENOEXEC         8
#endif
#ifndef ECHILD
#define ECHILD          10
#endif
#ifndef EACCES
#define EACCES          13
#endif
#ifndef EMFILE
#define EMFILE          24
#endif
#ifndef EISDIR
#define EISDIR          21
#endif
#ifndef EBADF
#define EBADF           9
#endif
#ifndef EFAULT
#define EFAULT          14
#endif
#ifndef EBUSY
#define EBUSY           16
#endif
#ifndef ENODEV
#define ENODEV          19
#endif
#ifndef ENFILE
#define ENFILE          23
#endif
#ifndef ENOSPC
#define ENOSPC          28
#endif
#ifndef ENOSYS
#define ENOSYS          38
#endif
#ifndef EMSGSIZE
#define EMSGSIZE        90
#endif
#ifndef EOPNOTSUPP
#define EOPNOTSUPP      95
#endif
#ifndef ENOPROTOOPT
#define ENOPROTOOPT     92
#endif
#ifndef EAFNOSUPPORT
#define EAFNOSUPPORT    97
#endif
#ifndef ENOTSOCK
#define ENOTSOCK        88
#endif
#ifndef EDESTADDRREQ
#define EDESTADDRREQ    89
#endif
#ifndef EPROTOTYPE
#define EPROTOTYPE      91
#endif
#ifndef EPROTONOSUPPORT
#define EPROTONOSUPPORT 93
#endif
#ifndef EADDRNOTAVAIL
#define EADDRNOTAVAIL   99
#endif
#ifndef ENETDOWN
#define ENETDOWN        100
#endif
#ifndef ENETUNREACH
#define ENETUNREACH     101
#endif
#ifndef ENETRESET
#define ENETRESET       102
#endif
#ifndef ESHUTDOWN
#define ESHUTDOWN       108
#endif
#ifndef ETIMEDOUT
#define ETIMEDOUT       110
#endif
#ifndef ECONNREFUSED
#define ECONNREFUSED    111
#endif
#ifndef EHOSTDOWN
#define EHOSTDOWN       112
#endif
#ifndef ENOTEMPTY
#define ENOTEMPTY       39
#endif
#ifndef EPIPE
#define EPIPE           32
#endif
#ifndef EINTR
#define EINTR           4
#endif
#ifndef EEXIST
#define EEXIST          17
#endif
#ifndef EOVERFLOW
#define EOVERFLOW       75
#endif

/* Freestanding lwIP socket helpers require errno storage. */
#ifndef errno
extern int errno;
#endif

static inline uint32_t sv_lwip_rand(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return lo ^ (hi * 0x9e3779b9u) ^ (uint32_t)KeGetTickCount();
}
#define LWIP_RAND() sv_lwip_rand()

#endif
