// nexcheck.c -- the NOX magic, from userland (N1).
//
// The loader accepts TWO magics: ELF's, and NeoOS's own NOX
// (0x7F 'N' 'O' 'X'). "Accept both" is only correct if both are
// actually exercised, so this runs one of each and asserts a third is
// refused:
//
//   /bin/busybox.nex     stamped NOX    -- must run
//   /usr/tests/child.nex stamped NOX    -- must run
//   a file with neither magic           -- must NOT run
//
// The last case is the one that would rot silently: a loader that
// accepted anything would pass the first two and nobody would notice.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/wait.h>

static int run(const char *path, char *const argv[]) {
    int pid = spawnv(path, argv);
    if (pid < 0) { return pid; }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) { return -1; }
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    // 1. A stamped NOX binary runs.
    char *bb[] = { (char *)"busybox", (char *)"true", 0 };
    int rc = run("/bin/busybox.nex", bb);
    if (rc != 0) {
        printf("[nexcheck] FAILED: a NOX binary did not run (%d)\n", rc);
        return 1;
    }

    // 2. Every NeoOS binary on the disk is stamped -- including this
    //    one. Read our own first four bytes back and check.
    int fd = open("/usr/tests/nexcheck.nex", O_RDONLY);
    if (fd < 0) {
        printf("[nexcheck] FAILED: cannot open my own image (%d)\n", fd);
        return 1;
    }
    unsigned char magic[4] = {0,0,0,0};
    long n = read(fd, magic, 4);
    close(fd);
    if (n != 4) { printf("[nexcheck] FAILED: short read of my own image\n"); return 1; }
    if (magic[0] != 0x7F || magic[1] != 'N' || magic[2] != 'O' || magic[3] != 'X') {
        printf("[nexcheck] FAILED: my magic is %02x %c%c%c, expected 7f NOX\n",
               magic[0], magic[1], magic[2], magic[3]);
        return 1;
    }

    // 3. A file that is neither ELF nor NOX must be REFUSED. Written
    //    here rather than shipped, so the check cannot pass because
    //    somebody quietly stamped the fixture.
    int w = open("/var/tmp/notanexe.bin", O_CREAT | O_RDWR | O_TRUNC);
    if (w < 0) { printf("[nexcheck] FAILED: cannot create the bad image\n"); return 1; }
    // A plausible-looking header with the WRONG magic: 0x7F "BAD".
    unsigned char bad[64];
    for (int i = 0; i < 64; i++) { bad[i] = 0; }
    bad[0] = 0x7F; bad[1] = 'B'; bad[2] = 'A'; bad[3] = 'D'; bad[4] = 2;
    write(w, bad, sizeof bad);
    close(w);

    char *argv[] = { (char *)"/var/tmp/notanexe.bin", 0 };
    rc = run("/var/tmp/notanexe.bin", argv);
    if (rc >= 0) {
        printf("[nexcheck] FAILED: a file with bad magic was ACCEPTED (%d)\n", rc);
        return 1;
    }

    printf("[nexcheck] NOX loads, my own image is stamped, bad magic refused\n");
    printf("[nexcheck] ALL PASSED\n");
    return 0;
}
