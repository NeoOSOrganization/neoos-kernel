# os-builder: install ports as real disk files, not embedfs

## 1. Problem

`neoos-os-builder`'s `build.sh` currently treats every entry in a
config's `ports:` list identically to the regression suite: it adds
each port's `build/` output to `neoos-kernel`'s `EMBED_DIRS`, which
links the files into the kernel binary via `gen-embedfs.py` and
(if the port's `<name>.test.json` declares any) patches an inittab
entry that auto-launches it at boot.

That was the right mechanism for the regression suite — it needs to
run unattended, every boot, with zero disk-image dependency. It is the
wrong mechanism for an end-user-chosen port (busybox, 3d-ascii-viewer,
doom, ...): a port is something a user picks to have *available*, not
something that should auto-run at boot, and `gen-embedfs.py` only
embeds files ending in `.nex` in the first place — a port that ships a
non-executable data file alongside its binary (doom's `doom1.wad`) has
no path through embedfs at all today. Tonight's real Doom regression
run only worked because the WAD was hand-`mcopy`'d onto the disk image
and the inittab hand-edited — steps `os-builder` cannot reproduce.

**Goal:** embedfs stays reserved for the regression test suite only.
Ports get installed as real files on the disk image, launchable by the
user from an interactive shell (`nsh`) by name — no auto-launch, no
inittab involvement.

## 2. Scope

This spec covers exactly one thing: how a port's build output gets
onto the disk image and becomes runnable by name. It explicitly does
NOT cover (raised in the same conversation, deliberately deferred to
their own future specs):

- Dropping the `.nex`/`.elf` executable-extension convention. Ports
  built under this spec still produce and install `<name>.nex`
  binaries.
- ext4 support / real Unix permission bits. FAT stays the only disk
  filesystem; this design does not need permission bits (see §4 — a
  port is just "present on disk," not marked executable-vs-not, the
  same way every other program on the FAT disk image already works).

## 3. What changes, and where

Two repositories change, in tandem, sharing one interface:

- **`neoos-kernel`**: the `disk-image` Make target gains a new
  variable, `PORT_DIRS`, and a new step in its recipe that installs
  each named directory's contents onto the FAT image. `userland/init.c`'s
  default `$PATH` also gains `/usr/local/bin` (see §4's correction —
  that is where a port's binary actually lands, since `/bin` is an
  embedfs mount and cannot hold one), and `userland/musl/login.c`'s
  own PATH fallback is kept identical to it.
- **`neoos-os-builder`**: `scripts/build.sh`'s port loop stops adding
  to `EMBED_DIRS` and builds a `PORT_DIRS` string instead, passed
  through to the kernel's own `make ... disk-image` call (which is
  already how this tool works — it asks `neoos-kernel` to assemble its
  own images rather than reimplementing that).

`tests.include: true` is untouched: the regression suite keeps using
`EMBED_DIRS` exactly as it does today.

## 4. Install layout

> **Correction, found during Task 4 end-to-end verification:** the
> original version of this section put a port's `.nex` at `/bin/`.
> That is wrong — `/bin` is mounted as its own embedfs volume
> (`kernel.c: vfs_mount_fs("bin", "/bin", "embedfs")`), which shadows
> anything written to the underlying FAT disk's own `::bin` directory
> entirely. A real boot with a port installed that way failed with
> `[process] FAILED: file not found: /bin/doom.nex` — the bytes were
> genuinely on `disk.img`, just never visible to the running kernel.
> The section below describes the corrected, verified design
> (`/usr/local/bin/` instead of `/bin/`).

For a port directory (e.g. `doom`) whose `build/` output is
`doom.nex`, `doom.test.json`, `doom1.wad`:

- Every `*.nex` file found in that output is copied to
  `/usr/local/bin/` (so `/usr/local/bin/doom.nex`) — NOT `/bin/`, for
  the reason above. `/usr/local/bin` is a real, unshadowed FAT
  directory, and the conventional Unix home for a locally-installed
  binary in the first place. `nsh`'s `$PATH` (via `init.c`'s
  `base_env`) is extended to `/bin:/usr/tests:/usr/local/bin` so this
  is found by name exactly like any embedfs-provided binary.
- Everything else in that output, minus `*.test.json`/
  `*.manifest.json` (embedfs-only manifests, meaningless once nothing
  reads them via embedfs), is copied verbatim onto the disk image
  under `/opt/<name>/` — so `/opt/doom/doom1.wad`. A port with no
  extra data (busybox, 3d-ascii-viewer) simply gets no `/opt/<name>/`
  directory at all.

