// kernel/fs/flock.c -- POSIX advisory record locks (fcntl F_GETLK,
// F_SETLK, F_SETLKW). Desktop M0 task 4; SQLite's locking is built on
// these. See docs/stdlib.md for the semantics and divergences.
//
// One global table of lock ranges, one lock over it, one wait queue for
// F_SETLKW sleepers (woken on every release -- a waiter re-checks its
// own range). NeoOS's lock counts are small; a per-vnode structure would
// buy nothing but lifetime questions. Records come from a fixed pool,
// so taking a lock never allocates: a full pool is -ENOLCK, Linux's
// answer for the same condition.
#include "fs/flock.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "sched/proc.h"
#include "errno.h"

#define FLOCK_MAX     512
#define FLOCK_WAITERS 64
#define END_OF_FILE   UINT64_MAX            // l_len 0: "to EOF and beyond"

struct lock_rec {
    int           in_use;
    struct vnode *vn;
    int           pid;
    int           type;                     // F_RDLCK / F_WRLCK
    uint64_t      start, end;               // inclusive
};

// A process sleeping in F_SETLKW, and what it wants -- the edge of the
// wait-for graph deadlock detection walks.
struct waiter {
    int           in_use;
    int           pid;
    struct vnode *vn;
    int           type;
    uint64_t      start, end;
};

static struct lock_rec recs[FLOCK_MAX];
static struct waiter   waiters[FLOCK_WAITERS];
static struct spinlock flock_lock;
static struct waitq    flock_wait;

void flock_init(void) {
    spin_init(&flock_lock, LOCK_RANK_FLOCK, "flock");
    waitq_init(&flock_wait);
}

static int overlaps(uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2) {
    return s1 <= e2 && s2 <= e1;
}

// The first lock held by someone OTHER than pid that forbids taking
// `type` on [s, e] of vn. Two read locks never conflict.
static struct lock_rec *conflict_locked(struct vnode *vn, int pid, int type, uint64_t s, uint64_t e) {
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct lock_rec *r = &recs[i];
        if (!r->in_use || r->vn != vn || r->pid == pid) { continue; }
        if (!overlaps(r->start, r->end, s, e)) { continue; }
        if (r->type == F_WRLCK || type == F_WRLCK) { return r; }
    }
    return 0;
}

static struct lock_rec *rec_alloc_locked(void) {
    for (int i = 0; i < FLOCK_MAX; i++) { if (!recs[i].in_use) { recs[i].in_use = 1; return &recs[i]; } }
    return 0;
}

static int free_recs_locked(void) {
    int n = 0;
    for (int i = 0; i < FLOCK_MAX; i++) { if (!recs[i].in_use) { n++; } }
    return n;
}

// Removes [s, e] from every lock pid holds on vn -- trimming, deleting,
// or splitting one lock around the hole. Returns -ENOLCK only if a
// split needs a record and none is free (checked before changing
// anything, so a failure leaves the table as it was).
static int carve_locked(struct vnode *vn, int pid, uint64_t s, uint64_t e) {
    int splits = 0;
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct lock_rec *r = &recs[i];
        if (r->in_use && r->vn == vn && r->pid == pid && r->start < s && r->end > e) { splits++; }
    }
    if (splits > free_recs_locked()) { return -ENOLCK; }
    for (int i = 0; i < FLOCK_MAX; i++) {
        struct lock_rec *r = &recs[i];
        if (!r->in_use || r->vn != vn || r->pid != pid || !overlaps(r->start, r->end, s, e)) { continue; }
        if (r->start >= s && r->end <= e) { r->in_use = 0; continue; }        // wholly inside
        if (r->start < s && r->end > e) {                                    // hole in the middle
            struct lock_rec *tail = rec_alloc_locked();
            *tail = *r;
            tail->start = e + 1;
            r->end = s - 1;
            continue;
        }
        if (r->start < s) { r->end = s - 1; } else { r->start = e + 1; }    // trim one side
    }
    return 0;
}

// Joins pid's same-type locks on vn that overlap or touch, so the
// table stays minimal (and F_GETLK reports whole ranges, as Linux does).
static void merge_locked(struct vnode *vn, int pid) {
    for (int changed = 1; changed; ) {
        changed = 0;
        for (int i = 0; i < FLOCK_MAX && !changed; i++) {
            struct lock_rec *a = &recs[i];
            if (!a->in_use || a->vn != vn || a->pid != pid) { continue; }
            for (int j = 0; j < FLOCK_MAX; j++) {
                struct lock_rec *b = &recs[j];
                if (j == i || !b->in_use || b->vn != vn || b->pid != pid || b->type != a->type) { continue; }
                int touch = overlaps(a->start, a->end, b->start, b->end) ||
                            (a->end != END_OF_FILE && a->end + 1 == b->start) ||
                            (b->end != END_OF_FILE && b->end + 1 == a->start);
                if (!touch) { continue; }
                if (b->start < a->start) { a->start = b->start; }
                if (b->end > a->end) { a->end = b->end; }
                b->in_use = 0;
                changed = 1;
                break;
            }
        }
    }
}

// Would pid, by sleeping until `holder` lets go, close a cycle? Follows
// holder -> what holder is waiting for -> who holds that, until it
// reaches pid (deadlock) or someone who is not waiting.
static int deadlock_locked(int pid, int holder) {
    for (int hops = 0; hops < FLOCK_WAITERS; hops++) {
        if (holder == pid) { return 1; }
        struct waiter *w = 0;
        for (int i = 0; i < FLOCK_WAITERS; i++) {
            if (waiters[i].in_use && waiters[i].pid == holder) { w = &waiters[i]; break; }
        }
        if (!w) { return 0; }
        struct lock_rec *next = conflict_locked(w->vn, w->pid, w->type, w->start, w->end);
        if (!next) { return 0; }
        holder = next->pid;
    }
    return 0;
}

