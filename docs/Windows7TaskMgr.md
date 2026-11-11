# Original Windows 7 Task Manager

The AMD64 `taskmgr.exe` and its en-US MUI were extracted read-only from the
user-provided `win7.vdi`. They are staged at `C:\WINDOWS7\TASKMGR.EXE` and
`TASKMGR.EXE.MUI` by the existing Windows 7 file packaging rule.

SHA-256:

- taskmgr.exe: `0be126c381b0061170e340b9ac15a7d1c6c9a19d3fa385269f9b6c901ee5bc6b`
- taskmgr.exe.mui: `04dbe5f78433cd22ab9e9cb68f678565574810ba2624c43399e3edb22e8bed4e`

Startup compatibility is under development.
The initial import inventory is captured in
`build/diagnostics/win7-taskmgr-imports.txt`. Static dependencies include process
and thread inspection, USER desktop/window APIs, network interface queries,
performance counters, credentials and event subscriptions. Delay dependencies
also include session management, utility helpers and dump generation.

This work uses the original executable. Task Manager's process, CPU and memory
views must be backed by Svindows state; successful import binding or an empty
window alone does not establish that those views work.

## Current runtime evidence (2026-11-11)

The release ISO now opens the original executable without a kernel stop and
renders its main window, menu, Applications/Processes/Services tabs, Applications
list columns and action buttons. The status bar previously remained 0x0; the
application used its top edge to lay out the tab and obtained a negative tab
height. Native status control sizing and multipart text storage fix that
geometry. `TCM_ADJUSTRECT` now reserves the tab header when laying out pages.
Captures: `build/diagnostics/win7-taskmgr-release-29.png` and `release-30.png`
(the latter has the same `win7-taskmgr-` filename prefix).

Foreign window titles, owners and ancestors can be inspected using the native
window metadata without granting write access to another process's HWND.
This is required for the Applications view; a populated Applications view has
not yet been verified.

IRQ0 now samples the interrupted privilege mode before scheduling. Thread and
process CPU totals are expressed in 100 ns units and process totals survive
thread exit. Processor kernel time includes idle; DPC time is a kernel-time
subset. The timer resolution is 10 ms. `SystemProcessorPerformanceInformation`
returns the single executing BSP's counters, consistent with the currently
parked secondary CPUs and `SystemBasicInformation`. Hardware IRQ handlers run
with interrupts disabled, so they cannot receive an IRQ0 time sample;
InterruptTime is consequently zero at this sampling resolution.

`build/diagnostics/taskmgr-debug-32.txt` records increasing real process/thread
CPU counters while the original application runs. Host sanitizer checks cover
status geometry and text lifetime, tab geometry, CPU accounting and the 48-byte
processor performance ABI (`status-control-check`, `tab-layout-check`,
`cpu-accounting-check`, `system-cpu-check`); `win32k-check` covers foreign HWND
metadata and access isolation.

The task remains incomplete. Process/CPU/memory views are not yet verified as
working. The startup trace in `build/diagnostics/taskmgr-debug-31.txt` confirms
that Task Manager requests `SystemPerformanceInformation` (class 2, 328 bytes),
which is currently unsupported. The performance-counter provider in `pcwum`
also remains unsupported. These need actual native counters, followed by live
process-list and changing CPU/memory verification; the visible shell of the
application is not sufficient.
