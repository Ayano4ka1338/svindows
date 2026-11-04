/* First-stage x86-64 Linux syscall ABI. Linux errors are negative errno. */
#include "Internal.h"
#include "SocketPrivate.h"

UINT64 LinuxSystemCallDispatch(UINT64 number, const UINT64 args[6])
{
    PKTHREAD thread = KeGetCurrentThread();
    PKPROCESS process = thread ? thread->Process : NULL;
    if (!process || !process->LinuxContext) return LinuxError(LINUX_ENOSYS);
    switch (number) {
    case 0: /* read */
    case 1: /* write */
        return LinuxReadWrite(process, number == 1, args[0], args[1], args[2]);
    case 2: /* open */
        return LinuxOpenAt(process, (UINT64)(INT64)-100, args[0], args[1]);
    case 3: /* close */
        return LinuxClose(process, args[0]);
    case 4: /* stat */
    case 6: /* lstat; FAT has no symbolic links */
        return LinuxStatPath(process, (UINT64)(INT64)-100, args[0],
                             args[1], number == 6 ? 0x100u : 0);
    case 7: /* poll */
        return LinuxPoll(process, args[0], args[1], (INT64)args[2]);
    case 23: /* select */
        return LinuxSelect(process, args[0], args[1], args[2], args[3],
                           args[4], FALSE);
    case 5: /* fstat */
        return LinuxFstat(process, args[0], args[1]);
    case 8: /* lseek */
        return LinuxSeek(process, args[0], (INT64)args[1], args[2]);
    case 9: /* mmap */
        return LinuxMmap(process, args);
    case 10: /* mprotect */
        return LinuxMprotect(process, args[0], args[1], args[2]);
    case 11: /* munmap */
        return LinuxMunmap(process, args[0], args[1]);
    case 12: /* brk */
        return LinuxBrk(process, args[0]);
    case 13: /* rt_sigaction */
    case 14: /* rt_sigprocmask */
    case 15: /* rt_sigreturn */
    case 131: /* sigaltstack */
        /* Signals are not delivered in Svindows. Accepting the registration
         * lets a program (Xorg installs handlers for SIGSEGV/SIGBUS/...) finish
         * its startup instead of aborting on ENOSYS. The action and mask are
         * discarded; we do not write back any "old" action, which callers here
         * ignore. A real fault still terminates the process via the kernel
         * trap path. */
        return 0;
    case 16: { /* ioctl: requests are 32-bit and may be sign-extended by libc */
        UINT64 command = (UINT32)args[1];
        if (args[0] < LINUX_MAX_FDS &&
            process->LinuxContext->Files[args[0]].Used &&
            process->LinuxContext->Files[args[0]].Kind == LINUX_FD_PTY)
            return LinuxPtyIoctl(process, args[0], command, args[2]);
        if (args[0] < LINUX_MAX_FDS &&
            process->LinuxContext->Files[args[0]].Used &&
            process->LinuxContext->Files[args[0]].Kind == LINUX_FD_TTY)
            return LinuxVtIoctl(process, args[0], command, args[2]);
        if (args[0] < LINUX_MAX_FDS &&
            process->LinuxContext->Files[args[0]].Used &&
            process->LinuxContext->Files[args[0]].Kind == LINUX_FD_EVENT)
            return LinuxEventIoctl(process, args[0], command, args[2]);
        return LinuxFramebufferIoctl(process, args[0], command, args[2]);
    }
    case 17: /* pread64 */
        return LinuxPread(process, args[0], args[1], args[2], args[3]);
    case 19: /* readv */
        return LinuxReadv(process, args[0], args[1], args[2]);
    case 21: /* access */
        return LinuxAccess(process, args[0], args[1]);
    case 20:  /* writev */
        return LinuxWritev(process, args[0], args[1], args[2]);
    case 22: /* pipe */
        return LinuxPipe(process, args[0], 0);
    case 24:  /* sched_yield */
        KeYield();
        return 0;
    case 25: /* mremap */
        return LinuxMremap(process, args);
    case 32: /* dup */
        return LinuxDup(process, args[0], 0, 0, FALSE, FALSE);
    case 33: /* dup2 */
        return LinuxDup(process, args[0], args[1], 0, TRUE, FALSE);
    case 35: /* nanosleep */
        return LinuxNanosleep(args[0], args[1]);
    case 39:  /* getpid */
        return process->ProcessId;
    case 41: /* socket */
        return LinuxSocketCreate(process, args[0], args[1], args[2]);
    case 42: /* connect */
        return LinuxSocketConnect(process, args[0], args[1], args[2]);
    case 43: /* accept */
        return LinuxSocketAccept(process, args[0], args[1], args[2], 0);
    case 44: /* sendto */
        if (args[0] < LINUX_MAX_FDS &&
            process->LinuxContext->Files[args[0]].Used &&
            process->LinuxContext->Files[args[0]].Kind == LINUX_FD_SOCKET &&
            process->LinuxContext->Files[args[0]].Socket->Family == LINUX_AF_INET)
            return LinuxInetSendToRecvFrom(process, TRUE, args[0], args[1],
                                            args[2], args[3], args[4], args[5]);
        if (args[4]) return LinuxError(LINUX_EISCONN);
        return LinuxSocketReadWrite(process, TRUE, args[0], args[1],
                                    args[2], args[3]);
    case 45: /* recvfrom */
        if (args[0] < LINUX_MAX_FDS &&
            process->LinuxContext->Files[args[0]].Used &&
            process->LinuxContext->Files[args[0]].Kind == LINUX_FD_SOCKET &&
            process->LinuxContext->Files[args[0]].Socket->Family == LINUX_AF_INET)
            return LinuxInetSendToRecvFrom(process, FALSE, args[0], args[1],
                                            args[2], args[3], args[4], args[5]);
        {
            UINT64 result = LinuxSocketReadWrite(process, FALSE, args[0],
                                                  args[1], args[2], args[3]);
            if ((INT64)result >= 0 && args[4]) {
                UINT64 error = LinuxSocketName(process, args[0], args[4],
                                                args[5], TRUE);
                if (error && result == 0) return error;
            }
            return result;
        }
    case 46: /* sendmsg */
    case 47: /* recvmsg */
        return LinuxSocketMessage(process, number == 46, args[0],
                                  args[1], args[2]);
    case 48: /* shutdown */
        return LinuxSocketShutdown(process, args[0], args[1]);
    case 49: /* bind */
        return LinuxSocketBind(process, args[0], args[1], args[2]);
    case 50: /* listen */
        return LinuxSocketListen(process, args[0], args[1]);
    case 51: /* getsockname */
    case 52: /* getpeername */
        return LinuxSocketName(process, args[0], args[1], args[2], number == 52);
    case 53: /* socketpair */
        return LinuxSocketPair(process, args[0], args[1], args[2], args[3]);
    case 54: /* setsockopt */
    case 55: /* getsockopt */
        return LinuxSocketOption(process, args[0], args[1], args[2], args[3],
                                  args[4], number == 55);
    case 56:  /* clone: shared-address-space threads only */
        return LinuxClone(thread, args);
    case 57:  /* fork */
        return LinuxFork(thread);
    case 59:  /* execve */
        return LinuxExecve(thread, args[0], args[1], args[2]);
    case 61:  /* wait4 */
        return LinuxWait4(process, (INT64)args[0], args[1], args[2], args[3]);
    case 63:  /* uname */
        return LinuxUname(args[0]);
    case 72: /* fcntl */
        return LinuxFcntl(process, args[0], args[1], args[2]);
    case 73: /* flock */
        return LinuxFlock(process, args[0], args[1]);
    case 166: { /* umask; mode bits are advisory on the FAT-backed root */
        UINT32 previous = process->LinuxContext->UmaskInitialized ?
            process->LinuxContext->Umask : 0022u;
        process->LinuxContext->Umask = (UINT32)args[0] & 0777u;
        process->LinuxContext->UmaskInitialized = TRUE;
        return previous;
    }
    case 79: /* getcwd */
        return LinuxGetcwd(process, args[0], args[1]);
    case 80: /* chdir */
        return LinuxChdir(process, args[0]);
    case 83: /* mkdir */
        return LinuxMkdir(process, args[0], args[1]);
    case 82: /* rename */
        return LinuxRename(process, args[0], args[1]);
    case 86: /* link */
        return LinuxLink(process, args[0], args[1]);
    case 87: /* unlink */
        return LinuxUnlink(process, args[0]);
    case 88: /* symlink; regular-file targets are copied on FAT */
        return LinuxSymlink(process, args[0], args[1]);
    case 89: /* readlink */
        return LinuxReadlinkAt(process, (UINT64)(INT64)-100, args[0],
                               args[1], args[2]);
    case 90: /* chmod; mode bits are advisory on FAT */
        return LinuxChmodPath(process, args[0], args[1]);
    case 91: /* fchmod */
        return LinuxFchmod(process, args[0], args[1]);
    case 268: /* fchmodat; mode bits are advisory on FAT */
        return LinuxChmodAt(process, args[0], args[1], args[2], args[3]);
    case 92: /* chown */
    case 94: /* lchown; FAT has no symbolic links */
        return LinuxChownPath(process, args[0], args[1], args[2]);
    case 93: /* fchown */
        return LinuxFchown(process, args[0], args[1], args[2]);
    case 260: /* fchownat */
        return LinuxFchownAt(process, args[0], args[1], args[2], args[3], args[4]);
    case 261: /* futimesat */
        return LinuxFutimesAt(process, args[0], args[1], args[2]);
    case 99: /* sysinfo */
        return LinuxSysinfo(args[0]);
    case 97: /* getrlimit */
        return LinuxGetrlimit(process, args[0], args[1]);
    case 102: /* getuid */
    case 104: /* getgid */
    case 107: /* geteuid */
    case 108: /* getegid */
        return 0; /* FAT has no Unix ownership; stat reports UID/GID 0. */
    case 105: /* setuid */
    case 106: /* setgid */
        return args[0] == 0 ? 0 : LinuxError(LINUX_EPERM);
    case 117: /* setresuid */
    case 119: /* setresgid */
        return ((UINT32)args[0] == 0 || (UINT32)args[0] == ~0u) &&
               ((UINT32)args[1] == 0 || (UINT32)args[1] == ~0u) &&
               ((UINT32)args[2] == 0 || (UINT32)args[2] == ~0u) ?
               0 : LinuxError(LINUX_EPERM);
    case 109: /* setpgid */
        return LinuxSetPgId(process, args[0], args[1]);
    case 110: /* getppid */
        return process->ParentProcessId;
    case 111: /* getpgrp */
        return process->LinuxContext->ProcessGroupId;
    case 112: /* setsid */
        return LinuxSetSid(process);
    case 121: /* getpgid */
        return LinuxGetPgId(process, args[0]);
    case 124: /* getsid */
        return LinuxGetSid(process, args[0]);
    case 158: /* arch_prctl */
        return LinuxArchPrctl(thread, args[0], args[1]);
    case 186: /* gettid */
        return thread->LinuxThread->Tid;
    case 202: /* futex */
        return LinuxFutex(process, args[0], args[1], args[2], args[3]);
    case 213: /* epoll_create */
        return args[0] ? LinuxEpollCreate(process, 0) :
                         LinuxError(LINUX_EINVAL);
    case 217: /* getdents64 */
        return LinuxGetdents64(process, args[0], args[1], args[2]);
    case 218: /* set_tid_address */
        return LinuxSetTidAddress(thread, args[0]);
    case 228: /* clock_gettime */
        return LinuxClockGettime(args[0], args[1]);
    case 229: /* clock_getres */
        return LinuxClockGetres(args[0], args[1]);
    case 257: /* openat */
        return LinuxOpenAt(process, args[0], args[1], args[2]);
    case 264: /* renameat */
        return LinuxRenameAt(process, args[0], args[1], args[2], args[3]);
    case 266: /* symlinkat; regular-file targets are copied on FAT */
        return LinuxSymlinkAt(process, args[0], args[1], args[2]);
    case 262: /* newfstatat */
        return LinuxStatPath(process, args[0], args[1], args[2], args[3]);
    case 263: /* unlinkat */
        return LinuxUnlinkAt(process, args[0], args[1], args[2]);
    case 267: /* readlinkat */
        return LinuxReadlinkAt(process, args[0], args[1], args[2], args[3]);
    case 280: /* utimensat */
        return LinuxUtimensAt(process, args[0], args[1], args[2], args[3]);
    case 270: /* pselect6; masks are inert until signal delivery exists */
        return LinuxSelect(process, args[0], args[1], args[2], args[3],
                           args[4], TRUE);
    case 281: /* epoll_pwait; signal masks need Linux signal delivery */
        return args[4] ? LinuxError(LINUX_EOPNOTSUPP) :
                         LinuxEpollWait(process, args[0], args[1], args[2],
                                        (INT64)args[3]);
    case 231: /* exit_group */
        LinuxExitGroup(thread, (NTSTATUS)(UINT8)args[0]);
    case 232: /* epoll_wait */
        return LinuxEpollWait(process, args[0], args[1], args[2],
                              (INT64)args[3]);
    case 233: /* epoll_ctl */
        return LinuxEpollCtl(process, args[0], args[1], args[2], args[3]);
    case 291: /* epoll_create1 */
        return LinuxEpollCreate(process, args[0]);
    case 292: /* dup3 */
        return LinuxDup(process, args[0], args[1], args[2], TRUE, TRUE);
    case 293: /* pipe2 */
        return LinuxPipe(process, args[0], args[1]);
    case 288: /* accept4 */
        return LinuxSocketAccept(process, args[0], args[1], args[2], args[3]);
    case 302: /* prlimit64 */
        return LinuxPrlimit64(process, args[0], args[1], args[2], args[3]);
    case 318: /* getrandom */
        return LinuxGetRandom(args[0], args[1], args[2]);
    case 60:  /* exit */
        thread->ExitStatus = (NTSTATUS)(UINT8)args[0];
        KeTerminateThread();
    default:
        return LinuxError(LINUX_ENOSYS);
    }
}
