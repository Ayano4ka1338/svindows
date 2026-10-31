#!/usr/bin/env python3
"""Static checks for the Alpine feh/Imlib2 path and loader integration."""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
process = (ROOT / "kernel/ps/ProcessObject.c").read_text(encoding="utf-8")
linux_file = (ROOT / "kernel/linux/File.c").read_text(encoding="utf-8")
linux_path = (ROOT / "kernel/linux/Path.c").read_text(encoding="utf-8")
stager = (ROOT / "tools/StageAlpineFeh.py").read_text(encoding="utf-8")
makefile = (ROOT / "Makefile").read_text(encoding="utf-8")

required = (
    '"IMLIB2_LOADER_PATH=/usr/lib/imlib2/loaders"',
    '"IMLIB2_FILTER_PATH=/usr/lib/imlib2/filters"',
    '"FEH_SKIP_MAGIC=1"',
)
for item in required:
    assert item in process, f"missing ELF environment default: {item}"

assert "LinuxCapturePathAt(process, directory_fd, path, dos_path)" in linux_file
assert "IoCanonicalizeDosPathKernel(result, canonical)" in linux_path
assert "return LinuxError(LINUX_ENAMETOOLONG);" in linux_path
assert "return LinuxError(LINUX_ENOENT);" in linux_path
assert r"path[in] == '/' ? '\\' : path[in]" in linux_path
assert "[linux-fs] Imlib2 loader lookup" in linux_file
assert "[linux-fs] Imlib2 loader open" in linux_file
assert '"png.so", "jpeg.so", "gif.so"' in stager
assert 'path.startswith("usr/lib/imlib2/")' in stager
assert "feh-path-check" in makefile
print("feh-path-check: PASS")
