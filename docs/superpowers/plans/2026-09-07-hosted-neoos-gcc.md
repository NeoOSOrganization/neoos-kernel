# Hosted x86_64-neoos-linux-musl GCC/G++ Toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a persistent, hosted `x86_64-neoos-linux-musl` GCC/G++ cross-toolchain (real `neoos-musl` libc, real CRT startup, real C++ exception unwinding) installed at `~/opt/cross-x86_64-neoos`, proven via a real `throw`/`catch` C++ program booting on NeoOS.

**Architecture:** `musl-cross-make`'s standard three-stage bootstrap (binutils → freestanding stage-1 GCC → target libc → full stage-2 GCC/G++), with ONE substitution: the libc stage builds from real musl v1.2.5 sources patched via generated diffs mirroring `NeoOS/third_party/shim/*` exactly, instead of stock musl — so the toolchain's bundled libc is genuinely NeoOS's own patched musl.

**Tech Stack:** `musl-cross-make` (GCC 9.4.0/binutils 2.44, its own tested defaults), musl 1.2.5.

**Spec:** `docs/superpowers/specs/2026-09-07-hosted-neoos-gcc-design.md`

## Global Constraints

- Target triple: `x86_64-neoos-linux-musl` (new, not a variant of the existing bare-metal `x86_64-elf` triple).
- `MUSL_VER = 1.2.5`, matching `neoos-musl/upstream`'s own pinned tag exactly (verified: `git describe --tags` → `v1.2.5`; a tarball hash for `1.2.5` already exists in `musl-cross-make/hashes/`, so no `git-` version override is needed).
- Shim patches applied via `musl-cross-make`'s own `patches/musl-1.2.5/*.diff` mechanism (`cowpatch.sh -p1`, i.e. standard unified diffs, one leading path component stripped) — never hand-edited inside the fetched musl source tree.
- The shim's source of truth stays `NeoOS/third_party/shim/*` — this repo's patches are a generated, regeneratable reflection of it, not a second copy to maintain by hand.
- Compiled toolchain installs to `~/opt/cross-x86_64-neoos` (host-local, not committed to any repo) — alongside, not replacing, `~/opt/cross-x86_64-elf`.
- New repo: `NeoOSOrganization/neoos-hosted-gcc`, cloned to `/home/neo/projects/personal/neoos-hosted-gcc`.
- Boot-verified only, via QEMU + serial log capture, no host-runnable unit tests for the on-NeoOS half.

---

### Task 1: Repo scaffold, generated patches, and a dry-run proof they apply cleanly

**Files (new repo `neoos-hosted-gcc`, cloned to `/home/neo/projects/personal/neoos-hosted-gcc`):**
- Create: `README.md`
- Create: `.gitignore` (ignores `mcm/` — the vendored `musl-cross-make` checkout, and `build.log`)
- Create: `config.mak` (musl-cross-make's config, copied into `mcm/` at build time)
- Create: `patches/musl-1.2.5/001-syscall_arch.diff` through `008-neoos_syscall.diff` (8 files)
- Create: `regen-patches.sh` (regenerates the 8 diffs above from a fresh musl v1.2.5 checkout + the current `NeoOS/third_party/shim/*`, for whenever the shim changes)
- Create: `build.sh` (clones `musl-cross-make`, copies in `config.mak` + `patches/`, runs the build)

**Interfaces:**
- Produces: `patches/musl-1.2.5/*.diff` — consumed by Task 2's actual bootstrap build (via `build.sh` copying them into the vendored `musl-cross-make` checkout's own `patches/` directory before `make`).

- [ ] **Step 1: Create and clone the repo**

```bash
cd /home/neo/projects/personal
gh repo create NeoOSOrganization/neoos-hosted-gcc --public \
    --description "Hosted x86_64-neoos-linux-musl GCC/G++ toolchain for NeoOS"
git clone git@github.com:NeoOSOrganization/neoos-hosted-gcc.git
cd neoos-hosted-gcc
```

- [ ] **Step 2: Generate the 8 patch files from the shim's existing `.orig` pairs**

The shim's `.orig` backup files, already present in `neoos-musl/upstream` from earlier builds this session, ARE the pristine musl v1.2.5 originals by `third_party/shim/apply.sh`'s own design (`[ -f "$dst.orig" ] || cp "$dst" "$dst.orig"` — copied once, before ever being overwritten). Generating diffs directly from these pairs is exactly as correct as diffing against a fresh checkout, and doesn't require one:

