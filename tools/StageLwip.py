#!/usr/bin/env python3
"""Fetch and stage the pinned upstream lwIP release for the Svindows kernel."""
from __future__ import annotations

import json
import os
import shutil
import sys
import tarfile
import tempfile
import urllib.error
import urllib.request
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
LOCK = ROOT / "tools" / "lwip.lock.json"
USER_AGENT = "Svindows-build/1.0 (pinned third-party source fetcher)"


def download(url: str, target: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=60) as response, target.open("wb") as out:
        shutil.copyfileobj(response, out)


def safe_extract(archive: Path, destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as tf:
        for member in tf.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"unsafe path in lwIP archive: {member.name}")
            if not (member.isfile() or member.isdir()):
                continue
            tf.extract(member, destination, filter="data")


def locate_source(extracted: Path) -> Path:
    matches = list(extracted.glob("*/src/include/lwip/init.h"))
    if len(matches) != 1:
        raise RuntimeError("downloaded archive does not contain one lwIP src tree")
    return matches[0].parents[2]


def verify_version(source: Path) -> None:
    header = source / "include" / "lwip" / "init.h"
    content = header.read_text(encoding="utf-8", errors="strict")
    required = (
        "#define LWIP_VERSION_MAJOR      2",
        "#define LWIP_VERSION_MINOR      2",
        "#define LWIP_VERSION_REVISION   1",
    )
    for marker in required:
        if marker not in content:
            raise RuntimeError(f"lwIP version check failed: missing {marker!r}")

    # These paths are part of the lwIP public include layout. Catch an
    # incomplete or differently packaged source tree during staging rather
    # than failing later while compiling the kernel network adapter.
    for relative in ("include/lwip/arch.h", "include/netif/ethernet.h"):
        if not (source / relative).is_file():
            raise RuntimeError(f"lwIP source tree is missing required header: {relative}")


def write_makefile(destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    root = (destination / "src").resolve()
    text = (
        f"LWIP_ROOT := {root}\n"
        "LWIP_C_SRC := $(sort $(wildcard $(LWIP_ROOT)/core/*.c) "
        "$(wildcard $(LWIP_ROOT)/core/ipv4/*.c) "
        "$(wildcard $(LWIP_ROOT)/api/*.c) "
        "$(LWIP_ROOT)/netif/ethernet.c)\n"
    )
    (destination / "sources.mk").write_text(text, encoding="utf-8")


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    ready = destination / ".ready"
    source_root = destination / "src"
    if ready.exists() and (source_root / "include/lwip/init.h").exists():
        verify_version(source_root)
        write_makefile(destination)
        return

    destination.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="svindows-lwip-") as temp_name:
        temp = Path(temp_name)
        archive = temp / "lwip.tar.gz"
        extracted = temp / "extract"
        errors = []
        for url in (lock["primary_url"], lock["fallback_url"]):
            try:
                download(url, archive)
                safe_extract(archive, extracted)
                source = locate_source(extracted)
                verify_version(source)
                replacement = destination / "src.new"
                if replacement.exists(): shutil.rmtree(replacement)
                shutil.copytree(source, replacement)
                if source_root.exists(): shutil.rmtree(source_root)
                replacement.rename(source_root)
                break
            except Exception as exc:  # fallback to the official Savannah snapshot
                errors.append(f"{url}: {exc}")
                archive.unlink(missing_ok=True)
                if extracted.exists(): shutil.rmtree(extracted)
        else:
            raise RuntimeError("unable to stage pinned lwIP 2.2.1:\n" + "\n".join(errors))

    verify_version(source_root)
    write_makefile(destination)
    ready.write_text(lock["commit"] + "\n", encoding="ascii")
    print(f"Staged upstream lwIP {lock['version']} ({lock['commit']}) in {destination}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: StageLwip.py <destination>")
    stage(Path(sys.argv[1]))
