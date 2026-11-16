# Physical UEFI boot checks

The reported target uses UEFI, 16 GiB RAM and a PS/2 keyboard. The physical
failure has not been reproduced in QEMU; passing emulation does not establish
that this particular motherboard boots.

The startup review compared Svindows with YabroOS-32's `src/kernel/main.c`,
`pmm.c`, `vmm.c`, and kyronix's `kernel/mm/pmm.c` and
`kernel/arch/x86_64/lapic.c`. Both reference systems were read without changes.

Corrections from this review:

* Allocate initial page tables below 4 GiB until the new CR3/direct map is
  active. The bootstrap identity map cannot access arbitrary high RAM.
* Clear CR0.EM and CR0.TS before the scheduler's first FXSAVE. Multiboot does
  not supply a usable floating-point context or guarantee TS is clear.
* Wait for PIT channel 2 OUT to go low before accepting its calibration
  completion. A stale high output can otherwise produce a short measurement.
* Select LAPIC periodic mode before loading its initial count, so a short
  one-shot cannot expire before periodic mode is installed.
* Limit serial input work per polling pass so a continuously receiving or
  misbehaving UART cannot monopolize keyboard/cursor processing.
* Display the latest completed boot step on the UEFI framebuffer after its
  mapping is ready, until the collected boot report is replayed. Earlier
  memory initialization still precedes this display; no framebuffer status
  line does not identify a specific failed stage.

Validation commands:

```
make -j2
python3 tools/QemuUefiSmoke.py
make pmm-usage-check
cc -Wall -Wextra -Ikernel/include tools/checks/SerialChecks.c -o /tmp/SerialChecks
/tmp/SerialChecks
```

The UEFI test uses q35, OVMF, 16 GiB guest RAM and no serial controller. It
requires shell startup, an actual PS/2 `echo` command and advancing clock ticks;
it reads the console through QMP rather than depending on COM1. RAM uses a
sparse temporary file to avoid committing 16 GiB of anonymous host memory.
The framebuffer snapshot is `build/checks/uefi-16g-ps2.ppm`.

A later report identified `Initial user process [ok]` as the last visible
line. That is a creation marker, not proof that the thread entered user mode.
Startup now probes three delivered clock interrupts at DISPATCH_LEVEL before
queuing workers. If the selected clock fails its bounded polling probe, it
stops the LAPIC timer, enables PIT IRQ0 and probes again. Neither route working
is a visible fatal error rather than an indefinite idle wait. The initial idle
thread explicitly yields after enabling interrupts to dispatch queued work.
The display prints `Starting scheduler (clock IRQs verified)...` immediately
before that handoff. Bugchecks also paint the code/message on the GOP display
before attempting legacy log sinks.

The next hardware report reached an editable shell prompt but froze when
attempting commands. The bootstrap CPU now uses PAUSE rather than HLT in its
idle loop. A busy clock probe cannot prove that the LAPIC continues running in
firmware C1/C1E; without an independent broadcast timer, an idle timer stoppage
also prevents the input worker's polling timer from expiring. This is a
compatibility workaround, not confirmation that C-states caused this specific
hardware failure. It increases idle CPU use/power on the BSP. AP idle loops
are unchanged (SMP is disabled by default).

The UEFI smoke test additionally waits idle between three PS/2 full `dir` child-process launches and `echo` commands, requiring directory
output, command completion and clock progress each time.

The user subsequently confirmed that `echo` works but `dir` freezes even with
the PAUSE idle policy. This does not confirm an idle/C-state failure: external
process launch is now the primary suspect. Temporary `[exec]` markers delimit
process-object allocation, address-space creation, reading the file, PE loading,
thread/environment construction, returning to the parent, entering child user
mode and exiting the child. Kernel exceptions additionally display the vector,
RIP and CR2 on the GOP screen. A photo of the last marker/exception from the
physical machine is needed; this change is diagnostic, not a confirmed fix.

