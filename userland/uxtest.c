// userland/uxtest.c -- the GUI stack's kernel IPC conformance test.
//
// Grows across G3: ftruncate now, then memfd_create and its shared
// mmap, real AF_UNIX sockets, and SCM_RIGHTS fd passing. Every one of
// those is something a compositor needs and NeoOS did not have.
//
// Raw syscalls with NeoOS's own numbers, the way userland/epollet.c
// does it: libneoos has no wrappers for most of this, and testing the
// kernel directly keeps a shim bug from masking a kernel bug. `make
// uxtest` boots it alone. Green looks like: [uxtest] ALL PASSED

#include <stdint.h>
#include <stdio.h>

#define SYS_WRITE      1
#define SYS_READ       6
#define SYS_OPEN       7
#define SYS_CLOSE      8
#define SYS_UNLINK    10
#define SYS_LSEEK     11
#define SYS_FSTAT     57
#define SYS_MMAP       37
#define SYS_MUNMAP     38
#define SYS_FTRUNCATE 134
#define SYS_MEMFD_CREATE 135

#define PROT_READ    1
#define PROT_WRITE   2
#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_FAILED_MIN (-4095L)

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
#define neo0(n)                neo6((n), 0, 0, 0, 0, 0, 0)
#define neo1(n, a)             neo6((n), (long)(a), 0, 0, 0, 0, 0)
#define neo2(n, a, b)          neo6((n), (long)(a), (long)(b), 0, 0, 0, 0)
#define neo3(n, a, b, c)       neo6((n), (long)(a), (long)(b), (long)(c), 0, 0, 0)
#define neo4(n, a, b, c, d)    neo6((n), (long)(a), (long)(b), (long)(c), (long)(d), 0, 0)

static long slen(const char *s) { long n = 0; while (s[n]) { n++; } return n; }

// NeoOS's path-taking syscalls carry an explicit LENGTH after the
// pointer -- see copy_user_path_at, and the shim's neo_strlen reshape.
#define neo_open(path, flags, mode) \
    neo4(SYS_OPEN, (path), slen(path), (flags), (mode))
#define neo_unlink(path) neo2(SYS_UNLINK, (path), slen(path))

#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200

// Linux x86-64 struct stat, only as far as st_size.
struct k_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize, st_blocks;
    uint64_t rest[8];
};

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("[uxtest] FAILED: %s\n", what);
        failures++;
    }
}

// ---------------------------------------------------------- ftruncate

static void test_ftruncate(void) {
    const char *path = "/tmp/ux-trunc";
    (void)neo_unlink(path);
    long fd = neo_open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    check(fd >= 0, "open /tmp/ux-trunc");
    if (fd < 0) { return; }

    // Something to shrink away later, and a non-zero first page so the
    // grow case cannot pass by accident on an all-zero file.
    static char pattern[512];
    for (int i = 0; i < 512; i++) { pattern[i] = (char)(i | 1); }
    check(neo3(SYS_WRITE, fd, pattern, sizeof(pattern)) == sizeof(pattern), "seed write");

    struct k_stat st;
    check(neo3(SYS_FTRUNCATE, fd, 8192, 0) == 0, "ftruncate grow");
    check(neo2(SYS_FSTAT, fd, &st) == 0 && st.st_size == 8192, "size after grow");

    // The grown region is a hole. It must read as zeros, and the read
    // must not stop short at the first unwritten page.
    static char buf[8192];
    for (int i = 0; i < 8192; i++) { buf[i] = (char)0xAA; }
    check(neo3(SYS_LSEEK, fd, 0, 0) == 0, "rewind");
    long n = neo3(SYS_READ, fd, buf, sizeof(buf));
    check(n == 8192, "read back the whole grown file");
    int zeros_ok = 1;
    for (int i = 512; i < 8192; i++) { if (buf[i] != 0) { zeros_ok = 0; break; } }
    check(zeros_ok, "grown region reads as zeros");
    check(buf[0] == (char)(0 | 1) && buf[511] == (char)(511 | 1), "original bytes survived");

    // Shrink, then grow again: the bytes that were discarded must not
    // come back.
    check(neo3(SYS_FTRUNCATE, fd, 100, 0) == 0, "ftruncate shrink");
    check(neo2(SYS_FSTAT, fd, &st) == 0 && st.st_size == 100, "size after shrink");
    check(neo3(SYS_FTRUNCATE, fd, 1024, 0) == 0, "ftruncate regrow");
    check(neo3(SYS_LSEEK, fd, 0, 0) == 0, "rewind again");
    check(neo3(SYS_READ, fd, buf, 1024) == 1024, "read after regrow");
    int discarded_ok = 1;
    for (int i = 100; i < 1024; i++) { if (buf[i] != 0) { discarded_ok = 0; break; } }
    check(discarded_ok, "discarded bytes did not reappear");

    check(neo3(SYS_FTRUNCATE, fd, -1, 0) == -22, "negative length is -EINVAL");

    (void)neo1(SYS_CLOSE, fd);
    (void)neo_unlink(path);
    printf("[uxtest] ftruncate ok\n");
}

