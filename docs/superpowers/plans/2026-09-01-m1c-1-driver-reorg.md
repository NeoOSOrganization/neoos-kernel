# M1c-1 — kernel/drivers/ reorganisation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** move every hardware driver out of `kernel/dev/` into a
`kernel/drivers/{video,input,block,char,irq,acpi}/` tree, and the
terminal subsystem into `kernel/tty/`, with **zero behaviour change** —
so the driver-interface work in M1c-2 has a clean layout to build on.

**Architecture:** a scripted `git mv` of 38 files plus a global
`#include "dev/…"` → new-path rewrite, one Makefile `KERNEL_DIRS`
edit, and a `clean-kernel` fix for the now-deeper object tree. The
proof is that `make test`'s serial log is identical to a pre-reorg
baseline once the known run-to-run noise (pids, tick counters,
timestamps) is normalised away.

**Tech Stack:** GNU Make, the cross `gcc` (`-Ikernel`, so every include
is repo-root-relative), `git mv`, `sed`, headless QEMU + serial-log
capture, the parallel gauntlet.

**Spec:** `docs/superpowers/specs/2026-09-01-m1c-driver-model-and-vts-design.md` (§1, §9 row M1c-1)

## Global Constraints

- **No behaviour change.** No `.c`/`.h` *contents* change except the
  `#include` lines. **File names are kept** — `vga.c` stays `vga.c`
  (it moves to `drivers/video/`, it is not renamed to `vgacon.c` yet).
  The spec §1 table shows the *M1c-2* end-state names; M1c-1 only gets
  files into the right directory. Renaming files and symbols
  (`vga_*` → `vgacon_*`, `fb_*` → `vesafb_*`) is M1c-2's job.
- **`kernel/dev/` is deleted** once empty.
- **`-Ikernel` is the only include path** (plus GCC's implicit
  same-directory search, which nothing in the moved set relies on —
  every cross-file reference already uses a `"subdir/foo.h"` prefix).
  So a header at `kernel/drivers/video/fb.h` is included as
  `#include "drivers/video/fb.h"`.
- **The object tree deepens.** `kernel/drivers/video/fb.c` builds to
  `build/drivers/video/fb.o`. The pattern rule
  `$(BUILD_DIR)/%.o: kernel/%.c` with `mkdir -p $(dir $@)` already
  handles arbitrary depth. But `clean-kernel`'s
  `rm -f $(BUILD_DIR)/**/*.o` only reaches one level — it **must** be
  changed to `find $(BUILD_DIR) -name '*.o' -delete`.
- **`.asm` files do not move** — they are all under `kernel/arch/`,
  `kernel/ipc/`, `kernel/syscall/`. `linker.ld` names no paths.
