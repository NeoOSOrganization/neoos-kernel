#ifndef NEOOS_EMBEDFS_H
#define NEOOS_EMBEDFS_H

#include "fs/vfs.h"

// A read-only filesystem serving files linked directly into the kernel
// image (ld -r -b binary blobs), never touching real storage. Exists
// because executables shouldn't have to be loaded from the filesystem
// they may themselves be testing (VFS/FAT tests), and because ramfs's
// RAMFS_MAX_PAGES (16KiB/file) is far too small for a linked-in
// program like BusyBox. See docs/superpowers/specs/
// 2026-09-05-embedded-test-and-app-architecture.md.
//
// The backing table (g_embedfs_table/g_embedfs_table_count) is
// supplied by a build-generated translation unit (tools/gen-embedfs.py
// -> build/embedfs_table.c), not by this file -- embedfs.c only reads
// it. mount(m, source) takes "bin", "sbin", or "tests" as the
// category this mount serves; three separate vfs_mount_fs calls (one
// per category) share the same table.
// `end` rather than a precomputed `size`: GCC will fold a linker
// symbol's address into a pointer-typed static initializer (a
// relocation record), but rejects casting that address to an integer
// in one -- so the byte count is computed at runtime (`end - data`)
// instead of at link time.
struct embedfs_entry {
    const char *category;  // "bin", "sbin", or "tests"
    const char *name;      // e.g. "busybox.nex" -- exact match, no path
    const void *data;
    const void *end;
};

extern const struct vfs_ops embedfs_ops;

#endif