```bash
cd /home/neo/projects/personal/neoos-hosted-gcc
mkdir -p patches/musl-1.2.5
NM=/home/neo/projects/personal/neoos-musl/upstream

diff -u "$NM/arch/x86_64/syscall_arch.h.orig" "$NM/arch/x86_64/syscall_arch.h" \
    | sed "s|$NM/arch/x86_64/syscall_arch.h.orig|a/arch/x86_64/syscall_arch.h|; s|$NM/arch/x86_64/syscall_arch.h|b/arch/x86_64/syscall_arch.h|" \
    > patches/musl-1.2.5/001-syscall_arch.diff

diff -u "$NM/src/thread/x86_64/syscall_cp.s.orig" "$NM/src/thread/x86_64/syscall_cp.s" \
    | sed "s|$NM/src/thread/x86_64/syscall_cp.s.orig|a/src/thread/x86_64/syscall_cp.s|; s|$NM/src/thread/x86_64/syscall_cp.s|b/src/thread/x86_64/syscall_cp.s|" \
    > patches/musl-1.2.5/002-syscall_cp.diff

diff -u "$NM/src/thread/x86_64/__set_thread_area.s.orig" "$NM/src/thread/x86_64/__set_thread_area.s" \
    | sed "s|$NM/src/thread/x86_64/__set_thread_area.s.orig|a/src/thread/x86_64/__set_thread_area.s|; s|$NM/src/thread/x86_64/__set_thread_area.s|b/src/thread/x86_64/__set_thread_area.s|" \
    > patches/musl-1.2.5/003-set_thread_area.diff

diff -u "$NM/src/thread/x86_64/__unmapself.s.orig" "$NM/src/thread/x86_64/__unmapself.s" \
    | sed "s|$NM/src/thread/x86_64/__unmapself.s.orig|a/src/thread/x86_64/__unmapself.s|; s|$NM/src/thread/x86_64/__unmapself.s|b/src/thread/x86_64/__unmapself.s|" \
    > patches/musl-1.2.5/004-unmapself.diff

diff -u "$NM/src/thread/x86_64/clone.s.orig" "$NM/src/thread/x86_64/clone.s" \
    | sed "s|$NM/src/thread/x86_64/clone.s.orig|a/src/thread/x86_64/clone.s|; s|$NM/src/thread/x86_64/clone.s|b/src/thread/x86_64/clone.s|" \
    > patches/musl-1.2.5/005-clone.diff

diff -u "$NM/src/signal/x86_64/restore.s.orig" "$NM/src/signal/x86_64/restore.s" \
    | sed "s|$NM/src/signal/x86_64/restore.s.orig|a/src/signal/x86_64/restore.s|; s|$NM/src/signal/x86_64/restore.s|b/src/signal/x86_64/restore.s|" \
    > patches/musl-1.2.5/006-restore.diff

diff -u "$NM/src/process/x86_64/vfork.s.orig" "$NM/src/process/x86_64/vfork.s" \
    | sed "s|$NM/src/process/x86_64/vfork.s.orig|a/src/process/x86_64/vfork.s|; s|$NM/src/process/x86_64/vfork.s|b/src/process/x86_64/vfork.s|" \
    > patches/musl-1.2.5/007-vfork.diff

diff -u /dev/null "$NM/src/internal/neoos_syscall.c" \
    | sed "s|$NM/src/internal/neoos_syscall.c|b/src/internal/neoos_syscall.c|" \
    > patches/musl-1.2.5/008-neoos_syscall.diff

wc -l patches/musl-1.2.5/*.diff
```

Expected: 8 non-empty `.diff` files (the `008-` one is the largest — a whole-file addition).

- [ ] **Step 3: Write `regen-patches.sh`, for when the shim changes later**

