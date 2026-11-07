#!/usr/bin/env python3
"""Boot the Limine ISO and exercise the system from its FAT initrd."""
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
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--timeout", type=float, default=45)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--skip-network", action="store_true",
                        help="skip the DHCP/DNS/TCP/HTTP/HTTPS check when offline")
    parser.add_argument("--doom-probe", action="store_true",
                        help="launch the bundled Doom DOS/4G image and require VGA graphics startup")
    parser.add_argument("--doom-probe-timeout", type=float, default=180,
                        help="seconds to wait for Doom graphics startup (default: 180)")
    args = parser.parse_args()
    if args.timeout <= 0 or args.repeat <= 0 or args.doom_probe_timeout <= 0:
        parser.error("timeouts and repeat must be positive")
    log_dir = ROOT / "build" / "checks"
    log_dir.mkdir(parents=True, exist_ok=True)
    for iteration in range(args.repeat):
        raw = bytearray()
        process = None
        try:
            with tempfile.TemporaryDirectory(prefix="svindows-smoke-") as temporary:
                qmp_path = Path(temporary) / "qmp.sock"
                process = subprocess.Popen([
                    "qemu-system-x86_64", "-m", "1024M", "-no-reboot",
                    "-no-shutdown", "-boot", "d", "-machine", "pc", "-cpu", "max",
                    "-cdrom",
                    str(ROOT / "build/svindows.iso"), "-netdev", "user,id=svnet0",
                    "-device", "virtio-net-pci,netdev=svnet0,disable-modern=on,bus=pci.0,addr=0x04",
                    "-audiodev", "none,id=svaudio0",
                    "-device", "sb16,audiodev=svaudio0,irq=5,dma=1,dma16=5",
                    "-display", "none", "-monitor", "none", "-serial", "stdio",
                    "-qmp", f"unix:{qmp_path},server=on,wait=off",
                ], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                   stderr=subprocess.STDOUT)
                with selectors.DefaultSelector() as selector:
                    selector.register(process.stdout, selectors.EVENT_READ)

                    def wait_for(pattern, start=0, timeout=None):
                        deadline = time.monotonic() + (args.timeout if timeout is None else timeout)
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
                    if not args.skip_network:
                        wait_for(r"\[net\] DHCP lease address=", 0,
                                 timeout=args.timeout)
                        print("PASS QEMU DHCP lease", flush=True)
                    commands = [
                        *(([] if args.skip_network else [
                            ("ping -c 1 10.0.2.2", r"1 packets transmitted, 1 received"),
                            ("ping -c 1 example.com", r"1 packets transmitted, 1 received"),
                            ("curl -I http://example.com/",
                             r"HTTP/1\.[01] [1-5][0-9][0-9]"),
                            ("curl --version", r"curl 8\.14\.1"),
                            ("curl -I --connect-timeout 10 https://example.com/",
                             r"HTTP/(?:1\.[01]|2) [1-5][0-9][0-9]"),
                        ])),
                        ("ver", r"Svindows"),
                        ("hostname", r"(?i)SVINDOWS"),
                        ("dir", r"(?i)KERNEL32\.DLL"),
                        ("echo SVINDOWS_SMOKE_DATA > smoke.txt", None),
                        ("type smoke.txt", r"SVINDOWS_SMOKE_DATA"),
                        ("copy smoke.txt smoke2.txt", r"(?i)copied"),
                        ("del smoke.txt", None),
                        ("type smoke2.txt", r"SVINDOWS_SMOKE_DATA"),
                        ("ver | hostname", r"(?i)SVINDOWS"),
                        (r"C:\TESTS\COMSMOKE.COM", r"DOS-COM PASS"),
                        (r"C:\TESTS\MEMSMOKE.COM", r"DOS-MEM PASS"),
                        (r"C:\TESTS\DPMIINFO.COM", r"DOS-DPMI PASS"),
                        (r"C:\TESTS\MZSMOKE.EXE", r"DOS-MZ PASS"),
                        ("doscheck.bat", r"(?s)(?=.*DOS-COM PASS)(?=.*DOS-MEM PASS)(?=.*DOS-DPMI PASS)(?=.*DOS-MZ PASS)"),
                        ("mkdir smoketest", None),
                        ("cd smoketest", None),
                        ("cd ..", None),
                        ("rmdir smoketest", None),
                    ]
                    for index, check in enumerate(commands):
                        command, expected = check[:2]
                        expected_status = check[2] if len(check) > 2 else 0
                        # A separate echo synchronizes completion after external
                        # PE execution; matching requires its output line, not
                        # the echoed input/edit buffer.
                        start = len(ANSI.sub("", raw.decode("utf-8", errors="replace")))
                        marker = f"SMOKE_DONE_{index}"
                        status_marker = f"SMOKE_STATUS_{index}_"
                        process.stdin.write((command + "\n").encode())
                        process.stdin.flush()
                        # A batch can launch several DOS children. If we queue
                        # the synchronization commands immediately, its first
                        # child may consume those keystrokes as stdin. Wait for
                        # the batch's final expected output before sending the
                        # parent-shell markers.
                        if command.lower().endswith(".bat") and expected:
                            wait_for(expected, start)
                        process.stdin.write(("echo " + status_marker +
                                             "%ERRORLEVEL%\necho " + marker + "\n").encode())
                        process.stdin.flush()
                        # Serial kernel diagnostics can arrive immediately
                        # after a command's marker, before cmd emits CR/LF.
                        # The marker must start on an output line (so the
                        # echoed command/edit buffer cannot satisfy it), but
                        # need not end the line when a kernel log interleaves.
                        output = wait_for(r"[\r\n]" + marker + r"(?=[\r\n\[])", start)
                        if expected and not re.search(expected, output):
                            raise RuntimeError(f"unexpected output for {command!r}")
                        status_value = (r"[1-9][0-9]*" if expected_status == "nonzero"
                                        else "0")
                        if not re.search(r"[\r\n]" + status_marker +
                                         status_value + r"(?=[\r\n\[])", output):
                            raise RuntimeError(f"unexpected errorlevel for {command!r}")
                        if expected_status == 0 and re.search(
                                r"(?i)not recognized|cannot find|access is denied|failed",
                                output):
                            raise RuntimeError(f"command failed: {command!r}")
                        print(f"PASS {command}", flush=True)
                    if not args.skip_network:
                        print("PASS QEMU Internet: IPv4 + DHCP + DNS + ICMP + TCP + HTTP + HTTPS/TLS", flush=True)
                    if args.doom_probe:
                        # Require the real game startup, including WAD loading
                        # and mode 13h, then observe it for delayed CPU faults.
                        doom_start = len(ANSI.sub("", raw.decode("utf-8", errors="replace")))
                        process.stdin.write(b"doom\n")
                        process.stdin.flush()
                        output = wait_for(
                            r"\[dos\] DPMI initial 32-bit selectors",
                            doom_start, timeout=args.doom_probe_timeout)
                        output = wait_for(
                            r"\[dos-vga\] video mode=13|DOSVM:|thread .*DOOM.*terminated",
                            doom_start, timeout=args.doom_probe_timeout)
                        # Keep collecting after the first port access: an
                        # invalid CS default size can fault immediately after
                        # an apparently successful DPMI entry / IN instruction.
                        settle_deadline = time.monotonic() + 10
                        while time.monotonic() < settle_deadline:
                            if process.poll() is not None:
                                raise RuntimeError("QEMU exited during Doom probe")
                            for key, _ in selector.select(0.5):
                                chunk = os.read(key.fileobj.fileno(), 65536)
                                if not chunk:
                                    raise RuntimeError("QEMU serial output closed")
                                raw.extend(chunk)
                        output = ANSI.sub("", raw.decode("utf-8", errors="replace"))[doom_start:]
                        if re.search(r"DOSVM:|thread .*doom.*terminated|unsupported protected-mode instruction",
                                     output, re.IGNORECASE):
                            raise RuntimeError("Doom DPMI probe reached a protected-mode fault")
                        print("PASS Doom WAD loading and VGA graphics startup (gameplay not validated)", flush=True)
                process.terminate()
                process.wait(timeout=5)
                process = None
                print(f"PASS QEMU boot {iteration + 1}/{args.repeat}", flush=True)
        except (OSError, RuntimeError, ValueError, TimeoutError,
                subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
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
