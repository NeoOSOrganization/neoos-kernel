#include "dev/pty.h"
#include "dev/tty.h"
#include "fs/file.h"
#include "fs/devfs.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sched/proc.h"
#include "dev/serial.h"
#include "errno.h"

#define PTY_MAX 16

struct pty {
    struct tty slave;       // the /dev/pts/N side: full line discipline
    int  used;
    int  master_refs;       // open master fds; > 0 == master open
    int  slave_refs;        // open slave fds;  > 0 == slave open
    int  index;
};

static struct pty ptys[PTY_MAX];
static struct spinlock pool_lock;
static const struct file_ops ptm_file_ops;
static const struct file_ops pts_file_ops;
static int pts_devfs_open(struct file_descriptor *f);

// The slave backend: its cooked output is what the master reads. Called
// with slave->lock HELD (tty_out_cooked runs under it), so this only
// touches outq; tty.c wakes out_readers once the lock is dropped.
static void pty_output(struct tty *t, const char *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (t->out_len >= TTY_BUF) { break; }
        t->outq[(t->out_head + t->out_len) % TTY_BUF] = s[i];
        t->out_len++;
    }
}
static const struct tty_backend pty_backend = { pty_output };

// ---- refcounting --------------------------------------------------
// A struct pty lives until every fd on either end is closed. The counts
// are ints written under pool_lock; a read without it only risks an
// extra loop or a spurious EOF check, never a crash.

static void pty_ref(int *counter) {
    uint64_t fl = spin_lock_raw(&pool_lock);
    (*counter)++;
    spin_unlock_raw(&pool_lock, fl);
}

static void pty_unref(struct pty *pt, int *counter) {
    uint64_t fl = spin_lock_raw(&pool_lock);
    if (*counter > 0) { (*counter)--; }
    int gone = (pt->used && pt->master_refs == 0 && pt->slave_refs == 0);
    if (gone) { pt->used = 0; }
    spin_unlock_raw(&pool_lock, fl);
    if (!gone) { return; }
    char path[16] = "pts/";
    int n = pt->index, k = 4;
    if (n >= 10) { path[k++] = (char)('0' + n / 10); }
    path[k++] = (char)('0' + n % 10);
    path[k] = 0;
    devfs_unregister(path);
}

static int pty_slave_open(struct pty *pt) {
    uint64_t fl = spin_lock_raw(&pool_lock);
    int r = pt->slave_refs;
    spin_unlock_raw(&pool_lock, fl);
    return r > 0;
}

// ---- master (/dev/ptmx) -----------------------------------------------

static int64_t ptm_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    struct tty *t = &pt->slave;
    char *out = buf;

    uint64_t fl = spin_lock_irqsave(&t->lock);
    for (;;) {
        if (t->out_len > 0) { break; }
        if (!pty_slave_open(pt)) { spin_unlock_irqrestore(&t->lock, fl); return 0; }
        if (f->nonblock) { spin_unlock_irqrestore(&t->lock, fl); return -EAGAIN; }
        // waitq_sleep returns holding t->lock again (see tty_obj_read).
        int rc = waitq_sleep(&t->out_readers, &t->lock);
        if (rc != 0) { spin_unlock_irqrestore(&t->lock, fl); return rc; }
    }
    uint64_t k = 0;
    while (k < len && t->out_len > 0) {
        out[k++] = t->outq[t->out_head];
        t->out_head = (t->out_head + 1) % TTY_BUF;
        t->out_len--;
    }
    spin_unlock_irqrestore(&t->lock, fl);
    return (int64_t)k;
}

static int64_t ptm_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    const char *s = buf;
    for (uint64_t i = 0; i < len; i++) { tty_input_char(&pt->slave, s[i]); }
    return (int64_t)len;
}

static int64_t ptm_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    switch (req) {
    case TIOCGPTN:
        if (!arg) { return -EFAULT; }
        *(int *)arg = pt->index;
        return 0;
    case TIOCSPTLCK:
        return 0;
    case 0x5414:   // TIOCSWINSZ -- stored, no SIGWINCH (M1a divergence)
        if (!arg) { return -EFAULT; }
        { uint64_t fl = spin_lock_irqsave(&pt->slave.lock);
          pt->slave.win = *(const struct winsize_k *)arg;
          spin_unlock_irqrestore(&pt->slave.lock, fl); }
        return 0;
    case 0x5413:   // TIOCGWINSZ
        if (!arg) { return -EFAULT; }
        { uint64_t fl = spin_lock_irqsave(&pt->slave.lock);
          *(struct winsize_k *)arg = pt->slave.win;
          spin_unlock_irqrestore(&pt->slave.lock, fl); }
        return 0;
    default:
        return -ENOTTY;
    }
}

static int ptm_poll(struct file_descriptor *f, int events) {
    struct pty *pt = f->priv;
    if (!pt) { return 0; }
    uint64_t fl = spin_lock_irqsave(&pt->slave.lock);
    int mask = POLLOUT;
    if (pt->slave.out_len > 0) { mask |= POLLIN; }
    spin_unlock_irqrestore(&pt->slave.lock, fl);
    if (!pty_slave_open(pt)) { mask |= POLLIN | POLLHUP; }
    return mask & events;
}

