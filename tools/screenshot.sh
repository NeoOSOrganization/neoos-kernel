#!/bin/bash
# tools/screenshot.sh OUT.ppm [seconds]
#
# Boots the current image headless with the QEMU monitor on a socket,
# waits, and screendumps the framebuffer. The only way to check that
# something actually PAINTED -- the serial log can say a frame was
# composited but not what it looked like.
set -u
cd "$(dirname "$0")/.."
OUT="$(realpath -m "${1:-build/screen.ppm}")"
WAIT="${2:-24}"
SOCK="$(mktemp -u /tmp/neoos-qmon-XXXX.sock)"
rm -f "$OUT"

qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -vga std \
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
  -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
  -device usb-ehci -device usb-mouse \
  -no-reboot -display none -serial file:build/screenshot.log \
  -monitor "unix:$SOCK,server,nowait" &
QPID=$!

sleep "$WAIT"
printf 'screendump %s\nquit\n' "$OUT" | nc -q 2 -U "$SOCK" >/dev/null 2>&1
sleep 2
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
