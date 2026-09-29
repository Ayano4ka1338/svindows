#!/usr/bin/env python3
"""Boot and exercise shipped commands on a disposable copy of the FAT disk."""
import argparse
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--repeat", type=int, default=1)
    args = parser.parse_args()
    if args.timeout <= 0 or args.repeat <= 0:
        parser.error("timeout and repeat must be positive")
    log_dir = ROOT / "build" / "checks"
    log_dir.mkdir(parents=True, exist_ok=True)
    for iteration in range(args.repeat):
        raw = bytearray()
        process = None
        try:
            with tempfile.TemporaryDirectory(prefix="svindows-smoke-") as temporary:
                disk = Path(temporary) / "disk.img"
                shutil.copyfile(ROOT / "build/disk.img", disk)
                qmp_path = Path(temporary) / "qmp.sock"
                process = subprocess.Popen([
                    "qemu-system-x86_64", "-m", "1024M", "-no-reboot",
                    "-no-shutdown", "-boot", "d", "-cdrom",
                    str(ROOT / "build/svindows.iso"), "-drive",
                    f"file={disk},format=raw,if=ide", "-display", "none",
                    "-monitor", "none", "-serial", "stdio",
                    "-qmp", f"unix:{qmp_path},server=on,wait=off",
                ], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                   stderr=subprocess.STDOUT)
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)

                    def wait_for(pattern, start=0):
                        deadline = time.monotonic() + args.timeout
                        while True:
                            text = ANSI.sub("", raw.decode("utf-8", errors="replace"))
                            if re.search(r"BUGCHECK|CPU EXCEPTION|\[ failed", text):
                                raise RuntimeError("kernel boot/fault failure")
                            match = re.search(pattern, text[start:])
                            if match:
                                return text[start:]
                            if process.poll() is not None:
                                raise RuntimeError(f"QEMU exited: {process.returncode}")
                            remaining = deadline - time.monotonic()
                            if remaining <= 0:
                                raise TimeoutError(f"waiting for {pattern!r}")
                            for key, _ in selector.select(min(remaining, 0.5)):
                                chunk = os.read(key.fileobj.fileno(), 65536)
                                if not chunk:
                                    raise RuntimeError("QEMU serial output closed")
                                raw.extend(chunk)

                    wait_for(r"C:\\>")
                    # Exercise the real emulated PS/2 controller and ISR/DPC
                    # path, not only the serial input used by other commands.
                    start = len(ANSI.sub("", raw.decode("utf-8", errors="replace")))
                    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as qmp:
                        qmp.settimeout(args.timeout)
                        qmp.connect(str(qmp_path))
                        with qmp.makefile("rb") as replies:
                            if "QMP" not in json.loads(replies.readline()):
                                raise RuntimeError("missing QMP greeting")

                            def qmp_execute(command, arguments=None):
                                request = {"execute": command}
                                if arguments is not None:
                                    request["arguments"] = arguments
                                qmp.sendall((json.dumps(request) + "\n").encode())
                                while True:
                                    line = replies.readline()
                                    if not line:
                                        raise RuntimeError("QMP connection closed")
                                    reply = json.loads(line)
                                    if "error" in reply:
                                        raise RuntimeError(f"QMP error: {reply['error']}")
                                    if "return" in reply:
                                        return reply["return"]

                            qmp_execute("qmp_capabilities")
                            for char in "echo ps2ok\n":
                                code = {" ": "spc", "\n": "ret"}.get(char, char)
                                qmp_execute("send-key", {"keys": [{"type": "qcode", "data": code}],
                                                         "hold-time": 10})
                                time.sleep(0.03)  # release before the next key
                    wait_for(r"\nps2ok\r*\n", start)
                    print("PASS PS/2 keyboard ISR/DPC input", flush=True)
                    commands = [
                        ("ver", r"Svindows"),
                        ("hostname", r"(?i)SVINDOWS"),
                        ("dir", r"(?i)KERNEL32\.DLL"),
                        ("echo SVINDOWS_SMOKE_DATA > smoke.txt", None),
                        ("type smoke.txt", r"SVINDOWS_SMOKE_DATA"),
                        ("copy smoke.txt smoke2.txt", r"(?i)copied"),
                        ("del smoke.txt", None),
                        ("type smoke2.txt", r"SVINDOWS_SMOKE_DATA"),
                        ("ver | hostname", r"(?i)SVINDOWS"),
                        ("echo MANY_REDIRECTS > a.txt > b.txt > c.txt > d.txt", None),
                        ("type d.txt", r"MANY_REDIRECTS"),
                        ("mkdir smoketest", None),
                        ("cd smoketest", None),
                        ("cd ..", None),
                        ("rmdir smoketest", None),
                    ]
                    for index, (command, expected) in enumerate(commands):
                        # A separate echo synchronizes completion after external
                        # PE execution; matching requires its output line, not
                        # the echoed input/edit buffer.
                        start = len(ANSI.sub("", raw.decode("utf-8", errors="replace")))
                        marker = f"SMOKE_DONE_{index}"
                        status_marker = f"SMOKE_STATUS_{index}_"
                        process.stdin.write((command + "\necho " + status_marker +
                                             "%ERRORLEVEL%\necho " + marker + "\n").encode())
                        process.stdin.flush()
                        output = wait_for(r"(?:\r*\n)" + marker + r"\r*\n", start)
                        if expected and not re.search(expected, output):
                            raise RuntimeError(f"unexpected output for {command!r}")
                        if not re.search(r"\n" + status_marker + r"0\r*\n", output):
                            raise RuntimeError(f"nonzero errorlevel for {command!r}")
                        if re.search(r"(?i)not recognized|cannot find|access is denied|failed", output):
                            raise RuntimeError(f"command failed: {command!r}")
                        print(f"PASS {command}", flush=True)
                process.terminate()
                process.wait(timeout=5)
                process = None
                print(f"PASS QEMU boot {iteration + 1}/{args.repeat}", flush=True)
        except (OSError, RuntimeError, ValueError, TimeoutError, subprocess.TimeoutExpired) as error:
            print(f"FAIL {error}", flush=True)
            return 1
        finally:
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            (log_dir / f"qemu-{iteration + 1}.log").write_bytes(raw)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
