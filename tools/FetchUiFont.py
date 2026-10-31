#!/usr/bin/env python3
"""Fetch the TTF used to regenerate the kernel UI font atlas.

The generated atlas (kernel/hal/UiFontData.c) is committed, so this is only
needed when changing the glyph set or the rasterization parameters:

    python3 tools/FetchUiFont.py build/fontcache/selawk.ttf
    make ui-font

Selawik is Microsoft's SIL OFL 1.1 font that is metric-compatible with Segoe
UI; the release archive is fetched from the upstream GitHub release.
"""
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RELEASE = ("https://github.com/microsoft/Selawik/releases/download/1.01/"
           "Selawik_Release.zip")
MEMBER = "selawk.ttf"


def main():
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        ROOT / "build/fontcache/selawk.ttf"
    if target.exists():
        print(f"{target} already present ({target.stat().st_size} bytes)")
        return 0
    target.parent.mkdir(parents=True, exist_ok=True)
    archive = target.parent / "Selawik_Release.zip"
    print(f"fetching {RELEASE}")
    subprocess.run(["curl", "-sSL", "--fail", "-o", str(archive), RELEASE],
                   check=True)
    with zipfile.ZipFile(archive) as bundle:
        with bundle.open(MEMBER) as source:
            data = source.read()
    target.write_bytes(data)
    print(f"{target}: {len(data)} bytes, sha256 "
          f"{hashlib.sha256(data).hexdigest()[:16]}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
