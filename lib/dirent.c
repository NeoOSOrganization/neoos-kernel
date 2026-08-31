#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// Buffering exists so a listing loop is not one syscall per entry. The
// buffer is a BYTE buffer now, not an array of fixed structs: getdents64
// records are variable length, so how many fit depends on the names.
#define DIRENT_BUF_SIZE 512
#define MAX_OPEN_DIRS   4

struct DIR {
    int     in_use;
    int     fd;
    char    buf[DIRENT_BUF_SIZE] __attribute__((aligned(8)));
    int     valid;    // bytes of buf the kernel filled
    int     next;     // byte offset of the next record to hand out
    int     at_end;   // the kernel has reported end of directory
};

static struct DIR dirs[MAX_OPEN_DIRS];

DIR *opendir(const char *path) {
    struct DIR *d = 0;
    for (int i = 0; i < MAX_OPEN_DIRS; i++) {
        if (!dirs[i].in_use) { d = &dirs[i]; break; }
    }
    if (!d) { return 0; }

    int fd = open(path, O_RDONLY);
    if (fd < 0) { return 0; }

    // Reject a regular file here rather than at the first readdir, so a
    // successful opendir means the listing loop is safe to enter. The
    // first batch is read eagerly for the same reason -- getdents on a
    // non-directory is what returns -ENOTDIR.
    int rc = getdents(fd, d->buf, DIRENT_BUF_SIZE);
    if (rc < 0) { close(fd); return 0; }

    d->in_use = 1;
    d->fd     = fd;
    d->valid  = rc;
    d->next   = 0;
    d->at_end = (rc == 0);
    return d;
}

struct dirent *readdir(DIR *d) {
    if (!d || !d->in_use) { return 0; }

    if (d->next >= d->valid) {
        if (d->at_end) { return 0; }
        int rc = getdents(d->fd, d->buf, DIRENT_BUF_SIZE);
        if (rc <= 0) { d->at_end = 1; return 0; }
        d->valid = rc;
        d->next  = 0;
    }

    // The record is handed back IN PLACE. d_reclen is what steps to the
    // next one -- the whole reason the layout has that field, and why
    // the caller must not hold the pointer across another readdir.
    struct dirent *e = (struct dirent *)(d->buf + d->next);
    d->next += e->d_reclen;
    return e;
}

int closedir(DIR *d) {
    if (!d || !d->in_use) { return -EBADF; }
    int rc = close(d->fd);
    d->in_use = 0;
    d->fd     = -1;
    d->valid  = 0;
    d->next   = 0;
    d->at_end = 0;
    return rc;
}
