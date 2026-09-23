# SQLite port (`neoos-sqlite`)

Date: 2026-09-23. Status: draft for review. Part of the desktop roadmap
(`2026-09-23-desktop-00-roadmap.md`), milestone M1; needs kernel
prerequisite K1 (M0).

## Goals

- SQLite 3 built for NeoOS with the hosted musl toolchain, as a static
  library + headers + the `sqlite3` CLI, from a new port repo
  `neoos-sqlite` shaped exactly like `neoos-zlib` (`upstream/`,
  `build.sh`, `build-output/`).
- **Unmodified upstream** and **no custom VFS**: SQLite's stock `unix`
  VFS runs on NeoOS because the kernel grows the Linux-shaped primitives
  it needs (K1), not because SQLite is taught NeoOS. This is the repo's
  "translation, never emulation" rule applied to a port.
- Be the persistence layer for the start menu (spec 06), the desktop
  shell's settings (spec 04) and the system tools (Notepad's recent
  files, spec 09) — with a schema convention and migration scheme they
  all share.

## Non-goals

- WAL mode. It needs a shared-memory index file `mmap`ed `MAP_SHARED` by
  every connection plus shm locking; NeoOS supports `MAP_SHARED` of
  memfds, but shared file-backed mappings across processes and the
  `-shm` lock protocol are not something to depend on for v1. Rollback
  journal (`journal_mode=DELETE`) only. Recorded as future work.
- SQLite extensions (FTS5, JSON1 is built in to the amalgamation anyway
  and stays; R-Tree, session, ICU) — off unless a spec asks.
- A shared library. Static only in v1 (roadmap convention).
- A server, a daemon, or a "config service" in front of SQLite. Each
  process opens the database file itself.

## Upstream and version

Like every other NeoOS port, `upstream/` is the project's own source
tree as a **git submodule pinned at a release tag**: SQLite's official
GitHub mirror, `https://github.com/sqlite/sqlite.git`, tag
`version-3.46.1` (a 2024 release, not the newest — restricted-bandwidth
rule), fetched with `--depth 1`. The amalgamation (`sqlite3.c`,
`shell.c`) is **generated** from it by SQLite's own
`configure && make sqlite3.c` on the build host (needs `tclsh`); the
result is byte-identical to the amalgamation sqlite.org publishes for
3.46.1 (SHA3-256 `186a1baa…a7dad`, checked against the release log).

(An earlier draft of this spec vendored the published amalgamation zip
instead; changed on your review to match the other ports.)

## Build

`build.sh`, same shape as `neoos-zlib/build.sh`:

```sh
NEOOS_TOOLCHAIN="${NEOOS_TOOLCHAIN:-$HOME/opt/cross-x86_64-neoos}"
CC=x86_64-neoos-linux-musl-gcc
CFLAGS="-O2 -fPIC $SQLITE_OPTS"
$CC $CFLAGS -c upstream/sqlite3.c -o build/sqlite3.o
x86_64-neoos-linux-musl-ar rcs build-output/lib/libsqlite3.a build/sqlite3.o
$CC $CFLAGS -static upstream/shell.c build/sqlite3.o -o build-output/bin/sqlite3
cp upstream/sqlite3.h upstream/sqlite3ext.h build-output/include/
```

Compile-time options (`SQLITE_OPTS`), each with its reason:

| option | why |
|---|---|
| `-DSQLITE_THREADSAFE=1` | the shell is single-threaded, but the C# tools are not (NativeAOT threadpool); serialized mode is the safe default |
| `-DSQLITE_OMIT_LOAD_EXTENSION` | no `dlopen` of extensions wanted; removes the libdl dependency |
| `-DSQLITE_DEFAULT_MMAP_SIZE=0` | the default already; stated so nobody turns it on without reading this |
| `-DHAVE_USLEEP=1` | busy-handler sleeps; `nanosleep` is exact since the hrtimer milestone |
| `-DSQLITE_DQS=0` | double-quoted string literals are a footgun; new code, no legacy |
| `-DSQLITE_TEMP_STORE=2` | temp tables in memory: avoids temp files on the FAT volume |

