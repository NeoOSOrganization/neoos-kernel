#include "fs/devfs.h"
#include "fs/file.h"
#include "lib/rand.h"
#include "sync/poll_head.h"
#include "sched/proc.h"
#include "errno.h"
#include "drivers/char/serial.h"
#include "tty/tty.h"
#include "drivers/input/evdev.h"
#include "drivers/video/fb.h"
#include "drivers/audio/ac97.h"
#include "tty/pty.h"
#include "tty/vt.h"
#include <stddef.h>

// Forward declarations of file_ops implementations
extern const struct file_ops tty_file_ops;

// null and zero device file_ops (simple implementations)
static int64_t null_read(void *buf, uint32_t len) { (void)buf; (void)len; return 0; }
static int64_t null_write(const void *buf, uint32_t len) { (void)buf; return (int64_t)len; }

static int64_t null_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    return null_read(buf, (uint32_t)len);
}

static int64_t null_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    return null_write(buf, (uint32_t)len);
}

static int64_t null_fop_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t null_fop_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t null_fop_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static int null_fop_poll(struct file_descriptor *f, int events) {
    (void)f;
    return events;  // null/zero are always ready
}

static void null_fop_dup(struct file_descriptor *f) { (void)f; }
static void null_fop_close(struct file_descriptor *f) { (void)f; }

// CS5.2. /dev/null and its neighbours are always ready, so its readiness never
// changes and there is nothing to be woken about. The shared
// never-notified head says so, which is what keeps a poller holding only
// such fds off the global broadcast entirely.
static struct poll_head *null_poll_head(struct file_descriptor *f) {
    (void)f;
    return poll_head_always_ready();
}

static const struct file_ops null_file_ops = {
    .name     = "null",
    .read     = null_fop_read,
    .write    = null_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .poll_head = null_poll_head,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};

// zero device is similar to null but reads return zeros
static int64_t zero_read(void *buf, uint32_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (uint32_t i = 0; i < len; i++) { b[i] = 0; }
    return (int64_t)len;
}

static int64_t zero_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    return zero_read(buf, (uint32_t)len);
}

static const struct file_ops zero_file_ops = {
    .name     = "zero",
    .read     = zero_fop_read,
    .write    = null_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .poll_head = null_poll_head,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};

// /dev/kmsg -- write goes straight to the serial port and nowhere else.
// This is where system/test programs send their diagnostic chatter so
// the framebuffer console stays clean for the banner, the VTs, and a
// userland terminal. read returns EOF.
static int64_t kmsg_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    serial_write_raw_n((const char *)buf, (uint32_t)len);
    return (int64_t)len;
}
static int64_t kmsg_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f; (void)buf; (void)len;
    return 0;
}
static const struct file_ops kmsg_file_ops = {
    .name     = "kmsg",
    .read     = kmsg_fop_read,
    .write    = kmsg_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .poll_head = null_poll_head,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};
// /dev/urandom and /dev/random -- both the same CSPRNG (kernel/lib/rand.c),
// both non-blocking.
//
// Linux stopped distinguishing them in 5.6: once the pool is seeded,
// /dev/random no longer blocks and returns the same bytes /dev/urandom
// does. NeoOS seeds once at boot from the RTC, the TSC, a stack address
// and RDRAND where available, and never blocks -- so making the two
// devices differ would be inventing a distinction Linux has removed.
//
// Recorded in docs/stdlib.md: this is a CSPRNG seeded once, not an
// entropy pool with reseeding, so it is suitable for salts, cookies and
// stack guards rather than for long-term key material.
static int64_t random_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    if (len == 0) { return 0; }
    rand_bytes(buf, len);
    return (int64_t)len;
}

// Writing is accepted and DISCARDED. On Linux a write mixes into the
// pool; here there is no pool to mix into, and reporting an error would
// break the common `cat something > /dev/urandom` idiom for no gain.
static int64_t random_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f; (void)buf;
    return (int64_t)len;
}

static const struct file_ops random_file_ops = {
    .name     = "random",
    .read     = random_fop_read,
    .write    = random_fop_write,
    .lseek    = null_fop_lseek,
    .getdents = null_fop_getdents,
    .ioctl    = null_fop_ioctl,
    .poll     = null_fop_poll,
    .poll_head = null_poll_head,
    .dup      = null_fop_dup,
    .close    = null_fop_close,
};

