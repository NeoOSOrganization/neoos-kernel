#!/bin/bash
# tools/boot_until.sh LOG DONE-REGEX MAX-SECONDS -- QEMU-ARGS...
#
# Boots QEMU with -serial file:LOG and ends it as soon as LOG contains
# DONE-REGEX (the last line the caller's checks need; $COUNT times, if
# set, e.g. COUNT=2 for two windows mapping) or a kernel
# PANIC/[exception] -- never after a fixed sleep. MAX-SECONDS is only a
# hang bound. For headless runs of long-lived programs, like the
# compositor, which never exit on their own once idle.
#
# LOG is deleted first: QEMU truncates it only when it opens it, and a
# stale log from the previous run would satisfy DONE-REGEX instantly.
set -u
LOG="$1"; DONE="$2"; MAX="$3"; shift 3
[ "${1:-}" = "--" ] && shift
rm -f "$LOG"
qemu-system-x86_64 "$@" -serial "file:$LOG" > /dev/null 2>&1 &
QPID=$!
deadline=$((SECONDS + MAX))
while kill -0 "$QPID" 2>/dev/null; do
  n=$(grep -cE "$DONE" "$LOG" 2>/dev/null); n=${n:-0}   # empty until QEMU creates LOG
  if [ "$n" -ge "${COUNT:-1}" ]; then break; fi
  if grep -qE 'PANIC|\[exception\]' "$LOG" 2>/dev/null; then break; fi
  if [ $SECONDS -ge $deadline ]; then echo "boot_until: '$DONE' not seen in ${MAX}s"; break; fi
  sleep 0.5
done
sleep 0.5   # let the line's own write finish landing in the file
kill "$QPID" 2>/dev/null; wait "$QPID" 2>/dev/null
exit 0
