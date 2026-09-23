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
#include "drivers/char/timer.h"
#include "sched/proc.h"
#include "sched/proc_table.h"
#include "mm/heap.h"
#include "errno.h"
#include "smp/smp.h"
#include "time/ktime.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "drivers/char/serial.h"

// Inode numbering. The root is 0; a process's directory and its two
// files are derived from its pid so no table has to be kept in step
// with the process table.
#define PROC_INO_ROOT     0
// System-wide /proc/stat. A fixed inode below PROC_INO_BASE, so it
// cannot collide with a per-pid one.
#define PROC_INO_SYSSTAT  1
#define PROC_INO_MEMINFO  2
#define PROC_INO_UPTIME   3
#define PROC_INO_BASE     16
#define PROC_PER_PID      4
#define PROC_KIND_DIR     0
#define PROC_KIND_STAT    1
#define PROC_KIND_CMDLINE 2
#define PROC_KIND_STATUS  3

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

// What a process is doing, gathered once per render under the locks
// that protect each piece (p->lock for the thread list, mm_lock for the
// address space). Kept here so stat and status agree to the byte.
struct proc_facts {
    char     state;           // Linux's letters: R S T Z
    int      threads;
    int      nice;
    uint64_t cpu_ns;          // user + kernel: the scheduler does not split them
    uint64_t vsize;           // bytes
    uint64_t rss_pages;
};

static const char *state_word(char s) {
    return s == 'R' ? "running" : s == 'S' ? "sleeping" : s == 'T' ? "stopped" : "zombie";
}

static void gather(struct process *p, struct proc_facts *f) {
    f->threads = 0; f->nice = 0; f->cpu_ns = 0; f->vsize = 0; f->rss_pages = 0;
    int running = 0, stopped = 0, first = 1;
    uint64_t lf = spin_lock_irqsave(&p->lock);
    f->cpu_ns = p->cpu_ns_exited;
    for (struct thread *t = p->threads; t; t = t->proc_next) {
        f->threads++;
        f->cpu_ns += t->se.sum_exec_runtime;
        if (first) { f->nice = t->se.nice; first = 0; }
        if (t->state == THREAD_RUNNING || t->state == THREAD_READY) { running = 1; }
        if (t->state == THREAD_STOPPED) { stopped = 1; }
    }
    spin_unlock_irqrestore(&p->lock, lf);
    if (p->state == PROC_ZOMBIE || p->exiting) { f->state = 'Z'; return; }
    f->state = running ? 'R' : stopped ? 'T' : 'S';

    uint64_t mf = spin_lock_irqsave(&p->mm_lock);
    for (struct vma *v = p->vmas; v; v = v->next) { f->vsize += v->end - v->start; }
    f->rss_pages = paging_count_present_user(p->pml4_phys);
    spin_unlock_irqrestore(&p->mm_lock, mf);
}

// Linux's /proc/<pid>/stat, in field order, through field 24 (rss) --
// what procps, BusyBox ps/top and Task Manager read. Fields NeoOS does
// not track are 0, never omitted: the format is positional.
static int render_stat(struct process *p, char *out, int cap) {
    struct proc_facts f;
    gather(p, &f);
    int at = 0;
    at = put_int(out, cap, at, p->pid);
    at = put_str(out, cap, at, " (");
    at = put_str(out, cap, at, p->comm[0] ? p->comm : "?");
    at = put_str(out, cap, at, ") ");
    char st[3] = { f.state, ' ', 0 };
    at = put_str(out, cap, at, st);                          // 3 state
    at = put_int(out, cap, at, p->parent_pid);               // 4 ppid
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, p->pgid);                     // 5 pgrp
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, p->sid);                      // 6 session
    at = put_str(out, cap, at, " 0 0 0 0 0 0 0 ");           // 7 tty_nr .. 13 cmajflt
    at = put_int(out, cap, at, (long)(f.cpu_ns / TICK_NS));  // 14 utime (100 Hz ticks)
    at = put_str(out, cap, at, " 0 0 0 ");                   // 15 stime, 16 cutime, 17 cstime
    at = put_int(out, cap, at, 20 + f.nice);                 // 18 priority
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, f.nice);                      // 19 nice
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, f.threads);                   // 20 num_threads
    at = put_str(out, cap, at, " 0 ");                       // 21 itrealvalue
    at = put_int(out, cap, at, (long)(p->start_ns / TICK_NS)); // 22 starttime
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, (long)f.vsize);               // 23 vsize (bytes)
    at = put_str(out, cap, at, " ");
    at = put_int(out, cap, at, (long)f.rss_pages);           // 24 rss (pages)
    at = put_str(out, cap, at, "\n");
    out[at < cap ? at : cap - 1] = '\0';
    return at;
}

