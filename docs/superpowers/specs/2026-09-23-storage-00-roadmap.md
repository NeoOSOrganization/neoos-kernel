# Storage stack: block layer, SATA, NVMe, fs registry, ext2 — roadmap

Date: 2026-09-23. Status: **approved in brainstorming**, individual
sub-project specs to follow one at a time.

This is the sequencing document for replacing NeoOS's hard-wired
"FAT on ATA drive 0" storage with a real stack: a block-device layer
with partitions, AHCI (SATA) and NVMe drivers, a filesystem registry
with probing and a mount policy, and an ext2 driver that becomes the
default root filesystem.

Implementation **plans** (`docs/superpowers/plans/`) are written per
sub-project when it is picked up, against the code as it is then.

## Relationship to the current project goal

`CLAUDE.md` makes Gallium3D the project goal. This series exists because
the user directed it; it does not touch the graphics stack. It does
touch every QEMU invocation and every disk-image recipe, which the
graphics targets (`make desktop`, `make wm*`) also use — those must keep
working at every step.

## Why (what is hard-coded today)

Found by reading the kernel (2026-09-23), not assumed:

| layer | today | where |
|---|---|---|
| block devices | none: `"hd0"`/`"hd1"` strings parsed into ATA drive numbers inside the FAT driver; `blkcache` keyed by ATA drive int | `kernel/fs/fatfs.c:2021`, `kernel/fs/blkcache.h` |
| controllers | legacy IDE PIO only (`ata.c`, 239 lines); no AHCI, no NVMe, no virtio-blk | `kernel/drivers/block/` |
| partitions | none: FAT starts at LBA 0 of a raw disk | `fatfs.c:134` |
| fs selection | a `str_eq` chain in `vfs_mount_fs`; no probing | `kernel/fs/vfs.c:250-310` |
| mounts | `/` = FAT on hd0 and `/mnt` = FAT on hd1, hard-coded; no kernel cmdline, no fstab | `kernel/kernel.c:254-260` |
| boot selftests | `fat16_mount/selftest/write_selftest` assume FAT on drive 0; `vfs_selftest` asserts FAT fixture files | `kernel.c:248-250`, `vfs.c:712-750` |
| images | `mkfs.fat` + ~30 targets populating via mtools (`mmd`/`mcopy`/`mdel`) | `Makefile:263-452` and per-test targets |

## The sub-projects

| # | sub-project | delivers | depends on |
|---|---|---|---|
| 00 | this roadmap | order, shared decisions | — |
| 01 | Block layer + partitions | `struct blockdev` registry (name, sector size, sector count, read/write/flush ops); legacy ATA PIO re-homed as a blockdev driver; MBR + GPT scanner registering child partition blockdevs; `blkcache` keyed by blockdev and sector-size-aware; `/dev/<name>` block nodes in devfs; FAT driver moved onto blockdevs (still whole-disk) | — |
| 02 | AHCI (SATA) | PCI class 01:06 discovery, BAR5 ABAR, per-port command list / FIS / command tables, READ/WRITE DMA EXT (LBA48) + FLUSH CACHE EXT, polled completion; ports register as `sdX` | 01 |
| 03 | NVMe | PCI class 01:08:02, BAR0, admin queue + one I/O queue pair, Identify controller/namespace, Read/Write/Flush, polled completion; namespaces register as `nvmeXnY` | 01 |
| 04 | FS registry + mount policy | `register_filesystem()` with per-driver `probe(blockdev)`; `mount(2)` accepts `"auto"`; pseudo filesystems register too; the `vfs.c` name chain disappears; Multiboot2 cmdline parsed for `root=` / `rootfstype=` (default: probe for root); `/etc/fstab` mounted by init; `kernel.c` mounts only root + pseudo filesystems | 01 |
| 05 | ext2 + default switch | ext2 revision-1 driver (read/write); images become GPT + ext2 via `mke2fs -d` staging dir + `tools/imgput.sh` (debugfs) for per-test injection; both disks ext2; legacy `fat16_*` boot selftests removed; `make fattest` keeps FAT covered; os-builder + docs updated | 01, 04 (02/03 for the new QEMU layout) |
| 06 | Unix semantics | symlinks, hard links, on-disk mode/uid/gid, `chmod`/`chown`/`utimensat`/`symlink`/`link`/`readlink` through VFS + syscalls + shim | 05 — **later milestone**, not part of this series' "done" |

