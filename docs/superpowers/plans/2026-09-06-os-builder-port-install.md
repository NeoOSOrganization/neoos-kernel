# os-builder Port Installation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

> **Correction, found during Task 4's real end-to-end boot (not caught
> by Task 1's own `mdir`-only check):** every `/bin/` reference below
> is wrong. `/bin` is mounted as its own embedfs volume (`kernel.c`),
> which shadows anything written to the underlying FAT disk's `::bin`
> directory — a port's `.nex` installed there is bytes on disk the
> running kernel can never see. The actual shipped fix installs `.nex`
> files at `/usr/local/bin/` instead (kernel commit `46abeba`), with
> `init.c`'s `$PATH` extended to match. The design doc
> (`docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md`)
> has been corrected in place; this plan is left as the historical
> record of what was executed and is not itself corrected line-by-line
> — read the spec for the accurate design.

**Goal:** ports chosen in an `os-builder` config land on the built OS
image as real disk files under `/opt/<name>/` (plus a `/bin/` copy of
each `.nex` for `nsh`'s `$PATH`), launchable by hand — `EMBED_DIRS`
goes back to being the regression suite's mechanism only.

**Architecture:** `neoos-kernel`'s `disk-image` Make target gains a
`PORT_DIRS` variable (`name=path` pairs) and a new recipe step that
`mcopy`s each named directory onto the FAT image. `neoos-os-builder`'s
`build.sh` stops feeding ports into `EMBED_DIRS` and builds `PORT_DIRS`
instead. `neoos-doom` gets a small default-IWAD fallback so `doom`
alone works once installed this way.

**Tech Stack:** GNU Make, bash, `mtools` (`mcopy`/`mmd`/`mdir`/`mtype`),
C (musl userland), the existing `x86_64-elf-*` cross toolchain already
on `$PATH`.

**Spec:** `docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md`

## Global Constraints

- `PORT_DIRS` defaults to empty; a bare `make disk-image` (no
  `PORT_DIRS` passed) must produce byte-identical disk-image content
  to what it produces today.
- `*.test.json`/`*.manifest.json` files in a port's build output are
  embedfs-only manifests and must NOT be copied onto the disk image.
- Every `*.nex` file installed under `/opt/<name>/` also gets a copy
  at `/bin/<basename>` — FAT has no symlinks, so this is a real second
  copy, not a link.
- `tests.include: true` in an os-builder config keeps using
  `EMBED_DIRS` exactly as today — this plan does not touch that path.
- No inittab entry is created for a port. None of this plan's changes
  touch `tools/apply-inittab-patch.py` or any `boot_entries` machinery.
- This repo (`neoos-kernel`) builds and tests directly on `main`, no
  feature branches (project convention). `neoos-os-builder` and
  `neoos-doom` are separate git repos with their own histories — each
  task below says which working directory it operates in.

---

### Task 1: `neoos-kernel` — `PORT_DIRS` support in the disk-image build

**Working directory:** `/home/neo/projects/personal/NeoOS` (this repo, on `main`)

**Files:**
- Modify: `Makefile:131-135` (the `EMBED_DIRS` comment block)
- Modify: `Makefile:236-268` (the `$(DISK_IMG)` rule)

**Interfaces:**
- Produces: `PORT_DIRS` Make variable, default empty, format
  `name1=path1 name2=path2 ...`. Consumed by nothing else in this
  repo; `neoos-os-builder` (Task 2) is the only external consumer, by
  passing it on the `make` command line exactly like `EMBED_DIRS`.

- [ ] **Step 1: Update the `EMBED_DIRS` comment to stop describing ports**

`Makefile:127-135` currently reads:

```make
# ---- embedfs ----------------------------------------------------------
#
# Boot-critical apps (init/login/term/nsh) are ALWAYS embedded --
# without init.nex there is nothing for the kernel to spawn as PID 1
# (kernel/kernel.c). Everything else (the regression suite, ports) is
# optional: EMBED_DIRS is empty by default, so a bare `make` embeds
# only these four. See docs/superpowers/specs/
# 2026-09-05-embedded-test-and-app-architecture.md.
EMBED_DIRS ?=
```

Change the parenthetical on the "Everything else" line and add the
`PORT_DIRS` variable right below it:

```make
# ---- embedfs ----------------------------------------------------------
#
# Boot-critical apps (init/login/term/nsh) are ALWAYS embedded --
# without init.nex there is nothing for the kernel to spawn as PID 1
# (kernel/kernel.c). Everything else (the regression suite) is
# optional: EMBED_DIRS is empty by default, so a bare `make` embeds
# only these four. See docs/superpowers/specs/
# 2026-09-05-embedded-test-and-app-architecture.md.
#
# Ports do NOT go through embedfs (see docs/superpowers/specs/
# 2026-09-06-os-builder-port-install-design.md): a port is something a
# user chooses to have available, not something that should auto-run
# at boot, and embedfs only ever embeds *.nex files -- a port shipping
# a data file alongside its binary (doom's doom1.wad) has no path
# through it at all. PORT_DIRS (below, in the $(DISK_IMG) rule)
# installs a port's build/ output onto the disk image as real files
# instead: /opt/<name>/ gets everything, /bin/ gets a copy of each
# .nex for nsh's $PATH lookup.
EMBED_DIRS ?=
```

- [ ] **Step 2: Add the `PORT_DIRS` install step to the `$(DISK_IMG)` rule**

`Makefile:236-268` currently ends (right before the `printf` that
builds `inittab.base`):

```make
	mcopy -i $(DISK_IMG) $(DISK_SRC)/dir/nested.txt ::usr/share/test/dir/nested.txt
	@# Ports and the regression suite are not built by this repo at all
	@# (spec: "does NOT contain ports"/tests) -- they arrive purely via
	@# EMBED_DIRS, pointed at neoos-kernel-tests-common's and each
	@# port's build/ output. See the embedfs_table.c rule above.
	@echo "disk: EMBED_DIRS=$(EMBED_DIRS)"
	printf '%s\n' \
```

Replace those five lines (the `mcopy ... nested.txt` line stays; the
three-line comment, the `@echo`, and everything from there down gets
the new step inserted between the comment/echo and the `printf`):

```make
	mcopy -i $(DISK_IMG) $(DISK_SRC)/dir/nested.txt ::usr/share/test/dir/nested.txt
	@# The regression suite is not built by this repo at all -- it
	@# arrives purely via EMBED_DIRS, pointed at
	@# neoos-kernel-tests-common's build/ output. See the
	@# embedfs_table.c rule above.
	@echo "disk: EMBED_DIRS=$(EMBED_DIRS)"
	@# Ports (PORT_DIRS: "name=path name=path ..."), by contrast, are
	@# installed as REAL FILES on this disk image, not via embedfs --
	@# see docs/superpowers/specs/
	@# 2026-09-06-os-builder-port-install-design.md. Every file in a
	@# port's build output lands under /opt/<name>/ (minus the
	@# embedfs-only *.test.json/*.manifest.json manifests, which mean
	@# nothing here); each *.nex among them ALSO gets a copy in /bin/,
	@# since FAT has no symlinks to alias one location to the other and
	@# nsh's $PATH only ever looks in /bin and /usr/tests.
	@echo "disk: PORT_DIRS=$(PORT_DIRS)"
	@for pair in $(PORT_DIRS); do \
		name=$${pair%%=*}; path=$${pair#*=}; \
		mmd -i $(DISK_IMG) "::opt/$$name" 2>/dev/null || true; \
		for f in "$$path"/*; do \
			base=$$(basename "$$f"); \
			case "$$base" in \
				*.test.json|*.manifest.json) continue ;; \
			esac; \
			mcopy -i $(DISK_IMG) "$$f" "::opt/$$name/$$base"; \
			case "$$base" in \
				*.nex) mcopy -i $(DISK_IMG) "$$f" "::bin/$$base" ;; \
			esac; \
		done; \
	done
	printf '%s\n' \
```

Note the `mmd ... || true`: `PORT_DIRS` is empty by default, so the
`for pair in $(PORT_DIRS)` loop runs zero times and this whole block
is a no-op — exactly the "byte-identical when unset" constraint. The
`|| true` on `mmd` guards a real re-run case (not the empty-loop case):
running `make disk-image` twice in a row without `clean` would try to
`mmd` a directory that already exists, which `mmd` treats as an error.

Add `PORT_DIRS ?=` right next to where `EMBED_DIRS ?=` is declared
(from Step 1), so both empty-by-default variables are visible
together:

```make
EMBED_DIRS ?=
PORT_DIRS ?=
```

- [ ] **Step 3: Verify the no-op case — existing gauntlet stays green**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
rm -f build/kernel.elf build/disk.img build/disk2.img build/neoos.iso
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
```

Expected: `PASS: no FAILED lines, boot reached the scheduler, all
suites reported` — identical to every prior run in this session, since
`PORT_DIRS` is unset here.

- [ ] **Step 4: Verify the port-install case manually**

Using the already-built `neoos-doom` checkout as the test port (its
`build/` already has `doom.nex`, `doom.test.json`, `doom1.wad` from
earlier in this session):

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/disk.img build/disk2.img
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    PORT_DIRS="doom=/home/neo/projects/personal/neoos-doom/build" disk-image
mdir -i build/disk.img ::opt/doom
mdir -i build/disk.img ::bin
```

Expected: `::opt/doom` lists `doom.nex` and `doom1.wad` (NOT
`doom.test.json`); `::bin` lists `doom.nex` alongside the existing
`term.nex`/`init.nex`/`nsh.nex`/`login.nex`.

- [ ] **Step 5: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add Makefile
git commit -m "build: PORT_DIRS installs ports as real disk files, not embedfs

Ports chosen for an OS build now land under /opt/<name>/ on the disk
image (plus a /bin/ copy of each .nex for nsh's \$PATH), installed by
a new PORT_DIRS variable in the disk-image recipe. embedfs/EMBED_DIRS
goes back to being the regression suite's mechanism only -- it never
had a path for a port's non-.nex data file (doom's doom1.wad) anyway,
and auto-launching a user-chosen port via an inittab entry was never
the right default.

PORT_DIRS defaults to empty; unset, this is a no-op and the
disk-image output is unchanged.

See docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 2: `neoos-doom` — default IWAD fallback

**Working directory:** `/home/neo/projects/personal/neoos-doom` (separate repo, on `main`)

**Files:**
- Modify: `neoos-shim/main.c`

**Interfaces:**
- Consumes: nothing from Task 1 (independent of it — this is a
  standalone argv-processing change; Task 4 is what proves the two
  fit together).
- Produces: `doom.nex`, when run with no `-iwad` anywhere in its argv,
  behaves as if `-iwad /opt/doom/doom1.wad` had been passed.

- [ ] **Step 1: Write the fallback**

`neoos-shim/main.c` currently reads:

```c
#include "doomgeneric.h"

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
```

Replace it with:

```c
#include "doomgeneric.h"
#include <string.h>

// If the caller didn't pass -iwad, default to the WAD this port
// installs at /opt/doom/doom1.wad when built as an os-builder port
// (see docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md
// section 7) -- so `doom` alone, typed at an nsh prompt after
// installing this port, just works without the user needing to know
// or type the WAD's path.
int main(int argc, char **argv) {
    int has_iwad = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-iwad") == 0) { has_iwad = 1; break; }
    }

    char *real_argv[argc + 3];   // + "-iwad" + the path + the NULL terminator
    int real_argc = 0;
    for (int i = 0; i < argc; i++) { real_argv[real_argc++] = argv[i]; }
    if (!has_iwad) {
        real_argv[real_argc++] = "-iwad";
        real_argv[real_argc++] = "/opt/doom/doom1.wad";
    }
    real_argv[real_argc] = 0;

    doomgeneric_Create(real_argc, real_argv);
    for (;;) {
        doomgeneric_Tick();
    }
    return 0;
}
```

- [ ] **Step 2: Build and smoke-test**

```bash
cd /home/neo/projects/personal/neoos-doom
make MUSL_DIR=../neoos-musl/build-output
./smoke-test.sh
```

Expected: builds clean (no new warnings — `string.h` is already used
by four other files in `neoos-shim/`, so no new include path is
needed), `smoke-test: OK -- ELF64 executable`.

- [ ] **Step 3: Verify the fallback logic directly**

There is no host runtime to run this binary in (freestanding musl
target, same constraint as `neoos-kernel` — no host-runnable unit
tests). Verify by reading the built binary's behavior is unreachable
to test on the host; instead confirm the logic itself with a tiny
host-side throwaway that mirrors the exact loop (not committed, just a
correctness check before trusting the in-kernel run in Task 4):

```bash
cat > /tmp/iwad_check.c <<'EOF'
#include <stdio.h>
#include <string.h>
int main(void) {
    // Case 1: no -iwad given.
    char *argv1[] = {"doom", 0};
    int argc1 = 1;
    int has_iwad = 0;
    for (int i = 1; i < argc1; i++) { if (strcmp(argv1[i], "-iwad") == 0) { has_iwad = 1; break; } }
    char *real_argv[argc1 + 3];
    int real_argc = 0;
    for (int i = 0; i < argc1; i++) { real_argv[real_argc++] = argv1[i]; }
    if (!has_iwad) { real_argv[real_argc++] = "-iwad"; real_argv[real_argc++] = "/opt/doom/doom1.wad"; }
    real_argv[real_argc] = 0;
    printf("case1 argc=%d: ", real_argc);
    for (int i = 0; i < real_argc; i++) printf("%s ", real_argv[i]);
    printf("\n");

    // Case 2: -iwad already given -- must be left alone.
    char *argv2[] = {"doom", "-iwad", "custom.wad", 0};
    int argc2 = 3;
    has_iwad = 0;
    for (int i = 1; i < argc2; i++) { if (strcmp(argv2[i], "-iwad") == 0) { has_iwad = 1; break; } }
    char *real_argv2[argc2 + 3];
    real_argc = 0;
    for (int i = 0; i < argc2; i++) { real_argv2[real_argc++] = argv2[i]; }
    if (!has_iwad) { real_argv2[real_argc++] = "-iwad"; real_argv2[real_argc++] = "/opt/doom/doom1.wad"; }
    real_argv2[real_argc] = 0;
    printf("case2 argc=%d: ", real_argc);
    for (int i = 0; i < real_argc; i++) printf("%s ", real_argv2[i]);
    printf("\n");
    return 0;
}
EOF
gcc /tmp/iwad_check.c -o /tmp/iwad_check && /tmp/iwad_check
rm /tmp/iwad_check.c /tmp/iwad_check
```

Expected output:
```
case1 argc=3: doom -iwad /opt/doom/doom1.wad
case2 argc=3: doom -iwad custom.wad
```

- [ ] **Step 4: Commit**

```bash
cd /home/neo/projects/personal/neoos-doom
git add neoos-shim/main.c
git commit -m "shim: default -iwad to /opt/doom/doom1.wad when not given

Matches the install convention from
docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md
(neoos-kernel repo) section 7 -- a user who installed this port via
os-builder can type \`doom\` alone instead of needing to know or spell
out the WAD's path.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
git push origin main
```

---

### Task 3: `neoos-os-builder` — build ports onto disk, not into embedfs

**Working directory:** `/home/neo/projects/personal/neoos-os-builder` (separate repo, on `main`; already cloned locally at this path for this session)

**Files:**
- Modify: `scripts/build.sh`
- Modify: `README.md`

**Interfaces:**
- Consumes: `PORT_DIRS` Make variable from Task 1 (`name=path` pairs,
  space-separated).
- Produces: nothing further downstream — this is the top of the chain.

- [ ] **Step 1: Change the port loop to build `PORT_DIRS`**

`scripts/build.sh` currently reads (lines 46, 57-69):

```sh
EMBED_DIRS=""

if [ "$TESTS_INCLUDE" = "true" ]; then
    echo "-- building neoos-kernel-tests-common (tests.include: true) --"
    git clone --depth 1 "$org/neoos-kernel-tests-common" "$WORK/neoos-kernel-tests-common" 2>&1 | tail -1
    (cd "$WORK/neoos-kernel-tests-common" && make \
        LIBNEOOS_DIR="$WORK/neoos-libneoos/build-output" \
        MUSL_DIR="$WORK/neoos-musl/build-output")
    EMBED_DIRS="$EMBED_DIRS $WORK/neoos-kernel-tests-common/build"
fi

for port in $PORTS; do
    echo "-- building neoos-$port --"
    git clone --depth 1 --recurse-submodules "$org/neoos-$port" "$WORK/$port" 2>&1 | tail -1
    (cd "$WORK/$port" && make MUSL_DIR="$WORK/neoos-musl/build-output")
    EMBED_DIRS="$EMBED_DIRS $WORK/$port/build"
done

echo "-- building neoos-kernel + assembling image (EMBED_DIRS:$EMBED_DIRS ) --"
(cd "$WORK/neoos-kernel" && make \
    LIBNEOOS_DIR="$WORK/neoos-libneoos/build-output" \
    MUSL_DIR="$WORK/neoos-musl/build-output" \
    EMBED_DIRS="$EMBED_DIRS" \
    iso disk-image)
```

Replace with (the `tests.include` block is untouched; only the port
loop and the final `make` invocation change):

```sh
EMBED_DIRS=""
PORT_DIRS=""

if [ "$TESTS_INCLUDE" = "true" ]; then
    echo "-- building neoos-kernel-tests-common (tests.include: true) --"
    git clone --depth 1 "$org/neoos-kernel-tests-common" "$WORK/neoos-kernel-tests-common" 2>&1 | tail -1
    (cd "$WORK/neoos-kernel-tests-common" && make \
        LIBNEOOS_DIR="$WORK/neoos-libneoos/build-output" \
        MUSL_DIR="$WORK/neoos-musl/build-output")
    EMBED_DIRS="$EMBED_DIRS $WORK/neoos-kernel-tests-common/build"
fi

# Ports install as real disk files, not via embedfs -- see
# docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md
# in neoos-kernel. EMBED_DIRS stays reserved for the regression suite
# above: a port is something a user chooses to have available, not
# something that should auto-run at boot.
for port in $PORTS; do
    echo "-- building neoos-$port --"
    git clone --depth 1 --recurse-submodules "$org/neoos-$port" "$WORK/$port" 2>&1 | tail -1
    (cd "$WORK/$port" && make MUSL_DIR="$WORK/neoos-musl/build-output")
    PORT_DIRS="$PORT_DIRS $port=$WORK/$port/build"
done

echo "-- building neoos-kernel + assembling image (EMBED_DIRS:$EMBED_DIRS PORT_DIRS:$PORT_DIRS) --"
(cd "$WORK/neoos-kernel" && make \
    LIBNEOOS_DIR="$WORK/neoos-libneoos/build-output" \
    MUSL_DIR="$WORK/neoos-musl/build-output" \
    EMBED_DIRS="$EMBED_DIRS" \
    PORT_DIRS="$PORT_DIRS" \
    iso disk-image)
```

- [ ] **Step 2: Update the README's port description**

`README.md:43-55` currently reads:

```markdown
ports:
  - busybox
  - 3d-ascii-viewer

iso:
  name: "neoos-custom-build"
```

`tests.include: true` additionally clones and builds
[neoos-kernel-tests-common](https://github.com/NeoOSOrganization/neoos-kernel-tests-common)
and adds its `build/` to the kernel's `EMBED_DIRS` — a production
image shouldn't carry the regression suite by default, but a
development or CI image can opt in.
```

Change the paragraph after the config block (leave the YAML block
itself unchanged — the config schema doesn't change, only what the
tool does with it):

```markdown
ports:
  - busybox
  - 3d-ascii-viewer

iso:
  name: "neoos-custom-build"
```

`tests.include: true` additionally clones and builds
[neoos-kernel-tests-common](https://github.com/NeoOSOrganization/neoos-kernel-tests-common)
and adds its `build/` to the kernel's `EMBED_DIRS` — a production
image shouldn't carry the regression suite by default, but a
development or CI image can opt in.

Each entry under `ports:` is installed as real files on the built
image, not baked into the kernel binary: a port's build output lands
under `/opt/<name>/` on disk, and each `.nex` binary it produces also
gets a copy in `/bin/` so it's directly runnable by name from an
`nsh` login shell (e.g. `doom`, `busybox`). Nothing auto-launches —
picking a port makes it *available*, the same way installing a
package on a real OS doesn't run it for you.
```

- [ ] **Step 3: Add a doom entry to the example config for manual testing**

`config/example.yaml`'s `ports:` list currently reads (confirmed by
reading the file):

```yaml
ports:
  - busybox
  - 3d-ascii-viewer
```

Change it to (this is what Task 4's end-to-end run builds against):

```yaml
ports:
  - busybox
  - 3d-ascii-viewer
  - doom
```

- [ ] **Step 4: Commit**

```bash
cd /home/neo/projects/personal/neoos-os-builder
git add scripts/build.sh README.md config/example.yaml
git commit -m "build: install ports as real disk files via PORT_DIRS, not embedfs

Matches neoos-kernel's new PORT_DIRS mechanism (see
docs/superpowers/specs/2026-09-06-os-builder-port-install-design.md
in that repo): a chosen port's build output lands under /opt/<name>/
on the built disk image, with a /bin/ copy of each .nex for PATH
lookup, launchable by hand from an nsh prompt. EMBED_DIRS is now only
ever used for tests.include's regression suite.

Also adds doom to config/example.yaml's ports list, exercising the
new path (including a port with a non-.nex data file, doom1.wad) in
every default build from here on.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
git push origin main
```

---

### Task 4: End-to-end verification

**Working directory:** `/home/neo/projects/personal/neoos-os-builder`

**Files:** none (verification only)

**Interfaces:**
- Consumes: Task 1's `PORT_DIRS` (via `neoos-kernel` on `main`,
  already pushed since this repo builds directly on `main`), Task 2's
  `neoos-doom` fallback (pushed to its `main`), Task 3's `build.sh`
  change (this repo, on `main` after Task 3's push).

This is the first REAL run of the whole chain together: os-builder
cloning neoos-kernel fresh from `main` (which now has `PORT_DIRS`),
cloning `neoos-doom` fresh from its `main` (which now has the IWAD
fallback), and assembling an image neither Task 1 nor Task 2's local
manual checks actually exercised together.

- [ ] **Step 1: Run the full build**

```bash
cd /home/neo/projects/personal/neoos-os-builder
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make CONFIG=config/example.yaml
```

Expected: completes with `OK: build/neoos-custom-build.iso` (or
whatever `iso.name` is in `config/example.yaml`), no `error:` lines.
This clones fresh copies of `neoos-kernel`, `neoos-libneoos`,
`neoos-musl`, `neoos-busybox`, `neoos-3d-ascii-viewer`, and
`neoos-doom` into a scratch `mktemp -d` directory and builds all of
them — expect this to take several minutes.

- [ ] **Step 2: Verify disk contents**

```bash
cd /home/neo/projects/personal/neoos-os-builder
mdir -i build/disk1.img ::opt/doom
mdir -i build/disk1.img ::opt/busybox
mdir -i build/disk1.img ::opt/3d-ascii-viewer
mdir -i build/disk1.img ::bin
```

Expected: `::opt/doom` lists `doom.nex` and `doom1.wad`; `::opt/busybox`
and `::opt/3d-ascii-viewer` each list their own `.nex`; `::bin` lists
`doom.nex`, `busybox.nex`, and whatever the 3d-ascii-viewer port's
binary is named, alongside the existing boot-critical four
(`init.nex`/`login.nex`/`term.nex`/`nsh.nex`).

- [ ] **Step 3: Boot it and run doom interactively**

```bash
cd /home/neo/projects/personal/neoos-os-builder/build
./qemu-run.sh
```

At the login prompt: log in as `neo`/`neo` (or `god`/`god`), then type
`doom` and press enter. Expected: the game starts with no `-iwad`
argument typed — Task 2's fallback finds `/opt/doom/doom1.wad` on its
own. Confirm visually (the title screen or a rendered level, the same
kind of confirmation as this session's earlier `doom-shot.png`) rather
than just "it didn't crash" — the point of this step is proving the
`/opt/doom/` convention actually resolves at runtime, not just that
the files exist on disk (Step 2 already proved that).

- [ ] **Step 4: Confirm `make test` (headless boot check) still passes**

```bash
cd /home/neo/projects/personal/neoos-os-builder
make CONFIG=config/example.yaml test
```

Expected: `TEST PASSED: image booted and reached the scheduler` — this
config now includes `doom` in `ports:`, so this also proves a
port-installed image still boots clean headless (no port-related boot
regression), not just that Doom itself runs interactively.

No commit for this task — it's verification only, spanning artifacts
already committed in Tasks 1-3.
