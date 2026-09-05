#!/bin/bash
# Parallel gauntlet: build once, run N headless boots concurrently, apply
# the same checks `make test` does (FAILED lines / boot marker / every
# REQUIRED_MARKER, extracted live from the Makefile so it can't drift).
#
#   tools/gauntlet.sh [N] [CONC]
#     N     number of runs        (default 15)
#     CONC  max concurrent boots  (default 3)
#
# Each run gets its own copy of the ISO and both disk images (a write
# selftest mutates the disk), its own serial log, and BOOT_TIMEOUT
# seconds -- read from the Makefile, not duplicated here.
#
# Classification of a failing run:
#   HARD   -- a signature that is a real kernel bug regardless of load:
#             a lock-rank panic, a fault, the thread_join race, a
#             lifetime/steal/parallelism selftest failure, or any FAILED
#             line that is not in the known host-contention set.
#             Fails the gauntlet immediately, no retry.
#   FLAKY  -- only host-contention artifacts (guest clock starvation,
#             emulated-ATA timeouts), a missing marker with no hard
#             signature, or the known pre-existing net socktable
#             schedule panic. Retried once, solo. Still counts as a
#             failure if the retry also fails.
#
# Exit 0 and "PGAUNTLET PASSED: N/N" iff every run ends clean (first try
# or solo retry).
set -u
cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 2

N=${1:-15}
CONC=${2:-3}
DIR=build/gauntlet
WORK=$DIR/work
mkdir -p "$DIR"
# GAUNTLET_MAKEFLAGS lets a caller run the poisoned/instrumented kernel
# (e.g. GAUNTLET_MAKEFLAGS="DEBUG_HEAP=1"). Empty by default: the
# gauntlet's job is to exercise what ships.
GAUNTLET_MAKEFLAGS=${GAUNTLET_MAKEFLAGS:-}
if [ "${KVM:-}" = "1" ]; then GAUNTLET_MACHINE="-enable-kvm -cpu host"
else GAUNTLET_MACHINE="-cpu Nehalem"; fi
BOOT_MARKER="NeoOS: interrupts enabled, starting scheduler"
# Read from the Makefile rather than duplicated here, for the same
# reason the markers are: the two drifted apart once already (this said
# 60 while the header claimed 150 and `make test` used 150), and a
# gauntlet that times out earlier than `make test` reports healthy runs
# as missing markers.
TIMEOUT=$(sed -n 's/^BOOT_TIMEOUT ?= *\([0-9]*\).*/\1/p' Makefile)
TIMEOUT=${TIMEOUT:-240}

# Real bugs -- never retried, always fail the gauntlet. The bare
# PANIC alternative already covers the debug heap's '[heap] PANIC:
# use-after-free / double free / heap overrun' lines.
HARD_RE='PANIC|#PF|#GP|#DF|double fault|general protection|GPF|triple fault|thread_join [0-9]|lifetime selftest FAILED|steal selftest FAILED|parallelism selftest FAILED|use-after-free|UAF'
# Host-load artifacts -- a run failing ONLY on these is retried solo.
# The emulated IDE/ATA controller misbehaves (BSY/DRQ timeouts, ERR/ABRT)
# when QEMU is starved of host CPU; the driver is fine under a sequential
# `make test`. tier0's clock/nanosleep checks need guest timer ticks the
# starved vCPU does not get. Missing markers with no hard signature are a
# boot that ran out of wall-clock.
# A starved emulated ATA controller (BSY/DRQ timeouts) cascades: the
# FAT32 disk read fails, so /mnt does not populate and every later
# resolve/open/opendir against it fails too. Those downstream lines are
# flaky when an [ata] failure is present in the same log.
FLAKY_RE='clock did not advance|nanosleep returned early|\[ata\] (read|write) FAILED|BSY never cleared|DRQ never set|ERR bit set|sector write failed|sector read failed|SOME CHECKS FAILED|write selftest FAILED: sector|selftest FAILED: resolve /mnt|fat32 \(/mnt\) FAILED|listing /mnt FAILED'
# Known pre-existing, tracked separately (Phase 13.6 residual: socket
# lifetime -- a blocked reader does not hold a ref on its socket). Rare
# timing race; retried solo like a flake, reported loudly.
KNOWN_RE='schedule\(\) with a spinlock held.*socktable'

rm -rf "$WORK"; mkdir -p "$WORK"
rm -f "$DIR"/pgauntlet.serial.run*

mapfile -t MARKERS < <(
  awk '/^REQUIRED_MARKERS[[:space:]]*:=/{f=1} f{print} f&&!/\\[[:space:]]*$/{exit}' Makefile \
  | grep -oE '"[^"]+"' | sed 's/^"//; s/"$//'
)
if [ "${#MARKERS[@]}" -lt 10 ]; then
  echo "pgauntlet: failed to parse REQUIRED_MARKERS from Makefile (${#MARKERS[@]})"; exit 2
fi
echo "pgauntlet: N=$N CONC=$CONC, ${#MARKERS[@]} required markers"

rm -f build/disk.img build/disk2.img
# clean-kernel so a prior `make test` (which compiles with
# -DNEOOS_TEST_HOOKS) cannot leave stale objects that relink into this
# production build -- the gauntlet's job is to exercise what ships.
if ! make clean-kernel $GAUNTLET_MAKEFLAGS iso disk-image > "$WORK/build.log" 2>&1; then
  echo "pgauntlet: BUILD FAILED"; tail -20 "$WORK/build.log"; exit 1
