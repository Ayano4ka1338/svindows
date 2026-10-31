#!/usr/bin/env python3
"""Stage the real Alpine x86_64 curl binary and its runtime dependency closure."""
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
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

LOCK = Path(__file__).with_name("alpine-curl.lock.json")
BASE_URLS = (
    "https://dl-cdn.alpinelinux.org/alpine",
    "https://mirrors.huaweicloud.com/repository/alpine",
)


def fetch(relative: str) -> bytes:
    preferred = os.environ.get("ALPINE_MIRROR", "").rstrip("/")
    failures = []
    for base in dict.fromkeys(([preferred] if preferred else []) + list(BASE_URLS)):
        url = f"{base}/{relative}"
        try:
            with urlopen(url, timeout=45) as response:
                return response.read()
        except (HTTPError, URLError, TimeoutError, OSError) as error:
            failures.append(f"{url}: {error}")
    raise RuntimeError("Unable to fetch Alpine data:\n  " + "\n  ".join(failures))


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


def parse_index(raw: bytes, repository: str, arch: str) -> dict[str, dict]:
    result = {}
    with tarfile.open(fileobj=BytesIO(raw), mode="r:gz") as archive:
        member = archive.extractfile("APKINDEX")
        if member is None:
            raise RuntimeError(f"APKINDEX missing from {repository}")
        records = member.read().decode("utf-8", "strict").strip().split("\n\n")
    for record in records:
        fields = {}
        for line in record.splitlines():
            if len(line) >= 2 and line[1] == ":":
                fields[line[0]] = line[2:]
        if fields.get("A") == arch and fields.get("P"):
            fields["repository"] = repository
            result[fields["P"]] = fields
    return result


def load_indexes(branch: str, arch: str) -> dict[str, dict]:
    packages = {}
    for repository in ("main", "community"):
        raw = fetch(f"{branch}/{repository}/{arch}/APKINDEX.tar.gz")
        for name, record in parse_index(raw, repository, arch).items():
            # main wins if a package name is present in both repositories.
            if name not in packages or repository == "main":
                packages[name] = record
    return packages


def dependency_name(token: str) -> str:
    # APKINDEX dependency constraints use forms such as pkg>=1.2 or
    # so:libfoo.so.1=1.2. The installed package set is from one pinned branch.
    for operator in (">=", "<=", "=", "~", ">", "<"):
        if operator in token:
            token = token.split(operator, 1)[0]
            break
    return token


def resolve_closure(index: dict[str, dict], roots: dict[str, str]) -> list[tuple[str, dict]]:
    providers: dict[str, list[tuple[str, dict]]] = {}
    for name, record in index.items():
        keys = [name]
        # APKINDEX may attach a version to virtual SONAME providers, e.g.
        # so:libzstd.so.1=1.5.7. Normalize provider tokens exactly like
        # dependency tokens so the closure resolver can match them.
        keys.extend(dependency_name(capability)
                    for capability in record.get("p", "").split())
        for key in keys:
            providers.setdefault(key, []).append((name, record))
    for key in providers:
        providers[key].sort(key=lambda item: (item[1].get("repository") != "main", item[0]))

    selected: dict[str, dict] = {}
    pending = list(roots.items())
    while pending:
        requested, pinned_version = pending.pop()
        key = dependency_name(requested)
        candidates = providers.get(key, [])
        if not candidates:
            # Some APK dependency names are virtual command capabilities;
            # curl's runtime closure should not need these, so fail explicitly.
            raise RuntimeError(f"No Alpine package provides dependency {requested}")
        name, record = candidates[0]
        if pinned_version and record.get("V") != pinned_version:
            exact = [(n, r) for n, r in candidates if r.get("V") == pinned_version]
            if not exact:
                raise RuntimeError(f"Pinned Alpine package {requested}={pinned_version} not found")
            name, record = exact[0]
        if name in selected:
            continue
        selected[name] = record
        for dep in record.get("D", "").split():
            dep = dependency_name(dep)
            if dep.startswith(("cmd:", "pc:", "!")):
                continue
            pending.append((dep, ""))
    return sorted(selected.items())


def download_apk(branch: str, arch: str, name: str, record: dict) -> bytes:
    filename = f"{name}-{record['V']}.apk"
    relative = f"{branch}/{record['repository']}/{arch}/{filename}"
    apk = fetch(relative)
    members = split_apk_members(apk)
    checksum = record.get("C", "")
    if not checksum.startswith("Q1"):
        raise RuntimeError(f"APKINDEX has no SHA-1 control checksum for {filename}")
    expected = b64decode(checksum[2:], validate=True)
    if sha1(members[-2]).digest() != expected:
        raise ValueError(f"Alpine APK control checksum mismatch: {filename}")
    return members[-1]


def copy_member(tar: tarfile.TarFile, member: tarfile.TarInfo,
                destination: Path) -> None:
    if not (member.isfile() or member.issym() or member.islnk()):
        return
    source = tar.extractfile(member)
    if source is None:
        raise RuntimeError(f"Cannot read Alpine package member {member.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read())


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    branch, arch = lock["branch"], lock["architecture"]
    index = load_indexes(branch, arch)
    selected = resolve_closure(index, lock["packages"])

    if destination.exists():
        shutil.rmtree(destination)
    root = destination / "root"
    root.mkdir(parents=True)
    cache = destination / "packages"
    cache.mkdir(parents=True)

    staged = set()
    for name, record in selected:
        apk_payload = download_apk(branch, arch, name, record)
        with tarfile.open(fileobj=BytesIO(apk_payload), mode="r:gz") as archive:
            for member in archive:
                path = member.name.removeprefix("./")
                output = None
                if name == "curl" and path == "usr/bin/curl":
                    output = root / "usr/bin/curl"
                elif ((path.startswith("lib/") or path.startswith("usr/lib/"))
                      and path.count("/") <= 2 and ".so" in Path(path).name):
                    output = root / "LIB" / Path(path).name
                elif path in ("etc/ssl/cert.pem", "etc/ssl/certs/ca-certificates.crt"):
                    output = root / path
                if output is not None:
                    copy_member(archive, member, output)
                    staged.add(path)

    binary = root / "usr/bin/curl"
    if not binary.is_file():
        raise RuntimeError("Alpine curl package did not contain /usr/bin/curl")
    binary.chmod(0o755)
    if not (root / "LIB/ld-musl-x86_64.so.1").is_file():
        raise RuntimeError("Alpine musl interpreter was not staged")
    if not (root / "LIB/libcurl.so.4").is_file():
        raise RuntimeError("Alpine libcurl.so.4 was not staged")
    if not (root / "etc/ssl/cert.pem").is_file() and not (
            root / "etc/ssl/certs/ca-certificates.crt").is_file():
        raise RuntimeError("Alpine CA certificate bundle was not staged")
    (destination / ".ready").write_text(
        "Alpine v3.22 curl 8.14.1-r4 x86_64\n", encoding="ascii")


if __name__ == "__main__":
    stage(Path(os.sys.argv[1]))