static int random_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &random_file_ops;
    return 0;
}

static int kmsg_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &kmsg_file_ops;
    return 0;
}

// Device open functions
static int null_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &null_file_ops;
    return 0;
}

static int zero_open(struct file_descriptor *f) {
    f->priv = NULL;
    f->ops = &zero_file_ops;
    return 0;
}

static int tty_open(struct file_descriptor *f) {
    // TTY opens as the console, with priv pointing to the console tty
    f->ops = &tty_file_ops;
    // The tty_file_ops will handle priv initialization
    return 0;
}

// /dev/tty0 .. /dev/tty6 all share vt_file_ops; what distinguishes them
// is the VT index, which is the digit at the end of the device name.
// devfs is the only place that knows those names, so the mapping lives
// here and vt.c reads the result out of f->priv. Index 0 (/dev/tty0)
// means "the active VT", resolved per call.
static int vt_dev_open(struct file_descriptor *f) {
    const struct devfs_dev *dev = f->vn ? (const struct devfs_dev *)f->vn->fs_private : 0;
    int index = 0;
    if (dev && dev->name) {
        const char *n = dev->name;
        while (n[1]) { n++; }                       // last character
        if (*n >= '0' && *n <= '9') { index = *n - '0'; }
    }
    f->ops  = &vt_file_ops;
    f->priv = vt_fd_new(index);
    if (!f->priv) { return -ENOMEM; }
    return 0;
}

