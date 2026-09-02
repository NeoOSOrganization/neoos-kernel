#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[stattest] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

// The layout is the whole point: musl compiles its own copy of this
// struct into every caller, so a field at the wrong offset is not a
// bug that shows up as a wrong number -- it shows up as st_size read
// out of st_gid. Checked from USERLAND, against the library's copy of
// the header, so kernel and lib cannot silently drift apart.
static void check_layout(void) {
    if (sizeof(struct stat) != 144) {
        fail("sizeof(struct stat) is not Linux's 144", (int)sizeof(struct stat));
        return;
    }
    struct stat s;
    char *base = (char *)&s;
    struct { const char *name; long off; long want; } f[] = {
        { "st_dev",     (char *)&s.st_dev     - base,   0 },
        { "st_ino",     (char *)&s.st_ino     - base,   8 },
        { "st_nlink",   (char *)&s.st_nlink   - base,  16 },
        { "st_mode",    (char *)&s.st_mode    - base,  24 },
        { "st_uid",     (char *)&s.st_uid     - base,  28 },
        { "st_gid",     (char *)&s.st_gid     - base,  32 },
        { "st_rdev",    (char *)&s.st_rdev    - base,  40 },
        { "st_size",    (char *)&s.st_size    - base,  48 },
        { "st_blksize", (char *)&s.st_blksize - base,  56 },
        { "st_blocks",  (char *)&s.st_blocks  - base,  64 },
    };
    for (unsigned i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        if (f[i].off != f[i].want) {
            printf("[stattest] FAILED: %s at offset %d, expected %d\n",
                   f[i].name, (int)f[i].off, (int)f[i].want);
            failures++;
            return;
        }
    }
    printf("[stattest] struct stat layout passed\n");
}

static void check_stat_file(void) {
    struct stat s;
    int rc = stat("/usr/share/test/hello.txt", &s);
    if (rc != 0) { fail("stat(/HELLO.TXT)", rc); return; }

    if (!S_ISREG(s.st_mode)) { fail("HELLO.TXT is not reported as a regular file", (int)s.st_mode); return; }
    if (S_ISDIR(s.st_mode))  { fail("HELLO.TXT reported as a directory", (int)s.st_mode); return; }
    if (s.st_size != 24)     { fail("HELLO.TXT size (expected 24)", (int)s.st_size); return; }
    if (s.st_blksize != 512) { fail("st_blksize", (int)s.st_blksize); return; }
    // 24 bytes rounds up to one 512-byte block.
    if (s.st_blocks != 1)    { fail("st_blocks for a 24-byte file", (int)s.st_blocks); return; }
    if (s.st_nlink != 1)     { fail("st_nlink for a file", (int)s.st_nlink); return; }
    if (s.st_dev == 0)       { fail("st_dev should never be 0", 0); return; }
    printf("[stattest] stat on a file passed (size=%d blocks=%d)\n",
           (int)s.st_size, (int)s.st_blocks);
}

static void check_stat_dir(void) {
    struct stat s;
    int rc = stat("/usr/share/test/dir", &s);
    if (rc != 0) { fail("stat(/DIR)", rc); return; }
    if (!S_ISDIR(s.st_mode)) { fail("/usr/share/test/dir is not reported as a directory", (int)s.st_mode); return; }
    if (S_ISREG(s.st_mode))  { fail("/usr/share/test/dir reported as a regular file", (int)s.st_mode); return; }
    if (s.st_nlink != 2)     { fail("st_nlink for a directory", (int)s.st_nlink); return; }
    printf("[stattest] stat on a directory passed\n");
}

static void check_fstat(void) {
    int fd = open("/usr/share/test/hello.txt", O_RDONLY);
    if (fd < 0) { fail("open for fstat", fd); return; }

    struct stat byfd, bypath;
    int rc = fstat(fd, &byfd);
    close(fd);
    if (rc != 0) { fail("fstat", rc); return; }
    if (stat("/usr/share/test/hello.txt", &bypath) != 0) { fail("stat for comparison", -1); return; }

    // The same file by two routes must agree on identity and size.
    if (byfd.st_ino != bypath.st_ino || byfd.st_dev != bypath.st_dev ||
        byfd.st_size != bypath.st_size || byfd.st_mode != bypath.st_mode) {
        fail("fstat and stat disagree about the same file", 0);
        return;
    }
    printf("[stattest] fstat matches stat passed\n");
}

