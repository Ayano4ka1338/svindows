# Original Windows 7 Task Manager

The AMD64 `taskmgr.exe` and its en-US MUI were extracted read-only from the
user-provided `win7.vdi`. They are staged at `C:\WINDOWS7\TASKMGR.EXE` and
`TASKMGR.EXE.MUI` by the existing Windows 7 file packaging rule.

SHA-256:

- taskmgr.exe: `0be126c381b0061170e340b9ac15a7d1c6c9a19d3fa385269f9b6c901ee5bc6b`
- taskmgr.exe.mui: `04dbe5f78433cd22ab9e9cb68f678565574810ba2624c43399e3edb22e8bed4e`

Run `C:\WINDOWS7\TASKMGR.EXE` from the Svindows shell. Applications reads
live window titles from Win32k. Processes shows native process snapshots, with
CPU time and private resident memory from the scheduler and page tables.
Performance shows CPU and physical memory history, free memory, commit, uptime
and handle, process and thread counts.

CPU samples are taken every 10 ms and the processor query exposes the active
boot processor, including its idle and privileged time. The physical allocator
backs commitment eagerly with RAM. Svindows has no page file or standby list,
so those mechanisms do not contribute to the memory figures. The process
snapshot covers native user processes, System and System Idle Process.

The original Task Manager and Notepad ran together in QEMU. On the release
image, confirming End Process closed the empty Notepad and removed its row.
A diagnostic copy of the release image added a test process that committed
64 MiB and used the CPU for about six seconds. Its row showed 95% CPU and
70,348 KiB of private resident memory. Total CPU usage went from 7% to 100%,
physical memory usage from 49% to 56%, and the process count from five to six.
All three returned to their starting values after exit. Captures are in
`build/diagnostics/win7-taskmgr-verification-51-*.png` and
`build/diagnostics/win7-taskmgr-termination-50-*.png`.

Windows account and session management, the Resource Monitor executable,
and several other Windows-only services are outside the current compatibility
coverage. The display of those features depends on the facilities provided
by Svindows.
