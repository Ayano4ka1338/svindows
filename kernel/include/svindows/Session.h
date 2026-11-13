/* Kernel-owned USER window stations, desktops and their attachments. */
#ifndef SVINDOWS_SESSION_H
#define SVINDOWS_SESSION_H

#include <nt/Ntdef.h>
#include <nt/Ntstatus.h>

struct _KPROCESS;
struct _KTHREAD;

NTSTATUS W32kInitializeSessions(void);
NTSTATUS W32kInitializeProcessSession(struct _KPROCESS *process,
    struct _KPROCESS *parent, struct _KTHREAD *creator);
NTSTATUS W32kInitializeThreadSession(struct _KTHREAD *thread);
NTSTATUS W32kInitializeThreadSessionForCreator(struct _KTHREAD *thread,
    struct _KTHREAD *creator);
NTSTATUS W32kAssignProcessStartupDesktop(struct _KPROCESS *process,
    const UINT16 *name);
NTSTATUS W32kInheritSessionHandles(struct _KPROCESS *process,
    struct _KPROCESS *parent);
void W32kCleanupThreadSession(struct _KTHREAD *thread);
void W32kCleanupProcessSession(struct _KPROCESS *process);

/* Bind before publishing a window; unbind after its final destruction. */
NTSTATUS W32kBindWindowDesktop(UINT16 slot, struct _KTHREAD *thread,
    UINT16 parent);
void W32kUnbindWindowDesktop(UINT16 slot);
BOOLEAN W32kWindowOnCurrentDesktop(UINT16 slot);
BOOLEAN W32kWindowOnInputDesktop(UINT16 slot);
BOOLEAN W32kWindowsShareDesktop(UINT16 first, UINT16 second);
PVOID W32kGetCurrentWindowStationIdentity(void);
PVOID W32kGetProcessWindowStationIdentity(struct _KPROCESS *process);
PVOID W32kGetCurrentDesktopIdentity(void);
PVOID W32kGetInputDesktopIdentity(void);
NTSTATUS W32kQueryProcessDesktopName(struct _KPROCESS *process, UINT16 *output,
    ULONG capacity, ULONG *needed);
PVOID W32kGetWindowDesktopIdentity(UINT16 slot);

/* Identities remain referenced by their process/thread/window attachments.
 * A registered callback removes station/desktop-owned client subsystem state
 * before the final object storage is released. No callback owns that storage. */
typedef void (*W32K_SESSION_DELETE_NOTIFY)(PVOID identity, BOOLEAN station);
void W32kRegisterSessionDeleteNotify(W32K_SESSION_DELETE_NOTIFY notify);

#endif
