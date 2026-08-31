# Phase 14 Handoff Notes

## NEOOS_DEBUG_STOP_WINDOW

The `NEOOS_DEBUG_STOP_WINDOW` build flag widens the SIGSTOP/SIGCONT race window
to make the "continued thread never runs" bug reliably reproducible in tests.

**Usage:** `make test NEOOS_DEBUG_STOP_WINDOW=1`

**What it does:** Inserts an extra `schedule()` call in `signal_do_stop()` between
the state transition (setting `THREAD_STOPPED`) and the final `schedule()`. This
gives a SIGCONT signal ample opportunity to arrive during the race window.

**When to use it:** As a manual regression check when working on SIGSTOP/SIGCONT
synchronization. It is not part of CI — the normal build keeps the race window
tight and the test (`check_stop_continue_race`) runs as cheap smoke cover.

**Expected behavior:**
- With the fix in place: sigtest passes (the lock ensures atomicity)
- With the fix reverted: sigtest hangs (SIGCONT is lost, thread never wakes)

**Test boost:** With the debug window enabled, `STOP_RACE_ROUNDS` is set to 200
iterations (vs. 10 in the normal build), increasing coverage substantially.
