#!/usr/bin/env python3
"""Stage the upstream Fastfetch x86_64 executable and PCI identification data."""
from base64 import b64decode
from hashlib import sha1
from io import BytesIO
import gzip
import json
import os
from pathlib import Path
import shutil
import tarfile
import zlib
from AlpineFetch import fetch_many

LOCK = Path(__file__).with_name("alpine-fastfetch.lock.json")
BRANCH = ""
ARCH = ""


def package_index(repository: str, raw: bytes) -> dict[str, str]:
    with tarfile.open(fileobj=BytesIO(raw), mode="r:gz") as archive:
        member = archive.extractfile("APKINDEX")
        if member is None:
            raise RuntimeError(f"APKINDEX missing from {repository} index")
        records = member.read().decode("utf-8", "strict").strip().split("\n\n")
    result = {}
    for record in records:
        fields = {}
        for line in record.splitlines():
            if len(line) >= 2 and line[1] == ":":
                fields[line[0]] = line[2:]
        if fields.get("A") == ARCH:
            result[fields.get("P", "")] = fields
    return result


def gzip_members(data: bytes) -> list[bytes]:
    """Split an APK's concatenated gzip members without altering their bytes."""
    members = []
    remaining = data
    while remaining:
        decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
        decoder.decompress(remaining)
        if not decoder.eof:
            raise ValueError("truncated gzip member in Alpine APK")
        consumed = len(remaining) - len(decoder.unused_data)
        if consumed <= 0:
            raise ValueError("invalid gzip member boundary in Alpine APK")
        members.append(remaining[:consumed])
        remaining = decoder.unused_data
    if len(members) < 2:
        raise ValueError("unexpected Alpine APK layout")
    return members


def extract_package(repository: str, name: str, version: str,
                    destination: Path, index_raw: bytes, apk_path: Path) -> None:
    index = package_index(repository, index_raw)
    record = index.get(name)
    if record is None or record.get("V") != version:
        raise RuntimeError(f"Alpine {name} {version} not found in {BRANCH}/{repository}")
    checksum = record.get("C", "")
    if not checksum.startswith("Q1"):
        raise RuntimeError(f"Alpine index has no SHA-1 data checksum for {name}")

    filename = f"{name}-{version}.apk"
    apk = apk_path.read_bytes()
    members = gzip_members(apk)
    # APK v2 C:Q1 is the SHA-1 of the compressed control member,
    # immediately before the final data member. It is NOT a checksum
    # of data.tar.gz (members[-1]).
    control_member = members[-2]
    expected = b64decode(checksum[2:], validate=True)
    if sha1(control_member).digest() != expected:
        raise ValueError(f"Alpine APK control checksum mismatch: {filename}")

    # Package payload files live in the final data.tar.gz member.
    with tarfile.open(fileobj=BytesIO(members[-1]), mode="r:gz") as archive:
        for member in archive:
            path = member.name.removeprefix("./")
            if name == "fastfetch":
                keep = (path == "usr/bin/fastfetch" or
                        path.startswith("usr/share/fastfetch/"))
            else:
                keep = path in ("usr/share/hwdata/pci.ids",
                                "usr/share/hwdata/pci.ids.gz")
            if not keep or not member.isfile():
                continue
            source = archive.extractfile(member)
            if source is None:
                raise RuntimeError(f"Cannot extract {path} from {filename}")
            output_path = path
            payload = source.read()
            if path == "usr/share/hwdata/pci.ids.gz":
                output_path = "usr/share/hwdata/pci.ids"
                payload = gzip.decompress(payload)
            output = destination / output_path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(payload)


def stage(destination: Path) -> None:
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    branch = lock["branch"]
    architecture = lock["architecture"]
    global BRANCH, ARCH
    BRANCH, ARCH = branch, architecture
    repositories = sorted({package["repository"] for package in lock["packages"].values()})
    index_paths = fetch_many(
        {repository: f"{BRANCH}/{repository}/{ARCH}/APKINDEX.tar.gz"
         for repository in repositories}, immutable=False, label="fastfetch index")
    apk_paths = fetch_many(
        {name: f"{BRANCH}/{package['repository']}/{ARCH}/{name}-{package['version']}.apk"
         for name, package in lock["packages"].items()},
        immutable=True, label="fastfetch APK")
    for name, package in lock["packages"].items():
        repository = package["repository"]
        extract_package(repository, name, package["version"], destination,
                        index_paths[repository].read_bytes(), apk_paths[name])

    binary = destination / "usr/bin/fastfetch"
    if not binary.is_file():
        raise RuntimeError("Fastfetch package did not contain /usr/bin/fastfetch")
    header = binary.read_bytes()[:20]
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[4] != 2 or int.from_bytes(header[18:20], "little") != 62:
        raise RuntimeError("Fastfetch package is not an x86_64 ELF executable")
    binary.chmod(0o755)
    pci = destination / "usr/share/hwdata/pci.ids"
    if not pci.is_file() and not (destination / "usr/share/hwdata/pci.ids.gz").is_file():
        raise RuntimeError("PCI identification data package did not contain pci.ids")

    os_release = destination / "etc/os-release"
    os_release.parent.mkdir(parents=True, exist_ok=True)
    os_release.write_text(
        'NAME="Svindows"\n'
        'PRETTY_NAME="Svindows 0.6.0"\n'
        'ID=svindows\n'
        'VERSION_ID=0.6.0\n'
        'VERSION="0.6.0"\n'
        'ANSI_COLOR="0;36"\n',
        encoding="utf-8")
    (destination / ".ready").write_text(
        "Svindows Fastfetch 2.44.0-r0 x86_64\n", encoding="ascii")


if __name__ == "__main__":
    stage(Path(os.sys.argv[1]))