// l_whence/l_start/l_len -> [start, end], Linux's rules: whence is
// SET/CUR/END; len 0 means to EOF (and past it); a negative len covers
// the bytes BEFORE start.
static int range_of(struct file_descriptor *f, const struct k_flock *fl, uint64_t *s, uint64_t *e) {
    int64_t base;
    if (fl->l_whence == 0)      { base = 0; }
    else if (fl->l_whence == 1) { base = (int64_t)f->position; }
    else if (fl->l_whence == 2) { base = (int64_t)f->vn->size; }
    else { return -EINVAL; }
    int64_t start = base + fl->l_start;
    int64_t len = fl->l_len;
    if (len < 0) { start += len; len = -len; }
    if (start < 0) { return -EINVAL; }
    *s = (uint64_t)start;
    *e = len == 0 ? END_OF_FILE : (uint64_t)start + (uint64_t)len - 1;
    return 0;
}

int flock_fcntl(struct file_descriptor *f, int cmd, struct k_flock *fl) {
    if (!f->vn || f->vn->type != VNODE_FILE) { return -EINVAL; }   // pipes, sockets, dirs
    int type = fl->l_type;
    if (type != F_RDLCK && type != F_WRLCK && type != F_UNLCK) { return -EINVAL; }
    if (cmd != F_GETLK && type == F_WRLCK && !f->writable) { return -EBADF; }   // Linux: write lock needs a writable fd
    uint64_t s, e;
    int rc = range_of(f, fl, &s, &e);
    if (rc != 0) { return rc; }
    struct vnode *vn = f->vn;
    int pid = current_proc() ? current_proc()->pid : 0;

    uint64_t lf = spin_lock_irqsave(&flock_lock);
    if (cmd == F_GETLK) {
        struct lock_rec *c = type == F_UNLCK ? 0 : conflict_locked(vn, pid, type, s, e);
        if (!c) {
            fl->l_type = F_UNLCK;
        } else {
            fl->l_type   = (int16_t)c->type;
            fl->l_whence = 0;
            fl->l_start  = (int64_t)c->start;
            fl->l_len    = c->end == END_OF_FILE ? 0 : (int64_t)(c->end - c->start + 1);
            fl->l_pid    = c->pid;
        }
        spin_unlock_irqrestore(&flock_lock, lf);
        return 0;
    }

    if (type == F_UNLCK) {
        rc = carve_locked(vn, pid, s, e);
        spin_unlock_irqrestore(&flock_lock, lf);
        waitq_wake_all(&flock_wait);
        return rc;
    }

    for (;;) {
        struct lock_rec *c = conflict_locked(vn, pid, type, s, e);
        if (!c) { break; }
        if (cmd == F_SETLK) { spin_unlock_irqrestore(&flock_lock, lf); return -EAGAIN; }
        if (deadlock_locked(pid, c->pid)) { spin_unlock_irqrestore(&flock_lock, lf); return -EDEADLK; }
        struct waiter *w = 0;
        for (int i = 0; i < FLOCK_WAITERS; i++) { if (!waiters[i].in_use) { w = &waiters[i]; break; } }
        if (!w) { spin_unlock_irqrestore(&flock_lock, lf); return -ENOLCK; }
        *w = (struct waiter){ 1, pid, vn, type, s, e };
        // waitq_sleep drops flock_lock only once this thread is queued,
        // and takes it back before returning: a release in between wakes us.
        int wrc = waitq_sleep(&flock_wait, &flock_lock);
        w->in_use = 0;
        if (wrc == -EINTR) { spin_unlock_irqrestore(&flock_lock, lf); return -EINTR; }
    }

    // Take it: whatever pid already held in the range is replaced (a
    // read lock can become a write lock and back), then neighbours merge.
    if (free_recs_locked() < 1) { spin_unlock_irqrestore(&flock_lock, lf); return -ENOLCK; }
    rc = carve_locked(vn, pid, s, e);
    if (rc == 0) {
        struct lock_rec *r = rec_alloc_locked();
        if (!r) { rc = -ENOLCK; }
        else { r->vn = vn; r->pid = pid; r->type = type; r->start = s; r->end = e; merge_locked(vn, pid); }
    }
    spin_unlock_irqrestore(&flock_lock, lf);
    // A write lock downgraded to read (or a range shrunk) can let a
    // waiter through.
    waitq_wake_all(&flock_wait);
    return rc;
}

void flock_release_vnode(struct vnode *vn, int pid) {
    int any = 0;
    uint64_t lf = spin_lock_irqsave(&flock_lock);
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (recs[i].in_use && recs[i].vn == vn && recs[i].pid == pid) { recs[i].in_use = 0; any = 1; }
    }
    spin_unlock_irqrestore(&flock_lock, lf);
    if (any) { waitq_wake_all(&flock_wait); }
}

void flock_release_pid(int pid) {
    int any = 0;
    uint64_t lf = spin_lock_irqsave(&flock_lock);
    for (int i = 0; i < FLOCK_MAX; i++) {
        if (recs[i].in_use && recs[i].pid == pid) { recs[i].in_use = 0; any = 1; }
    }
    spin_unlock_irqrestore(&flock_lock, lf);
    if (any) { waitq_wake_all(&flock_wait); }
}
