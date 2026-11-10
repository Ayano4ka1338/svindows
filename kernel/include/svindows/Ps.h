/*
 * svindows/Ps.h - Process/thread manager (Ps).
 *
 * Process creation, per-process PEB/TEB setup, and personality transitions.
 */
#ifndef _SVINDOWS_PS_H_
#define _SVINDOWS_PS_H_

#include <nt/Ntdef.h>
#include <nt/Ntobject.h>
#include <svindows/Ke.h>

struct _OBJECT_TYPE;

NTSTATUS PsCreateLinuxSpawn(PKPROCESS creator, const char *image,
                            const char *command_line,
                            const char *current_directory,
                            HANDLE standard_input, HANDLE standard_output,
                            HANDLE standard_error,
                            const char *const *environment,
                            UINT32 environment_count,
                            HANDLE *process_handle, ULONG *process_id);
NTSTATUS PsSetProcessEnvironment(PKPROCESS process,
                                 const char *const *entries, UINT32 count);

/*
 * Fallback client id for early threads. It has to be even: a GDI handle's cell
 * carries the owning process id in a field whose low bit is a flag, and every
 * gdi32 handle check compares (cell.ProcessId & ~1) against its own cached
 * copy. An odd id therefore fails every check -- with an id of 1, gdi32
 * rejected every device context before it made a single syscall. Real Windows
 * process ids are multiples of four for the same reason.
 */
#define PROCESS_CLIENT_ID 4

/* A process's PEB virtual address, shared by its threads. */
#define PROCESS_PEB_VA 0x0000000000061000ULL
#define PROCESS_MAIN_TEB_VA 0x0000000000050000ULL
#define PROCESS_TEB_SIZE    0x0000000000002000ULL

/* User region for the loader's module list (PEB_LDR_DATA + module entries +
 * name buffers), which PEB.Ldr points into. Six pages, below the process
 * parameters, are reserved for loaded modules. */
#define PROCESS_LDR_VA 0x0000000000062000ULL
#define PROCESS_LDR_SIZE 0x6000ULL

/* User page for RTL_USER_PROCESS_PARAMETERS (command line, image path). */
#define PROCESS_PARAMS_VA 0x0000000000068000ULL
#define PROCESS_ENV_VA    0x0000000000069000ULL

/* The native GDI client indexes this table directly through PEB+0xF8. Each
 * GDI handle entry is 24 bytes and the handle index is 16 bits. */
#define PROCESS_GDI_SHARED_TABLE_VA   0x0000000001000000ULL
#define PROCESS_GDI_SHARED_TABLE_SIZE (0x10000ULL * 24ULL)

/* USER32's SHAREDINFO points at a parallel 16-bit handle table. */
#define PROCESS_USER_SHARED_TABLE_VA   0x0000000001180000ULL
#define PROCESS_USER_SHARED_TABLE_SIZE (0x10000ULL * 24ULL)

/* SERVERINFO sits between the two handle tables and the object arena. It is
 * read-only shared memory with a published address (SHAREDINFO.psi), so it
 * keeps its own range and the arena starts above it: the two used to overlap,
 * which left the client reading a zeroed page. */
#define PROCESS_SERVER_INFO_VA   0x0000000001300000ULL
#define PROCESS_SERVER_INFO_SIZE 0x0000000000004000ULL

#define PROCESS_USER_OBJECT_ARENA_VA   0x0000000001304000ULL
/* 0x200 bytes per USER object head. Windows take indices 1..127;
 * cursors, accelerator tables and the other USER object types are
 * allocated above them, so the arena has to be larger than the window
 * range alone. */
#define PROCESS_USER_OBJECT_ARENA_SIZE 0x0000000000040000ULL

/* User-writable arena for the per-object GDI attribute blocks (DC_ATTR and
 * friends) that a GDI cell's pUserInfo points at. GDI32 caches DC state there
 * -- SetTextColor and SetBkColor never enter the kernel -- so win32k has to
 * read the drawing colours back out of it. */
/* Leave the page immediately after the USER arena for its heap-range
 * descriptor (Process.c); the GDI attributes must not overlap either map. */
#define PROCESS_GDI_ATTR_VA   0x0000000001350000ULL
#define PROCESS_GDI_ATTR_SIZE 0x0000000000010000ULL

_Static_assert(PROCESS_GDI_SHARED_TABLE_VA + PROCESS_GDI_SHARED_TABLE_SIZE <=
               PROCESS_USER_SHARED_TABLE_VA, "GDI and USER tables overlap");
_Static_assert(PROCESS_USER_SHARED_TABLE_VA + PROCESS_USER_SHARED_TABLE_SIZE <=
               PROCESS_SERVER_INFO_VA, "USER table and SERVERINFO overlap");
_Static_assert(PROCESS_SERVER_INFO_VA + PROCESS_SERVER_INFO_SIZE <=
               PROCESS_USER_OBJECT_ARENA_VA, "SERVERINFO and USER arena overlap");
