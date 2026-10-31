#!/usr/bin/env python3
"""Stage Alpine's feh image viewer and its complete musl/ELF runtime closure."""
from base64 import b64decode
from hashlib import sha1
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import tarfile
import zlib
from urllib.error import HTTPError, URLError
from urllib.request import urlopen

LOCK = Path(__file__).with_name("alpine-feh.lock.json")
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
    raise RuntimeError("Unable to fetch Alpine package data:\n  " + "\n  ".join(failures))


def split_apk_members(data: bytes) -> list[bytes]:
    """Split the concatenated gzip members that make up an APK v2 archive."""
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
            if name not in packages or repository == "main":
                packages[name] = record
    return packages


def dependency_name(token: str) -> str:
    for operator in (">=", "<=", "=", "~", ">", "<"):
        if operator in token:
            token = token.split(operator, 1)[0]
            break
    return token


def resolve_closure(index: dict[str, dict], roots: dict[str, str]) -> list[tuple[str, dict]]:
    providers: dict[str, list[tuple[str, dict]]] = {}
    for name, record in index.items():
        keys = [name]
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


def copy_member(archive: tarfile.TarFile, member: tarfile.TarInfo,
                destination: Path) -> bool:
    if not (member.isfile() or member.issym() or member.islnk()):
        return False
    source = archive.extractfile(member)
    if source is None:
        raise RuntimeError(f"Cannot read Alpine package member {member.name}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(source.read())
    return True


def wanted_file(package: str, path: str) -> str | None:
    """Return the guest-root path for payload members needed by feh."""
    if package == "feh":
        if path == "usr/bin/feh":
            return path
        if path.startswith(("usr/share/feh/", "usr/share/icons/hicolor/")):
            return path
        if path == "usr/share/applications/feh.desktop":
            return path

    # Runtime ELF objects are flattened into /lib, matching the existing
    # Alpine curl/Xorg staging convention used by this guest's musl loader.
    if ((path.startswith("lib/") or path.startswith("usr/lib/"))
            and path.count("/") <= 2 and ".so" in Path(path).name):
        return "LIB/" + Path(path).name

    # Imlib2 discovers image codecs at runtime with dlopen(). These modules
    # are deeper than the top-level shared-library paths and must retain their
    # Alpine directory layout. Keep filters as well as loaders.
    if package == "imlib2" and path.startswith("usr/lib/imlib2/") and path.endswith(".so"):
        return path
    if package == "imlib2" and path.startswith("usr/share/imlib2/"):
        return path

    # libmagic expects the compiled database at its standard Alpine path.
    if package == "libmagic" and path == "usr/share/misc/magic.mgc":
        return path

    return None


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    branch, arch = lock["branch"], lock["architecture"]
    index = load_indexes(branch, arch)
    selected = resolve_closure(index, lock["packages"])

    if destination.exists():
        shutil.rmtree(destination)
    root = destination / "root"
    root.mkdir(parents=True)
    (destination / "packages").mkdir(parents=True)

    for name, record in selected:
        payload = download_apk(branch, arch, name, record)
        with tarfile.open(fileobj=BytesIO(payload), mode="r:gz") as archive:
            for member in archive:
                path = member.name.removeprefix("./")
                output_path = wanted_file(name, path)
                if output_path is None:
                    continue
                output = root / output_path
                copy_member(archive, member, output)

    binary = root / "usr/bin/feh"
    required = (
        root / "LIB/ld-musl-x86_64.so.1",
        root / "LIB/libImlib2.so.1",
        root / "LIB/libX11.so.6",
        root / "LIB/libXinerama.so.1",
        root / "LIB/libmagic.so.1",
        root / "LIB/libcurl.so.4",
        root / "LIB/libpng16.so.16",
    )
    if not binary.is_file():
        raise RuntimeError("Alpine feh package did not contain /usr/bin/feh")
    missing = [str(path.relative_to(root)) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError("Alpine feh runtime closure is incomplete: " + ", ".join(missing))
    loader_dir = root / "usr/lib/imlib2/loaders"
    missing_loaders = [name for name in ("png.so", "jpeg.so", "gif.so")
                       if not (loader_dir / name).is_file()]
    if missing_loaders:
        raise RuntimeError("Imlib2 image loaders missing: " + ", ".join(missing_loaders))
    if not (root / "usr/share/misc/magic.mgc").is_file():
        raise RuntimeError("libmagic database was not staged")

    binary.chmod(0o755)
    (destination / ".ready").write_text(
        "Alpine Linux v3.22 feh 3.10.3-r0 x86_64; closure="
        + ",".join(name for name, _ in selected) + "\n",
        encoding="ascii")


if __name__ == "__main__":
    stage(Path(os.sys.argv[1]))
