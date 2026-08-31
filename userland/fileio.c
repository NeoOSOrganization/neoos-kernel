#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

static int check_bytes_equal(const char *a, const char *b, uint64_t len) {
    for (uint64_t i = 0; i < len; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // Create, write, close.
    int fd = open("/FILEIO.TXT", O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) {
        printf("[fileio] open for write FAILED: %d\n", fd);
        return 1;
    }
    const char msg[] = "hello from fileio\n"; // 18 bytes
    uint64_t msg_len = strlen(msg);
    if (write(fd, msg, msg_len) != (int64_t)msg_len) {
        printf("[fileio] write FAILED\n");
        return 1;
    }
    close(fd);

    // Reopen for read+write, lseek back, overwrite mid-file:
    // "from f" (position 6, 6 bytes) -> "FILEIO".
    fd = open("/FILEIO.TXT", O_RDWR);
    if (fd < 0) {
        printf("[fileio] reopen for rdwr FAILED: %d\n", fd);
        return 1;
    }
    if (lseek(fd, 6, SEEK_SET) != 6) {
        printf("[fileio] lseek FAILED\n");
        return 1;
    }
    const char patch[] = "FILEIO";
    if (write(fd, patch, 6) != 6) {
        printf("[fileio] mid-file write FAILED\n");
        return 1;
    }
    close(fd);

    // Reopen for read, verify the overwrite landed correctly.
    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] reopen for read FAILED: %d\n", fd);
        return 1;
    }
    char readback[64];
    int64_t got = read(fd, readback, sizeof(readback) - 1);
    close(fd);
    const char expected[] = "hello FILEIOileio\n";
    if (got != (int64_t)msg_len || !check_bytes_equal(readback, expected, (uint64_t)got)) {
        printf("[fileio] mid-file overwrite readback mismatch\n");
        return 1;
    }

    // mkdir + create/write/read a file inside it.
    if (mkdir("/FIODIR") != 0) {
        printf("[fileio] mkdir FAILED\n");
        return 1;
    }
    fd = open("/FIODIR/INNER.TXT", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("[fileio] create nested file FAILED: %d\n", fd);
        return 1;
    }
    const char nested_msg[] = "nested\n";
    uint64_t nested_len = strlen(nested_msg);
    if (write(fd, nested_msg, nested_len) != (int64_t)nested_len) {
        printf("[fileio] nested write FAILED\n");
        return 1;
    }
    close(fd);

    fd = open("/FIODIR/INNER.TXT", O_RDONLY);
    if (fd < 0) {
        printf("[fileio] reopen nested file FAILED: %d\n", fd);
        return 1;
    }
    got = read(fd, readback, sizeof(readback) - 1);
    close(fd);
    if (got != (int64_t)nested_len || !check_bytes_equal(readback, nested_msg, (uint64_t)got)) {
        printf("[fileio] nested readback mismatch\n");
        return 1;
    }

    // unlink and verify it's gone.
    if (unlink("/FILEIO.TXT") != 0) {
        printf("[fileio] unlink FAILED\n");
        return 1;
    }
    fd = open("/FILEIO.TXT", O_RDONLY);
    if (fd >= 0) {
        printf("[fileio] FILEIO.TXT still openable after unlink\n");
        return 1;
    }

    // O_CREAT must be Linux's 0x40: a program compiled against Linux
    // headers passes exactly this.
#define LINUX_O_CREAT 0x40
    fd = open("/OCREAT.TMP", 1 /*O_WRONLY*/ | LINUX_O_CREAT);
    if (fd < 0) {
        printf("[fileio] FAILED: O_CREAT 0x40 did not create, rc=%d\n", fd);
        return 1;
    }
    close(fd);
    struct stat st;
    if (stat("/OCREAT.TMP", &st) != 0) {
        printf("[fileio] FAILED: created file not stat-able\n");
        return 1;
    }
    unlink("/OCREAT.TMP");
    printf("[fileio] O_CREAT=0x40 passed\n");

    printf("[fileio] all checks passed\n");
    return 0;
}
