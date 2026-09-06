// kernel/ipc/socketpair.c -- socketpair(2), AF_UNIX only.
//
// Two of pipe.c's ring-buffer pipes, cross-wired: end 0 reads pipe A
// and writes pipe B; end 1 reads pipe B and writes pipe A. Each pipe
// ends up with exactly one reader and one writer, same shape as a
// plain two-end pipe -- pipe_init_ends's own initialization already
// fits both cases.
//
// The one thing a plain pipe fd never needs and a socketpair end
// does: ONE fd whose readable side and writable side are two
// DIFFERENT pipe objects. That is why this drives pipe.c's exposed
// _ep functions directly instead of going through pipe_read/
// pipe_write/etc, which assume a single fd names a single pipe.

#include "ipc/socketpair.h"
#include "ipc/pipe.h"
#include "fs/file.h"
#include "sync/poll_head.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "mm/heap.h"
#include "errno.h"
#include "net/socket.h"   // AF_UNIX, SOCK_STREAM, SOCK_DGRAM, SOCK_NONBLOCK
#include "drivers/char/serial.h"

#define SOCK_TYPE_MASK 0xF   // the type-code bits, apart from SOCK_NONBLOCK/SOCK_CLOEXEC
// SOCK_CLOEXEC is accepted in `type` but not tracked, matching
// pipe2(2)'s own existing PIPE_O_CLOEXEC gap on NeoOS.

struct spair_end {
    struct pipe *rd;
    struct pipe *wr;
};

static int64_t spair_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct spair_end *e = (struct spair_end *)f->priv;
    if (!e) { return -EBADF; }
    return pipe_read_ep(e->rd, f->nonblock, buf, len);
}

static int64_t spair_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct spair_end *e = (struct spair_end *)f->priv;
    if (!e) { return -EBADF; }
    return pipe_write_ep(e->wr, f->nonblock, buf, len);
}

static int64_t spair_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t spair_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t spair_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f; (void)request; (void)arg;
    return -ENOTTY;
}

static void spair_dup(struct file_descriptor *f) {
    struct spair_end *e = (struct spair_end *)f->priv;
    if (!e) { return; }
    pipe_dup_ep(e->rd, /*as_reader=*/1, /*as_writer=*/0);
    pipe_dup_ep(e->wr, /*as_reader=*/0, /*as_writer=*/1);
}

static void spair_close(struct file_descriptor *f) {
    struct spair_end *e = (struct spair_end *)f->priv;
    if (!e) { return; }
    f->priv = 0;
    pipe_close_ep(e->rd, /*as_reader=*/1, /*as_writer=*/0);
    pipe_close_ep(e->wr, /*as_reader=*/0, /*as_writer=*/1);
    kfree(e);
}

static int spair_poll(struct file_descriptor *f, int events) {
    struct spair_end *e = (struct spair_end *)f->priv;
    if (!e) { return POLLERR; }
    int mask = pipe_poll_ep(e->rd, /*as_reader=*/1, /*as_writer=*/0, events | POLLIN | POLLHUP);
    mask    |= pipe_poll_ep(e->wr, /*as_reader=*/0, /*as_writer=*/1, events | POLLOUT);
    return mask & events;
}

// A socketpair end's readiness depends on TWO pipes, so it cannot
// register on either one's own poll_head the way a plain pipe fd
// does. Returning 0 opts this fd into poll_core's global broadcast
// instead (see net/socket.c's sock_poll_head for the identical
// reasoning on TCP sockets) -- which is exactly what
// pipe_read_ep/pipe_write_ep/pipe_close_ep's added waitq_poll_notify()
// calls now feed.
static struct poll_head *spair_poll_head(struct file_descriptor *f) {
    (void)f;
    return 0;
}

static const struct file_ops spair_ops = {
    .name      = "socketpair",
    .read      = spair_read,
    .write     = spair_write,
    .lseek     = spair_lseek,
    .getdents  = spair_getdents,
    .ioctl     = spair_ioctl,
    .poll      = spair_poll,
    .poll_head = spair_poll_head,
    .dup       = spair_dup,
    .close     = spair_close,
};

int socketpair_create(int domain, int type, int protocol, int fds[2]) {
    if (domain != AF_UNIX) { return -EAFNOSUPPORT; }
    int base_type = type & SOCK_TYPE_MASK;
    if (base_type != SOCK_STREAM && base_type != SOCK_DGRAM) { return -EINVAL; }
    if (protocol != 0) { return -EPROTONOSUPPORT; }

    struct process *proc = current_proc();
    if (!proc) { return -ESRCH; }

    struct pipe *a = pipe_alloc();
    if (!a) { return -ENOMEM; }
    struct pipe *b = pipe_alloc();
    if (!b) { pipe_free(a); return -ENOMEM; }
    pipe_init_ends(a);
    pipe_init_ends(b);

    struct spair_end *e0 = (struct spair_end *)kmalloc(sizeof(struct spair_end));
    struct spair_end *e1 = e0 ? (struct spair_end *)kmalloc(sizeof(struct spair_end)) : 0;
    if (!e0 || !e1) {
        if (e0) { kfree(e0); }
        pipe_free(a); pipe_free(b);
        return -ENOMEM;
    }
    e0->rd = a; e0->wr = b;
    e1->rd = b; e1->wr = a;

    // Neither fd's ops/priv is wired up until BOTH allocations succeed
    // (matching pipe_create's own ordering), so every failure branch
    // below frees e0/e1/a/b explicitly rather than through a close
    // callback that has nothing to call yet.
    int fd0 = fd_table_alloc(proc->fd_table);
    if (fd0 < 0) { kfree(e0); kfree(e1); pipe_free(a); pipe_free(b); return fd0; }
    int fd1 = fd_table_alloc(proc->fd_table);
    if (fd1 < 0) {
        fd_table_close(proc->fd_table, fd0);
        kfree(e0); kfree(e1); pipe_free(a); pipe_free(b);
        return fd1;
    }

    struct file_descriptor *f0 = fd_table_get(proc->fd_table, fd0);
    struct file_descriptor *f1 = fd_table_get(proc->fd_table, fd1);
    if (!f0 || !f1) {
        fd_table_close(proc->fd_table, fd0);
        fd_table_close(proc->fd_table, fd1);
        kfree(e0); kfree(e1); pipe_free(a); pipe_free(b);
        return -EBADF;
    }

    int nb = (type & SOCK_NONBLOCK) ? 1 : 0;
    f0->ops = &spair_ops; f0->priv = e0; f0->readable = 1; f0->writable = 1; f0->nonblock = nb;
    f1->ops = &spair_ops; f1->priv = e1; f1->readable = 1; f1->writable = 1; f1->nonblock = nb;

    fds[0] = fd0;
    fds[1] = fd1;
    return 0;
}