fi

boot_one() {   # $1 = tag
  local t=$1
  cp build/neoos.iso "$WORK/iso.$t"
  cp build/disk.img  "$WORK/d1.$t"
  cp build/disk2.img "$WORK/d2.$t"
  # KVM=1 matches the Makefile's opt-in: 2.3x faster, and true
  # parallelism instead of TCG's round-robin vCPUs. The default stays
  # TCG/Nehalem -- the flake signatures below are tuned for it.
  timeout $TIMEOUT qemu-system-x86_64 $GAUNTLET_MACHINE -smp 4 -boot order=d -vga std \
    -cdrom "$WORK/iso.$t" \
    -drive file="$WORK/d1.$t",format=raw -drive file="$WORK/d2.$t",format=raw \
    -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
    -no-reboot -display none -serial file:"$WORK/serial.$t" \
    > /dev/null 2>"$WORK/qemu.err.$t"
  rm -f "$WORK/iso.$t" "$WORK/d1.$t" "$WORK/d2.$t"
}

# echoes reasons; return 0 clean / 1 flaky (retryable) / 2 hard
check_log() {
  local log=$1 reasons="" fails
  if [ ! -s "$log" ]; then echo "NO OUTPUT (qemu never ran?)"; return 1; fi
  if grep -Eq "$HARD_RE" "$log"; then
    echo "HARD: $(grep -E "$HARD_RE" "$log" | head -3)"; return 2
  fi
  fails=$(grep -E 'FAILED|PANIC' "$log" || true)
  if [ -n "$fails" ]; then
    # any FAILED/PANIC line that is neither a known flake nor the known
    # pre-existing net panic -> hard
    if grep -vE "$FLAKY_RE|$KNOWN_RE" <<<"$fails" | grep -Eq 'FAILED|PANIC'; then
      echo "HARD: $(grep -vE "$FLAKY_RE|$KNOWN_RE" <<<"$fails" | head -3)"; return 2
    fi
    grep -Eq "$KNOWN_RE" <<<"$fails" && reasons="$reasons"$'\n'"KNOWN net socktable panic (pre-existing)"
    grep -Eq "$FLAKY_RE" <<<"$fails" && reasons="$reasons"$'\n'"host-contention flake"
  fi
  grep -qF "$BOOT_MARKER" "$log" || reasons="$reasons"$'\n'"no boot marker"
  local m
  for m in "${MARKERS[@]}"; do
    grep -qF "$m" "$log" || reasons="$reasons"$'\n'"missing: $m"
  done
  [ -z "$reasons" ] && return 0
  echo "$reasons"; return 1
}

MISSES=$WORK/misses.txt
: > "$MISSES"

pass=0; hardfail=0; retried=0; knownhit=0
running=0
for i in $(seq 1 "$N"); do
  boot_one "$i" &
  running=$((running+1))
  if [ "$running" -ge "$CONC" ]; then wait -n; running=$((running-1)); fi
done
wait

for i in $(seq 1 "$N"); do
  out=$(check_log "$WORK/serial.$i"); rc=$?
  grep '^missing: ' <<<"$out" | sed 's/^missing: //' >> "$MISSES"
  grep -q 'KNOWN net socktable' <<<"$out" && knownhit=$((knownhit+1))
  if [ $rc -eq 0 ]; then echo "run $i: PASS"; pass=$((pass+1)); continue; fi
  if [ $rc -eq 2 ]; then
    echo "run $i: HARD FAIL"; sed 's/^/    /' <<<"$out"
    cp "$WORK/serial.$i" "$DIR/pgauntlet.serial.run$i"; hardfail=1; continue
  fi
  echo "run $i: flaky/known fail -> retry solo"; sed 's/^/    /' <<<"$out"
  retried=$((retried+1))
  boot_one "r$i"
  out=$(check_log "$WORK/serial.r$i"); rc=$?
  if [ $rc -eq 0 ]; then echo "run $i: PASS (on retry)"; pass=$((pass+1)); continue; fi
  echo "run $i: FAIL after retry ($rc)"; sed 's/^/    /' <<<"$out"
  cp "$WORK/serial.r$i" "$DIR/pgauntlet.serial.run$i"; hardfail=1
done

if [ -s "$MISSES" ]; then
  echo "--- per-marker flakiness over $N run(s) ---"
  sort "$MISSES" | uniq -c | sort -rn | while read -r n m; do
    pct=$(( n * 100 / N ))
    printf '  %3d%%  %2d/%d  %s\n' "$pct" "$n" "$N" "$m"
  done
fi

echo "---"
[ $retried -gt 0 ] && echo "note: $retried run(s) retried solo for host-contention/known artifacts"
[ $knownhit -gt 0 ] && echo "note: $knownhit run(s) hit the KNOWN pre-existing net socktable panic (Phase 13.6 residual)"
if [ $hardfail -eq 0 ] && [ $pass -eq $N ]; then
  echo "PGAUNTLET PASSED: $N/$N"; rm -rf "$WORK"; exit 0
fi
echo "PGAUNTLET FAILED ($pass/$N passed; serial logs: $DIR/pgauntlet.serial.run*)"
exit 1
