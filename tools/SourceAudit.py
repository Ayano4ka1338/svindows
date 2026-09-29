#!/usr/bin/env python3
"""Run Clang's path-sensitive analyzer on every kernel/user C translation unit."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def main():
    failed = 0
    count = 0
    reports = []
    for tree in ("kernel", "user"):
        for source in sorted((ROOT / tree).rglob("*.c")):
            command = ["clang", "--analyze", "-ffreestanding", "-fno-builtin",
                       "-Xanalyzer", "-analyzer-output=text"]
            if tree == "kernel":
                # Kernel VAs, page tables, PEB/TEB mappings deliberately use
                # fixed addresses. This checker has no model of MmMapPage;
                # disable only its generic fixed-address warning.
                command += ["-std=gnu11", "-mgeneral-regs-only", "-Ikernel/include",
                            "-Wno-multichar", "-Xanalyzer", "-analyzer-disable-checker",
                            "-Xanalyzer", "core.FixedAddressDereference"]
            else:
                command += ["--target=x86_64-pc-windows-msvc", "-Iuser/include"]
            command.append(str(source.relative_to(ROOT)))
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            count += 1
            if result.returncode or "warning:" in result.stderr or "error:" in result.stderr:
                failed += 1
                print(f"FAIL {source.relative_to(ROOT)}", flush=True)
                print(result.stderr, flush=True)
            reports.append(f"{source.relative_to(ROOT)}\n{result.stdout}{result.stderr}")
    output = ROOT / "build" / "checks"
    output.mkdir(parents=True, exist_ok=True)
    (output / "source-audit.log").write_text("\n".join(reports))
    print(f"{'FAIL' if failed else 'PASS'} source audit: {count} C units, {failed} flagged")
    return int(bool(failed))


if __name__ == "__main__":
    raise SystemExit(main())
