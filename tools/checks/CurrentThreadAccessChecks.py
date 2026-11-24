#!/usr/bin/env python3
"""Regression check for migration-safe access to the current KTHREAD."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
header = (ROOT / "kernel/include/svindows/Ke.h").read_text(encoding="utf-8")
thread_c = (ROOT / "kernel/ke/Thread.c").read_text(encoding="utf-8")

fast_start = header.index("static ALWAYS_INLINE PKTHREAD KeGetCurrentThreadFast(void)")
fast_end = header.index("\n}", fast_start) + 2
fast_body = header[fast_start:fast_end]
assert 'movq %%gs:16, %0' in fast_body, fast_body
assert 'KeGetCurrentProcessorBlock()' not in fast_body, fast_body
assert '__builtin_offsetof(KPRCB, CurrentThread) == 0x10' in header
assert '__builtin_offsetof(KPRCB, Self) == 0x18' in header

slow_start = thread_c.index("PKTHREAD KeGetCurrentThread(void)")
slow_end = thread_c.index("\n}", slow_start) + 2
slow_body = thread_c[slow_start:slow_end]
assert 'return KeGetCurrentThreadFast();' in slow_body, slow_body
assert 'KeGetCurrentProcessorBlock()' not in slow_body, slow_body

print("PASS migration-safe current-thread GS access")
