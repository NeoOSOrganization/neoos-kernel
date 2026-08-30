#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>

// Writes a known string to `path`, reads it back, and reports whether
// it survived. Returns 1 on success.
static int roundtrip(const char *path, const char *label) {
    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 0) {
        printf("[vfstest] %s FAILED: open returned %d\n", label, fd);
        return 0;
    }

    const char *msg = "vfs-roundtrip";
    int64_t w = write(fd, msg, 13);
    if (w != 13) {
        printf("[vfstest] %s FAILED: write returned %d\n", label, (int)w);
        close(fd);
        return 0;
    }

    if (lseek(fd, 0, SEEK_SET) != 0) {
        printf("[vfstest] %s FAILED: lseek\n", label);
        close(fd);
        return 0;
    }

    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    int64_t r = read(fd, buf, 13);
    close(fd);

    if (r != 13) {
        printf("[vfstest] %s FAILED: read returned %d\n", label, (int)r);
        return 0;
    }
    for (int i = 0; i < 13; i++) {
        if (buf[i] != msg[i]) {
            printf("[vfstest] %s FAILED: content mismatch at %d\n", label, i);
            return 0;
        }
    }
    printf("[vfstest] %s roundtrip passed\n", label);
    return 1;
}

// The fd table's own contract, checked directly rather than inferred
// from the roundtrips: close has to REPORT success, a second close of
// the same fd has to fail, and a process has to be able to hold far
// more than the 16 fds the old flat array allowed.
#define FD_BURST 64

static int fd_lifecycle(void) {
    int fd = open("/tmp/FDTEST.TXT", O_CREAT | O_RDWR | O_TRUNC);
    if (fd < 3) {
        printf("[vfstest] fd lifecycle FAILED: open returned %d\n", fd);
        return 0;
    }
    int rc = close(fd);
    if (rc != 0) {
        printf("[vfstest] fd lifecycle FAILED: close returned %d (want 0)\n", rc);
        return 0;
    }
    rc = close(fd);
    if (rc >= 0) {
        printf("[vfstest] fd lifecycle FAILED: double close returned %d (want < 0)\n", rc);
        return 0;
    }

    int fds[FD_BURST];
    for (int i = 0; i < FD_BURST; i++) {
        fds[i] = open("/tmp/FDTEST.TXT", O_RDONLY);
        if (fds[i] < 3) {
            printf("[vfstest] fd lifecycle FAILED: open %d returned %d\n", i, fds[i]);
            for (int j = 0; j < i; j++) { close(fds[j]); }
            return 0;
        }
        for (int j = 0; j < i; j++) {
            if (fds[j] == fds[i]) {
                printf("[vfstest] fd lifecycle FAILED: fd %d handed out twice\n", fds[i]);
                for (int k = 0; k <= i; k++) { close(fds[k]); }
                return 0;
            }
        }
    }
    for (int i = 0; i < FD_BURST; i++) {
        if (close(fds[i]) != 0) {
            printf("[vfstest] fd lifecycle FAILED: close of fd %d\n", fds[i]);
            return 0;
        }
    }

    printf("[vfstest] fd lifecycle passed (%d concurrent fds)\n", FD_BURST);
    return 1;
}

static void list(const char *path) {
    DIR *d = opendir(path);
    if (!d) {
        printf("[vfstest] listing %s FAILED: opendir\n", path);
        return;
    }
    printf("[vfstest] listing %s:\n", path);
    struct dirent *e;
    while ((e = readdir(d)) != 0) {
        printf("[vfstest]   %s type=%d\n", e->name, e->type);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= roundtrip("/RT.TXT",     "fat16 (/)");
    ok &= roundtrip("/tmp/RT.TXT", "ramfs (/tmp)");
    ok &= roundtrip("/mnt/RT.TXT", "fat32 (/mnt)");
    ok &= fd_lifecycle();

    // Two fds on one path must share a vnode: the write through `a`
    // has to be visible through `b` with no reopen in between.
    int a = open("/tmp/ALIAS.TXT", O_CREAT | O_RDWR | O_TRUNC);
    int b = open("/tmp/ALIAS.TXT", O_RDONLY);
    if (a < 0 || b < 0) {
        printf("[vfstest] alias FAILED: open a=%d b=%d\n", a, b);
        ok = 0;
    } else {
        write(a, "shared", 6);
        char buf[8];
        for (int i = 0; i < 8; i++) { buf[i] = 0; }
        int64_t r = read(b, buf, 6);
        if (r != 6 || buf[0] != 's' || buf[5] != 'd') {
            printf("[vfstest] alias FAILED: read %d bytes, buf=%s\n", (int)r, buf);
            ok = 0;
        } else {
            printf("[vfstest] vnode aliasing passed\n");
        }
        close(a);
        close(b);
    }

    list("/");
    list("/dev");
    list("/tmp");
    list("/mnt");

    printf("[vfstest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