_Static_assert(PROCESS_USER_OBJECT_ARENA_VA + PROCESS_USER_OBJECT_ARENA_SIZE +
               0x1000 <= PROCESS_GDI_ATTR_VA,
               "USER arena descriptor and GDI attributes overlap");

/* Register the Event and Thread object types. Requires Ob. */
void PsInitialize(void);
void PsInitializeTokens(void);
UINT64 NtOpenProcessToken(UINT64 *args);
UINT64 NtOpenThreadToken(UINT64 *args);
UINT64 NtOpenThread(UINT64 *args);
UINT64 NtSuspendThread(UINT64 *args);
UINT64 NtResumeThread(UINT64 *args);
UINT64 NtQueryThreadPriority(UINT64 *args);
UINT64 NtSetThreadPriority(UINT64 *args);
NTSTATUS PsTerminateThreadByHandle(HANDLE handle, NTSTATUS exit_status);
UINT64 NtOpenEvent(UINT64 *args);
NTSTATUS PsCaptureNamedObjectPath(POBJECT_ATTRIBUTES attributes,
                                  char *path, SIZE_T capacity);
void PsJobProcessExited(PKPROCESS process);
void PsInitializeJobObjects(void);
UINT64 NtCreateJobObject(UINT64 *args);
UINT64 NtAssignProcessToJobObject(UINT64 *args);
UINT64 NtSetInformationJobObject(UINT64 *args);
UINT64 NtQueryInformationJobObject(UINT64 *args);
NTSTATUS PsValidateThreadHandle(HANDLE handle);
UINT64 NtQueryInformationToken(UINT64 *args);
UINT64 NtAdjustPrivilegesToken(UINT64 *args);
void PsInitializeTimerObjects(void);
BOOLEAN PsIsTimerObjectType(struct _OBJECT_TYPE *type);
UINT64 NtCreateTimer(UINT64 *args);
UINT64 NtSetTimer(UINT64 *args);
UINT64 NtCancelTimer(UINT64 *args);
UINT64 NtQueryTimer(UINT64 *args);

/*
 * Handle-based synchronization + thread services (signatures match the syscall
 * dispatcher: (a1, a2, a3, a4), result in the return value).
 *
 *   NtCreateEvent(notification, initial_state) -> HANDLE
 *   NtSetEvent(handle)                         -> previous state
 *   NtWaitForSingleObject(handle)              -> NTSTATUS
 *   NtCreateThread(entry, arg)                 -> thread HANDLE
 */
UINT64 NtCreateEvent(UINT64 *args);
UINT64 NtSetEvent(UINT64 *args);
UINT64 NtWaitForSingleObject(UINT64 *args);
UINT64 NtWaitForMultipleObjects(UINT64 *args);
UINT64 NtQueueApcThread(UINT64 *args);
UINT64 NtDequeueUserApc(UINT64 *args);
UINT64 NtTestAlert(UINT64 *args);
UINT64 NtResetEvent(UINT64 *args);
UINT64 NtCreateSemaphore(UINT64 *args);
UINT64 NtReleaseSemaphore(UINT64 *args);
UINT64 NtCreateMutant(UINT64 *args);
UINT64 NtReleaseMutant(UINT64 *args);
UINT64 NtQueryInformationProcess(UINT64 *args);
UINT64 SvNtQueryThreadExitStatus(UINT64 *args);
NTSTATUS PsQuerySystemProcessInformation(void *buffer, ULONG length,
                                         ULONG *return_length);
UINT64 SvNtReadProcessMemory(UINT64 *args);
UINT64 NtSetInformationProcess(UINT64 *args);
UINT64 NtDuplicateObject(UINT64 *args);
UINT64 NtCreateThreadEx(UINT64 *args);
UINT64 NtCreateUserProcessSimple(UINT64 *args);
NTSTATUS PsCreateInitialProcess(const char *application,
                                const char *command_line);
/* Linux execve of a PE image: turn the current Linux thread into the Windows
 * main thread of the same process. CR3 must already be the new image's
 * address space and StandardInput/Output/Error must hold the converted
 * handles. */
NTSTATUS PsSwitchThreadToWindows(PKTHREAD thread, PKPROCESS process,
                                 const char *image_path, const char *command);
NTSTATUS PsForkLinuxProcess(PKTHREAD parent, HANDLE *handle_out,
                            ULONG *pid_out);
BOOLEAN PsIsProcessObjectType(struct _OBJECT_TYPE *type);
UINT64 NtOpenProcess(UINT64 *args);
PKPROCESS PsGetProcessFromObject(PVOID object);
void *PsReferenceNextProcess(ULONG after_process_id, ULONG *process_id);

/* Create a notification (manual-reset) Event object and a handle to it in the
 * current process, for kernel-owned events ring 3 must be able to wait on --
 * the per-thread USER input event backing MsgWaitForMultipleObjects. The
 * caller keeps `event_out` to signal it without going through the handle. */
/* event_out receives an owned reference; release with ObDereferenceObject. */
NTSTATUS PsCreateNotificationEvent(PKEVENT *event_out, HANDLE *handle_out);

#endif /* _SVINDOWS_PS_H_ */
