#!/usr/bin/env python3
"""Stage pinned Alpine Xorg clients, fonts, and their musl libraries."""
from hashlib import sha256
from io import BytesIO
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
from AlpineFetch import fetch_many, seed_cache


LOCK = Path(__file__).with_name("alpine-xorg.lock.json")


def stage(destination: Path) -> None:
    lock = json.loads(LOCK.read_text())
    destination.mkdir(parents=True, exist_ok=True)
    root = destination / "root"
    if root.exists():
        shutil.rmtree(root)
    root.mkdir()

    apk_relatives = {
        name: f"{lock['branch']}/{package['repository']}/"
              f"{lock['architecture']}/{name}-{package['version']}.apk"
        for name, package in lock["packages"].items()}
    # Older builds kept verified Xorg APKs under build/alpine-xorg/packages.
    # Promote those files so upgrading to the shared cache does not re-download them.
    old_cache = destination / "packages"
    for name, package in lock["packages"].items():
        legacy_apk = old_cache / f"{name}-{package['version']}.apk"
        if legacy_apk.is_file():
            if sha256(legacy_apk.read_bytes()).hexdigest() == package["sha256"]:
                seed_cache(apk_relatives[name], legacy_apk)

    apk_paths = fetch_many(apk_relatives, immutable=True, label="Xorg APK")

    for name, package in sorted(lock["packages"].items()):
        filename = f"{name}-{package['version']}.apk"
        archive = apk_paths[name].read_bytes()
        if sha256(archive).hexdigest() != package["sha256"]:
            raise ValueError(f"Alpine package checksum mismatch: {filename}")

        with tarfile.open(fileobj=BytesIO(archive), mode="r:gz") as tar:
            for member in tar:
                path = member.name
                if name == "xorg-server" and path == "usr/libexec/Xorg":
                    output = root / "usr/bin/xorg"
                elif name == "dash" and path == "usr/bin/dash":
                    output = root / "bin/sh"
                elif name == "xkbcomp" and path == "usr/bin/xkbcomp":
                    output = root / path
                elif name in ("xterm", "xsetroot", "xeyes") and path == f"usr/bin/{name}":
                    output = root / path
                elif name == "xterm" and path.startswith(
                        "usr/lib/X11/app-defaults/XTerm"):
                    output = root / path
                elif name == "mkfontscale" and path == "usr/bin/mkfontscale":
                    output = destination / "tools/mkfontscale"
                elif name == "twm" and path in (
                        "usr/bin/twm", "usr/share/X11/twm/system.twmrc"):
                    output = root / path
                elif name == "xkeyboard-config" and path.startswith(
                        "usr/share/X11/xkb/"):
                    output = root / path
                elif name == "fontconfig" and path.startswith("etc/fonts/"):
                    output = root / path
                elif name == "ncurses-terminfo-base" and path.startswith(
                        "etc/terminfo/"):
                    output = root / path
                elif name in ("font-misc-misc", "font-alias", "encodings") and (
                        path.startswith("usr/share/fonts/misc/") or
                        path.startswith("usr/share/fonts/encodings/")):
                    output = root / path
                elif ((path.startswith("lib/") or path.startswith("usr/lib/"))
                      and path.count("/") <= 2 and ".so" in Path(path).name):
                    output = root / "LIB" / Path(path).name
                elif path.startswith(
                        "usr/lib/xorg/modules/") and path.endswith(".so"):
                    output = root / path
                else:
                    continue
                if not (member.isfile() or member.issym() or member.islnk()):
                    continue
                source = tar.extractfile(member)
                if source is None:
                    raise ValueError(f"Alpine package member unavailable: {path}")
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(source.read())

    # A minimal configuration. Svindows has no udev daemon, so Xorg's automatic
    # device discovery fails ("failed to initialise udev") and it finds neither
    # video nor input devices on its own. These snippets pin the fbdev video
    # driver and wire the emulated evdev devices (/dev/input/event0 keyboard,
    # event1 mouse) to the core keyboard and pointer by their literal paths,
    # which is the legacy xf86-input-evdev path that does not require udev.
    #
    # Xorg reads both the system and administrator configuration directories.
    # Install each section once, in the administrator directory.
    input_conf = (
        "Section \"InputDevice\"\n"
        "    Identifier \"svindows-keyboard\"\n"
        "    Driver \"evdev\"\n"
        "    Option \"Device\" \"/dev/input/event0\"\n"
        "    Option \"CoreKeyboard\" \"on\"\n"
        "EndSection\n\n"
        "Section \"InputDevice\"\n"
        "    Identifier \"svindows-mouse\"\n"
        "    Driver \"evdev\"\n"
        "    Option \"Device\" \"/dev/input/event1\"\n"
        "    Option \"CorePointer\" \"on\"\n"
        "EndSection\n\n"
        "Section \"ServerLayout\"\n"
        "    Identifier \"svindows-layout\"\n"
        "    InputDevice \"svindows-keyboard\" \"CoreKeyboard\"\n"
        "    InputDevice \"svindows-mouse\" \"CorePointer\"\n"
        "EndSection\n")
    video_conf = (
        "Section \"Files\"\n"
        "    FontPath \"/usr/share/fonts/misc\"\n"
        "EndSection\n\n"
        "Section \"Device\"\n"
        "    Identifier \"svindows-fb\"\n"
        "    Driver \"fbdev\"\n"
        "    Option \"fbdev\" \"/dev/fb0\"\n"
        "    Option \"ShadowFB\" \"off\"\n"
        "EndSection\n\n"
        "Section \"Screen\"\n"
        "    Identifier \"svindows-screen\"\n"
        "    Device \"svindows-fb\"\n"
        "    DefaultDepth 24\n"
        "EndSection\n")
    conf_dir = root / "etc/X11/xorg.conf.d"
    conf_dir.mkdir(parents=True, exist_ok=True)
    (conf_dir / "10-svindows-input.conf").write_text(input_conf)
    (conf_dir / "20-svindows-video.conf").write_text(video_conf)

    # The packaged TWM configuration asks the user to drag an outline before
    # an unpositioned window appears. Place xterm automatically at startup.
    twm_config = root / "usr/share/X11/twm/system.twmrc"
    twm_text = twm_config.read_text()
    if "NoGrabServer\n" not in twm_text:
        raise ValueError("unexpected TWM configuration")
    twm_config.write_text(twm_text.replace("NoGrabServer\n",
                                            "NoGrabServer\nRandomPlacement\n", 1))

    # Alpine's font package generates fonts.dir in a post-install hook. Our
    # unpack-only image needs the same index for xterm's core bitmap font.
    font_dir = root / "usr/share/fonts/misc"
    environment = os.environ.copy()
    environment["LD_LIBRARY_PATH"] = str(root / "LIB")
    loader = root / "LIB/ld-musl-x86_64.so.1"
    font_indexer = destination / "tools/mkfontscale"
    loader.chmod(0o755)
    font_indexer.chmod(0o755)
    subprocess.run([str(loader), str(font_indexer), "-b", "-s", "-l",
                    str(font_dir)], check=True, env=environment)
    if not (font_dir / "fonts.dir").is_file():
        raise ValueError("bitmap font index was not generated")

    if not (root / "usr/bin/xorg").is_file() or not (root / "LIB" /
            "ld-musl-x86_64.so.1").is_file():
        raise ValueError("Xorg or the musl interpreter is missing")
    (destination / ".ready").write_text("Alpine v3.22 x86_64 Xorg ELF\n")


if __name__ == "__main__":
    stage(Path(sys.argv[1]))
