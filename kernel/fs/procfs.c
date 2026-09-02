// kernel/fs/procfs.c -- just enough /proc for `ps` (BB5).
//
// Measured, not predicted: BusyBox's `ps` is the only thing that has
// asked for /proc, and it asked by name --
//     ps: can't open '/proc': No such file or directory
// -- so this provides exactly what procps_scan reads and nothing else:
//
//     /proc/<pid>/stat       the fields ps parses
//     /proc/<pid>/cmdline    the program name, NUL-terminated
//
// It is a READ-ONLY, synthetic filesystem: there is nothing on disk, and
// every read renders the current state of the process table into a small
// buffer at open time. That is a deliberate divergence from Linux, where
// /proc files are generated per-read; a process that exits between the
// open and the read here reports what it was, rather than an error.
// Recorded in docs/stdlib.md.
//
// What it does NOT provide: /proc/self, /proc/meminfo, /proc/mounts, or
// any of the per-process detail beyond the two files above. Those are
// not missing so much as unasked-for -- the same rule that produced this
// file in the first place.

#include "fs/vfs.h"
#include "fs/procfs.h"
#include "sched/proc.h"
#include "sched/proc_table.h"
#include "mm/heap.h"
#include "errno.h"
#include "drivers/char/serial.h"

// Inode numbering. The root is 0; a process's directory and its two
// files are derived from its pid so no table has to be kept in step
// with the process table.
#define PROC_INO_ROOT     0
#define PROC_INO_BASE     16
#define PROC_PER_PID      4
#define PROC_KIND_DIR     0
#define PROC_KIND_STAT    1
#define PROC_KIND_CMDLINE 2

static uint64_t ino_for(int pid, int kind) {
    return PROC_INO_BASE + (uint64_t)pid * PROC_PER_PID + (uint64_t)kind;
}
static int ino_pid(uint64_t ino) {
    if (ino < PROC_INO_BASE) { return -1; }
    return (int)((ino - PROC_INO_BASE) / PROC_PER_PID);
}
static int ino_kind(uint64_t ino) {
    if (ino < PROC_INO_BASE) { return -1; }
    return (int)((ino - PROC_INO_BASE) % PROC_PER_PID);
}

// ---- rendering ------------------------------------------------------

static int put_str(char *out, int cap, int at, const char *s) {
    while (*s && at < cap - 1) { out[at++] = *s++; }
    return at;
}

static int put_int(char *out, int cap, int at, long v) {
    char d[24];
    int n = 0;
    int neg = v < 0;
    unsigned long u = neg ? (unsigned long)(-v) : (unsigned long)v;
    do { d[n++] = (char)('0' + (u % 10)); u /= 10; } while (u && n < 24);
    if (neg && at < cap - 1) { out[at++] = '-'; }
    while (n > 0 && at < cap - 1) { out[at++] = d[--n]; }
    return at;
}

// Linux's /proc/<pid>/stat, in field order, as far as anything reads it.
// BusyBox's procps_scan wants pid, comm, state, ppid, pgrp, session and
// then a run of numeric fields it mostly discards. The fields NeoOS does
// not track are rendered as 0 rather than omitted: the format is
// positional, so a missing field would silently shift every field after
// it into the wrong slot.
static int render_stat(struct process *p, char *out, int cap) {
    int at = 0;
    at = put_int(out, cap, at, p->pid);
    at = put_str(out, cap, at, " (");
    at = put_str(out, cap, at, p->comm[0] ? p->comm : "?");
    at = put_str(out, cap, at, ") ");
    // State. Linux's letters; NeoOS distinguishes only these three.
    at = put_str(out, cap, at,
                 p->state == PROC_ZOMBIE ? "Z " :
                 p->exiting              ? "Z " : "R ");
    at = put_int(out, cap, at, p->parent_pid);   // ppid
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, p->pgid);         // pgrp
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, p->sid);          // session
    // tty_nr, tpgid, flags, and the fault/time/priority run. All zero:
    // see the note above about positional fields.
    for (int i = 0; i < 18; i++) { at = put_str(out, cap, at, " 0"); }
    at = put_str(out, cap, at, "\n");
    out[at < cap ? at : cap - 1] = '\0';
    return at;
}

