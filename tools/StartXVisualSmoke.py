#!/usr/bin/env python3
"""Boot startx and capture the Xorg/TWM/xterm screen and serial transcript."""

import argparse
import json
import os
from pathlib import Path
import re
import selectors
import socket
import subprocess
import tempfile
import time


ROOT = Path(__file__).resolve().parent.parent
CHECKS = ROOT / "build/checks"


def ppm_pixel(path, x, y):
    with path.open("rb") as image:
        if image.readline() != b"P6\n":
            raise RuntimeError("QEMU did not produce a binary PPM image")
        width, height = map(int, image.readline().split())
        if image.readline() != b"255\n" or x >= width or y >= height:
            raise RuntimeError("unexpected QEMU screen geometry")
        image.seek((y * width + x) * 3, os.SEEK_CUR)
        return tuple(image.read(3))


def qmp_screendump(path, screenshot, timeout, keyboard_probe=False):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as qmp:
        qmp.settimeout(timeout)
        qmp.connect(str(path))
        replies = qmp.makefile("rb")
        if "QMP" not in json.loads(replies.readline()):
            raise RuntimeError("QMP greeting missing")
        def run(request):
            qmp.sendall((json.dumps(request) + "\n").encode())
            while True:
                reply = json.loads(replies.readline())
                if "error" in reply:
                    raise RuntimeError(f"QMP error: {reply['error']}")
                if "return" in reply:
                    break

        run({"execute": "qmp_capabilities"})
        run({"execute": "screendump",
             "arguments": {"filename": str(screenshot)}})
        if not keyboard_probe:
            return
        # Move to the upper-right corner, then into the xterm client area
        # before clicking. A click at (0, 0) lands on the root window, so
        # following it with key events would never exercise the shell.
        for _ in range(12):
            run({"execute": "input-send-event", "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": 100}},
                {"type": "rel", "data": {"axis": "y", "value": -100}},
            ]}})
        run({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": -80}},
            {"type": "rel", "data": {"axis": "y", "value": 80}},
        ]}})
        for down in (True, False):
            run({"execute": "input-send-event", "arguments": {"events": [
                {"type": "btn", "data": {"down": down,
                                          "button": "left"}},
            ]}})
        for key in ("x", "e", "y", "e", "s", "ret"):
            for down in (True, False):
                run({"execute": "input-send-event", "arguments": {
                    "events": [{"type": "key", "data": {
                        "down": down,
                        "key": {"type": "qcode", "data": key},
                    }}],
                }})
            time.sleep(0.03)
        time.sleep(2)
        run({"execute": "screendump", "arguments": {
            "filename": str(CHECKS / "startx-typed.ppm")}})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--kvm", action="store_true")
    parser.add_argument("--keyboard-probe", action="store_true")
    args = parser.parse_args()
    CHECKS.mkdir(parents=True, exist_ok=True)
    screenshot = CHECKS / "startx-visual.ppm"
    transcript = CHECKS / "startx-visual.log"

    with tempfile.TemporaryDirectory(prefix="svindows-startx-") as temporary:
        temporary = Path(temporary)
        qmp_path = temporary / "qmp.sock"
        command = ["qemu-system-x86_64"]
        if args.kvm:
            command.append("-enable-kvm")
        command += [
            "-m", "1024M", "-no-reboot", "-no-shutdown", "-boot", "d",
            "-cdrom", str(ROOT / "build/svindows.iso"),
            "-display", "none", "-monitor", "none", "-serial", "stdio",
            "-qmp", f"unix:{qmp_path},server=on,wait=off",
        ]
        process = subprocess.Popen(command, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        output = bytearray()
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)

                def read_until(pattern, timeout):
                    deadline = time.monotonic() + timeout
                    while time.monotonic() < deadline:
                        rendered = output.decode("utf-8", errors="replace")
                        if re.search(pattern, rendered):
                            return
                        if "*** STOP:" in rendered:
                            raise RuntimeError("guest reported a kernel stop")
                        if process.poll() is not None:
                            raise RuntimeError(f"QEMU exited: {process.returncode}")
                        for key, _ in selector.select(0.5):
                            chunk = os.read(key.fileobj.fileno(), 65536)
                            if not chunk:
                                raise RuntimeError("QEMU serial output closed")
                            output.extend(chunk)
                    raise TimeoutError(f"waiting for {pattern}")

                read_until(r"C:\\>", args.timeout)
                process.stdin.write(b"startx.elf\n")
                process.stdin.flush()
                read_until(r"loaded 'C:\\usr\\bin\\xterm'", args.timeout)
                deadline = time.monotonic() + 20
                while time.monotonic() < deadline:
                    for key, _ in selector.select(0.5):
                        chunk = os.read(key.fileobj.fileno(), 65536)
                        if chunk:
                            output.extend(chunk)
                qmp_screendump(qmp_path, screenshot, args.timeout,
                               args.keyboard_probe)
                if args.keyboard_probe:
                    read_until(r"loaded 'C:\\usr\\bin\\xeyes'", 20)
            root_dark = ppm_pixel(screenshot, 100, 100)
            root_light = ppm_pixel(screenshot, 101, 101)
            terminal = ppm_pixel(screenshot, 1100, 100)
            rendered = output.decode("utf-8", errors="replace")
            if (root_dark != (160, 160, 160) or
                    root_light != (192, 192, 192) or
                    terminal != (255, 255, 255)):
                raise RuntimeError("root pattern or xterm window is not visible: "
                                   f"{root_dark}, {root_light}, {terminal}")
            if ("loaded 'C:\\usr\\bin\\cmd'" not in rendered or
                    "Invalid argument" in rendered):
                raise RuntimeError("xterm did not start cmd.exe")
            if args.keyboard_probe and "loaded 'C:\\usr\\bin\\xeyes'" not in rendered:
                raise RuntimeError("xterm keyboard could not launch xeyes from cmd.exe")
            print("PASS Xorg root pattern and xterm shell visible in QEMU")
            print(f"Captured {screenshot}")
            print(f"Serial transcript: {transcript}")
        finally:
            transcript.write_bytes(output)
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


if __name__ == "__main__":
    main()
