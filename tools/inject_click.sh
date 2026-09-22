#!/bin/bash
# tools/inject_click.sh SOCK dx dy
#
# Moves the PS/2 mouse by (dx,dy) relative to its current position,
# then clicks and releases the left button. SOCK is the QEMU monitor
# unix socket, e.g. from tools/screenshot.sh's own $SOCK.
#
# Two things learned empirically driving this against a TCG (-cpu
# Nehalem, no KVM) guest, neither obvious from QEMU's docs:
#
# 1. QEMU's emulated PS/2 aux port has a small fixed-size queue, and a
#    burst of `mouse_move`/`mouse_button` HMP commands sent faster than
#    the guest's own IRQ12 servicing can drain it are silently DROPPED,
#    not queued or blocked. A monitor script that fires every command
#    back-to-back (no delay) loses most of a multi-step move under a
#    real guest -- confirmed by instrumenting wm.c's pump_mouse():
#    bursts consistently arrived truncated, and independently of how
#    large or small each individual `mouse_move` step was. The fix is
#    real per-step pacing (STEP_DELAY below), not a smaller step size.
# 2. wm.c clamps cur_x/cur_y to the screen bounds
#    (0 <= cur_x < fb_w, 0 <= cur_y < fb_h). Deliberately overshooting
#    PAST an edge (dx/dy far larger than the screen) is more reliable
#    than aiming for an exact interior coordinate: it lands on a
#    screen CORNER regardless of exactly how many of the paced steps
#    the guest actually drained, which the exact-delta approach is not
#    robust to. Callers should pick dx/dy accordingly (see
#    wm-taskbar's use of this script in the Makefile).
set -u
SOCK="$1"; DX="$2"; DY="$3"
STEP=100
STEP_DELAY=1       # seconds between steps -- see note 1 above
{
  rx=$DX; ry=$DY
  while [ "${rx#-}" -gt 0 ] || [ "${ry#-}" -gt 0 ]; do
    sx=$STEP; [ "${rx#-}" -lt $STEP ] && sx=${rx#-}
    sy=$STEP; [ "${ry#-}" -lt $STEP ] && sy=${ry#-}
    [ "$rx" -lt 0 ] && sx=$((-sx))
    [ "$ry" -lt 0 ] && sy=$((-sy))
    printf 'mouse_move %d %d\n' "$sx" "$sy" | nc -q 0 -U "$SOCK" >/dev/null 2>&1
    rx=$((rx - sx)); ry=$((ry - sy))
    sleep "$STEP_DELAY"
  done
  # Settle: let the guest fully drain the last few queued motion
  # packets before the click, or the button press can land while
  # cur_x/cur_y are still mid-flight from an earlier step (confirmed:
  # without this, hit-testing saw a stale, pre-arrival position).
  sleep 5
  # The click itself is sent twice, a real distance apart, for the
  # same reason the move is paced: the same small-queue packet loss
  # (note 1 above) can just as easily eat a mouse_button command as a
  # mouse_move one. Both attempts land on the exact same clamped
  # corner (note 2), so repeating is free of side effects if the first
  # one *did* land -- clicking an already-open menu's Start button a
  # second time is a no-op (menu_open guards it in taskbar.c), and a
  # second click on bare desktop is still just "focus moved away".
  for attempt in 1 2; do
    printf 'mouse_button 1\n' | nc -q 0 -U "$SOCK" >/dev/null 2>&1
    sleep 1
    printf 'mouse_button 0\n' | nc -q 0 -U "$SOCK" >/dev/null 2>&1
    sleep 2
  done
}
