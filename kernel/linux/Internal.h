#ifndef SVINDOWS_LINUX_INTERNAL_H
#define SVINDOWS_LINUX_INTERNAL_H

#include <svindows/Linux.h>
#include <svindows/Mm.h>

#define LINUX_EBADF 9
#define LINUX_E2BIG 7
#define LINUX_ECHILD 10
#define LINUX_ENOTSOCK 88
#define LINUX_EAFNOSUPPORT 97
#define LINUX_EPROTONOSUPPORT 93
#define LINUX_EADDRINUSE 98
#define LINUX_ECONNREFUSED 111
#define LINUX_ENOTCONN 107
#define LINUX_EISCONN 106
#define LINUX_EPIPE 32
#define LINUX_EPERM 1
#define LINUX_ENXIO 6
#define LINUX_EBUSY 16
#define LINUX_EAGAIN 11
#define LINUX_EFAULT 14
#define LINUX_EINVAL 22
#define LINUX_ENOSYS 38
#define LINUX_EIO 5
#define LINUX_ENOENT 2
#define LINUX_EACCES 13
#define LINUX_EEXIST 17
#define LINUX_EMFILE 24
#define LINUX_ENOSPC 28
#define LINUX_ERANGE 34
#define LINUX_ENAMETOOLONG 36
#define LINUX_ESPIPE 29
#define LINUX_ENOMEM 12
#define LINUX_ENOTDIR 20
#define LINUX_EISDIR 21
#define LINUX_EROFS 30
#define LINUX_ENOTTY 25
#define LINUX_IO_MAX (1024u * 1024u)
#define LINUX_ENODEV 19
#define LINUX_ENETDOWN 100
#define LINUX_EOPNOTSUPP 95
#define LINUX_ETIMEDOUT 110
#define LINUX_MAX_FDS 64u
/* apk-tools maps one small region per package-index object while loading the
 * multi-megabyte Alpine repositories. Keep enough slots for that workload. */
#define LINUX_MAP_REGIONS 8192u
#define LINUX_FD_REGULAR 0u
#define LINUX_FD_FRAMEBUFFER 1u
#define LINUX_FD_DIRECTORY 2u
#define LINUX_FD_EPOLL 3u
#define LINUX_FD_SOCKET 4u
#define LINUX_FD_EVENT 5u
#define LINUX_FD_TTY 6u
#define LINUX_FD_PTY 7u
#define LINUX_FD_PROC 8u
#define LINUX_FD_CONSOLE 9u /* inherited native CONIN$/CONOUT$ endpoint */
#define LINUX_PROC_NONE 0u
#define LINUX_PROC_UNKNOWN 1u
#define LINUX_PROC_ROOT 2u
#define LINUX_PROC_SELF 3u
#define LINUX_PROC_STATUS 4u
#define LINUX_PROC_CMDLINE 5u
#define LINUX_PROC_VERSION 6u
#define LINUX_PROC_EXE 7u
#define LINUX_PROC_CWD 8u
#define LINUX_PROC_MEMINFO 9u
#define LINUX_PROC_CPUINFO 10u
#define LINUX_MAP_OWNED 0u
#define LINUX_MAP_FRAMEBUFFER 1u

struct _LINUX_PTY;

/* A PTY endpoint published as a native File object so PE programs can share
 * the same terminal stream. */
typedef struct _LINUX_PTY_BRIDGE {
    struct _LINUX_PTY *Pty;
    BOOLEAN Master;
    BOOLEAN Windows; /* Windows-console semantics: raw input, DEL as BS */
} LINUX_PTY_BRIDGE;

typedef struct _LINUX_FD_ENTRY {
    HANDLE Handle;
    UINT8 Mode;
    BOOLEAN Used;
    BOOLEAN Append;
    BOOLEAN Owned;
    BOOLEAN CloseOnExec;
    BOOLEAN NonBlocking;
    UINT8 Kind;
    UINT8 ProcNode;
    UINT8 EventDevice;   /* /dev/input/eventN index when Kind == EVENT */
    UINT8 Terminal;      /* /dev/ttyN; zero is the active-VT alias */
    struct _LINUX_PTY *Pty;
    BOOLEAN PtyMaster;
    UINT64 Offset;
    char *DirectoryPath;
    struct _LINUX_EPOLL_CONTEXT *Epoll;
    struct _LINUX_SOCKET *Socket;
} LINUX_FD_ENTRY;