```bash
#!/bin/bash
set -e
# Regenerates patches/musl-1.2.5/*.diff from a FRESH musl v1.2.5 checkout
# and the CURRENT third_party/shim/* in the NeoOS kernel repo -- run this
# whenever that shim changes. (Task 1's own patches were generated from
# the shim's pre-existing .orig pairs instead, which is equivalent but
# doesn't need a fresh checkout when those pairs already exist.)
SHIM_DIR="${SHIM_DIR:-../NeoOS/third_party/shim}"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

git clone --depth 1 --branch v1.2.5 https://git.musl-libc.org/git/musl "$WORK/musl"
cp "$WORK/musl/arch/x86_64/syscall_arch.h" "$WORK/musl/arch/x86_64/syscall_arch.h.pristine"
cp "$WORK/musl/src/thread/x86_64/syscall_cp.s" "$WORK/musl/src/thread/x86_64/syscall_cp.s.pristine"
cp "$WORK/musl/src/thread/x86_64/__set_thread_area.s" "$WORK/musl/src/thread/x86_64/__set_thread_area.s.pristine"
cp "$WORK/musl/src/thread/x86_64/__unmapself.s" "$WORK/musl/src/thread/x86_64/__unmapself.s.pristine"
cp "$WORK/musl/src/thread/x86_64/clone.s" "$WORK/musl/src/thread/x86_64/clone.s.pristine"
cp "$WORK/musl/src/signal/x86_64/restore.s" "$WORK/musl/src/signal/x86_64/restore.s.pristine"
cp "$WORK/musl/src/process/x86_64/vfork.s" "$WORK/musl/src/process/x86_64/vfork.s.pristine"

cp "$SHIM_DIR/syscall_arch.h" "$WORK/musl/arch/x86_64/syscall_arch.h"
cp "$SHIM_DIR/syscall_cp.s" "$WORK/musl/src/thread/x86_64/syscall_cp.s"
cp "$SHIM_DIR/__set_thread_area.s" "$WORK/musl/src/thread/x86_64/__set_thread_area.s"
cp "$SHIM_DIR/__unmapself.s" "$WORK/musl/src/thread/x86_64/__unmapself.s"
cp "$SHIM_DIR/clone.s" "$WORK/musl/src/thread/x86_64/clone.s"
cp "$SHIM_DIR/restore.s" "$WORK/musl/src/signal/x86_64/restore.s"
cp "$SHIM_DIR/vfork.s" "$WORK/musl/src/process/x86_64/vfork.s"

mkdir -p patches/musl-1.2.5
cd "$WORK/musl"
diff -u arch/x86_64/syscall_arch.h.pristine arch/x86_64/syscall_arch.h \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/001-syscall_arch.diff"
diff -u src/thread/x86_64/syscall_cp.s.pristine src/thread/x86_64/syscall_cp.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/002-syscall_cp.diff"
diff -u src/thread/x86_64/__set_thread_area.s.pristine src/thread/x86_64/__set_thread_area.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/003-set_thread_area.diff"
diff -u src/thread/x86_64/__unmapself.s.pristine src/thread/x86_64/__unmapself.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/004-unmapself.diff"
diff -u src/thread/x86_64/clone.s.pristine src/thread/x86_64/clone.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/005-clone.diff"
diff -u src/signal/x86_64/restore.s.pristine src/signal/x86_64/restore.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/006-restore.diff"
diff -u src/process/x86_64/vfork.s.pristine src/process/x86_64/vfork.s \
    | sed 's|\.pristine||' > "$OLDPWD/patches/musl-1.2.5/007-vfork.diff"
diff -u /dev/null "$SHIM_DIR/neoos_syscall.c" \
    | sed 's|.*neoos_syscall.c|b/src/internal/neoos_syscall.c|' \
    > "$OLDPWD/patches/musl-1.2.5/008-neoos_syscall.diff"
echo "Regenerated patches/musl-1.2.5/*.diff"
```

```bash
chmod +x regen-patches.sh
```

- [ ] **Step 4: Write `config.mak`**

```make
TARGET = x86_64-neoos-linux-musl
OUTPUT = /home/neo/opt/cross-x86_64-neoos
MUSL_VER = 1.2.5
```

- [ ] **Step 5: Write `build.sh`**

```bash
#!/bin/bash
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MCM_DIR="${MCM_DIR:-$HERE/mcm}"

if [ ! -d "$MCM_DIR" ]; then
    git clone https://github.com/richfelker/musl-cross-make "$MCM_DIR"
fi

cp "$HERE/config.mak" "$MCM_DIR/config.mak"
rm -rf "$MCM_DIR/patches/musl-1.2.5"
mkdir -p "$MCM_DIR/patches"
cp -r "$HERE/patches/musl-1.2.5" "$MCM_DIR/patches/musl-1.2.5"

cd "$MCM_DIR"
make -j"$(nproc)" 2>&1 | tee "$HERE/build.log"
make install 2>&1 | tee -a "$HERE/build.log"

if [ -x "/home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-gcc" ]; then
    echo ""
    echo "OK hosted toolchain built successfully at /home/neo/opt/cross-x86_64-neoos"
else
    echo "ERROR: build finished but x86_64-neoos-linux-musl-gcc not found" >&2
    exit 1
fi
```

```bash
chmod +x build.sh
```

- [ ] **Step 6: Dry-run proof the patches apply cleanly — BEFORE the multi-hour build**

This is the cheap, fast check that catches a malformed diff in seconds
instead of discovering it after a 30-90 minute build reaches the musl
stage:

```bash
cd /home/neo/projects/personal
git clone --depth 1 --branch v1.2.5 https://git.musl-libc.org/git/musl /tmp/musl-dryrun
cd /tmp/musl-dryrun
for p in /home/neo/projects/personal/neoos-hosted-gcc/patches/musl-1.2.5/*.diff; do
    echo "=== $p ==="
    patch -p1 --dry-run < "$p"
done
rm -rf /tmp/musl-dryrun
```

Expected: every patch reports `patching file ...` with no `FAILED`/
`rejected` lines — for `008-neoos_syscall.diff`, `patching file
src/internal/neoos_syscall.c` (a brand-new file being created).

- [ ] **Step 7: Commit**

```bash
cd /home/neo/projects/personal/neoos-hosted-gcc
git add config.mak patches/ regen-patches.sh build.sh README.md .gitignore
git commit -m "scaffold: config.mak, generated musl shim patches, build.sh"
git push origin main
```

---

### Task 2: Run the bootstrap build, verify the toolchain smoke test

**Files:** none new — this task runs Task 1's `build.sh`.

**Interfaces:**
- Consumes: `neoos-hosted-gcc/build.sh` (Task 1).
- Produces: `~/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-{gcc,g++}` — consumed by Task 3 and Task 4.

- [ ] **Step 1: Kick off the build in the background**

