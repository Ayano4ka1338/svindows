/* Linux process identity and the x86-64 UTS ABI. */
#include "Internal.h"
#include <svindows/Rtl.h>

#define LINUX_UTS_FIELD_SIZE 65u

typedef struct _LINUX_UTSNAME {
    char Sysname[LINUX_UTS_FIELD_SIZE];
    char Nodename[LINUX_UTS_FIELD_SIZE];
    char Release[LINUX_UTS_FIELD_SIZE];
    char Version[LINUX_UTS_FIELD_SIZE];
    char Machine[LINUX_UTS_FIELD_SIZE];
    char Domainname[LINUX_UTS_FIELD_SIZE];
} LINUX_UTSNAME;

_Static_assert(sizeof(LINUX_UTSNAME) == 390, "Linux UTS ABI size");

UINT64 LinuxUname(UINT64 destination)
{
    if (!MmProbeForWrite(destination, sizeof(LINUX_UTSNAME)))
        return LinuxError(LINUX_EFAULT);

    LINUX_UTSNAME result;
    memset(&result, 0, sizeof(result));
    /* This is a Linux-compatible ABI on Svindows; expose the host OS identity
     * so tools such as fastfetch do not mislabel the system as Linux. */
    memcpy(result.Sysname, "Svindows", sizeof("Svindows"));
    memcpy(result.Nodename, "svindows", sizeof("svindows"));
    memcpy(result.Release, "0.6.0", sizeof("0.6.0"));
    memcpy(result.Version, "Svindows 0.6.0", sizeof("Svindows 0.6.0"));
    memcpy(result.Machine, "x86_64", sizeof("x86_64"));
    memcpy((void *)(ULONG_PTR)destination, &result, sizeof(result));
    return 0;
}

static BOOLEAN linux_root_ownership(UINT64 uid, UINT64 gid)
{
    return ((UINT32)uid == 0 || (UINT32)uid == ~0u) &&
           ((UINT32)gid == 0 || (UINT32)gid == ~0u);
}

UINT64 LinuxChownPath(PKPROCESS process, UINT64 path, UINT64 uid, UINT64 gid)
{
    if (!linux_root_ownership(uid, gid)) return LinuxError(LINUX_EPERM);
    /* FAT metadata has no ownership fields; Linux stat reports 0:0. */
    return LinuxAccess(process, path, 0);
}

UINT64 LinuxFchown(PKPROCESS process, UINT64 fd, UINT64 uid, UINT64 gid)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    return linux_root_ownership(uid, gid) ? 0 : LinuxError(LINUX_EPERM);
}
