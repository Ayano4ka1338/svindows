#!/usr/bin/env python3
"""Stage Alpine's static apk client, its repository keys, and configuration."""
from base64 import b64decode
from hashlib import sha1
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import tarfile
import zlib

from AlpineFetch import fetch_many

LOCK = Path(__file__).with_name("alpine-apk.lock.json")


def split_apk_members(data: bytes) -> list[bytes]:
    members, remaining = [], data
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


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    branch, arch = lock["branch"], lock["architecture"]
    repo, package, version = lock["repository"], lock["package"], lock["version"]
    keys_package, keys_version = lock["keys_package"], lock["keys_version"]
    relative = f"{branch}/{repo}/{arch}"
    index_path = fetch_many(
        {"index": f"{relative}/APKINDEX.tar.gz"},
        immutable=False, label="apk-tools index")["index"]
    apk_path = fetch_many(
        {"apk": f"{relative}/{package}-{version}.apk"},
        immutable=True, label="apk-tools-static APK")["apk"]
    keys_apk_path = fetch_many(
        {"keys": f"{relative}/{keys_package}-{keys_version}.apk"},
        immutable=True, label="Alpine repository keys APK")["keys"]

    with tarfile.open(index_path, "r:gz") as archive:
        index_member = archive.extractfile("APKINDEX")
        if index_member is None:
            raise RuntimeError("Alpine main APKINDEX has no APKINDEX member")
        records = index_member.read().decode("utf-8", "strict").strip().split("\n\n")
    record = None
    keys_record = None
    for text in records:
        fields = {line[0]: line[2:] for line in text.splitlines()
                  if len(line) >= 2 and line[1] == ":"}
        if fields.get("P") == package and fields.get("A") == arch:
            record = fields
        if fields.get("P") == keys_package and fields.get("A") == arch:
            keys_record = fields
    if not record or record.get("V") != version:
        raise RuntimeError(f"Pinned Alpine {package} {version} is missing from {branch}/{repo}")
    if not keys_record or keys_record.get("V") != keys_version:
        raise RuntimeError(f"Pinned Alpine {keys_package} {keys_version} is missing from {branch}/{repo}")

    def verified_payload(path: Path, pkg_record: dict, pkg_name: str) -> bytes:
        members = split_apk_members(path.read_bytes())
        checksum = pkg_record.get("C", "")
        if (not checksum.startswith("Q1") or
                sha1(members[-2]).digest() != b64decode(checksum[2:], validate=True)):
            raise ValueError(f"{pkg_name} APK control checksum does not match APKINDEX")
        return members[-1]

    apk_payload = verified_payload(apk_path, record, package)
    keys_payload = verified_payload(keys_apk_path, keys_record, keys_package)

    if destination.exists():
        shutil.rmtree(destination)
    root = destination / "root"
    root.mkdir(parents=True)
    # apk installs packages into a deliberately small FAT-backed root rather
    # than a normal Alpine rootfs. Seed the usual top-level destinations so
    # package extraction and atomic rename have their parents from the start.
    # Empty directories are included by the recursive root copy into the
    # initrd, and mkdirat can then create deeper package-specific paths.
    for directory in ("bin", "etc", "lib", "sbin", "usr/bin",
                      "usr/lib", "usr/sbin"):
        (root / directory).mkdir(parents=True, exist_ok=True)
    with tarfile.open(fileobj=BytesIO(apk_payload), mode="r:gz") as archive:
        for member in archive:
            path = member.name.removeprefix("./")
            # The package also carries its own build-signing keys; those are
            # not repository trust anchors. Install only the static client.
            if path != "sbin/apk.static": continue
            if not member.isfile():
                continue
            source = archive.extractfile(member)
            if source is None:
                raise RuntimeError(f"Cannot extract {path} from apk-tools-static")
            output = root / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(source.read())
    with tarfile.open(fileobj=BytesIO(keys_payload), mode="r:gz") as archive:
        for member in archive:
            path = member.name.removeprefix("./")
            if not path.startswith("etc/apk/keys/") or not member.isfile():
                continue
            source = archive.extractfile(member)
            if source is None:
                raise RuntimeError(f"Cannot extract {path} from {keys_package}")
            output = root / path
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_bytes(source.read())
    binary = root / "sbin/apk.static"
    header = binary.read_bytes()[:20] if binary.is_file() else b""
    if len(header) < 20 or header[:4] != b"\x7fELF" or header[4] != 2 or int.from_bytes(header[18:20], "little") != 62:
        raise RuntimeError("Alpine apk.static is not an x86_64 ELF executable")
    binary.chmod(0o755)
    keys = list((root / "etc/apk/keys").glob("*.rsa.pub")) if (root / "etc/apk/keys").exists() else []
    if not keys:
        raise RuntimeError("Alpine repository key package contains no signing keys")

    # APK recognizes both names from its normal Alpine installations; the
    # filesystem is case-insensitive FAT, and both copies keep shell lookup simple.
    for path in (root / "sbin/apk", root / "usr/bin/apk", root / "usr/bin/apk.static"):
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(binary, path)
        path.chmod(0o755)
    repositories = root / "etc/apk/repositories"
    repositories.parent.mkdir(parents=True, exist_ok=True)
    repositories.write_text(
        f"https://dl-cdn.alpinelinux.org/alpine/{branch}/main\n"
        f"https://dl-cdn.alpinelinux.org/alpine/{branch}/community\n",
        encoding="ascii")
    # apk opens its installed database even for `apk update`, and expects the
    # world file when planning a later `apk add`. A newline is a valid empty
    # database/world on FAT (which may not preserve zero-length files reliably).
    installed = root / "lib/apk/db/installed"
    installed.parent.mkdir(parents=True, exist_ok=True)
    installed.write_bytes(b"\n")
    world = root / "etc/apk/world"
    world.write_bytes(b"\n")
    (destination / ".ready").write_text(
        f"Alpine {branch} apk-tools-static {version} with {keys_package} {keys_version} x86_64\n",
        encoding="ascii")


if __name__ == "__main__":
    stage(Path(os.sys.argv[1]))