// Device table using the new devfs_dev structure with file_ops
static const struct devfs_dev devices[] = {
    // Format: { name, type, fops, open }
    // Note: index 0 is reserved for root "/"
    { "console", VNODE_DEVICE, &tty_file_ops, tty_open },
    { "tty",     VNODE_DEVICE, &ctty_file_ops, ctty_open },
    { "null",    VNODE_DEVICE, &null_file_ops, null_open },
    { "zero",    VNODE_DEVICE, &zero_file_ops, zero_open },
    { "input",   VNODE_DIR,    NULL,           NULL },
    { "input/event0", VNODE_DEVICE, &evdev_file_ops, evdev_devfs_open },
    { "input/event1", VNODE_DEVICE, &evdev_file_ops, evdev_mouse_devfs_open },
    { "fb0",     VNODE_DEVICE, &fb_file_ops,  fb_open },
    { "ptmx",    VNODE_DEVICE, NULL,          ptmx_open },
    { "kmsg",    VNODE_DEVICE, &kmsg_file_ops, kmsg_open },
    { "urandom", VNODE_DEVICE, &random_file_ops, random_open },
    { "random",  VNODE_DEVICE, &random_file_ops, random_open },
    // The six kernel virtual terminals, plus tty0 = whichever is active.
    { "tty0",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty1",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty2",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty3",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty4",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty5",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    { "tty6",    VNODE_DEVICE, &vt_file_ops,  vt_dev_open },
    // Static parent for the dynamic /dev/pts/N entries (Task 6/7).
    // Must stay last: DEVFS_PTS_INODE == DEVFS_COUNT.
    { "snd",              VNODE_DIR,    NULL,                    NULL },
    { "snd/controlC0",    VNODE_DEVICE, &ac97_control_file_ops,  ac97_control_open },
    { "snd/pcmC0D0p",     VNODE_DEVICE, &ac97_pcm_file_ops,      ac97_pcm_open },
    { "pts",     VNODE_DIR,    NULL,          NULL },
};
#define DEVFS_COUNT (sizeof(devices) / sizeof(devices[0]))
#define DEVFS_PTS_INODE  DEVFS_COUNT      // "pts" is the last entry -> inode_id == DEVFS_COUNT

// ---- dynamic entries (/dev/pts/N) --------------------------------------
#include "sync/lock.h"
static int name_eq(const char *a, const char *b);
// Every /dev/pts/N registers one of these, so this is the REAL ceiling
// on concurrent ptys -- it was 32, and the pty pool growing past 16 in
// CS4 simply ran into it instead. Sized to match PTY_MAX (tty/pty.c) so
// the pool's own bound is the one that applies, at roughly 80 bytes an
// entry.
#define DEVFS_DYN_MAX  256
#define DEVFS_DYN_BASE 1000              // synthetic inode ids start here

static struct {
    char  path[24];                     // e.g. "pts/3"
    const struct file_ops *ops;
    void *priv;
    int (*open)(struct file_descriptor *f);
    struct devfs_dev dev;                // synthesized; fs_private points here
    int   used;
} dyn[DEVFS_DYN_MAX];
static struct spinlock dyn_lock;
static int dyn_lock_ready;

static int dyn_open(struct file_descriptor *f) {
    int slot = (int)(f->vn->inode_id - DEVFS_DYN_BASE);
    if (slot < 0 || slot >= DEVFS_DYN_MAX || !dyn[slot].used) { return -ENOENT; }
    f->ops  = dyn[slot].ops;
    f->priv = dyn[slot].priv;
    if (dyn[slot].open) { return dyn[slot].open(f); }
    return 0;
}

int devfs_register(const char *path, const struct file_ops *ops, void *priv,
                   int (*open)(struct file_descriptor *f)) {
    if (!dyn_lock_ready) { spin_init(&dyn_lock, LOCK_RANK_DEVFS, "devfs-dyn"); dyn_lock_ready = 1; }
    uint64_t fl = spin_lock_irqsave(&dyn_lock);
    int free_slot = -1;
    for (int i = 0; i < DEVFS_DYN_MAX; i++) {
        if (dyn[i].used && name_eq(dyn[i].path, path)) { spin_unlock_irqrestore(&dyn_lock, fl); return -EEXIST; }
        if (!dyn[i].used && free_slot < 0) { free_slot = i; }
    }
    if (free_slot < 0) { spin_unlock_irqrestore(&dyn_lock, fl); return -ENOSPC; }
    int j = 0;
    while (path[j] && j < (int)sizeof(dyn[free_slot].path) - 1) { dyn[free_slot].path[j] = path[j]; j++; }
    dyn[free_slot].path[j] = 0;
    dyn[free_slot].ops  = ops;
    dyn[free_slot].priv = priv;
    dyn[free_slot].open = open;
    dyn[free_slot].dev  = (struct devfs_dev){ dyn[free_slot].path, VNODE_DEVICE, ops, dyn_open };
    dyn[free_slot].used = 1;
    spin_unlock_irqrestore(&dyn_lock, fl);
    return 0;
}

void devfs_unregister(const char *path) {
    if (!dyn_lock_ready) { return; }
    uint64_t fl = spin_lock_irqsave(&dyn_lock);
    for (int i = 0; i < DEVFS_DYN_MAX; i++) {
        if (dyn[i].used && name_eq(dyn[i].path, path)) { dyn[i].used = 0; break; }
    }
    spin_unlock_irqrestore(&dyn_lock, fl);
}

// name is the leaf under /dev/pts. Returns the synthetic inode id or 0.
static uint64_t dyn_lookup_pts(const char *leaf) {
    if (!dyn_lock_ready) { return 0; }
    char want[24] = "pts/";
    int k = 4;
    while (leaf[k - 4] && k < (int)sizeof(want) - 1) { want[k] = leaf[k - 4]; k++; }
    want[k] = 0;
    uint64_t id = 0;
    uint64_t fl = spin_lock_irqsave(&dyn_lock);
    for (int i = 0; i < DEVFS_DYN_MAX; i++) {
        if (dyn[i].used && name_eq(dyn[i].path, want)) { id = DEVFS_DYN_BASE + i; break; }
    }
    spin_unlock_irqrestore(&dyn_lock, fl);
    return id;
}

// Root entry (reserved, not in the devices table)
static const struct devfs_dev root_dev = {
    .name = "/",
    .type = VNODE_DIR,
    .fops = NULL,
    .open = NULL,
};

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Simple memcpy for use in devfs.c
static void *memcpy_local(void *dest, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

static int devfs_mount_op(struct vfs_mount *m, const char *source) {
    (void)source;
    m->fs_private = 0;
    return 0;
}

static void devfs_umount_op(struct vfs_mount *m) { m->fs_private = 0; }

static int devfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    (void)m;
    // inode_id 0 is the root directory
    if (inode_id == 0) {
        out->type = VNODE_DIR;
        out->size = 0;
        out->fs_private = (void *)&root_dev;
        return 0;
    }
    // Dynamic /dev/pts/N entries.
    if (inode_id >= DEVFS_DYN_BASE) {
        int slot = (int)(inode_id - DEVFS_DYN_BASE);
        if (slot < 0 || slot >= DEVFS_DYN_MAX || !dyn[slot].used) { return -ENOENT; }
        out->type = VNODE_DEVICE;
        out->size = 0;
        out->fs_private = (void *)&dyn[slot].dev;
        return 0;
    }
    // inode_id 1+ are device entries
    if (inode_id > DEVFS_COUNT) { return -ENOENT; }
    const struct devfs_dev *dev = &devices[inode_id - 1];
    out->type = dev->type;
    out->size = 0;
    out->fs_private = (void *)dev;
    return 0;
}

static int devfs_sync_inode(struct vnode *vn) { (void)vn; return 0; }

// Length of the directory component of "dir/leaf", or 0 if the entry is
// flat. Everything below is derived from the table with this rather
// than from hardcoded inode numbers.
static uint64_t devfs_dir_len(const char *entry) {
    for (uint64_t i = 0; entry[i]; i++) {
        if (entry[i] == '/') { return i; }
    }
    return 0;
}

static int devfs_name_is(const char *entry, uint64_t len, const char *name) {
    for (uint64_t i = 0; i < len; i++) {
        if (name[i] != entry[i] || name[i] == 0) { return 0; }
    }
    return name[len] == 0;
}

static int devfs_copy_name(struct vfs_dirent *out, const char *src) {
    int j = 0;
    while (src[j] && j < VFS_NAME_MAX - 1) { out->name[j] = src[j]; j++; }
    out->name[j] = 0;
    return j;
}

static int devfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    // /dev/pts is the one directory whose children are made at runtime
    // rather than listed in the table.
    if (dir && dir->inode_id == DEVFS_PTS_INODE) {
        uint64_t id = dyn_lookup_pts(name);
        if (id) { *out_inode_id = id; return 0; }
        return -ENOENT;
    }

    // Any other directory: match "<thisdir>/<name>" against the table.
    //
    // This used to be one hardcoded block per directory -- inode 5 meant
    // "input" with its only child at inode 6, inode 20 meant "snd" with
    // children 21 and 22 -- and the table carried comments warning that
    // entries must not be reordered or those numbers would quietly point
    // at the wrong device. Deriving the ids is what made room for
    // input/event1 without renumbering anything.
    if (dir && dir->inode_id > 0 && dir->inode_id <= DEVFS_COUNT &&
        devices[dir->inode_id - 1].type == VNODE_DIR) {
        const char *dname = devices[dir->inode_id - 1].name;
        for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
            uint64_t dl = devfs_dir_len(devices[i].name);
            if (!dl) { continue; }
            if (devfs_name_is(devices[i].name, dl, dname) &&
                name_eq(devices[i].name + dl + 1, name)) {
                *out_inode_id = i + 1;
                return 0;
            }
        }
        return -ENOENT;
    }

    // Root: flat entries only. Each subdirectory has its own table
    // entry, so "input" is found here and "input/event0" is skipped.
    for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
        if (devfs_dir_len(devices[i].name)) { continue; }
        if (name_eq(devices[i].name, name)) {
            *out_inode_id = i + 1;
            return 0;
        }
    }
    return -ENOENT;
}