static int render_cmdline(struct process *p, char *out, int cap) {
    int at = put_str(out, cap, 0, p->comm[0] ? p->comm : "?");
    if (at < cap - 1) { out[at++] = '\0'; }   // argv[0] is NUL-terminated
    return at;
}

// ---- the process-table walk ----------------------------------------
//
// proc_table_for_each_ref holds a reference and no lock, and is not
// reentrant, so every walk here collects what it needs into a caller
// supplied buffer and does nothing else inside the callback.

#define PROC_MAX_LISTED 256

struct pid_list {
    int pids[PROC_MAX_LISTED];
    int n;
};

static void collect_pid(struct process *p, void *ctx) {
    struct pid_list *l = (struct pid_list *)ctx;
    if (l->n >= PROC_MAX_LISTED) { return; }
    if (p->state == PROC_ZOMBIE) { return; }
    l->pids[l->n++] = p->pid;
}

struct find_ctx {
    int   pid;
    int   found;
    char  buf[512];
    int   len;
    int   kind;
};

static void render_one(struct process *p, void *ctx) {
    struct find_ctx *f = (struct find_ctx *)ctx;
    if (p->pid != f->pid || f->found) { return; }
    f->found = 1;
    f->len = (f->kind == PROC_KIND_CMDLINE)
           ? render_cmdline(p, f->buf, (int)sizeof f->buf)
           : render_stat(p, f->buf, (int)sizeof f->buf);
}

// ---- vfs_ops --------------------------------------------------------

static int procfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    m->fs_private = 0;
    return 0;
}

static void procfs_umount_op(struct vfs_mount *m) { (void)m; }

static int procfs_read_inode(struct vfs_mount *m, uint64_t inode_id,
                             struct vnode *out) {
    (void)m;
    out->fs_private = 0;
    if (inode_id == PROC_INO_ROOT) {
        out->type = VNODE_DIR;
        out->size = 0;
        return 0;
    }
    int kind = ino_kind(inode_id);
    if (kind == PROC_KIND_DIR) {
        out->type = VNODE_DIR;
        out->size = 0;
        return 0;
    }
    if (kind != PROC_KIND_STAT && kind != PROC_KIND_CMDLINE) { return -ENOENT; }
    out->type = VNODE_FILE;
    // Size 0, exactly as Linux reports for /proc files, and for a
    // second reason here: read_inode runs with the VFS's own locks
    // held, and proc_table_for_each_ref takes the process table's lock,
    // which is ranked BELOW them. Rendering the file here to measure it
    // panicked the rank checker on the first boot that tried it. The
    // content is produced in procfs_read, where nothing is held.
    out->size = 0;
    return 0;
}

static int procfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

static int name_to_pid(const char *name) {
    int v = 0;
    if (!name[0]) { return -1; }
    for (const char *c = name; *c; c++) {
        if (*c < '0' || *c > '9') { return -1; }
        v = v * 10 + (*c - '0');
        if (v > 1 << 22) { return -1; }
    }
    return v;
}

static int procfs_lookup(struct vnode *dir, const char *name,
                         uint64_t *out_inode_id) {
    if (dir->inode_id == PROC_INO_ROOT) {
        int pid = name_to_pid(name);
        if (pid < 0) { return -ENOENT; }
        struct pid_list l = { .n = 0 };
        proc_table_for_each_ref(collect_pid, &l);
        for (int i = 0; i < l.n; i++) {
            if (l.pids[i] == pid) { *out_inode_id = ino_for(pid, PROC_KIND_DIR); return 0; }
        }
        return -ENOENT;
    }
    int pid = ino_pid(dir->inode_id);
    if (pid < 0 || ino_kind(dir->inode_id) != PROC_KIND_DIR) { return -ENOTDIR; }

    const char *s = "stat", *c = "cmdline";
    int eq = 1;
    for (int i = 0; ; i++) { if (name[i] != s[i]) { eq = 0; break; } if (!s[i]) { break; } }
    if (eq) { *out_inode_id = ino_for(pid, PROC_KIND_STAT); return 0; }
    eq = 1;
    for (int i = 0; ; i++) { if (name[i] != c[i]) { eq = 0; break; } if (!c[i]) { break; } }
    if (eq) { *out_inode_id = ino_for(pid, PROC_KIND_CMDLINE); return 0; }
    return -ENOENT;
}

