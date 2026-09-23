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

int main(void) {
    mkdir("/root", 0755);
    mkdir(DIR0, 0755);
    test_excl();
    test_pwrite();
    if (fails == 0) { printf("PASS m0test\n"); } else { printf("m0test: %d FAILED\n", fails); }
    return fails ? 1 : 0;
}