// devfs devices now use file_ops for read/write, handled through the file descriptor.
// These VFS operations are no longer used for device files.
static int64_t devfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len;
    return -EINVAL;  // Devices must be opened through a file descriptor
}

static int64_t devfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    (void)vn; (void)pos; (void)buf; (void)len;
    return -EINVAL;  // Devices must be opened through a file descriptor
}

// devfs is read-only as a namespace: its node set is fixed at compile
// time. These return -EPERM rather than being NULL so callers never
// have to know which driver they are talking to.
static int devfs_create(struct vnode *dir, const char *name, uint64_t *out_id) {
    (void)dir; (void)name; (void)out_id; return -EPERM;
}
static int devfs_mkdir(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_unlink(struct vnode *dir, const char *name) {
    (void)dir; (void)name; return -EPERM;
}
static int devfs_truncate(struct vnode *vn) { (void)vn; return -EPERM; }

static int devfs_readdir(struct vnode *dir, uint32_t index, struct vfs_dirent *out) {
    // /dev/pts: the used dynamic slots, in slot order.
    if (dir && dir->inode_id == DEVFS_PTS_INODE) {
        uint32_t seen = 0;
        for (int i = 0; i < DEVFS_DYN_MAX; i++) {
            if (!dyn[i].used) { continue; }
            if (seen == index) {
                devfs_copy_name(out, dyn[i].path + 4);   // skip "pts/"
                out->type = DT_CHR;
                out->ino  = DEVFS_DYN_BASE + i;
                return 0;
            }
            seen++;
        }
        return -ENOENT;
    }

    // Any other directory: its children are the table entries carrying
    // its name as a prefix.
    if (dir && dir->inode_id > 0 && dir->inode_id <= DEVFS_COUNT &&
        devices[dir->inode_id - 1].type == VNODE_DIR) {
        const char *dname = devices[dir->inode_id - 1].name;
        uint32_t seen = 0;
        for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
            uint64_t dl = devfs_dir_len(devices[i].name);
            if (!dl || !devfs_name_is(devices[i].name, dl, dname)) { continue; }
            if (seen == index) {
                devfs_copy_name(out, devices[i].name + dl + 1);
                out->type = (devices[i].type == VNODE_DIR) ? DT_DIR : DT_CHR;
                out->ino  = i + 1;
                return 0;
            }
            seen++;
        }
        return -ENOENT;
    }

    // Root. Flat entries only, and a directory is reported as one --
    // the previous version listed the first hierarchical entry's parent
    // once and skipped every other, so /dev never showed "snd" at all.
    uint32_t count = 0;
    for (uint64_t i = 0; i < DEVFS_COUNT; i++) {
        if (devfs_dir_len(devices[i].name)) { continue; }
        if (count == index) {
            devfs_copy_name(out, devices[i].name);
            out->type = (devices[i].type == VNODE_DIR) ? DT_DIR : DT_CHR;
            out->ino  = i + 1;
            return 0;
        }
        count++;
    }
    return -ENOENT;
}