// /proc/<pid>/status: the human-readable subset tools grep for.
static int render_status(struct process *p, char *out, int cap) {
    struct proc_facts f;
    gather(p, &f);
    int at = 0;
    at = put_str(out, cap, at, "Name:\t");
    at = put_str(out, cap, at, p->comm[0] ? p->comm : "?");
    at = put_str(out, cap, at, "\nState:\t");
    char st[2] = { f.state, 0 };
    at = put_str(out, cap, at, st);
    at = put_str(out, cap, at, " (");
    at = put_str(out, cap, at, state_word(f.state));
    at = put_str(out, cap, at, ")\nTgid:\t");
    at = put_int(out, cap, at, p->pid);
    at = put_str(out, cap, at, "\nPid:\t");
    at = put_int(out, cap, at, p->pid);
    at = put_str(out, cap, at, "\nPPid:\t");
    at = put_int(out, cap, at, p->parent_pid);
    at = put_str(out, cap, at, "\nThreads:\t");
    at = put_int(out, cap, at, f.threads);
    at = put_str(out, cap, at, "\nVmSize:\t");
    at = put_int(out, cap, at, (long)(f.vsize / 1024));
    at = put_str(out, cap, at, " kB\nVmRSS:\t");
    at = put_int(out, cap, at, (long)(f.rss_pages * (PMM_FRAME_SIZE / 1024)));
    at = put_str(out, cap, at, " kB\n");
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
    f->len = (f->kind == PROC_KIND_CMDLINE) ? render_cmdline(p, f->buf, (int)sizeof f->buf)
           : (f->kind == PROC_KIND_STATUS)  ? render_status(p, f->buf, (int)sizeof f->buf)
           :                                  render_stat(p, f->buf, (int)sizeof f->buf);
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
    if (inode_id == PROC_INO_SYSSTAT || inode_id == PROC_INO_MEMINFO || inode_id == PROC_INO_UPTIME) {
        out->type = VNODE_FILE;
        out->size = 0;
        return 0;
    }
    int kind = ino_kind(inode_id);
    if (kind == PROC_KIND_DIR) {
        out->type = VNODE_DIR;
        out->size = 0;
        return 0;
    }
    if (kind != PROC_KIND_STAT && kind != PROC_KIND_CMDLINE && kind != PROC_KIND_STATUS) { return -ENOENT; }
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

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int procfs_lookup(struct vnode *dir, const char *name,
                         uint64_t *out_inode_id) {
    if (dir->inode_id == PROC_INO_ROOT) {
        // System-wide files, distinct from the per-pid ones.
        if (streq(name, "stat"))    { *out_inode_id = PROC_INO_SYSSTAT; return 0; }
        if (streq(name, "meminfo")) { *out_inode_id = PROC_INO_MEMINFO; return 0; }
        if (streq(name, "uptime"))  { *out_inode_id = PROC_INO_UPTIME;  return 0; }
        // /proc/self: the CALLER's directory. A directory alias rather
        // than Linux's symlink -- NeoOS's VFS has no symlinks -- which
        // is invisible to anything that just opens /proc/self/<file>.
        if (streq(name, "self") && current_proc()) {
            *out_inode_id = ino_for(current_proc()->pid, PROC_KIND_DIR);
            return 0;
        }

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

    if (streq(name, "stat"))    { *out_inode_id = ino_for(pid, PROC_KIND_STAT);    return 0; }
    if (streq(name, "cmdline")) { *out_inode_id = ino_for(pid, PROC_KIND_CMDLINE); return 0; }
    if (streq(name, "status"))  { *out_inode_id = ino_for(pid, PROC_KIND_STATUS);  return 0; }
    return -ENOENT;
}

// /proc/stat, in Linux's shape:
//
//   cpu  <user> <nice> <system> <idle> <iowait> <irq> <softirq>
//
// NeoOS does not separate user from system time or account irq/iowait,
// so all busy time is reported as `user` and the rest is zero. A
// desktop reading this samples twice and takes the ratio of the
// deltas, which is exactly what the fields support. The DIVERGENCE --
// no user/system split -- is recorded in docs/stdlib.md.
static int put_cpu_line(char *out, int cap, int at, const char *name, uint64_t busy, uint64_t idle) {
    at = put_str(out, cap, at, name);
    at = put_int(out, cap, at, (long)busy);
    at = put_str(out, cap, at, " 0 0 ");
    at = put_int(out, cap, at, (long)idle);
    return put_str(out, cap, at, " 0 0 0 0 0 0\n");
}

static int render_sysstat(char *out, int cap) {
    uint64_t busy = 0, idle = 0;
    cpu_usage_ticks(&busy, &idle);
    int at = put_cpu_line(out, cap, 0, "cpu  ", busy, idle);
    for (int k = 0; k < smp_online_count(); k++) {
        char name[8] = { 'c', 'p', 'u', 0 };
        int n = 3;
        if (k >= 10) { name[n++] = (char)('0' + k / 10); }
        name[n++] = (char)('0' + k % 10);
        name[n++] = ' ';
        name[n] = 0;
        cpu_usage_ticks_one(k, &busy, &idle);
        at = put_cpu_line(out, cap, at, name, busy, idle);
    }
    out[at] = 0;
    return at;
}

// /proc/meminfo, in kB like Linux. NeoOS has no page cache and no swap,
// so MemAvailable == MemFree and the cache/swap lines are 0.
static int render_meminfo(char *out, int cap) {
    uint64_t kb = PMM_FRAME_SIZE / 1024;
    uint64_t total = pmm_total_frame_count() * kb, free = pmm_free_frame_count() * kb;
    int at = 0;
    at = put_str(out, cap, at, "MemTotal:       "); at = put_int(out, cap, at, (long)total); at = put_str(out, cap, at, " kB\n");
    at = put_str(out, cap, at, "MemFree:        "); at = put_int(out, cap, at, (long)free);  at = put_str(out, cap, at, " kB\n");
    at = put_str(out, cap, at, "MemAvailable:   "); at = put_int(out, cap, at, (long)free);  at = put_str(out, cap, at, " kB\n");
    at = put_str(out, cap, at, "Buffers:        0 kB\nCached:         0 kB\n"
                               "SwapTotal:      0 kB\nSwapFree:       0 kB\n");
    out[at] = 0;
    return at;
}

static int put_centis(char *out, int cap, int at, uint64_t centis) {
    at = put_int(out, cap, at, (long)(centis / 100));
    at = put_str(out, cap, at, ".");
    if (centis % 100 < 10) { at = put_str(out, cap, at, "0"); }
    return put_int(out, cap, at, (long)(centis % 100));
}

// /proc/uptime: seconds since boot, and idle seconds summed over CPUs.
static int render_uptime(char *out, int cap) {
    uint64_t busy = 0, idle = 0;
    cpu_usage_ticks(&busy, &idle);                 // 10 ms units = centiseconds
    int at = put_centis(out, cap, 0, ktime_get_ns() / 10000000ULL);
    at = put_str(out, cap, at, " ");
    at = put_centis(out, cap, at, idle);
    at = put_str(out, cap, at, "\n");
    out[at] = 0;
    return at;
}

static int64_t procfs_read(struct vnode *vn, uint64_t pos, void *buf,
                           uint32_t len) {
    if (vn->inode_id == PROC_INO_SYSSTAT || vn->inode_id == PROC_INO_MEMINFO ||
        vn->inode_id == PROC_INO_UPTIME) {
        char tmp[1024];
        int n = vn->inode_id == PROC_INO_SYSSTAT ? render_sysstat(tmp, (int)sizeof tmp)
              : vn->inode_id == PROC_INO_MEMINFO ? render_meminfo(tmp, (int)sizeof tmp)
              :                                    render_uptime(tmp, (int)sizeof tmp);
        if (pos >= (uint64_t)n) { return 0; }
        uint32_t k = (uint32_t)((uint64_t)n - pos);
        if (k > len) { k = len; }
        for (uint32_t i = 0; i < k; i++) { ((char *)buf)[i] = tmp[pos + i]; }
        return (int64_t)k;
    }
    int kind = ino_kind(vn->inode_id);
    if (kind != PROC_KIND_STAT && kind != PROC_KIND_CMDLINE && kind != PROC_KIND_STATUS) { return -EISDIR; }

    struct find_ctx f = { .pid = ino_pid(vn->inode_id), .found = 0, .len = 0,
                          .kind = kind };
    proc_table_for_each_ref(render_one, &f);
    if (!f.found) { return 0; }          // the process went away: EOF
    if (pos >= (uint64_t)f.len) { return 0; }

    uint32_t n = (uint32_t)((uint64_t)f.len - pos);
    if (n > len) { n = len; }
    uint8_t *dst = (uint8_t *)buf;
    for (uint32_t i = 0; i < n; i++) { dst[i] = (uint8_t)f.buf[pos + i]; }
    return (int64_t)n;
}

// Everything that would MODIFY the filesystem. /proc is synthetic and
// read-only, and saying so is better than a silent success.
static int64_t procfs_write(struct vnode *vn, uint64_t pos, const void *buf,
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

static void name_copy_short(char *out, const char *s) {
    int i = 0;
    while (s[i]) { out[i] = s[i]; i++; }
    out[i] = '\0';
}

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
    static const char *const root_files[] = { "stat", "meminfo", "uptime", "self" };
    static const uint64_t root_inos[] = { PROC_INO_SYSSTAT, PROC_INO_MEMINFO, PROC_INO_UPTIME, 0 };
    if (dir->inode_id == PROC_INO_ROOT) {
        if (index < 4) {
            name_copy_short(out->name, root_files[index]);
            if (index == 3) {
                if (!current_proc()) { return -ENOENT; }
                out->ino = ino_for(current_proc()->pid, PROC_KIND_DIR);
                out->type = VNODE_DIR;
            } else {
                out->ino = root_inos[index];
                out->type = VNODE_FILE;
            }
            return 0;
        }
        index -= 4;
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
    static const char *const pid_files[] = { "stat", "cmdline", "status" };
    static const int pid_kinds[] = { PROC_KIND_STAT, PROC_KIND_CMDLINE, PROC_KIND_STATUS };
    if (index >= 3) { return -ENOENT; }
    name_copy_short(out->name, pid_files[index]);
    out->ino = ino_for(pid, pid_kinds[index]);
    out->type = VNODE_FILE;
    return 0;
}

static int procfs_truncate_to(struct vnode *vn, uint64_t len) {
    (void)vn; (void)len; return -EINVAL;   // no size-setting on this fs
}

static int procfs_rename(struct vnode *od, const char *on, struct vnode *nd, const char *nn) {
    (void)od; (void)on; (void)nd; (void)nn; return -EPERM;
}

static int procfs_rmdir(struct vnode *d, const char *n) { (void)d; (void)n; return -EPERM; }

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
    .truncate_to = procfs_truncate_to,
    .readdir    = procfs_readdir,
    .rename     = procfs_rename,
    .rmdir      = procfs_rmdir,
};
