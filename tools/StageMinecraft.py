#!/usr/bin/env python3
"""Stage the supplied Minecraft client and an Alpine-compatible Java 8 JRE."""
from hashlib import sha256
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import zipfile
from urllib.request import Request, urlopen

LOCK = Path(__file__).with_name("minecraft.lock.json")
XTST = Path(__file__).parent / "third_party/libXtst.so.6.1.0"
XTST_SHA256 = "8b1078fa347e0385da22fccc9fa0127525a1fa8ffe3a5d05f6e9e635c2e31593"
CLIENT_JAR = Path(__file__).parents[1] / "user/d/minecraft/client.jar"


def optimize_client_window(client: Path) -> None:
    """Reduce Alpha's hard-coded desktop canvas to ease software rendering."""
    with zipfile.ZipFile(client, "r") as source:
        entries = [(item, source.read(item.filename)) for item in source.infolist()]
    target = "net/minecraft/client/Minecraft.class"
    found = False
    with zipfile.ZipFile(client, "w") as output:
        for item, data in entries:
            if item.filename == target:
                old = b"\x11\x03\x56\x11\x01\xe0"  # sipush 854, sipush 480
                new = b"\x11\x02\x80\x11\x01\x68"  # sipush 640, sipush 360
                if data.count(old) != 2:
                    raise RuntimeError("unexpected Alpha launcher canvas bytecode")
                data = data.replace(old, new)
                found = True
            output.writestr(item, data)
    if not found:
        raise RuntimeError("Alpha desktop launcher class is missing")


