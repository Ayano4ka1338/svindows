#ifndef _SVINDOWS_LINUX_H_
#define _SVINDOWS_LINUX_H_

#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>
#include <svindows/Ke.h>

BOOLEAN LinuxIsElfImage(const void *image, SIZE_T size);
typedef struct _LINUX_ELF_INFO {
    UINT64 Entry;
    UINT64 ProgramHeaders;
    UINT64 ImageEnd;
    UINT64 LoadBase;
    UINT16 ProgramCount;
    char Interpreter[260];
} LINUX_ELF_INFO;

NTSTATUS LinuxLoadElf64(const void *image, SIZE_T size, UINT64 pie_base,
                        BOOLEAN is_interpreter, LINUX_ELF_INFO *info);
NTSTATUS LinuxCreateInitialThread(PKPROCESS process, const char *name,
                                  const char *command, const void *image,
                                 SIZE_T size, const char *const *environment,
                                 UINT32 environment_count,
                                 PKTHREAD *thread_out);
NTSTATUS LinuxLoadExecImage(PKPROCESS process, const void *image, SIZE_T size,
                            const char *const *argv, UINT32 argc,
                            const char *const *env, UINT32 envc,
                            UINT64 *entry_out, UINT64 *stack_out);
UINT64 LinuxSystemCallDispatch(UINT64 number, const UINT64 args[6]);
void LinuxDestroyProcess(PKPROCESS process);
NTSTATUS LinuxCloneProcessContext(PKPROCESS parent, PKPROCESS child);
BOOLEAN LinuxSharedPage(UINT64 address, PVOID context);
NTSTATUS LinuxCreateForkThread(PKTHREAD parent, PKPROCESS process,
                               PDISPATCHER_HEADER termination,
                               PKTHREAD *out);
void LinuxDestroyThread(PKTHREAD thread);
void LinuxThreadExited(PKTHREAD thread);
BOOLEAN LinuxIsCloneThread(PKTHREAD thread);
const void *LinuxCloneRegisters(PKTHREAD thread);

/* Mark a thread that just execve'd a PE image: the next ring-3 re-entry uses
 * the Windows-exec path in the syscall stub with the given user argument. */
void LinuxThreadBeginWindowsExec(PKTHREAD thread, UINT64 argument);

/* Bring up the synthetic evdev input devices (/dev/input/eventN). Called once
 * after the PS/2 keyboard and mouse drivers are live. */
void LinuxInputInitialize(void);

#endif