`-mcmodel=large -fno-pic` is **not** needed: that was for libneoos-linked
binaries at `0x200000000000`. Musl/hosted-toolchain binaries link at the
stock `0x400000` since the dynamic-linking milestone (`neoos-lvgl` still
uses it for its own reasons; decide per consumer, not here).

The port gets `sqlite.test.json` (`{"category": "lib"}` plus the new
`deps` field — see the roadmap's os-builder section) so `neoos-os-builder`
can build it and place `sqlite3` at `/usr/local/bin/sqlite3`.

## What SQLite's `unix` VFS needs from NeoOS

Checked against the kernel and the shim on 2026-09-23:

| SQLite uses | NeoOS today | action |
|---|---|---|
| `open`/`close`/`read`/`write`/`lseek` | yes | — |
| `pread`/`pwrite` (only with `USE_PREAD`) | `pread` yes, `pwrite` **no** | K1 adds `pwrite64`; build keeps SQLite's default lseek+read/write path anyway |
| `fsync`/`fdatasync` | yes (MSC-3) | verify `fsync` actually flushes the FAT volume's block cache to the ATA device, not just the vnode — **assumption to verify** |
| `ftruncate` | yes | — |
| `fstat`/`stat`/`access`/`getcwd`/`unlink`/`mkdir`/`rmdir`/`readlink` | yes | — |
| `fcntl(F_SETLK/F_SETLKW/F_GETLK)` POSIX record locks | **no** | **K1** — the one real kernel feature this port needs |
| `open(O_EXCL)` | flag defined, **never checked** | K1 honours it (also needed by `unix-dotfile` and by Notepad's temp files) |
| `getpid`, `time`, `gettimeofday`, `sleep` | yes | — |
| `mmap` | only if `mmap_size > 0` | kept at 0 |
| `rename` | not by SQLite (rollback journal is unlinked) | K1 adds it for spec 09 |

### K1: POSIX advisory record locks

Linux-shaped `fcntl` record locks, because SQLite's locking protocol is
built on byte ranges (a PENDING byte, a RESERVED byte, a SHARED range at
the 1 GiB offset) and because every other ported Unix program that locks
a file uses the same call:

- `struct flock` with Linux x86_64 layout (`l_type, l_whence` as `short`,
  `l_start, l_len` as `off_t`, `l_pid` as `pid_t`), `F_RDLCK/F_WRLCK/
  F_UNLCK` = 0/1/2, `F_GETLK/F_SETLK/F_SETLKW` = 5/6/7 — Linux values.
- Locks are owned by (process, inode): a lock taken through one fd is
  seen through every fd of the same process on the same file, and **every
  lock the process holds on the inode is released when any fd for it is
  closed** — POSIX's notorious semantics, which SQLite knows and works
  around (it keeps an `unixInodeInfo` per inode for exactly this). NeoOS
  must reproduce it, not "fix" it; SQLite would misbehave against saner
  semantics.
- Per-vnode lock list (sorted ranges), under the vnode lock; blocking
  `F_SETLKW` sleeps on a per-vnode waitq, interruptible (`EINTR`),
  woken on every unlock of that vnode.
- Deadlock detection (`EDEADLK`) between two `F_SETLKW` waiters: the
  simple wait-for graph walk Linux does. SQLite never blocks (it uses
  `F_SETLK` + its busy handler), so this is for other programs; it may
  land after M1 if it grows, recorded as a divergence until then.
- Process exit releases all its locks (walk its fd table's vnodes).
- OFD locks (`F_OFD_*`) and `flock(2)`: out of scope, `EINVAL`, recorded.

A `locktest.nex` joins the gauntlet: two processes contend on overlapping
ranges; close-releases-all; exit releases; `F_GETLK` reports the holder's
pid.

## Storage location

- Databases live in **`/var/lib/neoos/`**, one file per owner:
  `desktop.db` (shell + start menu), `notepad.db`, … . Per-app files keep
  write contention to the processes that actually share data (only the
  shell writes `desktop.db`).
- `/var/lib/neoos/` is created by the image build (`mmd -D s` — never
  plain `mmd`, it hangs on an existing directory) and by the shell on
  first run if missing.
- **Assumption to verify:** that `/var` is on the writable disk volume
  and persists across boots. If the root volume is re-imaged by every
  `make` target, persistence is only a property of `make desktop`-style
  interactive images, which is fine but must be stated in the shell spec.
- FAT: long names (VFAT LFN) are supported by the kernel, so
  `desktop.db-journal` is a legal name. FAT has no permissions and no
  hard links; SQLite needs neither.

## A small shared helper: `nsql`

Not a wrapper around SQLite's API — a ~200-line C file, shipped in
`neoos-sqlite` as `libnsql.a` + `nsql.h`, that every consumer uses so the
conventions below are implemented once:

```c
// Opens (creating if needed) /var/lib/neoos/<name>.db with the NeoOS
// defaults: journal_mode=DELETE, synchronous=FULL, foreign_keys=ON,
// busy_timeout=2000 ms. Runs migrations (below). Returns SQLITE_OK or
// an sqlite error code; on error *db is closed and NULL.
int nsql_open(const char *name, const struct nsql_migration *m, int n_migrations,
              sqlite3 **db);

struct nsql_migration {
    int         version;   // 1, 2, 3 ... strictly increasing
    const char *sql;       // run inside one transaction
};

// Consistent online copy via the backup API (sqlite3_backup_*).
int nsql_backup(sqlite3 *db, const char *dest_path);
```

The C# tools bind these same three functions (spec 07), so C and C#
consumers cannot drift apart on pragmas.

## Schema conventions and migrations

- **Versioning** is `PRAGMA user_version`. `nsql_open` reads it and runs
  every migration with `version > user_version`, in order, each inside
  `BEGIN IMMEDIATE … COMMIT` together with `PRAGMA user_version = N`, so a
  crash mid-migration leaves the previous version intact.
- A database whose `user_version` is **newer** than the newest migration
  the binary knows is opened read-only and logged
  (`[nsql] desktop.db is v5, this build knows v3 -- read-only`) — a
  downgrade must never destroy data.
- Migrations are append-only. A released migration is never edited.
- Every table has an `INTEGER PRIMARY KEY` and, where rows come from
  outside the database (packages), a stable text `key` (see spec 06).

### Start-menu schema (owned by spec 06, defined here as migration 1)

The brief's three tables, extended only where spec 06 needs it (source
tracking for package-provided entries):

```sql
CREATE TABLE categories (
    id          INTEGER PRIMARY KEY,
    key         TEXT UNIQUE,              -- stable id from a manifest, NULL for user-made
    name        TEXT NOT NULL,
    sort_order  INTEGER NOT NULL DEFAULT 0,
    icon        TEXT                      -- path to an icon asset, NULL = default
);
CREATE TABLE apps (
    id          INTEGER PRIMARY KEY,
    key         TEXT UNIQUE,              -- manifest id, e.g. "org.neoos.doom"
    category_id INTEGER NOT NULL REFERENCES categories(id) ON DELETE CASCADE,
    name        TEXT NOT NULL,
    exec_path   TEXT NOT NULL,            -- absolute path to the executable
    args        TEXT NOT NULL DEFAULT '', -- see spec 06 for the quoting rule
    icon        TEXT,
    sort_order  INTEGER NOT NULL DEFAULT 0,
    source      TEXT NOT NULL DEFAULT 'user' CHECK (source IN ('user','package')),
    hidden      INTEGER NOT NULL DEFAULT 0 -- a user "removed" a package entry
);
CREATE INDEX apps_by_category ON apps(category_id, sort_order, name);
CREATE TABLE settings (
    key   TEXT PRIMARY KEY,
    value TEXT
);
```

## Concurrency

- **One writer per database by design**: only the shell writes
  `desktop.db`; only Notepad writes `notepad.db`. Readers (a future
  settings app reading `desktop.db`) are welcome.
- Correctness under *accidental* concurrency still holds, because it is
  SQLite's own locking over K1 — two `sqlite3` CLI sessions writing the
  same file must serialize, not corrupt. That is the M1 exit test.
- `busy_timeout=2000`: a reader never fails because the writer holds a
  lock for a moment.

## Backup and restore

- **Backup**: `nsql_backup()` (SQLite online backup API) to
  `/var/lib/neoos/backup/<name>-<unix-time>.db`. The shell takes one on
  every successful migration (before running it) and keeps the last 3.
- **Restore**: stop the owning process, copy a backup over `<name>.db`,
  delete any stale `<name>.db-journal` only if the backup is known good
  (a hot journal next to a restored file would be replayed onto it —
  documented in the shell's README as the one sharp edge).
- `sqlite3 /var/lib/neoos/desktop.db .dump` works for humans.

## Error handling and logging

- `nsql` logs every non-OK result as
  `[nsql] <db>: <operation> failed: <sqlite3_errstr> (<extended code>)`.
- `SQLITE_CORRUPT` / `SQLITE_NOTADB` at open: the file is renamed to
  `<name>.db.corrupt-<time>` (needs K1's `rename`), the newest backup is
  restored if one exists, otherwise a fresh database is created and the
  shell reseeds from manifests. Logged loudly; never a crash loop.
- `SQLITE_FULL` / `SQLITE_IOERR`: surfaced to the caller; the shell shows
  a dialog, it does not retry silently.

## Testing

- **Host**: the same amalgamation built natively runs `nsql`'s unit tests
  (migrations, downgrade-refusal, backup) — portable C, host-runnable.
- **Target** (`make sqlitetest` in NeoOS, `boot_until.sh` on a marker):
  `sqlitetest.nex` creates a DB, runs the start-menu migration, inserts,
  commits, then forks: child holds `BEGIN EXCLUSIVE`, parent's write gets
  `SQLITE_BUSY` then succeeds after the child commits. A second boot
  (disk image kept) verifies persistence. A deliberate `kill -9` mid-
  transaction followed by a reopen verifies hot-journal rollback.
- `locktest.nex` (K1) joins the gauntlet.

## Assumptions to verify

1. `fsync` on NeoOS flushes through the block cache to the device (not
   only to the cache) — SQLite's durability rests on it.
2. `/var/lib/neoos` persists across boots on the images where it matters.
3. `unlink` of an open file works (SQLite unlinks the journal while it is
   still open in some paths) — FAT has no inodes-without-names; check the
   VFS keeps the open vnode alive.
4. `lseek` beyond EOF followed by `write` (sparse extension) is handled
   by fatfs.

## Risks

| risk | mitigation |
|---|---|
| FAT durability: a torn write of the FAT itself corrupts more than the DB | rollback journal + `synchronous=FULL`; backups; documented |
| K1 lock semantics subtly un-POSIX (e.g. not releasing on close) → silent corruption under concurrency | `locktest` encodes the POSIX rules explicitly; the two-process SQLite test in M1 |
| Upstream size | amalgamation only (~9 MB tarball), vendored once |

## Open questions

- Should `sqlite3` CLI ship on every image or only `desktop.yaml`? Draft:
  only images that include the shell.
- Is a per-user database location needed? NeoOS has no users today; if
  it grows them, `/var/lib/neoos/` becomes `$HOME/.local/share/neoos/`.

## Decision log

| decision | alternatives | why |
|---|---|---|
| Stock `unix` VFS + kernel K1 | custom NeoOS VFS; `unix-none` (no locking); `unix-dotfile` | a custom VFS is emulation in the port; no-locking corrupts under the first accidental second writer; dotfile needs `O_EXCL` anyway and gives no shared locks. Record locks are the Linux-shaped primitive. |
| Rollback journal, no WAL | WAL | WAL needs shared file mappings + shm locks: much more kernel surface for no v1 benefit (one writer) |
| Static `libsqlite3.a` | shared `.so` | roadmap convention; NativeAOT `DirectPInvoke` wants static |
| Upstream git submodule at a release tag, amalgamation generated from it | vendored amalgamation zip | same shape as every other port; the generated file is byte-identical to the published one |
| One DB file per owning app | one system DB | contention and blast radius both stay per-app |
| `PRAGMA user_version` migrations in `nsql` | per-app ad-hoc | one implementation shared by C and C# |