// stat resolves through the cwd like every other path call.
static void check_relative(void) {
    if (chdir("/usr/share/test/dir") != 0) { fail("chdir(/usr/share/test/dir)", -1); return; }
    struct stat rel, abs;
    if (stat("nested.txt", &rel) != 0) { fail("stat on a relative path", -1); return; }
    if (stat("/usr/share/test/dir/nested.txt", &abs) != 0) { fail("stat on the absolute path", -1); return; }
    if (rel.st_ino != abs.st_ino || rel.st_size != abs.st_size) {
        fail("relative and absolute stat disagree", 0);
        return;
    }
    chdir("/");
    printf("[stattest] relative stat passed\n");
}

// lstat is identical to stat here: nothing NeoOS mounts holds a symlink.
static void check_lstat(void) {
    struct stat a, b;
    if (lstat("/usr/share/test/hello.txt", &a) != 0) { fail("lstat", -1); return; }
    if (stat("/usr/share/test/hello.txt", &b) != 0)  { fail("stat for lstat comparison", -1); return; }
    if (a.st_ino != b.st_ino || a.st_mode != b.st_mode) {
        fail("lstat and stat disagree", 0);
        return;
    }
    printf("[stattest] lstat passed\n");
}

static void check_fstatat(void) {
    struct stat s;
    int rc = fstatat(AT_FDCWD, "/usr/share/test/hello.txt", &s, 0);
    if (rc != 0) { fail("fstatat(AT_FDCWD)", rc); return; }
    if (!S_ISREG(s.st_mode)) { fail("fstatat mode", (int)s.st_mode); return; }

    // A real directory fd is refused rather than quietly treated as the
    // cwd -- there is no openat family to produce one yet.
    int fd = open("/usr/share/test/dir", O_RDONLY);
    if (fd >= 0) {
        rc = fstatat(fd, "nested.txt", &s, 0);
        close(fd);
        if (rc != -EBADF) { fail("fstatat with a real dirfd should be -EBADF", rc); return; }
    }
    printf("[stattest] fstatat passed\n");
}

static void check_errors(void) {
    struct stat s;
    int rc = stat("/no_such_file", &s);
    if (rc != -ENOENT) { fail("stat on a missing file should be -ENOENT", rc); return; }

    rc = fstat(4096, &s);
    if (rc != -EBADF) { fail("fstat on a bad fd should be -EBADF", rc); return; }

    // A pipe has no vnode, so there is no inode to report.
    int fds[2];
    if (pipe(fds) == 0) {
        rc = fstat(fds[0], &s);
        close(fds[0]); close(fds[1]);
        if (rc != -EINVAL) { fail("fstat on a pipe should be -EINVAL", rc); return; }
    }
    printf("[stattest] error cases passed\n");
}

static void check_mtime_is_real(void) {
    int fd = open("/var/tmp/mtime.tmp", 1 | 0x40);   // O_WRONLY|O_CREAT (Linux value)
    if (fd < 0) { fail("open for mtime", fd); return; }
    if (write(fd, "x", 1) != 1) { fail("write for mtime", -1); close(fd); return; }
    close(fd);

    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) { fail("clock_gettime", -1); return; }

    struct stat st;
    if (stat("/var/tmp/mtime.tmp", &st) != 0) { fail("stat MTIME.TMP", -1); return; }
    unlink("/var/tmp/mtime.tmp");

    long long skew = (long long)now.tv_sec - (long long)st.st_mtime_sec;
    if (skew < 0) skew = -skew;
    // FAT time resolution is 2s; the RTC epoch may be a few seconds
    // stale. Allow a generous window; the point is "not zero, and not 1980".
    if (st.st_mtime_sec == 0 || skew > 120) {
        fail("st_mtime not close to CLOCK_REALTIME", 0);
        return;
    }
    printf("[stattest] mtime is real passed\n");
}

int main(void) {
    check_layout();
    check_stat_file();
    check_stat_dir();
    check_fstat();
    check_relative();
    check_lstat();
    check_fstatat();
    check_errors();
    check_mtime_is_real();

    if (failures) {
        printf("[stattest] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[stattest] ALL PASSED\n");
    return 0;
}
