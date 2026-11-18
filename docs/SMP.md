# Multiprocessor scheduler

Svindows starts application processors from the ACPI MADT and admits them to
the executive scheduler. The x86-64 path supports up to 64 logical processors
with xAPIC or x2APIC firmware configuration.

## Scheduling

- All ready threads are kept in one priority-ordered global queue protected by
  an inter-processor spin lock. Online CPUs pull work from this queue.
- Native processes can run sibling threads simultaneously on different CPUs.
- Processes using the Linux personality are scheduled on the BSP until their
  process-global loader and libc state has SMP-safe locking.
- Each CPU has its own PRCB, idle thread, kernel entry stack, quantum, FPU
  context-switch handling, and execution-time counters.
- The BSP clock sends scheduler tick IPIs to APs. Queue wakeups and remote
  thread termination use separate reschedule IPIs.
- Active-processor accounting prevents process teardown while any thread in
  the process is still executing.
- Kernel worker threads in the System process are currently serialized. They
  share legacy subsystem globals that still need their own inter-CPU locks.

The queue and dispatch lock prioritize correctness over scaling to very high
core counts. Per-CPU run queues and load balancing are not implemented.

## Address spaces and TLBs

Page-table changes use a global page-table lock. After a mapping, unmapping, or
protection change, the modifying CPU invalidates its local translation and
sends an NMI shootdown to each admitted CPU using that address space. NMI
delivery works while a target has maskable interrupts disabled. The target set
comes from a per-CPU active-address-space field published before CR3 changes.
Each target checks its current CR3, invalidates the address when relevant, and
acknowledges the request. The caller waits for all acknowledgements before
returning, so unmap callers can reclaim physical pages safely. Kernel mapping
changes invalidate the address on every CPU because the kernel half is shared
by all processes.

## Build and validation

SMP is enabled by default; `make SMP=0` builds the single-CPU configuration.
The four-CPU QEMU smoke run exercises AP admission, shell startup, input, and
user-process lifecycle. The scheduler has not been validated on physical
hardware. Real-machine AP startup depends on firmware ACPI/APIC tables and the
platform's interrupt routing.
