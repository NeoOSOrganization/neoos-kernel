#!/bin/bash
# tools/inject_click.sh SOCK dx dy [EXPECT-REGEX LOG]
#
# Moves the PS/2 mouse by (dx,dy) relative to its current position,
# then clicks the left button. SOCK is the QEMU monitor unix socket.
#
# With EXPECT-REGEX and LOG, the click is CONFIRMED rather than hoped
# for: after each click this waits for EXPECT-REGEX to appear in LOG
# (the guest's serial log), and exits 0 as soon as it does, or 1 after
# three unconfirmed clicks. Callers must only use this for clicks that
# are idempotent (a second click on an already-open Start button, or on
# bare desktop, changes nothing).
#
# Two facts this relies on:
#
# 1. QEMU's emulated PS/2 aux queue holds only a few packets, and QEMU
#    splits one large `mouse_move` into several. Each step here is at
#    most 100 counts (one packet) and steps are sent one monitor
#    command at a time, so the queue never has to hold a backlog. This
#    pacing is about QEMU's host-side queue, not about guest progress --
#    guest progress is what EXPECT-REGEX checks.
# 2. wm.c clamps the cursor to the screen, so overshooting past an edge
#    (dx/dy far larger than the screen) lands exactly on a corner no
#    matter where the cursor started.
#
# The caller must not start this until the compositor has opened the
# mouse (wait for "[wm] surface mapped" in a log that was deleted
# before boot -- QEMU truncates -serial file: only once it opens it, so
# a stale log from the previous run satisfies any grep instantly; that
# was the real cause of the old "Start click missed" failures, whose
# moves all arrived before the compositor was reading the mouse).
set -u
SOCK="$1"; DX="$2"; DY="$3"; EXPECT="${4:-}"; LOG="${5:-}"
STEP=100
mon() { printf '%s\n' "$1" | nc -q 0 -U "$SOCK" >/dev/null 2>&1; }

rx=$DX; ry=$DY
while [ "${rx#-}" -gt 0 ] || [ "${ry#-}" -gt 0 ]; do
  sx=$STEP; [ "${rx#-}" -lt $STEP ] && sx=${rx#-}
  sy=$STEP; [ "${ry#-}" -lt $STEP ] && sy=${ry#-}
  [ "$rx" -lt 0 ] && sx=$((-sx))
  [ "$ry" -lt 0 ] && sy=$((-sy))
  mon "mouse_move $sx $sy"
  rx=$((rx - sx)); ry=$((ry - sy))
  sleep 0.2
done

for attempt in 1 2 3; do
  mon "mouse_button 1"; sleep 0.2; mon "mouse_button 0"
  [ -z "$EXPECT" ] && exit 0
  for i in $(seq 1 40); do
    grep -qE "$EXPECT" "$LOG" 2>/dev/null && exit 0
    sleep 0.25
  done
  echo "inject_click: '$EXPECT' not seen after click $attempt -- clicking again"
done
exit 1
