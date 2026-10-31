/* Resource limits exposed by the current Linux process implementation. */
#include "Internal.h"

#define LINUX_RLIMIT_NOFILE 7u

typedef struct _LINUX_RLIMIT {
    UINT64 Current;
    UINT64 Maximum;
} LINUX_RLIMIT;

static UINT64 linux_nofile_limit(UINT64 resource, UINT64 result)
{
    if (resource != LINUX_RLIMIT_NOFILE)
        return LinuxError(LINUX_EINVAL);
    if (!MmProbeForWrite(result, sizeof(LINUX_RLIMIT)))
        return LinuxError(LINUX_EFAULT);
    LINUX_RLIMIT limit = { LINUX_MAX_FDS, LINUX_MAX_FDS };
    *(LINUX_RLIMIT *)(ULONG_PTR)result = limit;
    return 0;
}

UINT64 LinuxGetrlimit(PKPROCESS process, UINT64 resource, UINT64 result)
{
    (void)process;
    return linux_nofile_limit(resource, result);
}

UINT64 LinuxPrlimit64(PKPROCESS process, UINT64 pid, UINT64 resource,
                      UINT64 requested, UINT64 previous)
{
    if (pid && pid != process->ProcessId)
        return LinuxError(LINUX_EPERM);
    if (requested) return LinuxError(LINUX_EOPNOTSUPP);
    return previous ? linux_nofile_limit(resource, previous) :
                      LinuxError(LINUX_EFAULT);
}