The physical machine stopped at `[exec] 6 return to parent`. This marker
precedes process-list publication; it did not prove that syscall return or
child dispatch succeeded. Tracing now separates list acquisition/release,
reference release, syscall/APC return, process waiting, and the first child
context switch (CR3, TLS MSRs, FPU). The marker is a fixed GOP status line,
independent of console scrolling, allocation, and kernel logging. Once the
child is fully published and the parent's CR3/loader context restored, a
user-originated creation explicitly yields to the scheduler. This handoff is
still subject to physical-machine verification.

The latest physical report stopped at `6w waiting for child`. The former single
status line could overwrite the child's earlier progress, so this is not proof
that the child never started. Diagnostics now use four independent display
rows: parent creation/wait, first child dispatch/exit, child syscall entry/return
(including service number), and a snapshot of the waited process/thread. Thread
states are 0 initialized, 1 ready, 2 running, 3 switching, 4 terminating,
5 waiting, 6 terminated, 7 suspended. The snapshot includes signal state,
active-CPU count, context switches, wait-block count and last user RIP.

The ISO also includes a `Svindows64 - PS2 safe mode` menu entry using the same
kernel/initrd with the Multiboot command-line option `safe-peripherals`. It
skips only the background USB/network initialization worker. PS/2, the clock,
filesystem and process loader remain active. Selecting this entry tests whether
real PCI/USB initialization is involved without requiring another build.

The earlier `dir kernel32.dll` test matched an absent root-level file and its
assertion could accept `0 File(s)`. It exercised executable launch but did not
prove successful full enumeration. The corrected UEFI test sends plain `dir`
and requires `CMD.EXE` plus `1 File(s)` from the root listing, followed by a
successful echo marker. Three cycles passed; the safe-mode menu test also
passed full directory output and command completion.

The physical photo showed child PID 12/TID 6 at `7 enter child user mode`,
with syscall `0x52` (`NtCreateFile`) returning success. The wait snapshot had
`state=1 active=0 switches=1 wait=0 RIP=0x1800014d4`, the RET instruction in
ntdll's NtCreateFile stub. Directory headers were already printed. Thus the
child did execute and was ready at the snapshot; this is not a PE-load failure.
The photo's report showed normal deferred USB/network initialization, not the
safe-mode marker.

Immediately after `6w`, the wait path logs its indefinite wait before entering
the dispatcher. The logger previously always scrolled the legacy VGA aperture,
even after GOP/linear framebuffer initialization. It now skips legacy VGA/CRTC
access once GfxAvailable is true. This removes hidden uncached device reads and
port writes under the log lock/interrupt mask; whether they caused this exact
physical hang remains unverified. `6w.1 entering dispatcher` and `6w.2 child
signalled` distinguish logging from the actual wait transition.

After the user requested no further diagnostic trials, the normal image was
changed to disable SVINDOWS_EXEC_TRACE (opt-in only). The following architectural
issues were corrected:

* First-entry Windows/Linux/clone routines were called with IF=1 and swapped
  GS to the user value before completing the CPL transition. An IRQ in that
  window would enter as CPL0 and use the user TEB as KPCR. Entry now masks IRQs
  and performs SWAPGS immediately before IRETQ, which restores user IF.
* Normal syscall and callback returns now build their entire IRETQ frame on
  the kernel stack; RDX and RAX are preserved in the normal return path. The
  previous path installed user RSP before the final return instruction.
* Data descriptors now have reserved L=0 and conventional D/B=1 attributes.
* Interrupt entry executes CLD while preserving the interrupted DF in the
  hardware frame, and normalizes AMD #VC/#SX error-code frames correctly.
* Each processor starts with an explicit clean x87/SSE FXRSTOR image instead
  of copying unspecified firmware FPU state into initial thread contexts.
* Logging contention has a bounded wait and drops only the contended record
  rather than spinning forever with IRQs masked.

Validation for this last revision: full compilation, assembly/link and ISO
creation succeeded; disassembly was inspected for the kernel-stack IRETQ frame
and CLI/SWAPGS/IRETQ ordering. No new runtime tests were run after the user's
request. These are concrete architectural fixes, but their causal relationship
to the observed physical hang and physical boot success remain unconfirmed.
