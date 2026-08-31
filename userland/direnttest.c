#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[direnttest] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// The record layout is the ABI. A program compiled against a real
// <dirent.h> reads these offsets directly out of the buffer.
static void check_layout(void) {
    struct dirent d;
    char *base = (char *)&d;
    if ((char *)&d.d_ino    - base != 0)  { fail("d_ino offset", 0); return; }
    if ((char *)&d.d_off    - base != 8)  { fail("d_off offset", 0); return; }
    if ((char *)&d.d_reclen - base != 16) { fail("d_reclen offset", 0); return; }
    if ((char *)&d.d_type   - base != 18) { fail("d_type offset", 0); return; }
    if ((char *)&d.d_name   - base != 19) { fail("d_name offset", 0); return; }
    if (DT_DIR != 4 || DT_REG != 8 || DT_CHR != 2) {
        fail("DT_* must be Linux's values", DT_DIR);
        return;
    }
    printf("[direnttest] record layout passed\n");
}

// Walk the raw buffer by d_reclen, exactly as a ported program does,
// rather than through readdir().
static void check_raw_walk(void) {
    int fd = open("/DIR", O_RDONLY);
    if (fd < 0) { fail("open(/DIR)", fd); return; }

    char buf[512] __attribute__((aligned(8)));
    int n = getdents(fd, buf, sizeof(buf));
    close(fd);
    if (n <= 0) { fail("getdents returned no bytes", n); return; }

    int off = 0, entries = 0, found_nested = 0;
    while (off < n) {
        struct dirent *e = (struct dirent *)(buf + off);
        if (e->d_reclen == 0) { fail("d_reclen of 0 would loop forever", 0); return; }
        if (e->d_reclen % 8 != 0) { fail("d_reclen not 8-byte aligned", e->d_reclen); return; }
        if (off + (int)e->d_reclen > n) { fail("record runs past the buffer", e->d_reclen); return; }
        if (e->d_ino == 0) { fail("d_ino should be a real inode", 0); return; }
        if (str_eq(e->d_name, "NESTED.TXT")) {
            found_nested = 1;
            if (e->d_type != DT_REG) { fail("NESTED.TXT d_type should be DT_REG", e->d_type); return; }
        }
        off += e->d_reclen;
        entries++;
    }
    if (off != n) { fail("records did not exactly fill the returned bytes", off); return; }
    if (!found_nested) { fail("NESTED.TXT missing from /DIR", entries); return; }
    printf("[direnttest] raw getdents64 walk passed (%d entries, %d bytes)\n", entries, n);
}

// readdir() over the same directory must agree, and must report a
// directory as DT_DIR.
static void check_readdir(void) {
    DIR *d = opendir("/");
    if (!d) { fail("opendir(/)", -1); return; }

    int saw_dir = 0, saw_reg = 0, entries = 0;
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        if (e->d_type == DT_DIR) { saw_dir = 1; }
        if (e->d_type == DT_REG) { saw_reg = 1; }
        if (e->d_name[0] == '\0') { fail("empty d_name", entries); closedir(d); return; }
        entries++;
    }
    closedir(d);
    if (!saw_dir) { fail("no DT_DIR entry in /", entries); return; }
    if (!saw_reg) { fail("no DT_REG entry in /", entries); return; }
    printf("[direnttest] readdir passed (%d entries)\n", entries);
}

// d_ino must be the same inode stat() reports, or a program pairing
// the two sees two different files.
static void check_ino_matches_stat(void) {
    DIR *d = opendir("/DIR");
    if (!d) { fail("opendir(/DIR)", -1); return; }
    struct dirent *e;
    int checked = 0;
    while ((e = readdir(d)) != 0) {
        if (!str_eq(e->d_name, "NESTED.TXT")) { continue; }
        struct stat s;
        if (stat("/DIR/NESTED.TXT", &s) != 0) { fail("stat for d_ino comparison", -1); break; }
        if (s.st_ino != e->d_ino) {
            printf("[direnttest] FAILED: d_ino %d != st_ino %d\n",
                   (int)e->d_ino, (int)s.st_ino);
            failures++;
            break;
        }
        checked = 1;
        break;
    }
    closedir(d);
    if (!checked && !failures) { fail("never found NESTED.TXT to compare", 0); return; }
    if (!failures) { printf("[direnttest] d_ino matches st_ino passed\n"); }
}

static void check_errors(void) {
    // A buffer too small for even one record is -EINVAL, not 0 -- 0
    // would look like an empty directory and truncate a listing.
    int fd = open("/DIR", O_RDONLY);
    if (fd < 0) { fail("open for the small-buffer case", fd); return; }
    char tiny[8];
    int rc = getdents(fd, tiny, sizeof(tiny));
    close(fd);
    if (rc != -EINVAL) { fail("undersized buffer should be -EINVAL", rc); return; }

    // getdents on a regular file is -ENOTDIR.
    fd = open("/HELLO.TXT", O_RDONLY);
    if (fd >= 0) {
        char buf[256] __attribute__((aligned(8)));
        rc = getdents(fd, buf, sizeof(buf));
        close(fd);
        if (rc != -ENOTDIR) { fail("getdents on a file should be -ENOTDIR", rc); return; }
    }
    printf("[direnttest] error cases passed\n");
}

int main(void) {
    check_layout();
    check_raw_walk();
    check_readdir();
    check_ino_matches_stat();
    check_errors();

    if (failures) {
        printf("[direnttest] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[direnttest] ALL PASSED\n");
    return 0;
}
