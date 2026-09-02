// randtest.c -- /dev/urandom and getrandom(2).
//
// A random source is easy to get wrong in ways that a "did it return
// bytes?" check cannot see: returning zeros, returning the same buffer
// every time, or returning the same stream to every process. Each of
// those passes a naive test and is catastrophic in use, so each is
// checked here.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>

#define N 256

static int all_same(const unsigned char *b, int n) {
    for (int i = 1; i < n; i++) { if (b[i] != b[0]) { return 0; } }
    return 1;
}

// Distinct byte values seen. A CSPRNG over 256 bytes should touch a
// large fraction of the 256 possible values; a counter or a repeating
// pattern would not.
static int distinct(const unsigned char *b, int n) {
    unsigned char seen[256];
    memset(seen, 0, sizeof seen);
    int d = 0;
    for (int i = 0; i < n; i++) { if (!seen[b[i]]) { seen[b[i]] = 1; d++; } }
    return d;
}

int main(void) {
    unsigned char a[N], b[N];

    // 1. /dev/urandom
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) { printf("[randtest] FAILED: open /dev/urandom (%d)\n", fd); return 1; }
    long n = read(fd, a, N);
    if (n != N) { printf("[randtest] FAILED: short read %d\n", (int)n); close(fd); return 1; }
    long n2 = read(fd, b, N);
    close(fd);
    if (n2 != N) { printf("[randtest] FAILED: short second read %d\n", (int)n2); return 1; }

    if (all_same(a, N)) { printf("[randtest] FAILED: /dev/urandom returned one repeated byte\n"); return 1; }
    if (memcmp(a, b, N) == 0) {
        printf("[randtest] FAILED: two reads returned identical bytes\n");
        return 1;
    }
    int d = distinct(a, N);
    if (d < 100) {
        printf("[randtest] FAILED: only %d distinct byte values in %d bytes\n", d, N);
        return 1;
    }

    // 2. /dev/random must work too, and must not be a copy of the same
    //    stream position.
    int rfd = open("/dev/random", O_RDONLY);
    if (rfd < 0) { printf("[randtest] FAILED: open /dev/random (%d)\n", rfd); return 1; }
    unsigned char c[N];
    if (read(rfd, c, N) != N) { printf("[randtest] FAILED: /dev/random short read\n"); close(rfd); return 1; }
    close(rfd);
    if (memcmp(a, c, N) == 0) { printf("[randtest] FAILED: /dev/random repeated /dev/urandom's bytes\n"); return 1; }

    // 3. getrandom(2), which needs no descriptor.
    unsigned char g[N];
    long gn = getrandom(g, N, 0);
    if (gn != N) { printf("[randtest] FAILED: getrandom returned %d\n", (int)gn); return 1; }
    if (all_same(g, N)) { printf("[randtest] FAILED: getrandom returned one repeated byte\n"); return 1; }
    if (memcmp(g, a, N) == 0) { printf("[randtest] FAILED: getrandom repeated an earlier buffer\n"); return 1; }

    // An unknown flag must be REFUSED, not quietly ignored: a caller
    // asking for a guarantee this kernel does not implement should hear
    // about it.
    if (getrandom(g, N, 0x8000) >= 0) {
        printf("[randtest] FAILED: getrandom accepted an unknown flag\n");
        return 1;
    }

    // 4. Two PROCESSES must not get the same stream. A per-process
    //    generator seeded identically would pass everything above.
    int p[2];
    if (pipe(p) != 0) { printf("[randtest] FAILED: pipe\n"); return 1; }
    int kid = fork();
    if (kid < 0) { printf("[randtest] FAILED: fork\n"); return 1; }
    if (kid == 0) {
        close(p[0]);
        unsigned char mine[N];
        getrandom(mine, N, 0);
        write(p[1], mine, N);
        close(p[1]);
        exit(0);
    }
    close(p[1]);
    unsigned char theirs[N];
    int got = 0;
    while (got < N) {
        long r = read(p[0], theirs + got, (unsigned long)(N - got));
        if (r <= 0) { break; }
        got += (int)r;
    }
    close(p[0]);
    int st = 0; waitpid(kid, &st, 0);
    if (got != N) { printf("[randtest] FAILED: child sent %d bytes\n", got); return 1; }

    unsigned char parent[N];
    getrandom(parent, N, 0);
    if (memcmp(parent, theirs, N) == 0) {
        printf("[randtest] FAILED: a child got the SAME bytes as its parent\n");
        return 1;
    }

    printf("[randtest] urandom, random and getrandom all differ; %d/256 distinct "
           "values; a child's stream differs from its parent's\n", d);
    printf("[randtest] ALL PASSED\n");
    return 0;
}
