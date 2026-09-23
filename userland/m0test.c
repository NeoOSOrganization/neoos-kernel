// m0test -- userland checks for the desktop M0 kernel prerequisites
// (docs/superpowers/plans/2026-09-23-desktop-m0-kernel-prereqs.md).
// Prints "m0test: ok <name>" / "m0test: FAIL <name> ..." per check and
// "PASS m0test" only if every check passed. Works on the FAT disk
// (/root/m0), which is where SQLite and the desktop keep their files.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static int fails;
#define CHECK(name, cond, ...) do { if (cond) printf("m0test: ok %s\n", name); \
    else { fails++; printf("m0test: FAIL %s: ", name); printf(__VA_ARGS__); printf(" (errno=%d)\n", errno); } } while (0)

#define DIR0 "/root/m0"

static void test_excl(void) {
    unlink(DIR0 "/excl");
    int a = open(DIR0 "/excl", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("excl_create", a >= 0, "first O_EXCL create failed");
    errno = 0;
    int b = open(DIR0 "/excl", O_CREAT | O_EXCL | O_RDWR, 0644);
    CHECK("excl_eexist", b < 0 && errno == EEXIST, "second O_EXCL create: fd=%d", b);
    int c = open(DIR0 "/excl", O_CREAT | O_RDWR, 0644);
    CHECK("creat_existing", c >= 0, "plain O_CREAT of an existing file failed");
    if (a >= 0) { close(a); }
    if (c >= 0) { close(c); }
}

static void test_pwrite(void) {
    int fd = open(DIR0 "/pw", O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) { CHECK("pwrite_open", 0, "open"); return; }
    write(fd, "0123456789", 10);
    lseek(fd, 2, SEEK_SET);
    ssize_t n = pwrite(fd, "AB", 2, 5);
    off_t pos = lseek(fd, 0, SEEK_CUR);
    char buf[16] = {0};
    pread(fd, buf, 10, 0);
    CHECK("pwrite", n == 2 && pos == 2 && memcmp(buf, "01234AB789", 10) == 0,
          "n=%zd pos=%ld buf=%.10s", n, (long)pos, buf);
    close(fd);
}

static int write_file(const char *p, const char *text) {
    int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { return -1; }
    int n = (int)write(fd, text, strlen(text));
    close(fd);
    return n;
}
static int read_file(const char *p, char *buf, int cap) {
    int fd = open(p, O_RDONLY);
    if (fd < 0) { return -1; }
    int n = (int)read(fd, buf, cap - 1);
    close(fd);
    buf[n < 0 ? 0 : n] = 0;
    return n;
}

static void test_rename(void) {
    char buf[64];
    mkdir(DIR0 "/ra", 0755); mkdir(DIR0 "/rb", 0755);
    unlink(DIR0 "/ra/one"); unlink(DIR0 "/ra/two"); unlink(DIR0 "/rb/three");

    write_file(DIR0 "/ra/one", "first");
    int rc = rename(DIR0 "/ra/one", DIR0 "/ra/two");
    CHECK("rename_same_dir", rc == 0 && access(DIR0 "/ra/one", F_OK) != 0 &&
          read_file(DIR0 "/ra/two", buf, sizeof buf) == 5 && !strcmp(buf, "first"), "rc=%d buf=%s", rc, buf);

    rc = rename(DIR0 "/ra/two", DIR0 "/rb/three");
    CHECK("rename_cross_dir", rc == 0 && read_file(DIR0 "/rb/three", buf, sizeof buf) == 5 &&
          access(DIR0 "/ra/two", F_OK) != 0, "rc=%d", rc);

    write_file(DIR0 "/ra/victim", "old contents");
    rc = rename(DIR0 "/rb/three", DIR0 "/ra/victim");
    CHECK("rename_replaces", rc == 0 && read_file(DIR0 "/ra/victim", buf, sizeof buf) == 5 &&
          !strcmp(buf, "first"), "rc=%d buf=%s", rc, buf);

    // An fd open across the rename keeps writing to the (moved) file.
    int fd = open(DIR0 "/ra/victim", O_WRONLY | O_APPEND);
    rc = rename(DIR0 "/ra/victim", DIR0 "/rb/moved");
    write(fd, "+more", 5);
    close(fd);
    CHECK("rename_open_fd", rc == 0 && read_file(DIR0 "/rb/moved", buf, sizeof buf) == 10 &&
          !strcmp(buf, "first+more"), "rc=%d buf=%s", rc, buf);

    // A directory moved to a new parent: its ".." follows.
    mkdir(DIR0 "/ra/sub", 0755);
    write_file(DIR0 "/ra/sub/f", "x");
    rc = rename(DIR0 "/ra/sub", DIR0 "/rb/sub");
    struct stat a, b;
    int ok = rc == 0 && stat(DIR0 "/rb/sub/f", &a) == 0 && stat(DIR0 "/rb/sub/..", &a) == 0 &&
             stat(DIR0 "/rb", &b) == 0;
    CHECK("rename_dir", ok, "rc=%d", rc);

    errno = 0;
    CHECK("rename_into_self", rename(DIR0 "/rb", DIR0 "/rb/sub/x") == -1 && errno == EINVAL, "no EINVAL");
    errno = 0;
    CHECK("rename_exdev", rename(DIR0 "/rb/moved", "/proc/moved") == -1 && errno == EXDEV, "no EXDEV");
    errno = 0;
    CHECK("rename_enoent", rename(DIR0 "/nope", DIR0 "/nope2") == -1 && errno == ENOENT, "no ENOENT");
}

int main(void) {
    mkdir("/root", 0755);
    mkdir(DIR0, 0755);
    test_excl();
    test_pwrite();
    test_rename();
    if (fails == 0) { printf("PASS m0test\n"); } else { printf("m0test: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