static void ptm_dup(struct file_descriptor *f) {
    struct pty *pt = f->priv;
    if (pt) { pty_ref(&pt->master_refs); }
}

static void ptm_close(struct file_descriptor *f) {
    struct pty *pt = f->priv;
    if (!pt) { return; }
    uint64_t fl = spin_lock_irqsave(&pt->slave.lock);
    pt->slave.hung_up = 1;
    spin_unlock_irqrestore(&pt->slave.lock, fl);
    // No SIGHUP: close() runs from process teardown, an unsafe place to
    // take the proc-table locks signal delivery needs. The EOF is enough
    // for M1a; M2's init hangs sessions up properly. See docs/stdlib.md.
    waitq_wake_all(&pt->slave.readers);
    f->priv = 0;
    pty_unref(pt, &pt->master_refs);
}

static int64_t ptm_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t ptm_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }

static const struct file_ops ptm_file_ops = {
    .name = "ptm", .read = ptm_read, .write = ptm_write, .lseek = ptm_lseek,
    .getdents = ptm_getdents, .ioctl = ptm_ioctl, .poll = ptm_poll,
    .dup = ptm_dup, .close = ptm_close,
};

// ---- slave (/dev/pts/N) --------------------------------------------

static int64_t pts_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    return tty_obj_read(&pt->slave, buf, (uint32_t)len, f->nonblock);
}
static int64_t pts_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    return tty_obj_write(&pt->slave, buf, (uint32_t)len);
}
static int64_t pts_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    struct pty *pt = f->priv;
    if (!pt) { return -EBADF; }
    return tty_obj_ioctl(&pt->slave, req, arg);
}
static int pts_poll(struct file_descriptor *f, int events) {
    struct pty *pt = f->priv;
    if (!pt) { return 0; }
    return tty_obj_poll(&pt->slave, events);
}
static void pts_dup(struct file_descriptor *f) {
    struct pty *pt = f->priv;
    if (pt) { pty_ref(&pt->slave_refs); }
}
static void pts_close(struct file_descriptor *f) {
    struct pty *pt = f->priv;
    if (!pt) { return; }
    f->priv = 0;
    pty_unref(pt, &pt->slave_refs);
    waitq_wake_all(&pt->slave.out_readers);   // last slave gone -> master EOF
}
static int64_t pts_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t pts_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }

static const struct file_ops pts_file_ops = {
    .name = "pts", .read = pts_read, .write = pts_write, .lseek = pts_lseek,
    .getdents = pts_getdents, .ioctl = pts_ioctl, .poll = pts_poll,
    .dup = pts_dup, .close = pts_close,
};

// devfs open hook for a pts slave. dyn_open already set f->priv = pt and
// f->ops = &pts_file_ops.
static int pts_devfs_open(struct file_descriptor *f) {
    struct pty *pt = f->priv;
    if (!pt || !pt->used) { return -ENOENT; }
    pty_ref(&pt->slave_refs);
    struct process *p = current_proc();
    uint64_t fl = spin_lock_irqsave(&pt->slave.lock);
    pt->slave.hung_up = 0;
    if (p && pt->slave.sid == 0) {
        pt->slave.sid     = p->sid;
        pt->slave.fg_pgid = p->pgid;
    }
    spin_unlock_irqrestore(&pt->slave.lock, fl);
    return 0;
}

// ---- lifecycle -----------------------------------------------------

void pty_init(void) {
    spin_init(&pool_lock, LOCK_RANK_PTY, "pty-pool");
}

int ptmx_open(struct file_descriptor *f) {
    uint64_t fl = spin_lock_raw(&pool_lock);
    struct pty *pt = 0;
    for (int i = 0; i < PTY_MAX; i++) {
        if (!ptys[i].used) { pt = &ptys[i]; break; }
    }
    if (pt) {
        pt->used = 1;
        pt->index = (int)(pt - ptys);
        pt->master_refs = 1;
        pt->slave_refs = 0;
    }
    spin_unlock_raw(&pool_lock, fl);
    if (!pt) { return -ENFILE; }

    tty_obj_init(&pt->slave, &pty_backend, pt);

    char path[16] = "pts/";
    int n = pt->index, k = 4;
    if (n >= 10) { path[k++] = (char)('0' + n / 10); }
    path[k++] = (char)('0' + n % 10);
    path[k] = 0;
    int rc = devfs_register(path, &pts_file_ops, pt, pts_devfs_open);
    if (rc != 0) {
        uint64_t g = spin_lock_raw(&pool_lock);
        pt->used = 0; pt->master_refs = 0;
        spin_unlock_raw(&pool_lock, g);
        return rc;
    }

    f->ops  = &ptm_file_ops;
    f->priv = pt;
    return 0;
}
