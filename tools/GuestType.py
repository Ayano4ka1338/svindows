#!/usr/bin/env python3
"""Run the guest once and type a single command, capturing all serial output.

Usage: GuestType.py "<command>" [seconds]
Boots build/svindows.iso against a disposable copy of build/disk.img, waits for
the shell prompt, types the command, and dumps everything seen on the serial
line for inspection. Used to reproduce the interactive `xorg` path (which uses
the lock file and signal setup) rather than the -nolock smoke path.
"""
import os
import re
import selectors
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-9;?]*[ -/]*[@-~]")


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: GuestType.py <command> [seconds]", file=sys.stderr)
        return 2
    command = sys.argv[1]
    limit = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0

    with tempfile.TemporaryDirectory() as temporary:
        disk = Path(temporary) / "disk.img"
        shutil.copyfile(ROOT / "build/disk.img", disk)
        process = subprocess.Popen([
            "qemu-system-x86_64", "-m", "1024M", "-no-reboot", "-no-shutdown",
            "-boot", "d", "-cdrom", str(ROOT / "build/svindows.iso"),
            "-drive", f"file={disk},format=raw,if=ide",
            "-display", "none", "-monitor", "none", "-serial", "stdio",
        ], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
           stderr=subprocess.STDOUT)
        captured = bytearray()
        sent = False
        deadline = time.monotonic() + limit
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while time.monotonic() < deadline:
                    events = selector.select(timeout=1.0)
                    for _key, _mask in events:
                        chunk = process.stdout.read(4096)
                        if not chunk:
                            break
                        captured.extend(chunk)
                    text = ANSI.sub("", captured.decode("utf-8", "replace"))
                    if not sent and re.search(r"C:\\>", text):
                        process.stdin.write((command + "\n").encode())
                        process.stdin.flush()
                        sent = True
                    if process.poll() is not None:
                        break
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        sys.stdout.write(ANSI.sub("", captured.decode("utf-8", "replace")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
