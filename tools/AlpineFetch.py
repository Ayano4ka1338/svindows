#!/usr/bin/env python3
"""Shared parallel, cached downloader for all Svindows Alpine package stages."""
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path, PurePosixPath
import os
import shutil
import sys
import threading
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen

BASE_URLS = (
    "https://dl-cdn.alpinelinux.org/alpine",
    "https://mirrors.huaweicloud.com/repository/alpine",
)
CACHE = Path(os.environ.get(
    "SVINDOWS_APK_CACHE", Path.home() / ".cache/svindows/alpine-apk"))
WORKERS = max(1, min(16, int(os.environ.get("ALPINE_DOWNLOAD_JOBS", "12"))))
TIMEOUT = max(5, int(os.environ.get("ALPINE_DOWNLOAD_TIMEOUT", "20")))
INDEX_TTL = max(60, int(os.environ.get("ALPINE_INDEX_TTL", str(6 * 60 * 60))))


def _cache_path(relative: str) -> Path:
    clean = PurePosixPath(relative)
    if clean.is_absolute() or ".." in clean.parts or not clean.parts:
        raise ValueError(f"unsafe Alpine relative path: {relative!r}")
    return CACHE.joinpath(*clean.parts)


def fetch_path(relative: str, *, immutable: bool = True,
               refresh: bool = False) -> Path:
    """Download one repository object atomically and return its persistent cache path.

    APKs are immutable and cached forever. APKINDEX files are refreshed after
    INDEX_TTL seconds so new builds see repository updates without downloading
    the indexes on every invocation.
    """
    cached = _cache_path(relative)
    if cached.is_file() and not refresh:
        if immutable or time.time() - cached.stat().st_mtime < INDEX_TTL:
            return cached

    preferred = os.environ.get("ALPINE_MIRROR", "").rstrip("/")
    extra = [item.strip().rstrip("/") for item in
             os.environ.get("ALPINE_MIRRORS", "").split(",") if item.strip()]
    bases = list(dict.fromkeys(([preferred] if preferred else []) + extra + list(BASE_URLS)))
    failures = []
    cached.parent.mkdir(parents=True, exist_ok=True)
    temporary = cached.with_name(
        cached.name + f".part-{os.getpid()}-{threading.get_ident()}")
    for base in bases:
        url = f"{base}/{relative}"
        try:
            request = Request(url, headers={"User-Agent": "Svindows-Alpine-Stager/1.0"})
            with urlopen(request, timeout=TIMEOUT) as response, temporary.open("wb") as out:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    out.write(chunk)
            if temporary.stat().st_size == 0:
                raise OSError("mirror returned an empty file")
            temporary.replace(cached)
            return cached
        except (HTTPError, URLError, TimeoutError, OSError) as error:
            failures.append(f"{url}: {error}")
            temporary.unlink(missing_ok=True)

    details = "\n  ".join(failures) if failures else "no mirrors configured"
    raise RuntimeError(f"Unable to fetch Alpine data {relative}. Tried:\n  {details}")



def seed_cache(relative: str, source: Path) -> Path:
    """Promote a previously verified build-local APK into the shared cache."""
    cached = _cache_path(relative)
    if cached.is_file() or not source.is_file():
        return cached
    cached.parent.mkdir(parents=True, exist_ok=True)
    temporary = cached.with_name(
        cached.name + f".seed-{os.getpid()}-{threading.get_ident()}")
    shutil.copyfile(source, temporary)
    temporary.replace(cached)
    return cached

def fetch(relative: str, *, immutable: bool = True) -> bytes:
    """Compatibility helper for callers that need in-memory bytes."""
    return fetch_path(relative, immutable=immutable).read_bytes()


def fetch_many(items, *, immutable: bool = True, label: str = "APK") -> dict:
    """Fetch a key->relative-path mapping concurrently, with progress output."""
    entries = list(items.items()) if hasattr(items, "items") else list(items)
    if not entries:
        return {}
    results = {}
    failures = []
    print(f"[alpine] {label}: {len(entries)} files, {WORKERS} workers; cache={CACHE}",
          file=sys.stderr, flush=True)
    with ThreadPoolExecutor(max_workers=min(WORKERS, len(entries))) as pool:
        jobs = {pool.submit(fetch_path, relative, immutable=immutable): key
                for key, relative in entries}
        completed = 0
        for future in as_completed(jobs):
            key = jobs[future]
            completed += 1
            try:
                results[key] = future.result()
                print(f"[alpine] {label} {completed}/{len(entries)}: {key}",
                      file=sys.stderr, flush=True)
            except Exception as error:
                failures.append(f"{key}: {error}")
    if failures:
        raise RuntimeError("Alpine downloads failed:\n  " + "\n  ".join(failures))
    return results
