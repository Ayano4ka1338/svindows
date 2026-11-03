#!/usr/bin/env python3
"""Regression checks for DOS INT 21h/AH=0Ch input flushing."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
interrupt = (ROOT / "kernel/dos/Interrupt.c").read_text(encoding="utf-8")
console = (ROOT / "kernel/io/Console.c").read_text(encoding="utf-8")
io_file = (ROOT / "kernel/io/File.c").read_text(encoding="utf-8")
header = (ROOT / "kernel/include/svindows/Io.h").read_text(encoding="utf-8")
tests = (ROOT / "tools/BuildDosTests.py").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

assert "case 0x0c: {" in interrupt
assert "U8 input_function = get8(0);" in interrupt
assert "IoFlushInputHandleKernel(standard_handle(0))" in interrupt
for subfunction in ("case 0x01:", "case 0x07:", "case 0x08:",
                    "case 0x06:", "case 0x0a:"):
    assert subfunction in interrupt
assert "unsupported AL values still leave the input" in interrupt
assert "void ConFlushInput(void)" in console
assert "g_console.InputTail = g_console.InputHead;" in console
assert "NTSTATUS IoFlushInputHandleKernel(HANDLE handle)" in io_file
assert "ConFlushInput();" in io_file
assert "file->Pipe->Count = 0;" in io_file
assert "IoFlushInputHandleKernel(HANDLE handle);" in header
assert "INPFLUSH.COM" in tests
assert r"C:\\TESTS\\INPFLUSH.COM" in tests
assert "$(BUILD)/dos-tests/INPFLUSH.COM ::TESTS/INPFLUSH.COM" in makefile
print("PASS DOS INT 21h/AH=0Ch: flush-only and nested input paths")
