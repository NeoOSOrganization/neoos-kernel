#!/usr/bin/env python3
# tools/apply-inittab-patch.py <base-inittab> <patch.json>
#
# Applies each {"after": name, "line": text} entry in the patch (an
# array, produced by tools/gen-embedfs.py from every manifest it found)
# in array order: finds the LAST line in the growing file containing
# "/<name>.nex", inserts `line` immediately after it. Comment lines
# (leading '#') in the base file are preserved but never matched as an
# anchor (a test's .nex path only ever appears in a real inittab
# line). Prints the result to stdout.
#
# "Last occurrence" matters when a name repeats (e.g. looper.nex spawns
# twice): processing entries strictly in order and always anchoring to
# the most recent match means a later entry anchored on that same name
# correctly lands after the LATEST occurrence, not the first.
import json
import sys


def main():
    base_path, patch_path = sys.argv[1], sys.argv[2]
    with open(base_path) as f:
        lines = [l.rstrip("\n") for l in f if l.strip()]
    with open(patch_path) as f:
        patch = json.load(f)

    for entry in patch:
        anchor = f"/{entry['after']}.nex"
        matches = [i for i, l in enumerate(lines) if not l.lstrip().startswith("#") and anchor in l]
        if not matches:
            print(f"error: no inittab line matches anchor '{anchor}' for entry {entry!r}", file=sys.stderr)
            sys.exit(1)
        idx = max(matches)
        lines.insert(idx + 1, entry["line"])

    print("\n".join(lines))


if __name__ == "__main__":
    main()
