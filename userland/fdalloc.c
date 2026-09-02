// fdalloc.c -- open() must return the LOWEST free descriptor.
//
// CS4. Two divergences from POSIX lived in fd_table_alloc, and both
// break shell redirection:
//
//   - the scan began at the bucket of the last allocation, so a
//     descriptor freed below that point was not reused until everything
//     above it filled;
//   - fds 0, 1 and 2 were never handed out at all, so the
//     `close(0); open(file)` idiom -- which is what `sh < file` compiles
//     down to -- returned 3, and the program went on reading its
//     original stdin.
//
// Nothing in the suite noticed, because nothing but a shell cares which
// NUMBER comes back. This does.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#define PROBE "/etc/inittab"

// Past FD_TABLE_SLOTS (512), so allocation moves into the second bucket
// and the per-bucket hint has somewhere wrong to point.
#define FDS 600

static int fail(const char *what, int got, int want) {
    printf("[fdalloc] FAILED: %s returned %d, expected %d\n", what, got, want);
    return 1;
}

int main(void) {
    // 1. Lowest-available in the ordinary case: the first open after
    //    0/1/2 is 3, and freeing 3 gives 3 back rather than 4.
    int a = open(PROBE, O_RDONLY);
    if (a != 3) { return fail("first open", a, 3); }
    int b = open(PROBE, O_RDONLY);
    if (b != 4) { return fail("second open", b, 4); }
    close(a);
    int c = open(PROBE, O_RDONLY);
    if (c != 3) { return fail("open after closing 3", c, 3); }
    close(b); close(c);

    // 2. A hole low down is filled before anything higher.
    //
    //    This must cross a BUCKET boundary to mean anything. The table
    //    is 32 buckets of 512 descriptors and the old hint was per
    //    bucket, so with 40 fds open -- all of them in bucket 0 -- the
    //    hint pointed at bucket 0 anyway and the bug did not show. Only
    //    once allocation has moved into bucket 1 does reopening a hole
    //    in bucket 0 have somewhere wrong to go.
    static int fds[FDS];
    int n = 0;
    for (; n < FDS; n++) {
        fds[n] = open(PROBE, O_RDONLY);
        if (fds[n] < 0) { break; }
    }
    if (n <= 512) {
        // Not a kernel failure -- the machine simply could not hold that
        // many open files -- but the check below would be vacuous, so
        // say so rather than reporting a pass it did not earn.
        printf("[fdalloc] only %d descriptors opened, under the 512 needed to "
               "cross a bucket -- the cross-bucket case was NOT exercised\n", n);
    }
    int hole = fds[5];
    close(hole);
    int refill = open(PROBE, O_RDONLY);
    if (refill != hole) { return fail("refilling a low hole", refill, hole); }
    fds[5] = refill;
    for (int i = 0; i < n; i++) { close(fds[i]); }

    // 3. The redirection idiom itself. stdin is closed and reopened on a
    //    real file; the descriptor must be 0, and reading it must give
    //    the FILE rather than the console.
    close(0);
    int in = open(PROBE, O_RDONLY);
    if (in != 0) { return fail("open after close(0)", in, 0); }
    char buf[16];
    long got = read(0, buf, sizeof(buf));
    if (got <= 0) {
        printf("[fdalloc] FAILED: read from redirected stdin got %d\n", (int)got);
        return 1;
    }
    close(0);

    // Leave fd 0 occupied again. It is not stdin any more -- nothing in
    // this process reads it -- but a process exiting with 0 closed is a
    // shape the rest of the suite need not be exposed to.
    int restore = open("/dev/console", O_RDONLY);
    if (restore != 0) { return fail("restoring fd 0", restore, 0); }

    printf("[fdalloc] lowest-available honoured across %d descriptors "
           "(%d buckets), including fd 0 and a reused hole at %d\n",
           n, (n + 511) / 512, hole);
    printf("[fdalloc] ALL PASSED\n");
    return 0;
}