typedef struct _LINUX_EPOLL_CONTEXT {
    KEVENT Event;
    UINT32 References;
    UINT32 DescriptorReferences;
    BOOLEAN Closed;
    struct {
        struct _LINUX_SOCKET *Socket;
        struct _LINUX_PTY *PtyObject;
        PVOID PipeObject;
        UINT64 Data;
        UINT32 Events;
        UINT32 Descriptor;
        UINT8 Kind;   /* LINUX_FD_* of the watched descriptor */
        UINT8 Mode;
        BOOLEAN PtyMaster;
        BOOLEAN Used; /* a watch occupies this slot */
        BOOLEAN Disabled; /* EPOLLONESHOT has delivered an event */
        UINT32 LastReady; /* readiness snapshot for EPOLLET */
        UINT64 LastReadSequence, LastWriteSequence;
    } Watches[LINUX_MAX_FDS];
} LINUX_EPOLL_CONTEXT;

typedef struct _LINUX_MAP_REGION {
    UINT64 Base;
    UINT64 Size;
    UINT8 Kind;
} LINUX_MAP_REGION;

typedef struct _LINUX_MEMORY_CONTEXT {
    UINT64 BrkBase;
    UINT64 BrkCurrent;
    UINT64 BrkMapped;
    UINT64 NextMap;
    BOOLEAN FramebufferOwner;
    LINUX_MAP_REGION Regions[LINUX_MAP_REGIONS];
} LINUX_MEMORY_CONTEXT;

void LinuxReleaseFramebufferOwner(LINUX_MEMORY_CONTEXT *memory);

typedef struct _LINUX_PROCESS_CONTEXT {
    LINUX_MEMORY_CONTEXT Memory;
    LINUX_FD_ENTRY Files[LINUX_MAX_FDS];
    struct _LINUX_PTY *ControllingPty;
    UINT32 SessionId;
    UINT32 ProcessGroupId;
    UINT32 Umask;
    BOOLEAN UmaskInitialized;
    struct {
        HANDLE ProcessHandle;
        ULONG Pid;
    } Children[32];
} LINUX_PROCESS_CONTEXT;

/* Saved user registers for a clone child resuming after SYSCALL. RCX and
 * R11 are the two registers the x86-64 syscall instruction destroys. */
typedef struct _LINUX_CLONE_REGISTERS {
    UINT64 Rbx, Rbp, R12, R13, R14, R15;
    UINT64 Rdi, Rsi, Rdx, R8, R9, R10;
} LINUX_CLONE_REGISTERS;

typedef struct _LINUX_THREAD_CONTEXT {
    LINUX_CLONE_REGISTERS Registers;
    UINT64 ClearChildTid;
    UINT64 Tid;
    BOOLEAN CloneStart;
    BOOLEAN ExecStart;
    BOOLEAN WindowsExec; /* execve replaced the image with a PE program */
    UINT8 Padding[5];    /* keep ExecArgument at the assembly offset 120 */
    UINT64 ExecArgument; /* PE entry argument (LDR init block VA) */
    KEVENT FutexEvent;
    UINT64 WaitingFutexAddress;
} LINUX_THREAD_CONTEXT;

static inline UINT64 LinuxError(INT32 code)
{
    return (UINT64)(INT64)-code;
}

UINT64 LinuxWritev(PKPROCESS process, UINT64 fd, UINT64 vectors, UINT64 count);
UINT64 LinuxReadv(PKPROCESS process, UINT64 fd, UINT64 vectors, UINT64 count);
UINT64 LinuxPoll(PKPROCESS process, UINT64 descriptors, UINT64 count,
                INT64 timeout_ms);
