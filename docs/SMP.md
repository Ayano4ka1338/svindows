# Multiprocessor scheduler (initial SMP stage)

The x86-64 kernel now admits application processors (APs) to the executive
scheduler instead of leaving them parked after firmware discovery. The design
is deliberately conservative while the memory manager still lacks remote TLB
shootdowns.

## Scheduling model

- All CPUs share one ready queue protected by the scheduler spin lock.
- Each CPU has its own PRCB current thread, idle context, kernel entry stack,
  quantum and accounting counters.
- APs are started from the ACPI MADT topology. The real-mode trampoline sets
  PAE, OSFXSR, OSXMMEXCPT, LME and NXE before entering the runtime page tables.
- `HAL_SMP_WAKE_VECTOR` (`0xF1`) is a non-periodic wake/termination IPI.
  `HAL_SMP_TICK_VECTOR` (`0xF2`) carries the BSP's 100-Hz scheduler tick to
  online APs. The BSP remains the source of system time.
- Ready threads can migrate between CPUs. The scheduler skips a candidate
  while its process address space is active on another CPU.

## Current safety boundary

`KPROCESS.ActiveProcessorCount` limits each process address space to one active
CPU. This avoids stale remote TLB entries while the VM's map/unmap/protect paths
do not yet issue cross-CPU shootdowns. Different processes can execute at the
same time on different processors; threads belonging to the same process are
serialized. Process/thread teardown sends a reschedule IPI and waits until a
remote thread has left its CPU before reclaiming its stack and thread-local
resources.

This is not yet a full SMP kernel. Some older shared data structures in other
subsystems are protected only by local interrupt disable (CLI), not by an
inter-processor lock. AP execution is therefore an experimental scheduler
stage; further work must audit those subsystems and add remote TLB shootdowns
before allowing multiple CPUs to execute one address space concurrently.

## Boot and build

SMP is enabled by default (`SMP := 1`); `make SMP=0` builds the single-CPU
configuration. Serial output should include lines similar to:

```text
[smp]  ACPI MADT: N enabled processor(s), BSP APIC ID ...
[smp]  CPU 1 (APIC ... ) initialized
[smp]  CPU 1 (APIC ... ): scheduler admission published
[smp]  multiprocessor scheduler online: N/N CPU
```

A CPU counts as online only after its AP initialization completed and its
scheduler admission flag was published. `SystemBasicInformation` and
`SystemProcessorPerformanceInformation` report only admitted processors.

Host validation can compile the changed kernel C files and run the CPU-accounting
and processor-information checks. A full ISO build, QEMU SMP boot and physical
hardware boot still need to be run with NASM, QEMU and the target machine
available; compilation of C files alone does not prove AP startup on hardware.