def fetch(item: dict, destination: Path) -> None:
    expected = item["sha256"]
    if destination.is_file() and sha256(destination.read_bytes()).hexdigest() == expected:
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".part")
    request = Request(item["url"], headers={"User-Agent": "Svindows-Minecraft-Stager/1.0"})
    with urlopen(request, timeout=60) as response, temporary.open("wb") as output:
        shutil.copyfileobj(response, output)
    actual = sha256(temporary.read_bytes()).hexdigest()
    if actual != expected:
        temporary.unlink(missing_ok=True)
        raise ValueError(f"SHA-256 mismatch for {destination.name}: {actual}")
    temporary.replace(destination)


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text(encoding="utf-8"))
    cache = destination.parent / "minecraft-downloads"
    java_archive = cache / "zulu8-jre-musl.tar.gz"
    fetch(lock["java"], java_archive)

    if destination.exists():
        shutil.rmtree(destination)
    root = destination / "root"
    jvm = root / "usr/lib/jvm/zulu8"
    launcher_dir = root / "usr/share/minecraft"
    jvm.mkdir(parents=True)
    launcher_dir.mkdir(parents=True)
    if not CLIENT_JAR.is_file():
        raise RuntimeError(f"Minecraft client jar is missing: {CLIENT_JAR}")
    if sha256(XTST.read_bytes()).hexdigest() != XTST_SHA256:
        raise ValueError("libXtst binary checksum does not match the pinned Alpine build")
    xtst = root / "lib/libXtst.so.6"
    xtst.parent.mkdir(parents=True, exist_ok=True)
    xtst.write_bytes(XTST.read_bytes())

    expected_top = lock["java"]["directory"]
    with tarfile.open(java_archive, "r:gz") as archive:
        entries = archive.getmembers()
        members = {member.name: member for member in entries}
        for member in entries:
            relative = PurePosixPath(member.name)
            if not relative.parts or relative.parts[0] != expected_top:
                raise ValueError(f"unexpected path in Java archive: {member.name}")
            tail = relative.parts[1:]
            if not tail or tail[0] == "man":
                continue
            target = jvm.joinpath(*tail)
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                source = archive.extractfile(member)
                if source is None:
                    raise RuntimeError(f"cannot read Java runtime file {member.name}")
                target.parent.mkdir(parents=True, exist_ok=True)
                with target.open("wb") as output:
                    shutil.copyfileobj(source, output)
                target.chmod(0o755 if member.mode & 0o111 else 0o644)
        # FAT has no symlinks. Materialize the two Java launcher links as copies.
        for member in entries:
            if not member.issym():
                continue
            relative = PurePosixPath(member.name)
            tail = relative.parts[1:]
            if not tail or tail[0] == "man":
                continue
            source_name = str(PurePosixPath(member.name).parent.joinpath(member.linkname))
            source_name = str(PurePosixPath(os.path.normpath(source_name)))
            source_member = members.get(source_name)
            if source_member is None or not source_member.isfile():
                raise ValueError(f"unsupported Java runtime symlink: {member.name}")
            source = archive.extractfile(source_member)
            if source is None:
                raise RuntimeError(f"cannot read Java runtime link target {source_name}")
            target = jvm.joinpath(*tail)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("wb") as output:
                shutil.copyfileobj(source, output)
            target.chmod(0o755)
    java = jvm / "bin/java"
    if not java.is_file() or java.read_bytes()[:4] != b"\x7fELF":
        raise RuntimeError("staged Java launcher is not an ELF executable")

    # Stage the supplied client jar as the exact desktop entrypoint target.
    runtime = destination.parent / "minecraft-runtime"
    runtime_root = runtime / "root"
    game_source = runtime_root / "home/user/.minecraft"
    if not (runtime / ".ready").is_file() or not (game_source / "a1.2.6.jar").is_file():
        raise RuntimeError("offline Alpha 1.2.6 runtime is missing from build/minecraft-runtime")
    game_root = root / "home/user/.minecraft"
    shutil.copytree(game_source, game_root, dirs_exist_ok=True)
    staged_client = game_root / "a1.2.6.jar"
    shutil.copy2(CLIENT_JAR, staged_client)
    optimize_client_window(staged_client)
    # OpenAL's native initialization currently crashes this Svindows Linux ABI.
    # Alpha 1.2.6 skips constructing SoundSystem when both volumes are zero.
    (game_root / "options.txt").write_text(
        "music:0.0\nsound:0.0\nviewDistance:3\nfancyGraphics:false\n"
        "limitFramerate:true\nbobView:false\n", encoding="ascii")
    natives = game_root / "natives"
    for path in natives.glob("libopenal*.so"):
        path.unlink()
    for source_name, loader_name in (
            ("liblwjgl64.so", "liblwjgl.so"),
            ("libjinput-linux64.so", "libjinput-linux.so")):
        source = natives / source_name
        if source.is_file():
            shutil.copy2(source, natives / loader_name)

    # LWJGL 2 ships glibc-linked native libraries. The runtime cache contains
    # Alpine's gcompat shims plus Mesa's software GL stack for this Xorg server.
    compat_libs = runtime_root / "lib"
    for filename in ("ld-linux-x86-64.so.2", "libgcompat.so.0", "libc.so.6",
                     "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
                     "libutil.so.1", "libresolv.so.2", "libcrypt.so.1"):
        source = compat_libs / filename
        if source.is_file():
            target = root / "lib" / filename
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)

    loader = compat_libs / "ld-musl-x86_64.so.1"
    loader.chmod(0o755)
    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(runtime_root / "usr/lib") + ":" + str(compat_libs)
    mesa_sources = [runtime_root / "usr/lib/libGL.so.1",
                    runtime_root / "usr/lib/dri/swrast_dri.so"]
    mesa_libs = set()
    for source in mesa_sources:
        listing = subprocess.run([str(loader), "--list", str(source)],
                                 check=True, capture_output=True, text=True, env=env).stdout
        for line in listing.splitlines():
            if " => " not in line:
                continue
            resolved = line.split(" => ", 1)[1].split(" (", 1)[0]
            path = Path(resolved)
            if path.is_file() and runtime_root / "usr/lib" in path.parents:
                mesa_libs.add(path)
        mesa_libs.add(source)
    mesa_target = root / "usr/lib"
    mesa_target.mkdir(parents=True, exist_ok=True)
    for source in sorted(mesa_libs):
        shutil.copy2(source, mesa_target / source.name)
    dri_target = mesa_target / "dri"
    dri_target.mkdir(parents=True, exist_ok=True)
    shutil.copy2(runtime_root / "usr/lib/dri/swrast_dri.so",
                 dri_target / "swrast_dri.so")

    openal = runtime_root / "usr/lib/libopenal.so.1"
    if openal.is_file():
        shutil.copy2(openal, mesa_target / "libopenal.so.1")
        shutil.copy2(openal, natives / "libopenal.so")
    for filename in ("libucontext.so.1", "libobstack.so.1"):
        source = runtime_root / "usr/lib" / filename
        if source.is_file():
            shutil.copy2(source, mesa_target / filename)
    xrandr = runtime_root / "usr/lib/libXrandr.so.2"
    if xrandr.is_file():
        shutil.copy2(xrandr, root / "lib/libXrandr.so.2")
    xxf86vm = runtime_root / "usr/lib/libXxf86vm.so.1"
    if xxf86vm.is_file():
        shutil.copy2(xxf86vm, root / "lib/libXxf86vm.so.1")
    (destination / ".ready").write_text(
        "Minecraft Alpha 1.2.6 client.jar; Azul Zulu Java 8u514; desktop main class\n",
        encoding="ascii")


if __name__ == "__main__":
    stage(Path(os.sys.argv[1]))
