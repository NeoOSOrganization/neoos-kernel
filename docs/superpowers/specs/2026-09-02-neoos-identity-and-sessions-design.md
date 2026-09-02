# NeoOS identity, sessions and native format — design

**Status:** design spec. Decomposed into N1–N5; each lands on `main`
with a green suite and a green gauntlet before the next begins.

**Goal:** NeoOS stops looking like a test harness that happens to boot
and starts looking like a system: its own executable format, a clean
filesystem, real user identity with `god` as the superuser, a login
session, and a native shell to land in.

---

## 1. What this is, and why it is five milestones

The request was one sentence and is five independent subsystems. They
are listed in dependency order; N1 and N2 land together because both
rename every binary and doing them separately means renaming twice.

| | milestone | depends on |
|---|---|---|
| N1 | `.nex` executables, `NOX` magic | — |
| N2 | lowercase names, clean directory layout, case-sensitive lookup | — |
| N3 | credentials: `uid`/`gid`, `god` = uid 0, `kill` permission | — |
| N4 | `nsh`, the native shell | N2 (paths) |
| N5 | login and sessions | N3, N4 |

The end state is the classic supervision chain, which is what the user
guessed and is correct:

```
init (PID 1, god)  ->  term  ->  login  ->  nsh (as the authenticated user)
```

`init` never authenticates anybody. It supervises, and `respawn` on the
login entry is what makes logging out return a fresh prompt instead of
powering the machine off.

## 2. Decisions taken

Recorded because each closes off an alternative that would otherwise be
re-litigated mid-implementation.

- **The loader accepts BOTH `ELF` and `NOX` magic**, and records which
  it saw. `NOX` is therefore a *capability marker*, not branding: later
  NeoOS-only behaviour keys off it. Refusing `ELF` outright was
  considered and rejected — it would make the format a rename rather
  than a signal.
- **Lookup is case-SENSITIVE**, not FAT's case-insensitive. The VFS's
  semantics are NeoOS's, and the case-sensitive filesystems planned
  later should not have to fight a rule inherited from FAT.
- **`god` is uid 0.** The name is NeoOS's; the number is Unix's,
  because every `uid == 0` check in ported software depends on it.
- **Test binaries live in `/usr/tests`, not `/bin`.** `ls /bin` showing
  fifty test programs beside the two real ones is the mess this
  milestone exists to remove.
- **Passwords are salted SHA-256**, documented plainly as a hash and
  **not** a password-hardening KDF. Plaintext was the alternative.

---

## N1 — `.nex` and the `NOX` magic

**Magic.** `0x7F 'N' 'O' 'X'`, mirroring ELF's shape byte for byte: one
non-printable guard byte then three characters. Everything from
`e_ident[4]` on is unchanged ELF64 — same class byte, same program
headers, same relocations. A `.nex` file *is* an ELF file with four
bytes changed.

That is the point. The toolchain keeps working (we link with GNU `ld`
and it produces ELF), the kernel keeps its existing loader, and the
magic becomes a one-bit question the loader can answer: *was this built
for NeoOS?*