// ------------------------------------------------------- memfd_create

static long mfd_map(long fd, long len, long prot, long flags) {
    return neo6(SYS_MMAP, 0, len, prot, flags, fd, 0);
}

static void test_memfd(void) {
    const char *nm = "surface";
    long fd = neo3(SYS_MEMFD_CREATE, nm, slen(nm), 0);
    check(fd >= 0, "memfd_create");
    if (fd < 0) { return; }

    check(neo3(SYS_FTRUNCATE, fd, 4096 * 4, 0) == 0, "size the memfd");

    long a = mfd_map(fd, 4096 * 4, PROT_READ | PROT_WRITE, MAP_SHARED);
    check(a > 0, "first mapping");
    long b = mfd_map(fd, 4096 * 4, PROT_READ | PROT_WRITE, MAP_SHARED);
    check(b > 0, "second mapping");
    check(a != b, "distinct addresses");
    if (a <= 0 || b <= 0) { (void)neo1(SYS_CLOSE, fd); return; }

    volatile uint32_t *pa = (volatile uint32_t *)a;
    volatile uint32_t *pb = (volatile uint32_t *)b;

    // The whole point: two mappings of one object share frames.
    pa[0] = 0xDEADBEEF;
    pa[4095] = 0x12345678;          // page 3, so it is not only page 0
    check(pb[0] == 0xDEADBEEF, "write visible through the other mapping");
    check(pb[4095] == 0x12345678, "and not only on the first page");
    pb[1] = 0xFEEDFACE;
    check(pa[1] == 0xFEEDFACE, "and in the other direction");

    // A fresh object is zeroed, never a recycled page's contents.
    const char *n2 = "fresh";
    long fd2 = neo3(SYS_MEMFD_CREATE, n2, slen(n2), 0);
    check(fd2 >= 0, "second memfd");
    check(neo3(SYS_FTRUNCATE, fd2, 4096, 0) == 0, "size it");
    long c = mfd_map(fd2, 4096, PROT_READ | PROT_WRITE, MAP_SHARED);
    check(c > 0, "map the fresh one");
    int zeroed = 1;
    if (c > 0) {
        volatile uint32_t *pc = (volatile uint32_t *)c;
        for (int i = 0; i < 1024; i++) { if (pc[i] != 0) { zeroed = 0; break; } }
    }
    check(zeroed, "a fresh memfd is zeroed");

    // MAP_PRIVATE is refused rather than silently shared: there is no
    // copy-on-write path for these pages. Documented divergence.
    long pv = mfd_map(fd, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE);
    check(pv == -22, "MAP_PRIVATE is -EINVAL");

    // read()/write() see the same bytes as the mapping.
    static uint32_t word;
    check(neo3(SYS_LSEEK, fd, 0, 0) == 0, "rewind the memfd");
    check(neo3(SYS_READ, fd, &word, 4) == 4, "read the memfd");
    check(word == 0xDEADBEEF, "read agrees with the mapping");

    (void)neo2(SYS_MUNMAP, a, 4096 * 4);
    (void)neo2(SYS_MUNMAP, b, 4096 * 4);
    if (c > 0) { (void)neo2(SYS_MUNMAP, c, 4096); }
    (void)neo1(SYS_CLOSE, fd);
    (void)neo1(SYS_CLOSE, fd2);
    printf("[uxtest] memfd ok\n");
}

int main(void) {
    printf("[uxtest] start\n");
    test_ftruncate();
    test_memfd();
    if (failures) {
        printf("[uxtest] %d FAILURES\n", failures);
        return 1;
    }
    printf("[uxtest] ALL PASSED\n");
    return 0;
}
