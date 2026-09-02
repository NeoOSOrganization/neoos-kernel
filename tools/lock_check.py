#!/usr/bin/env python3
"""Host-side lock-discipline checks (CS5.3, CS5.4).

Two things that can only be caught by reading the source, both of which
have already gone wrong once in this kernel:

1. Two LOCK_RANK_* constants with the same value and nothing saying they
   are meant to be the same. Ranks must be strictly ascending along any
   acquire path, so a collision turns a real inversion into a legal one
   -- the checker stops seeing the bug. CS0.2 hit exactly this: rand_lock
   was given LOCK_RANK_SERIAL, silently sharing a rank with the serial
   port. That is this script's first real finding, and it would have been
   caught at commit time rather than by reading the file.

   A deliberate alias is allowed, written either as an alias
   (`#define LOCK_RANK_TTY LOCK_RANK_DRIVER`) or as a literal whose
   preceding comment names the constant it shares with.

2. New spin_lock_raw / spin_unlock_raw call sites. Raw acquires skip the
   rank check entirely, so each one is a hole in the discipline. lock.h
   documents exactly two exceptions; anything else must be argued for in
   review rather than merged quietly.

Exit status is 1 on any finding, so this can gate a build the way
REQUIRED_MARKERS gates a boot.
"""

import re
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LOCK_H = os.path.join(ROOT, "kernel", "sync", "lock.h")

# The two documented raw-acquire sites (lock.h's own list). Paths are
# relative to the repo root; a raw acquire anywhere else is a finding.
RAW_ALLOWED = {
    "kernel/drivers/char/serial.c":      "runs before cpu_local_init()",
    "kernel/drivers/video/fbcon.c":      "panic path: fbcon_acquire when fbcon_panicking",
    "kernel/tty/vt.c":                   "panic path: skips its lock when vt_panicking",
}

DEFINE_RE = re.compile(r"^\s*#define\s+(LOCK_RANK_\w+)\s+(\S+)")


def check_ranks():
    """Returns a list of finding strings."""
    findings = []
    with open(LOCK_H) as f:
        lines = f.read().split("\n")

    # name -> (raw token, line index, comment block above it)
    defs = {}
    order = []
    for i, line in enumerate(lines):
        m = DEFINE_RE.match(line)
        if not m:
            continue
        name, token = m.group(1), m.group(2)
        # Gather the contiguous comment block immediately above.
        comment = []
        j = i - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            comment.append(lines[j])
            j -= 1
        defs[name] = (token, i + 1, "\n".join(reversed(comment)))
        order.append(name)

    if not defs:
        return ["lock_check: no LOCK_RANK_* definitions found -- has lock.h moved?"]

    # Resolve values. A token that names another rank is an ALIAS, which
    # is self-documenting and always allowed.
    values = {}
    aliases = set()
    for name in order:
        token, lineno, _ = defs[name]
        if token.startswith("LOCK_RANK_"):
            aliases.add(name)
            if token not in defs:
                findings.append(
                    "%s:%d: %s aliases %s, which is not defined"
                    % (LOCK_H, lineno, name, token))
                continue
            values[name] = defs[token][0]
        else:
            values[name] = token

    # Group the LITERAL-valued ranks by value.
    by_value = {}
    for name, val in values.items():
        if name in aliases:
            continue
        by_value.setdefault(val, []).append(name)

    for val, names in sorted(by_value.items()):
        if len(names) < 2:
            continue
        # A collision is allowed only if every name past the first says,
        # in its own comment, which constant it shares with.
        for name in names:
            _, lineno, comment = defs[name]
            others = [o for o in names if o != name]
            if any(o in comment for o in others):
                continue
            findings.append(
                "%s:%d: %s shares rank %s with %s, and its comment does not "
                "say so. Two locks at one rank make an inversion between them "
                "legal to the checker. Renumber, or write an alias "
                "(#define %s %s), or name the other constant in the comment."
                % (LOCK_H, lineno, name, val, ", ".join(others), name, others[0]))
    return findings


def check_raw_locks():
    findings = []
    for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, "kernel")):
        for fn in filenames:
            if not (fn.endswith(".c") or fn.endswith(".h")):
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, ROOT)
            if rel.startswith("kernel/sync/lock"):
                continue          # the implementation itself
            with open(path) as f:
                for i, line in enumerate(f, 1):
                    if "spin_lock_raw" not in line and "spin_unlock_raw" not in line:
                        continue
                    if rel in RAW_ALLOWED:
                        continue
                    findings.append(
                        "%s:%d: raw lock acquire outside the documented "
                        "exceptions.\n    %s\n    Raw acquires skip the rank "
                        "check, so each one is a hole in the lock discipline. "
                        "lock.h lists the two cases that are allowed; if this "
                        "is a third, it needs an argument there first."
                        % (rel, i, line.strip()))
    return findings


def main():
    findings = check_ranks() + check_raw_locks()
    if findings:
        print("lock-check: %d finding(s)\n" % len(findings))
        for f in findings:
            print(f)
            print("")
        return 1
    print("lock-check: rank values distinct (or aliased deliberately), "
          "no undocumented raw acquires")
    return 0


if __name__ == "__main__":
    sys.exit(main())
