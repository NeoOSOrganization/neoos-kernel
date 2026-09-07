#!/bin/bash
# Host side of `make epolltcp`. Waits for the guest's epoll accept loop
# to say it is listening, then fires REQS HTTP requests at it CONC at a
# time and checks every one came back with the expected body.
#
# The point of the concurrency is that Kestrel's SocketAsyncEngine is
# what this models: one epoll set, many connections in flight, each
# accepted and answered from an epoll readiness report. A serial driver
# would pass against a kernel that only ever has one connection queued.
set -u

PORT=${1:?port}
CONC=${2:?concurrency}
REQS=${3:?requests}
LOG=${4:?guest serial log}

# Wait for the guest to bind. The boot itself is ~25s; give it BOOT
# room but fail loudly rather than hanging if it never gets there.
for _ in $(seq 1 120); do
  [ -f "$LOG" ] && grep -q '\[epolltcp\] listening' "$LOG" && break
  grep -q '\[epolltcp\] FAIL' "$LOG" 2>/dev/null && break
  sleep 1
done
if ! grep -q '\[epolltcp\] listening' "$LOG" 2>/dev/null; then
  echo "EPOLLTCP FAILED: guest never reached 'listening'"
  tail -15 "$LOG" 2>/dev/null
  exit 1
fi

echo "epolltcp: guest listening; driving $REQS requests at concurrency $CONC"

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

running=0
for i in $(seq 1 "$REQS"); do
  (
    # --http1.0 + a fresh connection per request: the engine closes each
    # connection after answering, which is the path that exercises
    # close-without-EPOLL_CTL_DEL and fd-number reuse.
    code=$(curl -sS -m 25 -o "$OUT/body.$i" -w '%{http_code}' \
             "http://127.0.0.1:$PORT/" 2>"$OUT/err.$i" || echo 000)
    echo "$code" > "$OUT/code.$i"
  ) &
  running=$((running+1))
  if [ "$running" -ge "$CONC" ]; then wait -n; running=$((running-1)); fi
done
wait

ok=0; bad=0
for i in $(seq 1 "$REQS"); do
  code=$(cat "$OUT/code.$i" 2>/dev/null || echo 000)
  if [ "$code" = "200" ] && grep -q 'hello neoos' "$OUT/body.$i" 2>/dev/null; then
    ok=$((ok+1))
  else
    bad=$((bad+1))
    [ "$bad" -le 3 ] && echo "  req $i: code=$code $(head -c 120 "$OUT/err.$i" 2>/dev/null)"
  fi
done

echo "epolltcp: $ok/$REQS answered, $bad failed"

# Give the guest a moment to print its own verdict and power off.
for _ in $(seq 1 20); do
  grep -qE '\[epolltcp\] (ALL PASSED|FAIL)' "$LOG" && break
  sleep 1
done

if [ "$bad" -ne 0 ]; then
  echo "EPOLLTCP FAILED: $bad of $REQS requests did not get a 200"
  exit 1
fi
if ! grep -q '\[epolltcp\] ALL PASSED' "$LOG"; then
  echo "EPOLLTCP FAILED: guest never printed ALL PASSED"
  exit 1
fi
echo "EPOLLTCP PASSED: $ok/$REQS at concurrency $CONC"
exit 0
