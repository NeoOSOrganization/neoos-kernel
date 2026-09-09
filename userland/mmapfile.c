// userland/mmapfile.c -- file mapping and pread conformance (DL-1).
//
// Grows across the dynamic-linking milestone: pread first, then
// MAP_PRIVATE file mappings, then the concurrent-fault case that is the
// only way a missed re-validation in the fault path becomes visible.
//
// Raw syscalls with NeoOS's own numbers, the way userland/uxtest.c does
// it: testing the kernel directly keeps a shim bug from masking a
// kernel bug. `make mmapfile` boots it alone. Green is:
// [dyn] ALL PASSED

#include <stdint.h>
#include <stdio.h>

#define SYS_WRITE      1
#define SYS_READ       6
#define SYS_OPEN       7
#define SYS_CLOSE      8
#define SYS_UNLINK    10
#define SYS_LSEEK     11
#define SYS_MMAP      37
#define SYS_MUNMAP    38
#define SYS_FSTAT     57
#define SYS_TEST_HOOK 66
#define SYS_PREAD    136

#define TESTHOOK_PMM_FREE 3

#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200

#define PROT_READ   1
#define PROT_WRITE  2
#define MAP_PRIVATE 0x02

static long neo6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
                      : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}
#define neo1(n, a)          neo6((n), (long)(a), 0, 0, 0, 0, 0)
#define neo2(n, a, b)       neo6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define neo3(n, a, b, c)    neo6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define neo4(n, a, b, c, d) neo6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static long slen(const char *s) { long n = 0; while (s[n]) { n++; } return n; }

// NeoOS's path-taking syscalls carry an explicit LENGTH after the
// pointer -- see copy_user_path_at.
#define neo_open(path, flags, mode) \
    neo4(SYS_OPEN, (path), slen(path), (flags), (mode))
#define neo_unlink(path) neo2(SYS_UNLINK, (path), slen(path))

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("[dyn] FAILED: %s\n", what);
        failures++;
    }
}

// ---------------------------------------------------------------- pread

static void test_pread(void) {
    const char *path = "/tmp/dl-pread";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open /tmp/dl-pread");
    if (fd < 0) { return; }

    static char src[512];
    for (int i = 0; i < 512; i++) { src[i] = (char)(i & 0x7F); }
    check(neo3(SYS_WRITE, fd, src, sizeof src) == sizeof src, "seed write");

    // The position sits at EOF after that write. pread must ignore it
    // entirely -- that is the whole difference from read.
    static char buf[64];
    check(neo4(SYS_PREAD, fd, buf, 64, 100) == 64, "pread returns 64");
    int ok = 1;
    for (int i = 0; i < 64; i++) {
        if (buf[i] != (char)((100 + i) & 0x7F)) { ok = 0; break; }
    }
    check(ok, "pread read from the right offset");

    // ...and must not have moved it: a read() here is still at EOF.
    check(neo3(SYS_READ, fd, buf, 64) == 0, "pread did not move the position");

    check(neo4(SYS_PREAD, fd, buf, 64, 1000) == 0, "pread past EOF is 0");
    check(neo4(SYS_PREAD, fd, buf, 64, -1) == -22, "negative offset is -EINVAL");

    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[dyn] pread ok\n");
}

int main(void) {
    printf("[dyn] start\n");
    test_pread();
    if (failures) {
        printf("[dyn] %d FAILURES\n", failures);
        return 1;
    }
    printf("[dyn] ALL PASSED\n");
    return 0;
}