static int devfs_truncate_to(struct vnode *vn, uint64_t len) {
    (void)vn; (void)len; return -EINVAL;   // no size-setting on this fs
}

const struct vfs_ops devfs_ops = {
    .mount      = devfs_mount_op,
    .umount     = devfs_umount_op,
    .read_inode = devfs_read_inode,
    .sync_inode = devfs_sync_inode,
    .lookup     = devfs_lookup,
    .read       = devfs_read,
    .write      = devfs_write,
    .create     = devfs_create,
    .mkdir      = devfs_mkdir,
    .unlink     = devfs_unlink,
    .truncate   = devfs_truncate,
    .truncate_to = devfs_truncate_to,
    .readdir    = devfs_readdir,
};

void devfs_selftest(void) {
    static const struct file_ops dummy = { .name = "devfs-dummy" };
    if (devfs_register("pts/7", &dummy, (void *)0x1234, 0) != 0) {
        serial_write_string("[devfs] selftest FAILED: register\n"); return;
    }
    if (devfs_register("pts/7", &dummy, 0, 0) != -EEXIST) {
        serial_write_string("[devfs] selftest FAILED: duplicate register allowed\n");
        devfs_unregister("pts/7"); return;
    }
    struct vnode d; d.inode_id = DEVFS_PTS_INODE;
    uint64_t id = 0;
    if (devfs_lookup(&d, "7", &id) != 0 || id < DEVFS_DYN_BASE) {
        serial_write_string("[devfs] selftest FAILED: lookup\n");
        devfs_unregister("pts/7"); return;
    }
    devfs_unregister("pts/7");
    if (devfs_lookup(&d, "7", &id) == 0) {
        serial_write_string("[devfs] selftest FAILED: resolves after unregister\n"); return;
    }
    serial_write_string("[devfs] selftest passed\n");
}