This takes on the order of 30-90+ minutes (a real binutils+GCC C/C++
bootstrap). Run it backgrounded, not as a blocking foreground call:

```bash
cd /home/neo/projects/personal/neoos-hosted-gcc
./build.sh > build-run.log 2>&1
```

Poll `build-run.log`'s tail periodically rather than waiting
synchronously; the three real stage transitions to watch for in the
log are (in order): binutils's `make install` output, the stage-1
(`GCC_STAGE=1`, freestanding) compiler completing, musl's own
`configure`/`make install` (this is where a bad patch would surface,
already ruled out by Task 1 Step 6's dry run), and finally the full
stage-2 GCC/G++ `make install`.

- [ ] **Step 2: Toolchain smoke test**

```bash
export PATH="/home/neo/opt/cross-x86_64-neoos/bin:$PATH"
x86_64-neoos-linux-musl-gcc --version
x86_64-neoos-linux-musl-g++ --version
x86_64-neoos-linux-musl-gcc -print-file-name=crtbeginT.o
x86_64-neoos-linux-musl-gcc -print-file-name=libgcc_eh.a
```

Expected: both `--version` calls print real version strings (not
"command not found"); both `-print-file-name` calls print real
absolute paths ending in `crtbeginT.o`/`libgcc_eh.a` — NOT the bare
filename echoed back unchanged, which is GCC's way of saying "I could
not find this."

- [ ] **Step 3: Report**

No commit needed for this task (it produces host-local binaries, not
repo content) — proceed to Task 3's boot verification.

---

### Task 3: Real hosted C hello world on NeoOS

**Files (scratch, this session's scratchpad, not committed):**
- Create: `hosted_hello.c`

**Interfaces:**
- Consumes: `x86_64-neoos-linux-musl-gcc` (Task 2).
- Produces: nothing later tasks consume — this is this task's own boot proof.

- [ ] **Step 1: Write the test program**

```c
// hosted_hello.c -- proves the toolchain's bundled neoos-musl sysroot
// is wired correctly, linked the NORMAL hosted way (no manual
// -nostdlib/explicit crt1.o juggling -- that is the whole point of a
// hosted toolchain).
#include <stdio.h>

int main(void) {
    printf("hosted hello from neoos-neoos-musl-gcc\n");
    return 0;
}
```

- [ ] **Step 2: Build it the normal hosted way**

```bash
export PATH="/home/neo/opt/cross-x86_64-neoos/bin:$PATH"
SCRATCH=<this session's scratchpad>/hosted-gcc-probe
mkdir -p "$SCRATCH"
cp hosted_hello.c "$SCRATCH/"
cd "$SCRATCH"
x86_64-neoos-linux-musl-gcc -static hosted_hello.c -o hosted_hello.elf
```

Expected: links with no `-nostdlib`/`-T user.ld`/explicit `crt1.o` —
this toolchain's own bundled sysroot supplies everything.

- [ ] **Step 3: Nexify, embed, boot**

```bash
cd /home/neo/projects/personal/NeoOS
export PATH="/home/neo/opt/cross-x86_64-elf/bin:$PATH"
./tools/nexify.sh "$SCRATCH/hosted_hello.elf" "$SCRATCH/hosted_hello.nex"
echo '{"category":"bin"}' > "$SCRATCH/hosted_hello.test.json"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="$SCRATCH" iso disk-image
printf 'wait /bin/hosted_hello.nex\n' > /tmp/hosted-hello-inittab
mcopy -o -i build/disk.img /tmp/hosted-hello-inittab ::etc/inittab
timeout 20 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -no-reboot -display none -serial file:build/hosted-hello-test.log
grep -i "hosted hello\|panic\|exception\|halted\|fault" build/hosted-hello-test.log
```

Expected: `hosted hello from neoos-neoos-musl-gcc`, no
panic/exception/halted line, `nexify.sh` reports success on a binary
built via `x86_64-neoos-linux-musl-gcc` (note: `nexify.sh` and `x86_64-elf-`
tools are still used for the disk-image/ELF-to-`.nex` conversion step
only, not for compiling — the ELF this task boots was compiled
entirely by the new hosted toolchain).

---

### Task 4: Real C++ exceptions on NeoOS, docs, and regression

**Files (scratch, not committed):**
- Create: `cpp_exceptions_test.cpp`

**Modify:**
- `docs/stdlib.md` or a new `docs/toolchains.md` — whichever this repo
  already uses for host-tooling documentation (check for an existing
  section on `x86_64-elf-gcc` first and follow its placement).
- `docs/abi-compatibility.md` — not applicable (no kernel/ABI change);
  skip unless review finds a relevant section to note this in.

**Interfaces:**
- Consumes: `x86_64-neoos-linux-musl-g++` (Task 2).
- Produces: nothing later tasks consume — this milestone's final proof.

- [ ] **Step 1: Write the test program**

```cpp
// cpp_exceptions_test.cpp -- proves real C++ exception unwinding
// (crtbeginT.o + libgcc_eh.a, this toolchain's whole reason for
// existing) works against real neoos-musl on real NeoOS.
#include <cstdio>
#include <stdexcept>

static int risky(int x) {
    if (x < 0) {
        throw std::runtime_error("negative input");
    }
    return x * 2;
}

int main() {
    try {
        int r = risky(5);
        printf("risky(5) = %d\n", r);
        r = risky(-1);
        printf("unreachable: risky(-1) = %d\n", r);
    } catch (const std::exception& e) {
        printf("caught exception: %s\n", e.what());
    }
    printf("cpptest PASSED\n");
    return 0;
}
```

- [ ] **Step 2: Build, nexify, embed, boot**

```bash
export PATH="/home/neo/opt/cross-x86_64-neoos/bin:$PATH"
SCRATCH=<this session's scratchpad>/hosted-gcc-probe
cp cpp_exceptions_test.cpp "$SCRATCH/"
cd "$SCRATCH"
x86_64-neoos-linux-musl-g++ -static cpp_exceptions_test.cpp -o cpp_exceptions_test.elf

cd /home/neo/projects/personal/NeoOS
export PATH="/home/neo/opt/cross-x86_64-elf/bin:$PATH"
rm -f "$SCRATCH/hosted_hello.nex" "$SCRATCH/hosted_hello.test.json"
./tools/nexify.sh "$SCRATCH/cpp_exceptions_test.elf" "$SCRATCH/cpp_exceptions_test.nex"
echo '{"category":"bin"}' > "$SCRATCH/cpp_exceptions_test.test.json"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="$SCRATCH" iso disk-image
printf 'wait /bin/cpp_exceptions_test.nex\n' > /tmp/cpp-inittab
mcopy -o -i build/disk.img /tmp/cpp-inittab ::etc/inittab
timeout 20 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -no-reboot -display none -serial file:build/cpp-exceptions-test.log
grep -i "risky\|caught exception\|cpptest\|panic\|halted\|fault" build/cpp-exceptions-test.log
```

Expected: `risky(5) = 10`, `caught exception: negative input`,
`cpptest PASSED` — real stack unwinding across a real `throw`, no
panic/exception/halted line from the kernel itself.

- [ ] **Step 3: Full gauntlet regression**

```bash
cd /home/neo/projects/personal/NeoOS
export PATH="/home/neo/opt/cross-x86_64-elf/bin:$PATH"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output iso disk-image
timeout 580 tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`, zero retries — this milestone
touches no kernel code and no shared `neoos-musl` code (the generated
patches mirror the existing shim read-only; they are not edits to it).

- [ ] **Step 4: Document the new toolchain**

Check `docs/` for wherever `x86_64-elf-gcc`'s own installation is
documented (likely `docs/stdlib.md`'s intro or a dedicated toolchain
doc, or `CLAUDE.md` itself) and add a parallel entry for
`x86_64-neoos-linux-musl-gcc`/`g++`: what it is, where it lives
(`~/opt/cross-x86_64-neoos`), what it's for (real hosted C/C++,
real exceptions — contrast with the freestanding `x86_64-elf-gcc`
used for every existing port), and which repo owns its build recipe
(`neoos-hosted-gcc`).

- [ ] **Step 5: Commit and push**

```bash
cd /home/neo/projects/personal/NeoOS
git add docs/  # whichever file Step 4 touched
git commit -m "docs: document the hosted x86_64-neoos-linux-musl toolchain"
git push origin main

cd /home/neo/projects/personal/neoos-hosted-gcc
git status --short   # expect: empty (Task 1 already pushed everything)
```

- [ ] **Step 6: Report completion**

Summarize: the hosted toolchain exists and works (real C hello world,
real C++ exceptions, both booting on NeoOS), installed persistently at
`~/opt/cross-x86_64-neoos`, recipe committed to `neoos-hosted-gcc`.
Note that retrying the dotnet NativeAOT static link against this new
toolchain (the original trigger for this whole milestone) is the
natural next step, now unblocked.

## Self-Review Notes

- **Spec coverage**: the design doc's Architecture section (target
  triple, `musl-cross-make` + patches substitution, repo/install
  layout, cost) maps to Task 1 (scaffold + patches) and Task 2 (the
  actual build). The Testing Plan's four items map 1:1 to Task 2 Step
  2 (smoke test), Task 3 (C hello world), Task 4 Steps 1-2 (C++
  exceptions), and Task 4 Step 3 (gauntlet).
