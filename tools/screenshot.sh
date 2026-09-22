#!/bin/bash
# tools/screenshot.sh OUT.ppm [max-seconds] [ready-regex]
#
# Boots the current image headless with the QEMU monitor on a socket,
# waits until the guest is READY, and screendumps the framebuffer. The
# only way to check that something actually PAINTED -- the serial log
# can say a frame was composited but not what it looked like.
#
# "Ready" is two conditions, never a fixed sleep (a fixed sleep either
# wastes time or, on a slow boot, shoots an empty desktop):
#   1. ready-regex (if given) has appeared in the serial log, and
#   2. the screen has settled: two consecutive dumps a second apart are
#      byte-identical, so the frame after that event has landed.
# max-seconds is only an upper bound; hitting it still dumps whatever
# is on screen, and says so.
set -u
cd "$(dirname "$0")/.."
OUT="$(realpath -m "${1:-build/screen.ppm}")"
MAX="${2:-60}"
READY="${3:-}"
LOG=build/screenshot.log
SOCK="$(mktemp -u /tmp/neoos-qmon-XXXX.sock)"
rm -f "$OUT"
# QEMU only truncates the log once it opens it, which is after this
# script's first poll could run -- a stale log from the previous boot
# would satisfy READY instantly. Remove it first.
rm -f "$LOG"

qemu-system-x86_64 -cpu Nehalem -smp 4 -m "${DESKTOP_MEM:-512}" -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -vga std \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
  -no-reboot -display none -serial file:$LOG \
  -monitor "unix:$SOCK,server,nowait" &
QPID=$!

deadline=$((SECONDS + MAX))
if [ -n "$READY" ]; then
  until grep -qE "$READY" "$LOG" 2>/dev/null; do
    if [ $SECONDS -ge $deadline ]; then echo "screenshot.sh: '$READY' never appeared in ${MAX}s"; break; fi
    sleep 0.5
  done
fi
left=$((deadline - SECONDS)); [ $left -lt 5 ] && left=5
tools/screenshot_settled.sh "$SOCK" "$OUT" "$left"

printf 'quit\n' | nc -q 2 -U "$SOCK" >/dev/null 2>&1
sleep 1
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
rm -f "$SOCK"

if [ ! -s "$OUT" ]; then echo "screenshot.sh: no image produced"; exit 1; fi
PNG="${OUT%.ppm}.png"
/home/neo/anaconda3/bin/python3 - "$OUT" "$PNG" <<'PY'
import sys
from PIL import Image
Image.open(sys.argv[1]).save(sys.argv[2])
print("wrote", sys.argv[2])
PY