Order: 01 → 02 → 03 → 04 → 05. 02 and 03 are independent of each other
and of 04; 04 is independent of 02/03. The listed order is the default
because it moves the QEMU disks onto the new controllers before the
filesystem switch, so 05 changes one thing at a time.

Every sub-project ends with the kernel booting, `make test` green and
the gauntlet at its bar (15/15 zero-retry), per repo convention.

## Cross-cutting decisions

**D1. Linux-shaped device names.** Device names are visible to userland
through `mount(2)` sources, `/dev`, and fstab, so they follow Linux:
`sda`, `sdb` (SATA *and* legacy ATA, as libata does), `nvme0n1`;
partitions `sda1`, `nvme0n1p1` (the `p` separator when the disk name ends
in a digit). Block nodes in `/dev` report `S_IFBLK`. Legacy `"hd0"`
source strings stop being accepted once 01 lands (no compat alias — no
application ever used them).

**D2. Partitioned images from sub-project 05 on.** New images carry a GPT
with one Linux-filesystem partition (`0FC63DAF-8483-4772-8E79-3D69D8477DE4`).
Whole-disk (unpartitioned) filesystems keep working through probing —
the FAT test image and any old image rely on that.

**D3. QEMU disk layout.** The `pc` machine stays (no `-M q35`: it would
renumber every PCI device and IRQ the NIC reroute and `AC97 addr=0x6`
rely on). Controllers are added explicitly:

| image | bus | device | mounted at |
|---|---|---|---|
| `build/disk.img` | `-device ahci,id=sata` port 0 (`ide-hd,bus=sata.0`) | `sda` / `sda1` | `/` (`root=/dev/sda1`) |
| `build/disk2.img` | `-device nvme,serial=neoos2` | `nvme0n1` / `nvme0n1p1` | `/mnt` via `/etc/fstab` |
| `build/neoos.iso` | IDE (unchanged) | — | — (GRUB boot) |

This layout lands in two steps: disk.img moves to AHCI in 02, disk2.img
to NVMe in 03. Every place that launches QEMU changes with it:
`QEMU_COMMON`, `tools/gauntlet.sh`, `tools/boot_until.sh`, and
neoos-os-builder's `qemu-run.sh.template` / `Makefile`.

**D4. Polled completion first.** AHCI and NVMe poll for completion.
`vfs.c` calls `read_inode` under the vnode-hash spinlock, which is legal
only because disk I/O never sleeps; interrupt-driven, sleeping I/O
requires moving I/O out from under that lock first. That is a named
follow-up (after 05), not something smuggled into a driver.

**D5. Legacy paths keep a test.** ATA PIO stays as a fallback driver and
gets `make atatest` (boots with disks on IDE); FAT stays compiled and
probeable and gets `make fattest` (boots a FAT image at `/mnt`). Neither
is mounted by the default boot after 05.

**D6. Each driver proves itself in `make test`.** AHCI and NVMe each add
a required marker (e.g. `ahci: port 0 sda <n> sectors`,
`nvme: nvme0n1 <n> blocks`), and a check asserts `/` is on `sda1` and
`/mnt` on `nvme0n1p1` — so a silent fall-back to legacy ATA fails the
test rather than passing it.

**D7. ABI obligations** (per `CLAUDE.md`). New user-visible surface —
block nodes in `/dev`, `mount(2)` sources and `"auto"`, `S_IFBLK`,
`/etc/fstab` format, statfs `f_type` for ext2 (`0xEF53`) — gets its
shim/`lib/` path, a `docs/stdlib.md` entry, and an
`docs/abi-compatibility.md` refresh at the end of each sub-project.

**D8. os-builder stays in sync.** Its image and QEMU template change in
the same sub-project as the kernel's (02, 03, 05), per the
"Keeping neoos-os-builder in sync" rule in `CLAUDE.md`.

## Known constraints carried forward (not fixed by this series)

- VFS file positions and `vnode.size` are 32-bit (`vfs.h:72`); ext2
  files above 4 GiB are out of reach until that widens.
- No sleeping I/O (D4); the global `fs_lock` serialises all filesystem
  work.
- `blkcache` is 64 KiB of 512-byte entries, write-through; 01 makes it
  sector-size-aware (NVMe namespaces may be 4 KiB-formatted) but does
  not grow it into a page cache.

## Follow-ups after this series

- 06 (Unix semantics) — its own spec.
- Interrupt-driven AHCI/NVMe with sleeping waits (D4).
- virtio-blk (cheap once 01 exists).