**Loader.** `kernel/elf.c`'s check (currently `e_ident[0..3] == 0x7F
"ELF"`) accepts either magic. `struct elf_info` gains `int is_nox`, set
from which magic matched, and carried for the life of the process on
`struct process`. Nothing reads it yet; it exists so the first
NeoOS-only feature has somewhere to hang.

**Stamping happens at disk-image time, not in place.** The linker output
in `build/` stays a valid ELF, and only the copy written into the FAT
image is stamped. This is deliberate: `objdump`, `readelf`, `nm` and
`gdb` all refuse a file whose magic they do not recognise, and
disassembling a userland binary is how the fork/TLS bug in the BusyBox
track was actually found. Losing that to a cosmetic change would be a
bad trade.

A `tools/nexify.sh` does the stamp — copy, then write `NOX` at offset 1.

**Naming.** Every NeoOS executable is `.nex`. BusyBox is stamped too and
becomes `/bin/busybox.nex`: it is built by this system, so it is a
NeoOS executable.

**Test.** A `nexcheck` selftest asserts that a stamped binary loads and
reports `is_nox`, that an unstamped ELF still loads and does not, and
that a file with neither magic is refused. The first two matter because
"accept both" is only correct if both are actually exercised.

---

## N2 — lowercase, and a filesystem worth looking at

### The layout

```
/bin          busybox.nex, nsh.nex        — things a person would run
/sbin         init.nex, login.nex         — system programs
/etc          inittab, passwd
/dev                                       devfs
/proc                                      procfs
/tmp                                       ramfs
/mnt                                       the second FAT volume
/home/<user>                               ordinary users' homes
/root                                      god's home
/usr/tests    the ~50 test binaries
/usr/share/test  read-only fixtures the tests read
/var/tmp      scratch for tests that must write to a REAL filesystem
```

`/var/tmp` exists because `/tmp` is a ramfs and the FAT write tests need
FAT. Without it those tests keep writing into `/`, which is half of why
the root is a mess today.

### The mess being cleaned

The root currently holds `HELLO.TXT`, `BIGFILE.TXT`, `A Long File
Name.txt` and `DIR/` (shipped by the Makefile), plus `NEWDIR`, `RT.TXT`,
`writev.txt`, `SHORT.TXT`, `a rather long file name with spaces.text`,
`a long directory name/` and `TMPFILE.TXT` — all created by tests at
run time. Both halves have to move or the root refills on the next boot.

### Case sensitivity

Name comparison becomes exact. Two consequences, both documented in
`docs/stdlib.md`:

1. **The rename is a hard cutover.** Case-insensitive lookup would let
   old uppercase paths keep resolving during the transition;
   case-sensitive does not. Every path changes in one commit or the boot
   breaks. `REQUIRED_MARKERS` is the net: a test that stops running
   because its path moved fails the build rather than vanishing quietly.
2. **FAT cannot store `foo` and `FOO` at once.** NeoOS will consider
   them distinct names, and creating the second on a FAT volume will
   collide at the format level. That is FAT's limitation surfacing
   through a VFS that no longer hides it, not a VFS bug, and it
   disappears on the case-sensitive filesystems planned later.

**Test.** `casetest` asserts that `Foo` and `foo` are different names to
`open`, that a lookup differing only in case fails, and that the FAT
collision reports an error rather than silently overwriting.

---

## N3 — credentials, and `god`

**`struct process` gains `uid` and `gid`.** No `euid`/`egid` and no
set-uid bit: there is nothing yet that needs privilege elevation, and a
saved-uid model with no user of it is speculative complexity. When
set-uid arrives it brings its own milestone.

- Inherited across `fork` and preserved across `exec`.
- `init` starts as uid 0 — `god`.
- `getuid`/`geteuid`/`getgid`/`getegid` return the real values instead
  of today's hardcoded 0. This is a **behaviour change to a shipped
  syscall**: they answer 0 for everyone right now, and after N3 they
  answer the truth.
- New: `setuid`, `setgid`. Only uid 0 may change identity, and only
  downward. `login` is the one caller.

**Permission on signals.** `kill(pid, sig)` succeeds if the sender is
uid 0, or if the sender's uid equals the target's; otherwise `-EPERM`.
Since init runs as god, "a normal process cannot kill init" falls out of
that single rule rather than needing a special case for PID 1.

This is the Linux rule minus saved-uids, and it is deliberately the
*whole* model: no capabilities, no ptrace check, no session check.

**Test.** `permtest` forks a child that drops to an ordinary uid and
asserts: it cannot signal PID 1 (`-EPERM`), it cannot signal a process
owned by another uid, it CAN signal its own, and god can signal anything.
The negative cases are the point — a permission check that has never
been observed refusing anything is not known to work.

---

## N4 — `nsh`, the native shell

Deliberately minimal. BusyBox `ash` is present and better at being a
shell; `nsh` exists to be NeoOS's own, to be what a login session lands
in, and to be small enough to read.

- A prompt, a line read from stdin (the tty is in canonical mode, so a
  line arrives whole), whitespace splitting.
- Builtins: `cd`, `pwd`, `exit`, `echo`, `help`.
- Everything else: `PATH` lookup, `spawnve`, wait, report a non-zero
  exit status.
- **No quoting, no pipes, no redirection, no job control, no variables.**
  An argument containing a space cannot be written. This is a documented
  limit, not an oversight: `busybox sh` is one word away whenever the
  session needs a real shell.

**Test.** `nshtest` drives `nsh` over a pty the way `bbsh` drives ash:
run a program and check its output, check a builtin, check `PATH`
lookup, check a bad command reports and does not kill the shell.

---

## N5 — login and sessions

**`/etc/passwd`**, one record per line:

```
name:uid:gid:home:shell:salt:sha256hex
```

Linux's field order for the first five, with the hash kept in the same
file rather than a `/etc/shadow` — there is no privilege boundary yet
that a second file would enforce, and pretending otherwise would be
security theatre. Ships with `god` (uid 0, `/root`) and one ordinary
user.

**SHA-256** goes into `lib/` as `sha256.c`. Well-defined, about a
hundred lines, no dependencies. The stored value is
`sha256(salt || password)`, and `docs/stdlib.md` will say plainly that
this is a **hash, not a KDF**: it does not resist an offline
brute-force the way bcrypt or Argon2 does, and it is not trying to.

**`login`** runs as god because `init` spawns it. It:
1. prompts for a user name, then a password with echo disabled
   (`termios`, already implemented — `ttytest` covers get/set/restore);
2. reads `/etc/passwd`, hashes the given password with the record's
   salt, compares;
3. on failure, says so, pauses, and re-prompts — never revealing which
   half was wrong;
4. on success sets `HOME`, `USER`, `SHELL`, `PATH`, `PS1`, changes to
   the home directory, drops to the record's `gid` then `uid` (that
   order — the reverse loses the privilege needed for the second call),
   and `execve`s the shell.

**`init`** gets `respawn /sbin/login.nex` on the terminal entry, so
exiting the shell returns to a login prompt.

**Test.** `logintest` drives login over a pty: a wrong password is
refused and re-prompts, a correct one reaches a shell, and the resulting
shell reports the expected non-zero `getuid`. That last check is what
proves the privilege drop actually happened rather than being skipped.

---

## What this does not do

- **No set-uid, no capabilities, no ACLs.** File permission bits are not
  part of this either: FAT has nowhere to store them, and inventing a
  mode that nothing enforces would be worse than not having one.
- **No `/etc/shadow`**, per above.
- **No multi-user concurrency** — one session on one terminal. Multiple
  VTs each with their own login is a later milestone and needs nothing
  new from this one.
- **No password change tool.** `/etc/passwd` is edited by rebuilding the
  image.
- **`nsh` does not grow.** Pipes and redirection are `ash`'s job.

## Risks

- **N1+N2 is a large mechanical diff** — 54 Makefile paths, `INITTAB`,
  every test binary and every fixture path, all in one commit because
  case-sensitivity forbids a gradual cutover. Mitigation: the marker
  check fails the build on any test that stops running, and the gauntlet
  runs before the commit is considered done.
- **Stamping could break debugging** if it were done in place. It is not:
  `build/` keeps valid ELF, and only the disk copy is stamped.
- **N3 changes what `getuid` returns** to programs that already run.
  BusyBox reads it at startup; the suite covers that path today, so a
  regression shows up immediately.
- **`login` handles a password.** If the echo-disable fails, the password
  is echoed to the screen. `login` must verify the termios change took
  effect rather than assuming it, and refuse to prompt if it did not.