static int64_t procfs_read(struct vnode *vn, uint32_t pos, void *buf,
                           uint32_t len) {
    int kind = ino_kind(vn->inode_id);
    if (kind != PROC_KIND_STAT && kind != PROC_KIND_CMDLINE) { return -EISDIR; }

    struct find_ctx f = { .pid = ino_pid(vn->inode_id), .found = 0, .len = 0,
                          .kind = kind };
    proc_table_for_each_ref(render_one, &f);
    if (!f.found) { return 0; }          // the process went away: EOF
    if (pos >= (uint32_t)f.len) { return 0; }

    uint32_t n = (uint32_t)f.len - pos;
    if (n > len) { n = len; }
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) { dst[i] = (uint8_t)f.buf[pos + i]; }
    return (int64_t)n;
}

// Everything that would MODIFY the filesystem. /proc is synthetic and
// read-only, and saying so is better than a silent success.
static int64_t procfs_write(struct vnode *vn, uint32_t pos, const void *buf,
                            uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len; return -EPERM;
}
static int procfs_create(struct vnode *dir, const char *name, uint64_t *out) {
    (void)dir; (void)name; (void)out; return -EPERM;
}
static int procfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int procfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int procfs_truncate(struct vnode *vn) { (void)vn; return -EPERM; }

static void name_from_int(char *out, int v) {
    char d[16];
    int n = 0;
    if (v == 0) { d[n++] = '0'; }
    while (v > 0 && n < 16) { d[n++] = (char)('0' + (v % 10)); v /= 10; }
    int at = 0;
    while (n > 0) { out[at++] = d[--n]; }
    out[at] = '\0';
}

static int procfs_readdir(struct vnode *dir, uint32_t index,
                          struct vfs_dirent *out) {
    if (dir->inode_id == PROC_INO_ROOT) {
        struct pid_list l = { .n = 0 };
        proc_table_for_each_ref(collect_pid, &l);
        if (index >= (uint32_t)l.n) { return -ENOENT; }
        name_from_int(out->name, l.pids[index]);
        out->ino = ino_for(l.pids[index], PROC_KIND_DIR);
        out->type = VNODE_DIR;
        return 0;
    }
    int pid = ino_pid(dir->inode_id);
    if (pid < 0 || ino_kind(dir->inode_id) != PROC_KIND_DIR) { return -ENOTDIR; }
    if (index == 0) {
        out->name[0] = 's'; out->name[1] = 't'; out->name[2] = 'a';
        out->name[3] = 't'; out->name[4] = '\0';
        out->ino = ino_for(pid, PROC_KIND_STAT);
        out->type = VNODE_FILE;
        return 0;
    }
    if (index == 1) {
        const char *c = "cmdline";
        for (int i = 0; i < 8; i++) { out->name[i] = c[i]; }
        out->ino = ino_for(pid, PROC_KIND_CMDLINE);
        out->type = VNODE_FILE;
        return 0;
    }
    return -ENOENT;
}

const struct vfs_ops procfs_ops = {
    .mount      = procfs_mount_op,
    .umount     = procfs_umount_op,
    .read_inode = procfs_read_inode,
    .sync_inode = procfs_sync_inode,
    .lookup     = procfs_lookup,
    .read       = procfs_read,
    .write      = procfs_write,
    .create     = procfs_create,
    .mkdir      = procfs_mkdir,
    .unlink     = procfs_unlink,
    .truncate   = procfs_truncate,
    .readdir    = procfs_readdir,
};
