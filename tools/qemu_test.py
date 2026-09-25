#!/usr/bin/env python3
"""Run a serial-only SVINDOWS regression and stop on explicit PASS/FAIL markers."""

import argparse
import pathlib
import selectors
import subprocess
import sys
import time


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--name", default="qemu")
    parser.add_argument("--timeout", type=float, default=35.0)
    parser.add_argument("--wait-for", default="")
    parser.add_argument("--send", default="")
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument("--expect-count", action="append", default=[],
                        help="MARKER=COUNT: require at least COUNT occurrences")
    parser.add_argument("--fail", action="append", default=[])
    parser.add_argument("--iso", default="build/svindows.iso")
    parser.add_argument("--disk", default="build/disk.img")
    return parser.parse_args()


def parse_counts(entries):
    counts = []
    for entry in entries:
        marker, _, value = entry.rpartition("=")
        counts.append((marker, int(value)))
    return counts


def main():
    args = parse_args()
    expected_counts = parse_counts(args.expect_count)
    command = [
        "qemu-system-x86_64", "-m", "1024M", "-no-reboot",
        "-no-shutdown", "-boot", "d", "-cdrom", args.iso,
        "-drive", f"file={args.disk},format=raw,if=ide,index=0,media=disk",
        "-serial", "stdio", "-display", "none", "-monitor", "none",
    ]
    log_path = pathlib.Path("build") / f"{args.name}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    process = subprocess.Popen(
        command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, bufsize=0,
    )
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    output = bytearray()
    sent = not args.send
    deadline = time.monotonic() + args.timeout
    failure_markers = ["BUGCHECK", "=== TRAP:", "PANIC", "TEST: FAIL"]
    failure_markers.extend(args.fail)

    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                break
            for key, _ in selector.select(timeout=0.1):
                chunk = key.fileobj.read(4096)
                if chunk:
                    output.extend(chunk)
                    sys.stdout.buffer.write(chunk)
                    sys.stdout.buffer.flush()
            text = output.decode("utf-8", errors="replace")
            if not sent and (not args.wait_for or args.wait_for in text):
                process.stdin.write(args.send.encode("utf-8"))
                process.stdin.flush()
                sent = True
            failed = next((marker for marker in failure_markers
                           if marker in text), None)
            if failed:
                print(f"\n[{args.name}] FAIL: saw {failed!r}", file=sys.stderr)
                return 1
            counts_met = all(text.count(marker) >= count
                             for marker, count in expected_counts)
            if sent and counts_met and \
                    all(marker in text for marker in args.expect):
                print(f"\n[{args.name}] PASS")
                return 0
        text = output.decode("utf-8", errors="replace")
        missing = [marker for marker in args.expect if marker not in text]
        missing += [f"{marker} x{count} (saw {text.count(marker)})"
                    for marker, count in expected_counts
                    if text.count(marker) < count]
        reason = f"missing markers: {missing}" if missing else "QEMU exited"
        print(f"\n[{args.name}] FAIL: {reason}", file=sys.stderr)
        return 1
    finally:
        log_path.write_bytes(output)
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        print(f"[{args.name}] serial log: {log_path}")


if __name__ == "__main__":
    raise SystemExit(main())
