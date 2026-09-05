#!/usr/bin/env python3
# tools/gen-embedfs.py <output.c> <dir> [<dir> ...]
#
# For every *.nex in each directory, embeds it via `ld -r -b binary` and
# emits one row in a generated embedfs table (kernel/fs/embedfs.h). A
# *.test.json next to a *.nex supplies its "category" ("bin", "sbin", or
# "tests") plus optional "boot_entries"/"required_markers"; absent a
# manifest, category defaults to "tests". A *.manifest.json in a
# directory is a JSON array of manifest objects, each carrying its own
# "_test" name -- used for a large bundle of tests sharing one file
# instead of one <name>.test.json per binary.
#
# Directory and bundle-entry ORDER matters: boot_entries chain via
# "after" anchors (tools/apply-inittab-patch.py resolves each against
# whatever's already been placed), so a later directory's entries can
# anchor into an earlier directory's, and a bundle's entries are
# collected in the bundle file's own array order, NOT alphabetically by
# filename -- alphabetical would scramble a hand-tuned chain (e.g.
# "looper" would sort before "parent" even though it must anchor onto
# it). Pass directories on the command line in the order their anchors
# require (boot-critical apps, then the test suite, then anything that
# anchors into the test suite, like BusyBox).
#
# Outputs, alongside <output.c>:
#   embedfs-obj/*.o         one ld -r -b binary object per embedded file
#   embedfs-objs.txt        space-separated list of those object paths
#   embedfs-inittab-patch.json   collected boot_entries, in encounter order
#   embedfs-markers.txt     collected required_markers, one per line
import json
import os
import subprocess
import sys


def load_manifest_bundle_ordered(dirpath):
    """Returns (dict by _test, list in file-declared order)."""
    by_test = {}
    ordered = []
    for fname in sorted(os.listdir(dirpath)):
        if not fname.endswith(".manifest.json"):
            continue
        with open(os.path.join(dirpath, fname)) as f:
            for entry in json.load(f):
                if "_test" in entry:
                    by_test[entry["_test"]] = entry
                    ordered.append(entry)
    return by_test, ordered


def main():
    out_c = sys.argv[1]
    dirs = sys.argv[2:]
    out_dir = os.path.dirname(os.path.abspath(out_c)) or "."
    obj_dir = os.path.join(out_dir, "embedfs-obj")
    os.makedirs(obj_dir, exist_ok=True)

    entries = []       # (category, name, symbol, obj_path) -- order doesn't matter
    boot_entries = []  # order DOES matter -- anchors chain against it
    markers = []

    for d in dirs:
        if not d or not os.path.isdir(d):
            continue
        bundle_by_test, bundle_ordered = load_manifest_bundle_ordered(d)
        nex_files = sorted(f for f in os.listdir(d) if f.endswith(".nex"))
        consumed_by_bundle = set()

        # 1. Embed every .nex file and add its table row (order-independent).
        for fname in nex_files:
            base = fname[:-len(".nex")]
            single_path = os.path.join(d, base + ".test.json")
            if os.path.exists(single_path):
                with open(single_path) as f:
                    manifest = json.load(f)
            elif base in bundle_by_test:
                manifest = bundle_by_test[base]
                consumed_by_bundle.add(base)
            else:
                manifest = {}
            category = manifest.get("category", "tests")

            abs_path = os.path.abspath(os.path.join(d, fname))
            symbol = "".join(c if c.isalnum() else "_" for c in abs_path)
            # Keyed by the full mangled symbol, not just the basename:
            # the same filename (e.g. "looper.nex") can legitimately
            # appear in two different EMBED_DIRS (a boot-critical copy
            # in neoos-kernel itself, a regression-suite copy in
            # neoos-kernel-tests-common) -- basename-only collided,
            # silently overwriting one object file with the other's
            # and producing an undefined-reference/multiple-definition
            # link failure instead of two independent table entries.
            obj_path = os.path.join(obj_dir, symbol + ".o")

            ld = os.environ.get("LD", "x86_64-elf-ld")
            subprocess.run(
                [ld, "-r", "-b", "binary", "-o", obj_path, abs_path],
                check=True,
            )
            entries.append((category, fname, symbol, obj_path))

        # 2. Collect boot_entries/markers in a STABLE, anchor-safe order:
        #    the bundle's own declared order first (this is what preserves
        #    the hand-tuned chain), then any single <name>.test.json files
        #    for names not already covered by the bundle, alphabetically.
        for manifest in bundle_ordered:
            boot_entries.extend(manifest.get("boot_entries", []))
            markers.extend(manifest.get("required_markers", []))
        for fname in nex_files:
            base = fname[:-len(".nex")]
            if base in consumed_by_bundle:
                continue
            single_path = os.path.join(d, base + ".test.json")
            if not os.path.exists(single_path):
                continue
            with open(single_path) as f:
                manifest = json.load(f)
            boot_entries.extend(manifest.get("boot_entries", []))
            markers.extend(manifest.get("required_markers", []))

    with open(out_c, "w") as f:
        f.write('#include "fs/embedfs.h"\n#include <stddef.h>\n\n')
        for _, _, symbol, _ in entries:
            f.write(f"extern char _binary_{symbol}_start[], _binary_{symbol}_end[];\n")
        f.write("\nconst struct embedfs_entry g_embedfs_table[] = {\n")
        for category, name, symbol, _ in entries:
            # Both fields are plain symbol addresses (pointer-typed
            # relocations), which GCC folds fine in a static
            # initializer -- unlike `_end - _start` cast to an integer,
            # which it rejects. The byte count is computed at runtime
            # from these two (embedfs.c).
            f.write(
                f'    {{"{category}", "{name}", _binary_{symbol}_start, _binary_{symbol}_end}},\n'
            )
        if not entries:
            f.write("    {0, 0, 0, 0},\n")
        f.write("};\n")
        f.write(f"const int g_embedfs_table_count = {len(entries)};\n")

    with open(os.path.join(out_dir, "embedfs-objs.txt"), "w") as f:
        f.write(" ".join(p for _, _, _, p in entries))

    with open(os.path.join(out_dir, "embedfs-inittab-patch.json"), "w") as f:
        json.dump(boot_entries, f)

    with open(os.path.join(out_dir, "embedfs-markers.txt"), "w") as f:
        f.write("\n".join(markers))


if __name__ == "__main__":
    main()