// ------------------------------------------------------------- selftest
//
// Exercises the cross-wiring itself -- not socketpair_create(), which
// needs a live process/fd table that does not exist yet this early in
// boot (the same reason pipe_selftest, pipe.c, drives pipe_alloc/
// ring_read/ring_write directly instead of pipe_create). Two raw
// pipes, used exactly as socketpair_create sets them up (each with
// pipe_init_ends: one reader, one writer), driven through the same
// pipe_*_ep functions a real socketpair end would call.
void socketpair_selftest(void) {
    struct pipe *a = pipe_alloc();
    struct pipe *b = pipe_alloc();
    if (!a || !b) {
        serial_write_string("[socketpair] selftest FAILED: allocation\n");
        if (a) { pipe_free(a); }
        if (b) { pipe_free(b); }
        return;
    }
    pipe_init_ends(a);
    pipe_init_ends(b);

    // end0 writes A, reads B; end1 writes B, reads A -- matching
    // socketpair_create's e0={rd:a,wr:b}/e1={rd:b,wr:a}.
    const char msg0[] = "end0->end1";
    if (pipe_write_ep(b, /*nonblock=*/1, msg0, sizeof msg0) != (int64_t)sizeof msg0) {
        serial_write_string("[socketpair] selftest FAILED: end0 write to b\n");
        goto out;
    }
    char buf[32] = {0};
    if (pipe_read_ep(b, /*nonblock=*/1, buf, sizeof buf) != (int64_t)sizeof msg0) {
        serial_write_string("[socketpair] selftest FAILED: end1 read from b\n");
        goto out;
    }
    for (uint32_t i = 0; i < sizeof msg0; i++) {
        if (buf[i] != msg0[i]) {
            serial_write_string("[socketpair] selftest FAILED: end1 read wrong bytes\n");
            goto out;
        }
    }

    const char msg1[] = "end1->end0!";
    if (pipe_write_ep(a, /*nonblock=*/1, msg1, sizeof msg1) != (int64_t)sizeof msg1) {
        serial_write_string("[socketpair] selftest FAILED: end1 write to a\n");
        goto out;
    }
    if (pipe_read_ep(a, /*nonblock=*/1, buf, sizeof buf) != (int64_t)sizeof msg1) {
        serial_write_string("[socketpair] selftest FAILED: end0 read from a\n");
        goto out;
    }
    for (uint32_t i = 0; i < sizeof msg1; i++) {
        if (buf[i] != msg1[i]) {
            serial_write_string("[socketpair] selftest FAILED: end0 read wrong bytes\n");
            goto out;
        }
    }

    // poll: both directions of both ends should report correctly with
    // both pipes empty and both peers still open.
    int mask0 = pipe_poll_ep(a, /*as_reader=*/0, /*as_writer=*/1, POLLOUT)
              | pipe_poll_ep(b, /*as_reader=*/1, /*as_writer=*/0, POLLIN);
    if (!(mask0 & POLLOUT) || (mask0 & POLLIN)) {
        serial_write_string("[socketpair] selftest FAILED: end0 poll mask wrong when idle\n");
        goto out;
    }

    // Close end1 (as_reader on b, as_writer on a) and confirm end0
    // sees EOF on its read side (from a, whose only writer just left)
    // and SIGPIPE-shaped -EPIPE on its write side (into b, whose only
    // reader just left) -- the exact rules pipe.c's own header comment
    // documents, now exercised across the cross-wired pair.
    pipe_close_ep(b, /*as_reader=*/1, /*as_writer=*/0);
    pipe_close_ep(a, /*as_reader=*/0, /*as_writer=*/1);

    int64_t rc = pipe_read_ep(a, /*nonblock=*/1, buf, sizeof buf);
    if (rc != 0) {
        serial_write_string("[socketpair] selftest FAILED: no EOF after peer closed\n");
        goto out;
    }
    rc = pipe_write_ep(b, /*nonblock=*/1, msg0, sizeof msg0);
    if (rc != -EPIPE) {
        serial_write_string("[socketpair] selftest FAILED: no EPIPE after peer closed\n");
        goto out;
    }

    // end0's own close: last reference on each pipe, so both are freed
    // by pipe_close_ep itself -- nothing left to pipe_free here.
    pipe_close_ep(a, /*as_reader=*/1, /*as_writer=*/0);
    pipe_close_ep(b, /*as_reader=*/0, /*as_writer=*/1);
    serial_write_string("[socketpair] selftest passed\n");
    return;

out:
    pipe_free(a);
    pipe_free(b);
}
