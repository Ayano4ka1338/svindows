/* Linux descriptor and status flags for native Linux file numbers. */
#include "Internal.h"
#include "SocketPrivate.h"

#define LINUX_F_GETFD 1u
#define LINUX_F_DUPFD 0u
#define LINUX_F_SETFD 2u
#define LINUX_F_GETFL 3u
#define LINUX_F_SETFL 4u
#define LINUX_FD_CLOEXEC 1u
#define LINUX_O_APPEND 0x400u
#define LINUX_O_NONBLOCK 0x800u
#define LINUX_O_LARGEFILE 0x8000u
#define LINUX_F_DUPFD_CLOEXEC 1030u

UINT64 LinuxFcntl(PKPROCESS process, UINT64 fd, UINT64 command,
                  UINT64 argument)
{
    if (fd >= LINUX_MAX_FDS || !process->LinuxContext->Files[fd].Used)
        return LinuxError(LINUX_EBADF);
    LINUX_FD_ENTRY *entry = &process->LinuxContext->Files[fd];
    if (command == LINUX_F_DUPFD || command == LINUX_F_DUPFD_CLOEXEC)
        return argument < LINUX_MAX_FDS ?
            LinuxDup(process, fd, argument,
                     command == LINUX_F_DUPFD_CLOEXEC ? 0x80000u : 0,
                     FALSE, FALSE) : LinuxError(LINUX_EINVAL);
    if (command == LINUX_F_GETFD)
        return entry->CloseOnExec ? LINUX_FD_CLOEXEC : 0;
    if (command == LINUX_F_SETFD) {
        entry->CloseOnExec = !!(argument & LINUX_FD_CLOEXEC);
        return 0;
    }
    if (command == LINUX_F_GETFL) {
        BOOLEAN nonblocking = entry->NonBlocking;
        if (entry->Kind == LINUX_FD_SOCKET)
            nonblocking = entry->Socket->NonBlocking;
        return entry->Mode | (entry->Append ? LINUX_O_APPEND : 0) |
               (nonblocking ? LINUX_O_NONBLOCK : 0);
    }
    if (command == LINUX_F_SETFL) {
        if (argument & ~(UINT64)(3u | LINUX_O_APPEND | LINUX_O_NONBLOCK |
                                 LINUX_O_LARGEFILE))
            return LinuxError(LINUX_EINVAL);
        if (entry->Kind == LINUX_FD_SOCKET) {
            UINT64 irq = KiIrqSave();
            BOOLEAN enabled = !!(argument & LINUX_O_NONBLOCK);
            if (entry->Socket->Family == LINUX_AF_INET) {
                KiIrqRestore(irq);
                UINT64 status = LinuxInetSetNonBlocking(entry->Socket, enabled);
                if (!status) entry->NonBlocking = enabled;
                return status;
            }
            entry->Socket->NonBlocking = enabled;
            entry->NonBlocking = enabled;
            KiIrqRestore(irq);
            return 0;
        }
        if (entry->Kind == LINUX_FD_TTY || entry->Kind == LINUX_FD_PTY ||
            entry->Kind == LINUX_FD_CONSOLE) {
            entry->NonBlocking = !!(argument & LINUX_O_NONBLOCK);
            return 0;
        }
        if (entry->Kind == LINUX_FD_REGULAR) {
            entry->Append = !!(argument & LINUX_O_APPEND);
            entry->NonBlocking = !!(argument & LINUX_O_NONBLOCK);
            return 0;
        }
        return LinuxError(LINUX_EINVAL);
    }
    return LinuxError(LINUX_EINVAL);
}