UINT32 LinuxPollDescriptor(PKPROCESS process, UINT32 fd);
UINT32 LinuxEpollPoll(PKPROCESS process, UINT64 fd);
UINT64 LinuxSelect(PKPROCESS process, UINT64 nfds, UINT64 read_set,
                   UINT64 write_set, UINT64 except_set, UINT64 timeout,
                   BOOLEAN pselect);
UINT64 LinuxClockGettime(UINT64 clock_id, UINT64 destination);
UINT64 LinuxClockGetres(UINT64 clock_id, UINT64 destination);
UINT64 LinuxUtimensAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                      UINT64 times, UINT64 flags);
UINT64 LinuxFutimesAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                      UINT64 times);
UINT64 LinuxNanosleep(UINT64 requested, UINT64 remaining);
UINT64 LinuxUname(UINT64 destination);
UINT64 LinuxChownPath(PKPROCESS process, UINT64 path, UINT64 uid, UINT64 gid);
UINT64 LinuxFchown(PKPROCESS process, UINT64 fd, UINT64 uid, UINT64 gid);
UINT64 LinuxFchownAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                     UINT64 uid, UINT64 gid, UINT64 flags);
UINT64 LinuxChmodPath(PKPROCESS process, UINT64 path, UINT64 mode);
UINT64 LinuxChmodAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                    UINT64 mode, UINT64 flags);
UINT64 LinuxFchmod(PKPROCESS process, UINT64 fd, UINT64 mode);
UINT64 LinuxArchPrctl(PKTHREAD thread, UINT64 command, UINT64 value);
UINT64 LinuxClone(PKTHREAD thread, const UINT64 args[6]);
UINT64 LinuxFork(PKTHREAD parent);
UINT64 LinuxExecve(PKTHREAD thread, UINT64 path, UINT64 argv, UINT64 envp);
UINT64 LinuxWait4(PKPROCESS process, INT64 pid, UINT64 status,
                  UINT64 options, UINT64 usage);
UINT64 LinuxSetTidAddress(PKTHREAD thread, UINT64 address);
UINT64 LinuxFutex(PKPROCESS process, UINT64 address, UINT64 operation,
                  UINT64 value, UINT64 timeout);
UINT64 LinuxFutexWake(PKPROCESS process, UINT64 address, UINT32 maximum);
NORETURN void LinuxExitGroup(PKTHREAD thread, NTSTATUS exit_status);
void LinuxInitializeFiles(PKPROCESS process);
/* Convert a Linux descriptor into an NT handle usable as a PE child's
 * standard stream (regular files reuse their handle; PTY/VT endpoints are
 * published as char-device File objects). */
NTSTATUS LinuxStdHandleFromFd(PKPROCESS process, UINT64 fd, BOOLEAN write,
                              HANDLE *out);
UINT64 LinuxOpenAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                   UINT64 flags);
UINT64 LinuxClose(PKPROCESS process, UINT64 fd);
UINT64 LinuxDup(PKPROCESS process, UINT64 source, UINT64 target,
                UINT64 flags, BOOLEAN exact, BOOLEAN dup3);
UINT64 LinuxFcntl(PKPROCESS process, UINT64 fd, UINT64 command,
                  UINT64 argument);
UINT64 LinuxFlock(PKPROCESS process, UINT64 fd, UINT64 operation);
void LinuxFlockReleaseProcess(PKPROCESS process);
UINT64 LinuxSeek(PKPROCESS process, UINT64 fd, INT64 offset, UINT64 whence);
UINT64 LinuxReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                      UINT64 buffer, UINT64 length);
UINT64 LinuxPread(PKPROCESS process, UINT64 fd, UINT64 buffer,
                  UINT64 length, UINT64 offset);
BOOLEAN LinuxFileHandle(PKPROCESS process, UINT64 fd, BOOLEAN write,
                        HANDLE *handle_out);
NTSTATUS LinuxPrepareWrite(PKPROCESS process, UINT64 fd);
UINT64 LinuxCapturePath(PKPROCESS process, UINT64 user, char result[260]);
UINT64 LinuxCapturePathAt(PKPROCESS process, UINT64 directory_fd,
                          UINT64 user, char result[260]);
