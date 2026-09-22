#!/bin/bash
# tools/screenshot_settled.sh SOCK OUT.ppm [max-seconds]
#
# Screendumps a running QEMU (monitor on unix socket SOCK) once the
# screen has SETTLED: two consecutive dumps a second apart are
# byte-identical, so whatever frame the last guest event triggered has
# landed. max-seconds (default 30) bounds the wait; hitting it still
# leaves the latest dump in OUT, and says so.
set -u
SOCK="$1"; OUT="$(realpath -m "$2")"; MAX="${3:-30}"
PREV="$(mktemp /tmp/neoos-shot-XXXX.ppm)"
dump() { printf 'screendump %s\n' "$1" | nc -q 1 -U "$SOCK" >/dev/null 2>&1; }
deadline=$((SECONDS + MAX)); settled=0
while [ $SECONDS -lt $deadline ]; do
  dump "$OUT"; sleep 1; dump "$PREV"
  if [ -s "$OUT" ] && cmp -s "$OUT" "$PREV"; then settled=1; break; fi
done
[ $settled = 1 ] || echo "screenshot_settled: screen never settled in ${MAX}s -- keeping the last dump"
rm -f "$PREV"
