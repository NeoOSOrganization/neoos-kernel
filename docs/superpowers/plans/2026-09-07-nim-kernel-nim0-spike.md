# NIM-0 — Nim-in-the-Kernel Spike Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prove that Nim code can be compiled into the NeoOS kernel and
run: one trivial `--mm:none` Nim module, called from C, calling back
into C, with a boot selftest marker and a deliberate-panic negative
test — and the gauntlet still green.

**Architecture:** `nim c --compileOnly` emits C from the Nim module
graph into `build/nimcache/`; those `.c` files are compiled by the
*same* rule as hand-written kernel `.c` and linked into `kernel.elf`.
A tiny C `panicoverride`/shims layer + a 30-line change to `kmain`
that calls the Nim runtime init. `--os:any --mm:none --panics:on`.

**Tech Stack:** Nim 2.2.4 (the version the userland `nim-probe` used),
installed persistently at `~/opt/nim-2.2.4`; `x86_64-elf-gcc` (the
existing kernel cross-compiler); the existing kernel flags.

**Spec:** `docs/superpowers/specs/2026-09-07-nim-kernel-adoption-design.md`
(sections "Nim kernel configuration", "The freestanding runtime shims",
"Build integration", "Migration order → NIM-0").

## Global Constraints

- Nim version: **exactly 2.2.4**. Installed at `~/opt/nim-2.2.4`
  (persistent, matching how `x86_64-elf-gcc` lives at
  `~/opt/cross-x86_64-elf`), with `~/opt/nim-2.2.4/bin` the only
  place the Makefile looks (`NIM ?= $(HOME)/opt/nim-2.2.4/bin/nim`).