UINT64 LinuxRenameAt2(PKPROCESS process, UINT64 old_directory_fd,
                      UINT64 old_path, UINT64 new_directory_fd,
                      UINT64 new_path, UINT64 flags);
UINT64 LinuxMkdirAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                    UINT64 mode);
UINT64 LinuxSyncFile(PKPROCESS process, UINT64 fd);
UINT64 LinuxOpenDirectory(PKPROCESS process, UINT64 slot,
                          const char *path, UINT64 flags);
UINT8 LinuxProcPathKind(const char *path);
UINT64 LinuxProcOpen(PKPROCESS process, UINT64 slot, const char *path,
                     UINT64 flags);
UINT64 LinuxProcRead(PKPROCESS process, LINUX_FD_ENTRY *entry,
                     UINT64 buffer, UINT64 length, UINT64 offset,
                     BOOLEAN advance);
UINT64 LinuxProcSize(PKPROCESS process, UINT8 node);
UINT64 LinuxProcGetdents(LINUX_FD_ENTRY *entry, UINT64 buffer, UINT64 length);
UINT64 LinuxSysinfo(UINT64 destination);
UINT64 LinuxProcReadlink(PKPROCESS process, UINT8 node,
                         UINT64 buffer, UINT64 size);
UINT64 LinuxGetdents64(PKPROCESS process, UINT64 fd,
                        UINT64 buffer, UINT64 length);
UINT64 LinuxDirectorySeek(LINUX_FD_ENTRY *entry, INT64 offset, UINT64 whence);
UINT64 LinuxEpollCreate(PKPROCESS process, UINT64 flags);
UINT64 LinuxEpollCtl(PKPROCESS process, UINT64 epfd, UINT64 operation,
                     UINT64 target_fd, UINT64 event);
UINT64 LinuxEpollWait(PKPROCESS process, UINT64 epfd, UINT64 events,
                      UINT64 maximum, INT64 timeout_ms);
void LinuxEpollClose(LINUX_EPOLL_CONTEXT *epoll);
UINT64 LinuxSocketCreate(PKPROCESS process, UINT64 family, UINT64 type,
                         UINT64 protocol);
UINT64 LinuxSocketPair(PKPROCESS process, UINT64 family, UINT64 type,
                       UINT64 protocol, UINT64 pair);
UINT64 LinuxSocketBind(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 length);
UINT64 LinuxSocketListen(PKPROCESS process, UINT64 fd, UINT64 backlog);
UINT64 LinuxSocketConnect(PKPROCESS process, UINT64 fd, UINT64 address,
                          UINT64 length);
UINT64 LinuxSocketAccept(PKPROCESS process, UINT64 fd, UINT64 address,
                         UINT64 address_length, UINT64 flags);
UINT64 LinuxSocketReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                            UINT64 buffer, UINT64 length, UINT64 flags);
UINT64 LinuxSocketMessage(PKPROCESS process, BOOLEAN write, UINT64 fd,
                          UINT64 message, UINT64 flags);
UINT64 LinuxSocketName(PKPROCESS process, UINT64 fd, UINT64 address,
                       UINT64 address_length, BOOLEAN peer);
UINT64 LinuxSocketOption(PKPROCESS process, UINT64 fd, UINT64 level,
                         UINT64 option, UINT64 value, UINT64 length,
                         BOOLEAN get);
