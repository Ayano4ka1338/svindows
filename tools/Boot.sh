#!/usr/bin/env bash
# Headless boot helper: boots the ISO in QEMU, captures the serial log, takes a
# framebuffer screendump, then shuts the machine down.
#
#   tools/Boot.sh [seconds] [PROGRAM]
#
# Artifacts:
#   build/serial.log   the kernel serial log
#   build/screen.ppm   the framebuffer contents at the end of the run
set -eu

SECS=${1:-25}
PROGRAM=${2:-cmd.exe}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 1

LOG=build/serial.log
SHOT=build/screen.ppm
BOOT_TEMP=$(mktemp -d /tmp/svindows-boot.XXXXXX)
MON="$BOOT_TEMP/monitor.sock"
# The QEMU monitor splits arguments on whitespace, so the screendump target has
# to be a path without spaces; the project root contains one.
RAWSHOT="$BOOT_TEMP/screen.ppm"
BOOT_PID=
cleanup() {
    if [ -n "$BOOT_PID" ]; then
        kill "$BOOT_PID" 2>/dev/null || true
        wait "$BOOT_PID" 2>/dev/null || true
    fi
    rm -f "$MON" "$RAWSHOT"
    rmdir "$BOOT_TEMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

make -s PROGRAM="$PROGRAM" iso build/initrd.fat >/dev/null || exit 1
rm -f "$LOG" "$SHOT"

qemu-system-x86_64 -m 1024M -cpu max -no-reboot -no-shutdown -boot d \
    -cdrom build/svindows.iso \
    -serial "file:$LOG" -display none \
    -monitor "unix:$MON,server,nowait" &
BOOT_PID=$!

for _ in $(seq 1 50); do [ -S "$MON" ] && break; sleep 0.1; done
sleep "$SECS"

if [ -S "$MON" ]; then
    { printf 'screendump %s\n' "$RAWSHOT"; sleep 2; } | \
        socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1
    [ -f "$RAWSHOT" ] && cp "$RAWSHOT" "$SHOT"
fi

kill "$BOOT_PID" 2>/dev/null || true
wait "$BOOT_PID" 2>/dev/null || true
BOOT_PID=

echo "serial: $(wc -l < "$LOG" 2>/dev/null || echo 0) lines -> $LOG"
[ -f "$SHOT" ] && echo "screen: $(head -c 32 "$SHOT" | tr '\n' ' ') -> $SHOT"
