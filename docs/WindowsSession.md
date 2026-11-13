# Windows session startup and executive USER objects

The graphical boot program is `wininit.exe`. It prepares the system directory
layout, a real process environment, registry configuration and file
associations. It creates a noninteractive `SvindowsServices` window station
and `Services` desktop, then creates and supervises `userinit.exe` on
`WinSta0\Default`. No service/LSASS initialization is inferred from a program
being present on disk.

`userinit.exe` prepares the profile associated with the actual token SID:
`ProfileList\<SID>\ProfileImagePath`, `USERPROFILE`, `APPDATA`, `LOCALAPPDATA`,
temporary directories, user/common shell folders, and standard InternetShortcut
files for installed applications. It reads the configured Explorer command
from the Windows `Winlogon` registry key and supervises the child process.
`AutoRestartShell`, `ShellRestartLimit` and `ShellRestartDelay` control recovery.
A shell that repeatedly exits during initialization causes a reported session
failure rather than an endless restart loop.

Readiness uses named executive events:

- `Global\Svindows.Session.1.ShellReady`: the shell signals it after its actual
  windows and shell registrations have been created.
- `Global\Svindows.Session.1.Ready`: userinit signals it after shell readiness
  and a live child-process check.
- `Global\Svindows.Session.1.Shutdown`: a request for supervisor shutdown;
  wininit gives userinit time to close its supervised child.

## Window stations and desktops

`kernel/win32k/Session.inc` stores stations and desktops as reference-counted
Object Manager entities. A station is named in
`\Sessions\1\Windows\WindowStations`. Desktop names belong to their station.
Names retain UTF-16 spelling and use generated Unicode simple uppercase data
for case-insensitive lookup. Desktop enumeration returns a captured name
snapshot before callbacks can change the namespace.

A process holds its station/default-desktop context. A thread retains its
explicit desktop association, and each window retains its creation desktop.
Process creation inherits the creator's selected desktop, while the startup
desktop option selects a named station/desktop explicitly. New local threads
inherit their creator's selected desktop. Cross-desktop parenting is rejected.
Desktop render/input filtering and scene changes use the input-desktop object
identity, rather than a process-local numeric token.

Borrowed station/desktop handles cannot be closed by either the USER close API
or generic `NtClose`. An attached desktop handle remains busy until its thread
association is released. A thread with windows cannot change to a different
desktop. Window/thread/process teardown drops the corresponding references;
final object deletion invokes the registered subsystem cleanup notification.

## Process and runtime contracts

`SvNtCreateUserProcessEx` captures a structured request containing UTF-16
paths, the environment block, startup desktop/flags, standard handles and
process/thread security attributes. Public `CreateProcessA/W` use the standard
double-NUL environment representation; the old kernel/ELF vector entry remains
a separate internal interface. The returned main-thread handle is a waitable
Thread object, with an actual thread ID and suspend/resume behavior.

Inherited executive handles preserve their encoded handle value, object,
granted access and protection/inheritance flags. Table inheritance validates
the destination before publishing entries. Explicit standard handles are
preserved independently of console attachment; a hosted-console child waits
for its real host readiness before successful process creation is reported.
The initial system program receives actual boot-console File handles.

Process environments use immutable, referenced kernel blocks. Replacement is
captured before publication, child creation retains a complete block, and
failed Linux/PE exec transitions restore a referenced snapshot. The legacy
inline storage is a compatibility mirror for small blocks, rather than the
storage limit. UTF-8/WTF-8 storage preserves UTF-16 environment code units.
`GetEnvironmentStrings` returns an independent heap copy, and mutation updates
both the live PEB and the kernel inheritance state.

## Stored security descriptors

`kernel/se/Security.c` captures self-relative or absolute descriptors into
referenced canonical self-relative storage. Object headers own descriptors;
generic access maps to each actual object type's rights. Process/thread opens
evaluate ordered allow/deny ACEs against the token RID/groups, and owner
standard rights are evaluated explicitly. Object descriptor queries return
stored data and required sizes. Replacements retain old query snapshots and
merge updates without dropping concurrently changed descriptor components.

This does not claim complete Windows security/logon equivalence. The current
token model supplies the implemented primary identity/groups, without thread
impersonation or a complete privilege/logon provider. Conditional callback ACEs
are not converted into unevaluated access grants. Persistent filesystem ACLs
are separate from executive object descriptors.

## Checks

`SessionChecks.c` includes actual Se/Ob/USER session code and checks typed
handles, Unicode lookup, stable enumeration, busy/borrowed close, attachment,
desktop isolation and resource release. `SecurityObjectChecks.c` checks
stored descriptors, ordered ACL access, required-size queries, malformed
offsets, inherited/protected handles and descriptor lifetime.
`ProcessEnvironmentChecks.c` checks complete blocks beyond the old inline
capacity, clone immutability, failed-exec/allocation rollback, Unicode and
balanced cleanup. QEMU boot and application/console interactions complement
these host checks.
