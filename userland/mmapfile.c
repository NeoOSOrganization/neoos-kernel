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

// ----------------------------------------------- MAP_PRIVATE of a file

static void test_map_file(void) {
    const char *path = "/tmp/dl-map";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open /tmp/dl-map");
    if (fd < 0) { return; }

    // Two pages plus a bit, so the past-EOF case at the end is real.
    static char src[8192 + 100];
    for (unsigned i = 0; i < sizeof src; i++) { src[i] = (char)((i * 7) & 0xFF); }
    check(neo3(SYS_WRITE, fd, src, sizeof src) == (long)sizeof src, "seed write");

    long a = neo6(SYS_MMAP, 0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    check(a > 0, "mmap MAP_PRIVATE of a file");
    if (a <= 0) { (void)neo1(SYS_CLOSE, fd); return; }
    const volatile unsigned char *p = (const volatile unsigned char *)a;

    check(p[0]    == 0,                                  "first byte");
    check(p[4095] == (unsigned char)((4095 * 7) & 0xFF), "last byte of page 0");
    check(p[4096] == (unsigned char)((4096 * 7) & 0xFF), "first byte of page 1");
    check(p[8191] == (unsigned char)((8191 * 7) & 0xFF), "last byte of page 1");

    // MAP_PRIVATE: a write stays private. It must not reach the file,
    // and a second mapping must not see it.
    volatile unsigned char *w = (volatile unsigned char *)a;
    w[10] = 0xEE;
    check(w[10] == 0xEE, "private write took");

    long b = neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    check(b > 0, "second mapping");
    if (b > 0) {
        check(((const volatile unsigned char *)b)[10] == (unsigned char)((10 * 7) & 0xFF),
              "private write did not leak to a second mapping");
        (void)neo2(SYS_MUNMAP, b, 4096);
    }
    static char vfy[16];
    check(neo4(SYS_PREAD, fd, vfy, 16, 0) == 16, "read the file back");
    check(vfy[10] == (char)((10 * 7) & 0xFF), "private write did not reach the file");

    // A non-page-aligned offset is -EINVAL, as on Linux.
    check(neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 100) == -22,
          "unaligned offset is -EINVAL");

    // Past EOF reads as zeros -- what a .bss at the end of a data
    // segment depends on.
    long c = neo6(SYS_MMAP, 0, 4096, PROT_READ, MAP_PRIVATE, fd, 8192);
    check(c > 0, "map the last partial page");
    if (c > 0) {
        const volatile unsigned char *q = (const volatile unsigned char *)c;
        check(q[99] == (unsigned char)(((8192 + 99) * 7) & 0xFF), "last real byte");
        int zeros = 1;
        for (int i = 100; i < 4096; i++) { if (q[i] != 0) { zeros = 0; break; } }
        check(zeros, "beyond EOF reads as zeros");
        (void)neo2(SYS_MUNMAP, c, 4096);
    }

    (void)neo2(SYS_MUNMAP, a, 8192);
    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[dyn] map file ok\n");
}

int main(void) {
    printf("[dyn] start\n");
    test_pread();
    test_map_file();
    if (failures) {
        printf("[dyn] %d FAILURES\n", failures);
        return 1;
    }
    printf("[dyn] ALL PASSED\n");
    return 0;
}