UINT64 LinuxSocketShutdown(PKPROCESS process, UINT64 fd, UINT64 how);
struct _LINUX_SOCKET *LinuxSocketReferenceFd(PKPROCESS process, UINT64 fd);
void LinuxSocketRelease(struct _LINUX_SOCKET *socket);
void LinuxSocketClose(struct _LINUX_SOCKET *socket);
void LinuxSocketInherit(struct _LINUX_SOCKET *socket);
void LinuxEpollInherit(LINUX_EPOLL_CONTEXT *epoll);
UINT32 LinuxSocketPoll(struct _LINUX_SOCKET *socket);
BOOLEAN LinuxSocketPathPresent(const char *dos_path);
BOOLEAN LinuxSocketUnlinkPath(const char *dos_path);
UINT64 LinuxAccess(PKPROCESS process, UINT64 path, UINT64 mode);
UINT64 LinuxGetcwd(PKPROCESS process, UINT64 buffer, UINT64 size);
UINT64 LinuxChdir(PKPROCESS process, UINT64 path);
UINT64 LinuxMkdir(PKPROCESS process, UINT64 path, UINT64 mode);
UINT64 LinuxUnlink(PKPROCESS process, UINT64 path);
UINT64 LinuxUnlinkAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                    UINT64 flags);
UINT64 LinuxLink(PKPROCESS process, UINT64 old_path, UINT64 new_path);
UINT64 LinuxRename(PKPROCESS process, UINT64 old_path, UINT64 new_path);
UINT64 LinuxRenameAt(PKPROCESS process, UINT64 old_directory_fd,
                     UINT64 old_path, UINT64 new_directory_fd,
                     UINT64 new_path);
UINT64 LinuxSymlink(PKPROCESS process, UINT64 target, UINT64 link_path);
UINT64 LinuxSymlinkAt(PKPROCESS process, UINT64 target, UINT64 directory_fd,
                      UINT64 link_path);
UINT64 LinuxLinkAt(PKPROCESS process, UINT64 old_directory_fd,
                   UINT64 old_path, UINT64 new_directory_fd,
                   UINT64 new_path, UINT64 flags);
UINT64 LinuxReadlinkAt(PKPROCESS process, UINT64 directory_fd, UINT64 path,
                       UINT64 buffer, UINT64 size);
UINT64 LinuxGetrlimit(PKPROCESS process, UINT64 resource, UINT64 result);
UINT64 LinuxPrlimit64(PKPROCESS process, UINT64 pid, UINT64 resource,
                      UINT64 requested, UINT64 previous);
UINT64 LinuxPipe(PKPROCESS process, UINT64 pair, UINT64 flags);
UINT64 LinuxFstat(PKPROCESS process, UINT64 fd, UINT64 destination);
UINT64 LinuxStatDosPath(PKPROCESS process, const char *path,
                        UINT64 destination);
UINT64 LinuxStatPath(PKPROCESS process, UINT64 dirfd, UINT64 path,
                     UINT64 destination, UINT64 flags);
NTSTATUS LinuxInitializeMemory(PKPROCESS process, UINT64 image_end);
UINT64 LinuxBrk(PKPROCESS process, UINT64 requested);
UINT64 LinuxMmap(PKPROCESS process, const UINT64 args[6]);
UINT64 LinuxMremap(PKPROCESS process, const UINT64 args[6]);
UINT64 LinuxMunmap(PKPROCESS process, UINT64 address, UINT64 length);
UINT64 LinuxMprotect(PKPROCESS process, UINT64 address, UINT64 length,
                     UINT64 protection);
BOOLEAN LinuxFramebufferPath(const char *path);
UINT64 LinuxFramebufferOpen(PKPROCESS process, UINT64 slot, UINT64 flags);
UINT64 LinuxFramebufferReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                                 UINT64 buffer, UINT64 length);
UINT64 LinuxFramebufferSeek(PKPROCESS process, UINT64 fd, INT64 offset,
                            UINT64 whence);
UINT64 LinuxFramebufferIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                             UINT64 argument);
UINT64 LinuxFramebufferSize(void);
BOOLEAN LinuxVtPath(const char *path, UINT8 *number);
UINT64 LinuxVtOpen(PKPROCESS process, UINT64 slot, UINT8 number, UINT64 flags);
void LinuxVtInherit(UINT8 number);
void LinuxVtClose(PKPROCESS process, UINT8 number);
void LinuxVtProcessExit(PKPROCESS process);
UINT64 LinuxVtIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                    UINT64 argument);
UINT64 LinuxVtReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                        UINT64 buffer, UINT64 length);
UINT32 LinuxVtPoll(PKPROCESS process, UINT64 fd);

