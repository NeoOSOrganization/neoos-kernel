// blkdevtest -- storage-01 from userland, through musl: raw block
// nodes, their ioctls, unaligned I/O, fsync, /proc/partitions, and a
// 64-bit lseek. Prints "PASS blkdevtest" or FAILED lines.
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

// Linux's values (<linux/fs.h>, which musl does not ship).
#define BLKGETSIZE    0x1260
#define BLKSSZGET     0x1268
#define BLKGETSIZE64  0x80081272UL

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("blkdevtest: FAILED: " __VA_ARGS__); printf("\n"); fails++; } \
                           else { printf("blkdevtest: ok " #c "\n"); } } while (0)

static unsigned char saved[9000], pat[9000], back[9000];

int main(void) {
    struct stat st;
    CHECK(stat("/dev/sda", &st) == 0, "stat /dev/sda errno=%d", errno);
    CHECK(S_ISBLK(st.st_mode), "/dev/sda not S_IFBLK mode=%o", st.st_mode);
    CHECK(major(st.st_rdev) == 8 && minor(st.st_rdev) == 0, "sda rdev %u:%u", major(st.st_rdev), minor(st.st_rdev));
    CHECK(stat("/dev/sdb", &st) == 0 && minor(st.st_rdev) == 16, "sdb minor");

    int fd = open("/dev/sda", O_RDONLY);
    CHECK(fd >= 0, "open /dev/sda errno=%d", errno);
    uint64_t sz = 0;
    int ss = 0;
    unsigned long secs = 0;
    CHECK(ioctl(fd, BLKGETSIZE64, &sz) == 0 && sz == 64ULL << 20, "BLKGETSIZE64 %llu", (unsigned long long)sz);
    CHECK(ioctl(fd, BLKSSZGET, &ss) == 0 && ss == 512, "BLKSSZGET %d", ss);
    CHECK(ioctl(fd, BLKGETSIZE, &secs) == 0 && secs == (64UL << 20) / 512, "BLKGETSIZE %lu", secs);
    unsigned char s0[512];
    CHECK(pread(fd, s0, 512, 0) == 512 && s0[510] == 0x55 && s0[511] == 0xAA, "sda LBA0 boot signature");
    CHECK(lseek(fd, 0, SEEK_END) == (off_t)sz, "SEEK_END");
    CHECK(read(fd, s0, 1) == 0, "read at end not EOF");
    CHECK(lseek(fd, (off_t)sz + 1, SEEK_SET) == -1 && errno == EINVAL, "seek past end not EINVAL");
    close(fd);

    // Non-destructive unaligned round trips near the end of sdb: one
    // crossing a sector boundary, one large enough to take the aligned
    // multi-sector path in the middle.
    fd = open("/dev/sdb", O_RDWR);
    CHECK(fd >= 0, "open /dev/sdb errno=%d", errno);
    uint64_t sz2 = 0;
    ioctl(fd, BLKGETSIZE64, &sz2);
    struct { off_t off; size_t len; } cases[] = {
        { (off_t)sz2 - 4096 + 400, 300 },
        { (off_t)sz2 - 16384 + 100, 9000 },
    };
    for (int c = 0; c < 2; c++) {
        size_t n = cases[c].len;
        for (size_t i = 0; i < n; i++) { pat[i] = (unsigned char)(i * 13 + c); }
        CHECK(pread(fd, saved, n, cases[c].off) == (ssize_t)n, "case %d save", c);
        CHECK(pwrite(fd, pat, n, cases[c].off) == (ssize_t)n, "case %d pwrite", c);
        CHECK(fsync(fd) == 0, "case %d fsync errno=%d", c, errno);
        CHECK(pread(fd, back, n, cases[c].off) == (ssize_t)n && memcmp(back, pat, n) == 0, "case %d readback", c);
        CHECK(pwrite(fd, saved, n, cases[c].off) == (ssize_t)n, "case %d restore", c);
    }
    close(fd);

    // /proc/partitions lists both disks.
    char pb[2048] = {0};
    fd = open("/proc/partitions", O_RDONLY);
    CHECK(fd >= 0 && read(fd, pb, sizeof pb - 1) > 0, "read /proc/partitions");
    if (fd >= 0) { close(fd); }
    CHECK(strncmp(pb, "major minor  #blocks  name", 26) == 0, "partitions header");
    CHECK(strstr(pb, " sda\n") && strstr(pb, " sdb\n"), "partitions list:\n%s", pb);

    // A 64-bit offset through musl and the shim, not truncated to 32 bits.
    fd = open("/tmp/blkdevtest.big", O_CREAT | O_RDWR, 0644);
    off_t five = (off_t)5 << 30;
    CHECK(fd >= 0 && lseek(fd, five, SEEK_SET) == five, "lseek 5GiB");
    CHECK(lseek(fd, 0, SEEK_CUR) == five, "SEEK_CUR after 5GiB");
    if (fd >= 0) { close(fd); unlink("/tmp/blkdevtest.big"); }

    if (fails == 0) { printf("PASS blkdevtest\n"); }
    return fails ? 1 : 0;
}