A port whose own code needs to find sibling data it shipped (like
doom's WAD) does so via the well-known `/opt/<name>/` location — this
is the convention every future data-shipping port follows (documented
in the org's porting guide as a result of this spec).

No permission bits are set or needed: a program becomes "runnable" on
NeoOS today purely by being a valid ELF at a path PATH-search finds —
there is no executable bit to set on FAT in the first place, so this
matches every other binary already on the disk image (`/bin/nsh.nex`,
etc.).

## 5. `PORT_DIRS` mechanics (`neoos-kernel`)

Format: a space-separated list of `name=path` pairs —

```
PORT_DIRS := doom=/abs/path/to/neoos-doom/build busybox=/abs/path/to/neoos-busybox/build
```

(`name=path` rather than a bare directory list, unlike `EMBED_DIRS`,
because the install step needs an explicit `/opt/<name>/` label that
directory-basename-guessing could get wrong or collide on.)

Default: empty, exactly like `EMBED_DIRS` — a bare `make disk-image`
installs zero ports and is byte-for-byte what it is today.

New step in the `disk-image` recipe (conceptually — real Makefile
syntax may shell out to a small loop rather than write this inline;
see §4's correction for why `.nex` goes to `/usr/local/bin`, not
`/bin`):

```
for pair in $(PORT_DIRS); do
    name="${pair%%=*}"; path="${pair#*=}"
    mmd -i $(DISK_IMG) ::usr/local/bin   # idempotent across ports
    for f in "$path"/*; do
        case "$f" in
            *.test.json|*.manifest.json) continue ;;
            *.nex) mcopy -i $(DISK_IMG) "$f" ::usr/local/bin/; continue ;;
        esac
        mmd -i $(DISK_IMG) ::opt/$name
        mcopy -i $(DISK_IMG) "$f" "::opt/$name/"
    done
done
```

This runs after the existing fixed test-fixture / inittab / passwd
steps in the `$(DISK_IMG)` rule, since it needs the root directory
structure (`mmd ::usr` etc.) already in place.

## 6. `neoos-os-builder` changes

`scripts/build.sh`'s port loop (currently):

```sh
for port in $PORTS; do
    echo "-- building neoos-$port --"
    git clone --depth 1 --recurse-submodules "$org/neoos-$port" "$WORK/$port" 2>&1 | tail -1
    (cd "$WORK/$port" && make MUSL_DIR="$WORK/neoos-musl/build-output")
    EMBED_DIRS="$EMBED_DIRS $WORK/$port/build"
done
```

becomes:

```sh
PORT_DIRS=""
for port in $PORTS; do
    echo "-- building neoos-$port --"
    git clone --depth 1 --recurse-submodules "$org/neoos-$port" "$WORK/$port" 2>&1 | tail -1
    (cd "$WORK/$port" && make MUSL_DIR="$WORK/neoos-musl/build-output")
    PORT_DIRS="$PORT_DIRS $port=$WORK/$port/build"
done
```

and the final kernel invocation passes `PORT_DIRS="$PORT_DIRS"`
alongside the existing `EMBED_DIRS="$EMBED_DIRS"` (which now only ever
carries the test suite, if `tests.include: true`).

No change to `config/example.yaml`'s schema: `ports:` already means
"install this port"; only the mechanism behind that word changes. The
README's example config, usage docs, and "ports get embedded" language
are updated to describe the new disk-install behavior and the
`/opt/<name>/` convention.

## 7. `neoos-doom`'s WAD default

Currently nothing tells `doom.nex` where its WAD lives unless
`-iwad <path>` is passed explicitly on argv — which is fine for
tonight's hand-built regression boot, but means `doom` alone, typed at
an `nsh` prompt after a normal install, would fail to find a WAD.

Fix, in `neoos-shim/main.c` (or wherever argv is assembled before
`doomgeneric_Create`): if no `-iwad` flag is present anywhere in
`argv`, append `-iwad /opt/doom/doom1.wad` before the call — matching
§4's install convention exactly. A user who installed the doom port
via `os-builder` can then just type `doom`.

## 8. Testing

- `neoos-kernel`: existing gauntlet must stay green with `PORT_DIRS`
  unset (default-empty — zero behavioral change to the existing
  `disk-image` target's output when the variable is not passed). A new
  manual check: build with `PORT_DIRS=doom=<path-to-a-built-doom-checkout>/build`,
  boot, and confirm via `mdir`/`mtype` against the produced
  `disk.img` that `/usr/local/bin/doom.nex` and `/opt/doom/doom1.wad`
  are present with the right contents, AND — this is the check that
  actually caught the `/bin`-is-embedfs bug during implementation —
  boot the image with a hand-crafted `wait /usr/local/bin/doom.nex`
  inittab entry (no `-iwad`) and confirm Doom's title screen actually
  renders. `mdir`/`mtype` alone proves bytes reached the disk; only a
  real boot proves the running kernel can see them at that path.
- `neoos-os-builder`: `make test`/`make run` against a config with
  `ports: [doom]` and no `tests.include`, verifying the build succeeds
  and the resulting image boots clean to the login prompt (the
  existing `make test` check), plus the same `mdir`/`mtype` disk
  content check as above run against `os-builder`'s own output disk
  image.
- `neoos-doom`: a unit-level check (or a short comment/manual trace,
  given this repo also has "no host-runnable unit tests" per its
  parent project's convention) confirming argv gets `-iwad
  /opt/doom/doom1.wad` appended when absent, and is left alone when
  the user did pass `-iwad` explicitly.

## 9. Explicitly out of scope (deferred, not forgotten)

- Extension-free executables (`doom` on disk instead of `doom.nex`) —
  next milestone after this one, its own brainstorm/spec.
- ext4 / real Unix permission bits — a later, larger milestone, its
  own brainstorm/spec, likely decomposed further (read support, then
  write, then permissions/journaling) given its size relative to any
  filesystem work this project has done so far.