BOOLEAN LinuxPtyPath(const char *path, UINT32 *number, BOOLEAN *master);
BOOLEAN LinuxPtyControllingPath(const char *path);
BOOLEAN LinuxPtyExists(UINT32 number);
void LinuxSignalProcessGroup(struct _LINUX_PTY *pty, UINT32 process_group,
                             NTSTATUS exit_status);
BOOLEAN LinuxPtyOwnsForegroundProcess(struct _LINUX_PTY *pty,
                                      LINUX_PROCESS_CONTEXT *process,
                                      UINT32 process_group);
NTSTATUS LinuxPtyCreateBridgeFile(struct _LINUX_PTY *pty, BOOLEAN master,
                                  HANDLE *out);
UINT64 LinuxPtyOpen(PKPROCESS process, UINT64 slot, UINT32 number,
                    BOOLEAN master, UINT64 flags);
UINT64 LinuxPtyOpenControlling(PKPROCESS process, UINT64 slot, UINT64 flags);
void LinuxPtyInherit(struct _LINUX_PTY *pty, BOOLEAN master);
void LinuxPtyClose(struct _LINUX_PTY *pty, BOOLEAN master);
void LinuxPtyControlInherit(struct _LINUX_PTY *pty);
void LinuxPtyControlClose(struct _LINUX_PTY *pty);
UINT64 LinuxPtyReadWrite(PKPROCESS process, BOOLEAN write, UINT64 fd,
                         UINT64 buffer, UINT64 length);
UINT32 LinuxPtyPoll(PKPROCESS process, UINT64 fd);
UINT32 LinuxPtyPollObject(struct _LINUX_PTY *pty, BOOLEAN master, UINT8 mode);
UINT64 LinuxPtyIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                     UINT64 argument);
UINT64 LinuxSetSid(PKPROCESS process);
UINT64 LinuxSetPgId(PKPROCESS process, UINT64 pid, UINT64 group);
UINT64 LinuxGetPgId(PKPROCESS process, UINT64 pid);
UINT64 LinuxGetSid(PKPROCESS process, UINT64 pid);
UINT64 LinuxFramebufferPhysicalAddress(void);

/* ------------------------------------------------------------------ */
/* Linux evdev input devices (/dev/input/eventN)                      */
/* ------------------------------------------------------------------ */

#define LINUX_EVENT_DEVICE_COUNT 3u
#define LINUX_EVENT_NAME_LENGTH 64u
#define LINUX_EVENT_RING_SIZE 256u

typedef struct _LINUX_EVENT_DEVICE {
    BOOLEAN Present;
    BOOLEAN Open;                 /* exclusive-ish single opener */
    volatile UINT32 OpenReferences;
    char Name[LINUX_EVENT_NAME_LENGTH];
    UINT16 Bus, Vendor, Product, Version;
    UINT64 Capabilities[8];       /* EV_* bitmasks, bit n => EV_* bit n   */
    UINT64 KeyBits[12];           /* KEY_* bitmask (max 768 codes)        */
    UINT64 RelBits[2];            /* REL_* bitmask                        */
} LINUX_EVENT_DEVICE;

UINT64 LinuxGetRandom(UINT64 destination, UINT64 length, UINT64 flags);

/* Path is the translated DOS path (e.g. C:\dev\input\event0). */
BOOLEAN LinuxEventPath(const char *dos_path, UINT32 *index_out);
UINT64 LinuxEventOpen(PKPROCESS process, UINT64 slot, UINT32 index,
                      UINT64 flags);
UINT64 LinuxEventRead(PKPROCESS process, UINT64 fd, UINT64 buffer,
                      UINT64 length);
UINT64 LinuxEventIoctl(PKPROCESS process, UINT64 fd, UINT64 command,
                       UINT64 argument);
UINT32 LinuxEventPoll(PKPROCESS process, UINT64 fd);
const LINUX_EVENT_DEVICE *LinuxEventDevice(UINT32 index);
void LinuxEventSetOpen(UINT32 index, BOOLEAN open);
void LinuxInputInitialize(void);
void LinuxInputPump(void);
BOOLEAN LinuxInputKeyboardActive(void);

#endif