- **Work on `main`, one commit.** The commit message contains the full
  file mapping. Trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`.
- **The reorg script is not kept** — it runs once, its effect is
  committed, and it is deleted in the same commit (or never added).
- **Regenerate disks** (`rm -f build/disk.img build/disk2.img`) before
  a bare `make test`. **Never `make` while the gauntlet runs.**

## The move mapping (authoritative)

| file(s) | from | to |
|---|---|---|
| `vga.{c,h}`, `fb.{c,h}`, `fbcon.{c,h}`, `font8x16.c` | `kernel/dev/` | `kernel/drivers/video/` |
| `keyboard.{c,h}`, `keymap_us.h`, `evdev.{c,h}`, `input.{c,h}` | `kernel/dev/` | `kernel/drivers/input/` |
| `ata.{c,h}` | `kernel/dev/` | `kernel/drivers/block/` |
| `serial.{c,h}`, `rtc.{c,h}`, `pit.{c,h}`, `timer.{c,h}` | `kernel/dev/` | `kernel/drivers/char/` |
| `lapic.{c,h}`, `ioapic.{c,h}`, `pic.{c,h}` | `kernel/dev/` | `kernel/drivers/irq/` |
| `acpi.{c,h}` | `kernel/dev/` | `kernel/drivers/acpi/` |
| `tty.{c,h}`, `pty.{c,h}`, `console.{c,h}` | `kernel/dev/` | `kernel/tty/` |

Header include rewrites (19 headers):

```
dev/acpi.h       -> drivers/acpi/acpi.h
dev/ata.h        -> drivers/block/ata.h
dev/console.h    -> tty/console.h
dev/evdev.h      -> drivers/input/evdev.h
dev/fb.h         -> drivers/video/fb.h
dev/fbcon.h      -> drivers/video/fbcon.h
dev/input.h      -> drivers/input/input.h
dev/ioapic.h     -> drivers/irq/ioapic.h
dev/keyboard.h   -> drivers/input/keyboard.h
dev/keymap_us.h  -> drivers/input/keymap_us.h
dev/lapic.h      -> drivers/irq/lapic.h
dev/pic.h        -> drivers/irq/pic.h
dev/pit.h        -> drivers/char/pit.h
dev/pty.h        -> tty/pty.h
dev/rtc.h        -> drivers/char/rtc.h
dev/serial.h     -> drivers/char/serial.h
dev/timer.h      -> drivers/char/timer.h
dev/tty.h        -> tty/tty.h
dev/vga.h        -> drivers/video/vga.h
```

---

## Task 1: the reorg

**Files:** every `kernel/**/*.c` and `*.h` that includes a `dev/`
header (61 files); the 38 files in `kernel/dev/`; `Makefile`.

- [ ] **Step 1: capture the pre-reorg baseline**

```bash
rm -f build/disk.img build/disk2.img
make test 2>&1 | tail -2                      # confirm it's green now
cp build/serial.log /tmp/m1c1-baseline.log
```

Define the normaliser (used in Step 6). It strips the run-to-run
noise and keeps the marker/selftest skeleton:

```bash
norm() {           # usage: norm <serial.log>
  sed -E \
    -e 's/pid=0x[0-9a-f]+/pid=PID/g' \
    -e 's/tick=0x[0-9a-f]+/tick=T/g' \
    -e 's/0x[0-9a-f]{6,}/0xADDR/g' \
    -e '/^\[timer\] tick=/d' \
    -e '/^\[rtc\] /d' \
    -e 's/code=0x[0-9a-f]+/code=C/g' \
    "$1" | grep -E 'passed|PASSED|FAILED|PANIC|selftest|\] (ALL|booted)|EXCEPTION'
}
```

- [ ] **Step 2: create the directories and move the files**

```bash
mkdir -p kernel/drivers/video kernel/drivers/input kernel/drivers/block \
         kernel/drivers/char kernel/drivers/irq kernel/drivers/acpi kernel/tty

git mv kernel/dev/vga.c kernel/dev/vga.h kernel/dev/fb.c kernel/dev/fb.h \
       kernel/dev/fbcon.c kernel/dev/fbcon.h kernel/dev/font8x16.c \
       kernel/drivers/video/

git mv kernel/dev/keyboard.c kernel/dev/keyboard.h kernel/dev/keymap_us.h \
       kernel/dev/evdev.c kernel/dev/evdev.h kernel/dev/input.c kernel/dev/input.h \
       kernel/drivers/input/

git mv kernel/dev/ata.c kernel/dev/ata.h kernel/drivers/block/

git mv kernel/dev/serial.c kernel/dev/serial.h kernel/dev/rtc.c kernel/dev/rtc.h \
       kernel/dev/pit.c kernel/dev/pit.h kernel/dev/timer.c kernel/dev/timer.h \
       kernel/drivers/char/

git mv kernel/dev/lapic.c kernel/dev/lapic.h kernel/dev/ioapic.c kernel/dev/ioapic.h \
       kernel/dev/pic.c kernel/dev/pic.h kernel/drivers/irq/

git mv kernel/dev/acpi.c kernel/dev/acpi.h kernel/drivers/acpi/

git mv kernel/dev/tty.c kernel/dev/tty.h kernel/dev/pty.c kernel/dev/pty.h \
       kernel/dev/console.c kernel/dev/console.h kernel/tty/

rmdir kernel/dev            # must be empty now; if not, a file was missed
```

- [ ] **Step 3: rewrite the includes**

```bash
grep -rlZ '"dev/' kernel | xargs -0 sed -i \
  -e 's#"dev/acpi\.h"#"drivers/acpi/acpi.h"#g' \
  -e 's#"dev/ata\.h"#"drivers/block/ata.h"#g' \
  -e 's#"dev/console\.h"#"tty/console.h"#g' \
  -e 's#"dev/evdev\.h"#"drivers/input/evdev.h"#g' \
  -e 's#"dev/fbcon\.h"#"drivers/video/fbcon.h"#g' \
  -e 's#"dev/fb\.h"#"drivers/video/fb.h"#g' \
  -e 's#"dev/input\.h"#"drivers/input/input.h"#g' \
  -e 's#"dev/ioapic\.h"#"drivers/irq/ioapic.h"#g' \
  -e 's#"dev/keyboard\.h"#"drivers/input/keyboard.h"#g' \
  -e 's#"dev/keymap_us\.h"#"drivers/input/keymap_us.h"#g' \
  -e 's#"dev/lapic\.h"#"drivers/irq/lapic.h"#g' \
  -e 's#"dev/pic\.h"#"drivers/irq/pic.h"#g' \
  -e 's#"dev/pit\.h"#"drivers/char/pit.h"#g' \
  -e 's#"dev/pty\.h"#"tty/pty.h"#g' \
  -e 's#"dev/rtc\.h"#"drivers/char/rtc.h"#g' \
  -e 's#"dev/serial\.h"#"drivers/char/serial.h"#g' \
  -e 's#"dev/timer\.h"#"drivers/char/timer.h"#g' \
  -e 's#"dev/tty\.h"#"tty/tty.h"#g' \
  -e 's#"dev/vga\.h"#"drivers/video/vga.h"#g'