- Kernel Nim config, non-negotiable for this spike:
  `--os:any --mm:none --threads:off --panics:on --exceptions:goto
  --stackTrace:off --lineTrace:off --lineDir:on --noMain:on
  -d:release`, `--cc:gcc --gcc.exe:x86_64-elf-gcc`, and `passC` with
  the kernel's own `-ffreestanding -fno-stack-protector -mno-red-zone
  -mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -O2 -Ikernel -Ishared`.
- **No heap allocation anywhere in Nim kernel code.** `--mm:none`
  makes `new`/`seq`/`string`(-building)/`@[]` compile errors; do not
  work around them. `cstring` literals and fixed `array`s only.
- **No `float` in kernel Nim** — the build is `-mno-sse`.
- Do not touch `boot/*.asm`, `kernel/arch/*.asm`, `context_switch`,
  `sched_post_switch`, `wait_off_cpu`.
- Verification one-liner (used verbatim in "run" steps):

  ```bash
  cd ~/projects/personal/NeoOS && \
  make clean-kernel iso disk-image QUIET=1 2>&1 | grep -iE "error:|Error [0-9]|undefined reference" ; \
  timeout 150 qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
    -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
    -drive file=build/disk2.img,format=raw -vga std \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
    -no-reboot -display none -serial file:build/serial.nim.log > /dev/null 2>&1 ; \
  grep -nE "\[nim\]|PANIC|\[exception\]|MISSING" build/serial.nim.log
  ```
- `tools/gauntlet.sh 15 3` at zero retries is the acceptance bar for
  Task 4 and Task 5.
- **If any task turns out genuinely painful — the shim list balloons,
  the codegen references SSE, the Makefile pattern can't be made
  clean — STOP and report.** NIM-0's whole purpose is to surface
  that before committing to the migration.

## File Structure

- **Create `~/opt/nim-2.2.4/`** — the pinned compiler (copied from the
  known-good build; Task 1).
- **Create `kernel/config.nims`** — the kernel Nim config, applied to
  every `.nim` under `kernel/`.
- **Create `kernel/panicoverride.nim`** — `rawoutput` / `panic` /
  `unhandledException` routed to `serial_write_string` + `lock_panic`.
- **Create `kernel/lib/nim_rt_shims.c`** — whatever `-nostdlib` leaves
  undefined that Nim's codegen references (empirically determined in
  Task 3; expected: nothing, or one or two libgcc-adjacent symbols).
- **Create `kernel/nimtest.nim`** — the spike module: a Nim proc
  `{.exportc: "nim_hello_selftest".}` that C calls, which itself
  calls `serial_write_string` and does a small pure computation, plus
  a `{.exportc: "nim_panic_selftest".}` guarded by a compile define
  for the negative test.
- **Modify `kernel/kernel.c`** — after `serial_init()`, call
  `nim_kernel_PreMain()` + `nim_kernel_NimMain()`; call
  `nim_hello_selftest()` in the boot selftest block.
- **Modify `Makefile`** — the Nim compile step + wiring the emitted
  objects into the `kernel.elf` link; `NIM` variable;
  `clean-kernel` blows `build/nimcache`; the new marker in
  `CORE_REQUIRED_MARKERS`.
- **Modify `docs/abi-compatibility.md`** — one line: kernel now
  contains Nim, C↔Nim boundary defined in the adoption spec.

---

### Task 1: Pin the Nim compiler

**Files:** Create `~/opt/nim-2.2.4/` ; Modify `Makefile` (the `NIM` var only).

- [ ] **Step 1: Install Nim 2.2.4 persistently**

A known-good Nim 2.2.4 build exists from the earlier userland probe.
Copy the whole tree (compiler + `lib/` + `config/`) out of scratch
into `~/opt`:

```bash
SRC=$(find /tmp/claude-1000 -maxdepth 6 -type d -name 'nim-2.2.4' -path '*nim-probe*' 2>/dev/null | head -1)
test -n "$SRC" || { echo "no cached nim-2.2.4 -- fetch nim-2.2.4 from nim-lang.org and 'sh build_all.sh'"; exit 1; }
mkdir -p ~/opt
cp -a "$SRC" ~/opt/nim-2.2.4
~/opt/nim-2.2.4/bin/nim --version | head -1     # expect: Nim Compiler Version 2.2.4
```

If the cache is gone: download `nim-2.2.4.tar.xz` from
`https://nim-lang.org/download/nim-2.2.4.tar.xz`, extract to
`~/opt/nim-2.2.4`, run `sh build_all.sh` inside it (needs a host C
compiler — `gcc`, not the cross one). One-time.

- [ ] **Step 2: Wire `NIM` into the Makefile**

Near the top of `Makefile`, by `CC := x86_64-elf-gcc`:

```make
NIM ?= $(HOME)/opt/nim-2.2.4/bin/nim
```

Add a guard target so a missing/wrong Nim fails loudly, not obscurely:

```make
.PHONY: nim-check
nim-check:
	@$(NIM) --version 2>/dev/null | grep -q '2\.2\.4' || \
	  { echo "error: need Nim 2.2.4 at $(NIM) (see docs/superpowers/plans/2026-09-07-nim-kernel-nim0-spike.md)"; exit 1; }
```

- [ ] **Step 3: Verify**

```bash
cd ~/projects/personal/NeoOS && make nim-check && echo OK
```
Expected: `OK`.

- [ ] **Step 4: Commit**

```bash
cd ~/projects/personal/NeoOS
git add Makefile
git commit -m "build: pin Nim 2.2.4 (\$HOME/opt/nim-2.2.4) + nim-check guard

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 2: Kernel Nim config + the runtime override

**Files:** Create `kernel/config.nims`, `kernel/panicoverride.nim`.

**Interfaces produced:** none C-visible yet — this is the Nim-side
runtime contract. `panicoverride.nim` must define `rawoutput(s:
string)` and `panic(s: string) {.noreturn.}` (the two symbols
`--os:any` requires a standalone program to provide).

- [ ] **Step 1: `kernel/config.nims`**

```nim
# Applied to every .nim compiled for the NeoOS kernel.
# See docs/superpowers/specs/2026-09-07-nim-kernel-adoption-design.md.
when defined(nimHasWarningAsError):
  discard

switch("os", "any")
switch("mm", "none")
switch("threads", "off")
switch("panics", "on")
switch("exceptions", "goto")
switch("stackTrace", "off")
switch("lineTrace", "off")
switch("lineDir", "on")
switch("noMain", "on")
switch("nimMainPrefix", "nim_kernel_")
switch("define", "release")

switch("cc", "gcc")
switch("gcc.exe", "x86_64-elf-gcc")
switch("gcc.linkerexe", "x86_64-elf-gcc")
switch("passC", "-ffreestanding -fno-stack-protector -mno-red-zone " &
                "-mno-mmx -mno-sse -mno-sse2 -mcmodel=kernel -std=gnu11 -O2 " &
                "-Ikernel -Ishared")

# Kernel prelude: forbid what must never appear.
switch("define", "nimKernel")
```

- [ ] **Step 2: `kernel/panicoverride.nim`**

```nim
# The standalone-runtime override --os:any requires.
proc c_serial(s: cstring) {.importc: "serial_write_string",
                            header: "drivers/char/serial.h".}
proc c_lockPanic(msg, where, holding: cstring) {.importc: "lock_panic",
                            header: "sync/lock.h", noreturn.}

{.push stackTrace: off, profiler: off.}

proc rawoutput(s: string) =
  # Under --mm:none, `s` here is always a compile-time or cstring-backed
  # literal; .cstring is a no-copy view.
  c_serial(s.cstring)

proc panic(s: string) {.noreturn.} =
  c_serial("[nim] panic: ")
  c_serial(s.cstring)
  c_serial("\n")
  c_lockPanic("nim defect".cstring, "nim/panicoverride".cstring, nil)
  while true: discard

{.pop.}
```

- [ ] **Step 3: Sanity-compile the override in isolation**

```bash
cd ~/projects/personal/NeoOS
$(HOME)/opt/nim-2.2.4/bin/nim c --compileOnly --noLinking \
  --nimcache:/tmp/nim0-probe -p:kernel -p:shared \
  --hints:off kernel/panicoverride.nim 2>&1 | tail -20
ls /tmp/nim0-probe/*.c | head
```
Expected: it emits `.c` with no error. (It won't *link* — that's
fine, `--noLinking`.) If Nim complains it can't find `config.nims`,
the file must sit next to the `.nim` being compiled or in a parent
dir Nim searches — put it at `kernel/config.nims` and compile with
CWD at repo root, or pass `--path:kernel`.

- [ ] **Step 4: Commit**

```bash
cd ~/projects/personal/NeoOS
git add kernel/config.nims kernel/panicoverride.nim
git commit -m "kernel: Nim freestanding config + panicoverride (routes to lock_panic)

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 3: The spike module + Makefile integration

**Files:** Create `kernel/nimtest.nim`, `kernel/lib/nim_rt_shims.c` ;
Modify `kernel/kernel.c`, `Makefile`.

**Interfaces produced:**
```c
// C-visible, from kernel/nimtest.nim:
void nim_hello_selftest(void);        // prints [nim] hello selftest passed/FAILED
// from Nim runtime:
void nim_kernel_PreMain(void);
void nim_kernel_NimMain(void);
```

- [ ] **Step 1: `kernel/nimtest.nim`**

```nim
{.push raises: [].}

proc c_serial(s: cstring) {.importc: "serial_write_string",
                            header: "drivers/char/serial.h".}
proc c_serialHex(v: uint64) {.importc: "serial_write_hex64",
                              header: "drivers/char/serial.h".}

# Pure compute: a Fibonacci sum and a fixed-array checksum, so the
# selftest proves real Nim codegen ran, not just a string print.
proc fibSum(n: int): uint64 =
  var a: uint64 = 0
  var b: uint64 = 1
  for _ in 0 ..< n:
    let t = a + b
    a = b
    b = t
    result += a

proc arrChecksum(): uint64 =
  var xs: array[16, uint64]
  for i in 0 ..< xs.len:
    xs[i] = uint64(i) * 2654435761'u64
  for x in xs:
    result = (result xor x) + (result shl 6) + (result shr 2)

proc nimHelloSelftest*() {.exportc: "nim_hello_selftest", cdecl.} =
  let f = fibSum(20)          # deterministic
  let c = arrChecksum()       # deterministic
  if f != 17710'u64:
    c_serial("[nim] hello selftest FAILED: fibSum\n"); return
  if c == 0'u64:
    c_serial("[nim] hello selftest FAILED: checksum\n"); return
  c_serial("[nim] hello selftest passed (fib=")
  c_serialHex(f)
  c_serial(" cksum=")
  c_serialHex(c)
  c_serial(")\n")

# Negative test -- only compiled when -d:nimPanicTest is passed.
when defined(nimPanicTest):
  proc nimPanicSelftest*() {.exportc: "nim_panic_selftest", cdecl.} =
    c_serial("[nim] tripping a deliberate Defect...\n")
    var xs: array[3, int]
    {.push checks: on.}
    let i = arrChecksum().int mod 100 + 10   # >= 10, out of range, not const-foldable
    discard xs[i]                             # index Defect -> panic
    {.pop.}

{.pop.}
```

- [ ] **Step 2: Makefile — compile the Nim graph, wire the objects**

The generated-sources two-phase pattern. Add:

```make
NIMCACHE := $(BUILD_DIR)/nimcache
NIMFLAGS := c --compileOnly --noLinking --nimcache:$(NIMCACHE) \
            --path:kernel --path:shared --hints:off

# Compile the whole Nim module graph rooted at the kernel's Nim entry.
# For NIM-0 that root is nimtest.nim (it imports nothing kernel-side yet
# except via importc). Emits C into $(NIMCACHE); does not link.
$(NIMCACHE)/nim0.stamp: kernel/nimtest.nim kernel/panicoverride.nim kernel/config.nims | nim-check
	@mkdir -p $(NIMCACHE)
	$(NIM) $(NIMFLAGS) kernel/nimtest.nim
	@touch $@

# The emitted .c files -- known only after the stamp. Use a recursive
# make so the wildcard is evaluated after generation.
.PHONY: nim-objects
nim-objects: $(NIMCACHE)/nim0.stamp
	@$(MAKE) --no-print-directory $(patsubst %.c,%.o,$(wildcard $(NIMCACHE)/*.c))

$(NIMCACHE)/%.o: $(NIMCACHE)/%.c $(NIMCACHE)/nim0.stamp
	$(CC) $(CFLAGS) -I$(NIMCACHE) -Wno-unused -Wno-unused-parameter \
	    -Wno-unused-variable -c $< -o $@
```

Then make `kernel.elf` depend on `nim-objects` and include the
`.o`s in the link line. Because the object list isn't known at parse
time, the cleanest is: `kernel.elf`'s recipe runs
`$(MAKE) nim-objects` first, then links
`$(wildcard $(NIMCACHE)/*.o)`:

```make
$(BUILD_DIR)/kernel.elf: $(ASM_OBJECTS) $(C_OBJECTS) $(BUILD_DIR)/embedfs_table.o linker.ld nim-objects
	$(CC) -T linker.ld -o $@ -ffreestanding -O2 -nostdlib \
		$(ASM_OBJECTS) $(C_OBJECTS) $(wildcard $(NIMCACHE)/*.o) \
		$(BUILD_DIR)/embedfs_table.o \
		$$(cat $(BUILD_DIR)/embedfs-objs.txt 2>/dev/null) -lgcc
```

`clean-kernel`: add `rm -rf $(NIMCACHE)`.

- [ ] **Step 3: kmain calls the Nim runtime init + the selftest**

`kernel/kernel.c`:

```c
// near the other externs
void nim_kernel_PreMain(void);
void nim_kernel_NimMain(void);
void nim_hello_selftest(void);

// in kmain(), right after serial_init()/the earliest serial output:
    nim_kernel_PreMain();
    nim_kernel_NimMain();

// in the boot selftest block, next to rbtree_selftest():
    nim_hello_selftest();
```

(`nim_kernel_PreMain`/`NimMain` exist because of
`--nimMainPrefix:nim_kernel_` + `--noMain`. Under `--mm:none` with no
module-level allocation, `NimMain` just runs `nimtest.nim`'s
top-level `{.push.}`/`{.pop.}` — effectively nothing — but call it
anyway so the pattern is right for later modules.)

- [ ] **Step 4: Build; resolve undefined symbols into `nim_rt_shims.c`**

```bash
cd ~/projects/personal/NeoOS && make clean-kernel iso 2>&1 | grep -iE "undefined reference|error:" | sort -u
```

For each `undefined reference to 'X'`: if it's `memcpy`/`memset`/
`memmove`/`memcmp`/`strlen` — those already exist in the kernel
(`kernel/lib/` or `kernel/mm/`); the link order may need the Nim
objects before them, or they're `static` — expose them. Anything
else (rare — maybe `__stack_chk_*` despite `-fno-stack-protector`,
or a `nimZeroMem`-adjacent helper) gets a minimal definition in a
new `kernel/lib/nim_rt_shims.c`:

```c
// kernel/lib/nim_rt_shims.c -- symbols Nim's codegen references that
// -nostdlib does not provide. Keep this file SMALL; a large one is a
// signal the Nim config is wrong.
#include <stddef.h>
// (add definitions here only as the linker demands them)
```

Iterate: build, add one shim, build, until the link is clean.
**Record the final shim list in the commit message** — it is the
key NIM-0 finding.

- [ ] **Step 5: Boot; verify the marker**

Run the verification one-liner. Expected:
`[nim] hello selftest passed (fib=... cksum=...)` in
`build/serial.nim.log`, no `PANIC`, no `[exception]`, boot completes.

If it hangs before the marker: the Nim runtime init faulted — check
`build/serial.nim.log` for the last line before silence and whether
`nim_kernel_NimMain` is doing something unexpected (a `sysFatal`, an
allocation). Add `serial_write_string("kmain: pre nim\n")` /
`"kmain: post nim\n"` around the calls to bisect.

- [ ] **Step 6: Add the marker; commit**

`Makefile` `CORE_REQUIRED_MARKERS`: add
`"[nim] hello selftest passed"` — but note it has a `(fib=...)` tail,
so the marker must be a prefix `grep -F` matches: use
`"[nim] hello selftest passed"` (the required-marker check is
substring, per the Makefile comment).

```bash
cd ~/projects/personal/NeoOS
git add kernel/nimtest.nim kernel/lib/nim_rt_shims.c kernel/kernel.c Makefile
git commit -m "kernel: NIM-0 spike -- first Nim module compiled into the kernel, [nim] hello selftest passed

Nim 2.2.4, --os:any --mm:none --panics:on. C calls nim_hello_selftest
(real codegen: fibSum + array checksum), Nim calls back into
serial_write_string. Runtime shims needed: <FILL IN from Step 4>.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 4: The negative test — a Nim Defect must reach `lock_panic`

**Files:** Modify `kernel/kernel.c`, `Makefile` (a `nim-panic-test` target).

- [ ] **Step 1: A throwaway build target that trips the Defect**

`Makefile`:

```make
.PHONY: nim-panic-test
nim-panic-test:
	$(MAKE) clean-kernel
	$(NIM) $(NIMFLAGS) -d:nimPanicTest kernel/nimtest.nim
	@# hand-hack: call nim_panic_selftest from kmain for this build only
	@echo "build a kernel that calls nim_panic_selftest() and boot it by hand"
```

Simpler and less magic: temporarily add, under
`#ifdef NEOOS_NIM_PANIC_TEST` in `kernel.c`, a call to
`nim_panic_selftest()` right after `nim_hello_selftest()`, and build
with `make ... NIMFLAGS_EXTRA=-d:nimPanicTest CFLAGS_EXTRA=-DNEOOS_NIM_PANIC_TEST`
(thread those two through the Makefile). Revert the `kernel.c` hunk
after the test — it is not committed.

- [ ] **Step 2: Boot the panic-test kernel**

Build with the panic define, boot with the one-liner. Expected in
`build/serial.nim.log`:

```
[nim] tripping a deliberate Defect...
[nim] panic: index 1 not in 0 .. 2      (or Nim's exact IndexDefect text)
... lock_panic output / EXCEPTION - HALTED ...
```

The machine **halts cleanly** — no reboot loop, no silent wrong
answer, no continuing-past-the-bug. This is the property that makes
Nim safe to trust in the kernel: an out-of-bounds access is a loud
stop, not corruption.

- [ ] **Step 3: Revert the throwaway `kernel.c` hunk; confirm normal build still green**

Run the verification one-liner again (no panic define) →
`[nim] hello selftest passed`, clean boot.

- [ ] **Step 4: Commit the Makefile plumbing (not the kernel.c hunk)**

```bash
cd ~/projects/personal/NeoOS
git add Makefile
git commit -m "build: nim-panic-test plumbing -- verified a Nim Defect halts via lock_panic

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 5: Gauntlet + write-up

**Files:** Modify `docs/abi-compatibility.md` ; update
`docs/superpowers/specs/2026-09-07-nim-kernel-adoption-design.md`
(mark NIM-0 done, record findings).

- [ ] **Step 1: Full gauntlet**

```bash
cd ~/projects/personal/NeoOS && tools/gauntlet.sh 15 3
```
Expected: `PGAUNTLET PASSED: 15/15`. The Nim object adds a few KB to
`kernel.elf` and one selftest to boot; if a run times out, the Nim
compile is being re-run under the gauntlet's parallel builds —
check `build/gauntlet/work/build.log`.

- [ ] **Step 2: Boot-time delta**

Compare wall time `[timer] calibrated` → `NeoOS: interrupts enabled`
before (git stash) and after. Should be within noise — the Nim
selftest is microseconds of compute.

- [ ] **Step 3: Record the findings in the spec**

In `2026-09-07-nim-kernel-adoption-design.md`, under "Migration order
→ NIM-0", replace the milestone description with what actually
happened:
- exact runtime shim list (from Task 3 Step 4)
- whether `--exceptions:goto` bloated the object (size of the Nim
  `.o` vs a comparable C module) — informs the goto-vs-quirky call
- any `config.nims` setting that had to change
- `nim c` wall time for one module, and for `make clean-kernel`
- the demangled-vs-raw symbol names in `nm build/kernel.elf | grep -i
  nim` — informs whether `{.exportc.}`-everything or a demangle
  script is the plan for NIM-1
- **verdict**: is NIM-1 (solidify) worth doing, or did NIM-0 surface
  a blocker?

- [ ] **Step 4: `docs/abi-compatibility.md`**

One line under the internals note: "The kernel now contains Nim
(`kernel/*.nim`, compiled to C by Nim 2.2.4 and linked normally).
Internal only — no ABI impact. The C↔Nim boundary and the migration
plan are in `docs/superpowers/specs/2026-09-07-nim-kernel-adoption-design.md`."

- [ ] **Step 5: Commit**

```bash
cd ~/projects/personal/NeoOS
git add docs/
git commit -m "docs: NIM-0 spike complete -- Nim runs in the NeoOS kernel, gauntlet 15/15

Findings recorded in the adoption spec. Verdict: <FILL IN>.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

## Self-Review

**Spec coverage** (against the adoption spec's NIM-0 definition):
"one trivial `--mm:none` Nim module compiled into the kernel,
`{.exportc.}`ing one function that C calls, and a `[nim] hello
selftest passed` marker … Proves: `nim c` in the build, the runtime
shims, panicoverride, the C↔Nim call both ways, `--mm:none` reality,
gdb `#line` back to `.nim`, gauntlet still green" — Task 3 (module +
build + C↔Nim both ways + shims + `--mm:none`), Task 2 (panicoverride
+ config), Task 4 (panic reaches `lock_panic` — the negative test the
spec's "Testing" section requires), Task 5 (gauntlet + the
`#line`/mangling findings). The `--lineDir:on` gdb check is folded
into Task 5 Step 3's findings rather than its own step (it's an
observation, not a build gate).

**Placeholder scan:** the runtime-shim list in Task 3 Step 4 and
`nim_rt_shims.c`'s body are deliberately "add as the linker demands"
— this is empirical discovery, the *point* of the spike, not a
dodge; the step says exactly how to iterate and to record the result.
The "verdict" in Task 5 is filled at execution. No "TODO"/"handle
errors"/"etc." left as instructions.

**Type consistency:** `nim_hello_selftest` / `nim_panic_selftest` /
`nim_kernel_PreMain` / `nim_kernel_NimMain` are the C-visible names
used identically in `nimtest.nim` (`{.exportc.}`), `kernel.c`
(externs + calls), and the Makefile prefix (`--nimMainPrefix`).
`NIMCACHE` / `NIMFLAGS` / `NIM` Makefile vars consistent across
Tasks 1, 3, 4. `--os:any --mm:none --panics:on --exceptions:goto`
identical in Global Constraints, `config.nims`, and the panic-test
target.