- **Placeholder scan**: every code/script block is complete and
  specific — the `sed` path-rewriting in Task 1 Step 2 was verified
  against `musl-cross-make`'s real `cowpatch.sh -p1` convention
  (patches use `a/`/`b/` prefixes, one component stripped), not
  guessed.
- **Type/name consistency**: `x86_64-neoos-linux-musl` (the target triple)
  and `/home/neo/opt/cross-x86_64-neoos` (the install path) are used
  identically in `config.mak`, `build.sh`, and every later task's
  `PATH` export.

## Post-implementation notes (real-world deviations from the plan)

Recorded here rather than rewritten into the tasks above, since the
plan's own steps were followed faithfully -- these are things learned
DURING execution that the plan (reasonably) couldn't have anticipated:

- **`TARGET = x86_64-neoos-musl` is invalid.** GNU `config.sub`
  requires a recognized kernel component before a libc suffix like
  `musl` -- fixed to `x86_64-neoos-linux-musl` (vendor slot = `neoos`,
  kernel = `linux`, accurate: NeoOS's ABI genuinely is Linux-shaped).
- **`build.sh` needed `set -o pipefail`.** Without it, `make | tee`
  always exits 0 regardless of `make`'s real exit code, so a failed
  build silently proceeded to `make install`.
- **The shim's `.s` files needed `#` comments, not `//`.** A freshly
  built binutils rejected `//` as junk after a register operand in
  every shimmed `.s` file (not run through cpp the way `.S` files are,
  so the raw assembler must understand the comment syntax itself).
  `x86_64-elf-gcc`'s bundled assembler tolerating `//` turned out to be
  that one assembler's own leniency, not a portable guarantee. Fixed
  in `NeoOS/third_party/shim/*.s` (the canonical source), verified no
  regression against the existing `x86_64-elf-gcc` build + full
  gauntlet.
- **`-mno-red-zone` is required, and matters even for already-compiled
  library code.** `musl-cross-make`'s own musl build uses upstream's
  default CFLAGS (no red-zone protection); NeoOS's interrupt/syscall
  entry does not preserve the red zone. Added via `MUSL_CONFIG +=
  CFLAGS="-mno-red-zone ..."` and `GCC_CONFIG += CFLAGS_FOR_TARGET="
  -mno-red-zone ..."` (the latter for libgcc/libgcc_eh) -- a flag
  added only to future user-code compiles would leave musl's own
  already-compiled `.o` files inside `libc.a` unprotected.
- **`-mcmodel=large -fno-pic` are required for the same reason**:
  `user.ld` places NeoOS user code at `0x200000000000` (PML4 slot 64,
  deliberately not slot 0 -- see the file's own comment), far beyond
  what 32-bit-relative relocations (small/medium code model) can
  reach. Diagnosed via a real, hard debugging session: a binary linked
  WITHOUT `-T user.ld` landed at the small-model default `0x400000`
  and segfaulted on its very first instruction (`v=0e` page fault,
  error code showing instruction-fetch + protection-violation) because
  the intermediate PML4/PDPT/PD page-table entries covering that
  address range lack the User bit (x86-64 paging requires it set at
  EVERY level of the walk, not just the final PTE) -- `0x400000` was
  simply never meant to host NeoOS user code at all. Once linked
  correctly with `-T user.ld`, that binary needed `-mcmodel=large`
  too, for the reason above.
- **GCC's C++ exception handling and `-mcmodel=large` do not reliably
  coexist on x86-64** -- a documented, longstanding GCC/binutils
  limitation (`.eh_frame`'s PC-relative range encoding can't represent
  addresses as far apart as the large code model permits). A real
  `throw`/`catch` test using `std::runtime_error` also hit a separate,
  unrelated linker issue first (GCC's transactional-memory "clone"
  symbols, always emitted by `libstdc++`'s precompiled exception
  classes regardless of whether `libitm` is built) -- worked around by
  testing with a minimal custom exception type instead, which still
  isolated the REAL remaining issue: the `throw` itself reaches
  `abort()` instead of unwinding to the `catch`. **This is a live,
  unresolved limitation of this toolchain as configured** -- C++
  exceptions cannot be relied on for any future NeoOS port built with
  it. C code (no exceptions) is unaffected and fully proven working.
- **NativeAOT retry (the original trigger for this whole milestone):
  tried, with real, significant progress and one real, unresolved
  gap.** Using `-p:CppCompilerAndLinker=x86_64-neoos-linux-musl-gcc`
  (this toolchain) plus `-p:StaticExecutable=true
  -p:PositionIndependentExecutable=false` and a `-T user.ld`/
  `--no-relax` injection via a `<ExtraLinkerArg>` `ItemGroup` in the
  `.csproj`, `dotnet publish -p:PublishAot=true` for `linux-musl-x64`
  produced a genuinely well-formed static NeoOS binary from real C#
  source: correct ELF type (`EXEC`, not `DYN`), correct entry point
  address (`0x200000001000`, inside `user.ld`'s required range),
  correctly linked against NeoOS's own `neoos-musl` and this
  toolchain's own `crtbeginT.o`/`libgcc_eh.a` (needed one additional
  small static dependency built fresh against this toolchain: `libz.a`
  for `libSystem.IO.Compression.Native.a`, since neither `neoos-musl`
  nor a generic musl-cross-make build ships one). **This is a genuine
  result** -- the link-level story for statically linking a NativeAOT
  C# binary against a from-scratch OS now works.
  **What does NOT yet work: the binary crashes on startup** (SIGSEGV,
  `CR2=0x500000000008` -- a tagged/poisoned-pointer address pattern
  typical of CoreCLR's own GC/type-system bootstrap sequence hitting
  something not yet initialized the way it expects on a real Linux
  host). This is CoreCLR's native PAL (`libSystem.Native.a` and the GC
  runtime's own startup path) expecting OS behaviors/syscalls beyond
  what `neoos-musl`'s translation currently provides or what NeoOS's
  process-startup contract currently guarantees -- porting that
  startup sequence to a genuinely novel OS is its own substantial,
  open-ended effort (comparable in scope to a real CoreCLR PAL port),
  not a quick follow-up fix. Left for a dedicated future milestone
  with its own brainstorm/spec if pursued -- the interpreter-based
  `neoos-dotnet-clr` design (`docs/superpowers/specs/
  2026-09-07-neoos-dotnet-clr-design.md`) remains the lower-risk path
  to a working "C# on NeoOS" in the meantime, since it depends on none
  of CoreCLR's runtime internals.

## Follow-up: real kernel/linker fixes found chasing the NativeAOT crash further

Continuing the investigation above turned up two genuinely real bugs
and fixed both, committed to `neoos-kernel` (`1bec67e`, `60ea0fa`):

1. **`elf_load()` never registered a VMA for the process's own loaded
   image.** Every PT_LOAD segment was mapped via raw `paging_map_into()`
   calls, bypassing `vma_insert()` entirely -- meaning a process's own
   code/data had page-table entries but no `vma_find()`-visible record,
   so a later `mprotect()`/`munmap()` against its own image silently
   no-op'd instead of taking effect. No prior program in this codebase
   had ever called `mprotect()` on its own image (only on memory it
   `mmap()`'d itself) until this NativeAOT binary did. Fixed via a new
   `vma_register_image_segment()`, `elf_info` extended with a
   `segments[]`/`num_segments` array, called from both `spawn_argv()`
   and `exec_task()`.
2. **A bounded W^X exception.** dotnet NativeAOT's unboxing stubs
   (`__unbox`) are genuinely both executable and self-patched at
   runtime with zero `mprotect()` calls (confirmed by instrumenting
   `sys_mprotect` directly). Real Linux allows this by default (W^X
   lockdown is opt-in hardening there too). Accepted, capped at 16MiB
   (`ELF_WX_EXCEPTION_MAX_BYTES`) so this stays "one legitimate
   runtime's small stub region," not a general RWX amnesty -- a
   deliberate policy relaxation, made only after asking and getting
   an explicit answer, not unilaterally.

Alongside these, `userland/user.ld` (`60ea0fa`) now explicitly routes
`.init`/`.fini` (NativeAOT's 1-byte placeholders, also written to at
runtime with no `mprotect()`) and `__unbox` into the writable `.data`
output section — GNU ld's orphan-section placement had grouped them
with `.text` (read+exec only) since `user.ld` had no rule for them.

With both fixes above in place, the original page fault at the
`.init` boundary was gone, but a **different, later** fault appeared:
a `#GP` (vector 13) inside `.data`'s own address range
(`RIP=0x20000015e00e`, 14 bytes past `.data`'s start). The original
theory here — that `libbootstrapper.o`'s `InitializeRuntime()` writes
into a `.init` placeholder byte at runtime — was **wrong**, and was
disproven directly: disassembling `main()` (which inlines
`InitializeRuntime()`) shows it only *reads* the `__start___unbox`/
`__stop___unbox` boundary symbols (to hand the range to
`RhRegisterOSModule`), and never references `.init`/`.fini` at all.

The real mechanism, found via `-Wl,-Map`: `.init`/`.fini` are each
assembled from **multiple objects' contributions concatenated**
(`crti.o`'s one-byte opening, defining the `_init`/`_fini` symbols,
followed by `crtn.o`'s one-byte closing `pop %rax; ret`) — the normal
musl/GCC crt convention. NativeAOT's own link line passes
`-Wl,--gc-sections`, and `crtn.o`'s closing fragment is anonymous
(nothing relocates to it, only `crti.o`'s opening byte is reachable
via the `_init`/`_fini` symbols) — so gc-sections silently **dropped
it**. `_init` was left as a bare `push %rax` with no `ret`; musl's
crt1 called it, execution fell through into whatever bytes followed
in `.data`, and eventually hit one that happened to decode as `hlt`
— a ring-3 `#GP` (`hlt` is privileged). Fixed in `userland/user.ld` by
wrapping both in `KEEP()` (`*(.init .fini)` → `KEEP(*(.init))
KEEP(*(.fini))`), which marks them as GC roots gc-sections cannot
drop even though nothing in the retained relocation graph points at
them. Confirmed via the same `-Wl,-Map`: crtn.o's fragments now land
at their expected addresses, and the assembled bytes are
`push %rax; pop %rax; ret` for both `_init` and `_fini`.

With that fixed, the `#GP` was gone, but the binary still crashed —
this time with a plain `SIGSEGV` (`wait4` status `0xb`) and no
interrupt-trace evidence pointing at corrupted code, only a `#PF`
(not-present, write, user) at `cr2` about 1.5MiB below the main
thread's stack top, which `vma_fault()` correctly refused (no VMA
covers it — NeoOS's stack has no grow-down auto-extension; a miss
there is just `SIGSEGV` by design). Root cause: `USER_STACK_PAGES`
(`kernel/sched/proc.h`) was **4 pages — 16KB** — sized for NeoOS's own
small hand-written test binaries, and CoreCLR's startup (RhInitialize
/ GC init / the JIT-less interpreter's own C++ call depth) blew
through it in one large stack allocation that jumped straight past
the single guard page (no incremental stack probing touched it on the
way down) into genuinely unmapped memory. Raised to 2048 pages
(8MiB), matching Linux's typical default main-thread stack size. This
only affects a process's main thread and NeoOS's native
`thread_create()` path — a `pthread_create()`'d thread (`sys_clone`)
uses the caller's own musl-`mmap()`'d stack, a normal lazily-faulted
VMA, untouched by this constant.

With all three fixes in place (the two above plus the VMA/W^X pair
from earlier in this section), the hello-world binary ran to
completion with a clean exit and no signal — but exit code `-1`,
matching exactly `main()`'s own disassembled failure path
(`RhInitialize`/`RhRegisterOSModule` returning false, skipping
`__managed__Main` entirely). Direct instrumentation of the syscall
dispatcher (temporary, reverted before committing) traced this to
three genuinely unimplemented Linux syscalls CoreCLR's startup calls
unconditionally: `sched_getaffinity` (204), `membarrier` (324), and
`mlock` (149), all falling through the musl shim's `default:` case
(`third_party/shim/neoos_syscall.c`'s own comment: "this is the
signal that a primitive belongs in the kernel"). `mlock`'s failure
specifically is what the GC treats as fatal.

Implemented all three as real kernel primitives, not a shortcut:
`sched_getaffinity` reports every online CPU
(`kernel/smp/smp.h`'s `smp_online_count()`); `membarrier` is backed by
a genuine cross-CPU IPI broadcast (new `kernel/smp/membarrier.c`/`.h`,
mirroring `kernel/smp/tlb.c`'s shootdown pattern — IPI delivery is
itself a serializing event on x86-64, satisfying every command
including the `SYNC_CORE` variants); `mlock` is a real no-op success
(NeoOS has no swap, so every resident page already satisfies its
promise), bounded by a new `vma_range_mapped()` helper
(`kernel/mm/vma.c`/`.h`) that checks the range is genuinely mapped
without side effects.

That cleared the first wall, but retesting hit two more, one level
deeper each time — the same "CoreCLR calls a syscall unconditionally,
NeoOS doesn't have it" pattern repeating: `sysinfo` (99)/`statfs`
(137)/`get_mempolicy` (239) (the GC's own heap-sizing heuristics
needing real total-RAM figures), then `madvise` (28) alone (found by
its distinct signature — a total *hang* with no exit and no crash,
not another `-1`, because CoreCLR's GC retries indefinitely on
`-ENOSYS` there rather than treating it as fatal like the syscalls
before it). All were implemented the same way: real numbers where
NeoOS has them (`sysinfo`/`statfs` read `kernel/mm/pmm.h`'s frame
counters), a real single-node answer for `get_mempolicy`, and another
genuine advisory no-op for `madvise` (same `vma_range_mapped()`
bounds-check as `mlock`). All seven are documented in
`docs/stdlib.md` under one section, together, since they were all
found the same way in the same sitting.

**The syscall wall is cleared. What's left is not a missing syscall.**
With all seven in place, the binary makes it measurably further: it
mmaps a ~256KB GC segment, commits all of it via ~68 page faults
(traced and confirmed FAST — under 20ms total, the earlier impression
of "slow progress" across separate short test runs was an artifact of
comparing incomplete traces, not an actual bug), then hangs
completely — zero further syscalls, zero further page faults, for as
long as it was left running. A `-d int` trace of the hang shows the
CPU parked at a **fixed RIP** (`0x20000000bda2` in the published
binary) on every single timer sample. Disassembling that address
shows a textbook spin-wait: `lock cmpxchg` against `[rbx+0x38]`
followed by a `pause`-loop spinning **while that word is
non-negative**, waiting for something else to make it negative.
Nothing ever does: no `clone()` syscall appears anywhere in the
process's history, meaning CoreCLR never created the second thread
(a GC/finalizer thread, most likely) that a lock like this would
normally expect to release it. This is a CoreCLR-internals question
(why does WorkstationGC's startup reach a lock/monitor wait without
having spawned whatever is supposed to release it — a decision made
inside `libRuntime.WorkstationGC.a`, not in anything NeoOS's kernel or
shim controls), not a kernel gap — the natural stopping point for this
investigation. A concrete next step for whoever continues it:
disassemble backward from `0x20000000bda2` to identify the calling
function (likely something in `RestrictedCallouts::Initialize`,
`RuntimeInstance::Initialize`, or `RedhawkGCInterface::
InitializeSubsystems`, per `RhInitialize`'s own call order established
earlier in this section) and compare against CoreCLR's own source for
what condition normally clears that wait.