```

Order matters: `fbcon\.h` before `fb\.h` (the `sed` list above already
does this). Then check nothing was left behind:

```bash
grep -rn '"dev/' kernel && echo "STRAGGLERS ^" || echo "clean"
```

- [ ] **Step 4: Makefile**

`KERNEL_DIRS` — drop `kernel/dev`, add the seven new dirs:

```make
KERNEL_DIRS := kernel kernel/arch kernel/drivers/video kernel/drivers/input \
	kernel/drivers/block kernel/drivers/char kernel/drivers/irq \
	kernel/drivers/acpi kernel/tty kernel/ipc kernel/smp \
	kernel/syscall kernel/mm kernel/fs kernel/sched kernel/sync kernel/net kernel/lib
```

`clean-kernel` — reach the deeper object tree:

```make
clean-kernel:
	find $(BUILD_DIR) -name '*.o' -delete
```

- [ ] **Step 5: build**

```bash
rm -f build/disk.img build/disk2.img
make clean-kernel
make test 2>&1 | tee /tmp/m1c1-build.log | tail -5
```

Fix any compile error — it will be a missed include rewrite or a
missed `git mv` (a header that still lives in `kernel/dev/`). There
should be none if Steps 2–3 ran clean. Confirm the object tree:

```bash
find build -name '*.o' | grep -E 'drivers/|/tty/' | head
find build/dev -name '*.o' 2>/dev/null && echo "STALE build/dev ^"
```

- [ ] **Step 6: prove no behaviour change**

```bash
diff <(norm /tmp/m1c1-baseline.log) <(norm build/serial.log) && \
  echo "IDENTICAL — reorg is behaviour-neutral"
```

A non-empty diff means something actually changed — investigate before
committing (a reordered include can change nothing; a *dropped* one
would have failed to build). The marker set and order must match
exactly.

- [ ] **Step 7: gauntlet**

```bash
bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 2
```
→ `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 8: commit**

```bash
git add -A
git commit   # message body = the full mapping table from this plan,
             # plus "M1c-1: move hardware drivers to kernel/drivers/,
             # terminal subsystem to kernel/tty/. Pure move + include
             # rewrite; serial log normalised-identical; gauntlet 15/15."
```

Verify `git show --stat` shows only renames (`R100`) and
single-line-per-include edits — no logic hunks.

---

## Self-Review

**Spec coverage (§1, §9 M1c-1):**
- The move table matches spec §1 exactly on *directories*. It keeps
  filenames (`vga.c` not `vgacon.c`) — a deliberate refinement noted in
  Global Constraints so M1c-1 stays a pure path change; the spec's
  renamed targets are M1c-2's end state.
- `kernel/dev/` deleted → Step 2 `rmdir`.
- `KERNEL_DIRS` updated → Step 4.
- "serial log identical after normalising pids / tick / timestamps" →
  Step 1 defines `norm`, Step 6 diffs it.
- Gauntlet 15/15 → Step 7.
- Method "scripted `git mv` + `sed`, script not kept" → Steps 2–3 are
  inline shell, nothing is added to the repo.

**Placeholder scan:** every command is concrete and runnable. The
commit message body is "the mapping table from this plan" — a
transcription instruction, not a TODO. The `norm` function is given in
full. No "handle errors" hand-waving — Step 5 says exactly what a
failure will be (missed include / missed mv) and Step 6 says what a
non-empty diff means.

**Type consistency:** the 19 header rewrites in Task 1 Step 3 match the
mapping table one-for-one. `fbcon.h` is rewritten before `fb.h` in the
`sed` list (substring hazard). `KERNEL_DIRS` in Step 4 lists all seven
new dirs and no longer lists `kernel/dev`. `clean-kernel` uses `find`,
consistent with the Global Constraints note about the deeper tree.

**Scope:** one commit, no `.c`/`.h` logic changes, no new files kept,
no ABI or syscall changes. Correct for M1c-1 — the driver *interfaces*
(`fb_device`, `con_driver`) are M1c-2.
