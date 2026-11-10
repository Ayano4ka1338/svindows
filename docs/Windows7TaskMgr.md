# Original Windows 7 Task Manager

The AMD64 `taskmgr.exe` and its en-US MUI were extracted read-only from the
user-provided `win7.vdi`. They are staged at `C:\WINDOWS7\TASKMGR.EXE` and
`TASKMGR.EXE.MUI` by the existing Windows 7 file packaging rule.

SHA-256:

- taskmgr.exe: `0be126c381b0061170e340b9ac15a7d1c6c9a19d3fa385269f9b6c901ee5bc6b`
- taskmgr.exe.mui: `04dbe5f78433cd22ab9e9cb68f678565574810ba2624c43399e3edb22e8bed4e`

Startup compatibility is under development and has not yet been established.
The initial import inventory is captured in
`build/diagnostics/win7-taskmgr-imports.txt`. Static dependencies include process
and thread inspection, USER desktop/window APIs, network interface queries,
performance counters, credentials and event subscriptions. Delay dependencies
also include session management, utility helpers and dump generation.

This work uses the original executable. Task Manager's process, CPU and memory
views must be backed by Svindows state; successful import binding or an empty
window alone does not establish that those views work.
