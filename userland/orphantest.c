#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <neoos_test.h>

// Orphan reparenting: when a process exits, the kernel hands its still
// -alive children to PID 1. This test builds parent -> child -> grand-
// child, has the child exit while the grandchild is still running, and
// checks (via the SYS_TEST_HOOK that reads a pid's parent_pid) that the
// grandchild's parent becomes 1.
int main(void) {
    int p[2];
    if (pipe(p) != 0) { printf("[orphantest] FAILED: pipe\n"); return 1; }

    int child = fork();
    if (child == 0) {
        int gc = fork();
        if (gc == 0) {
            close(p[0]); close(p[1]);
            // Outlive our parent by a wide margin, then exit cleanly.
            for (volatile long i = 0; i < 120000000L; i++) { }
            printf("[orphantest] grandchild done\n");
            exit(0);
        }
        // child: hand the grandchild pid up, then exit -> orphans it
        close(p[0]);
        write(p[1], &gc, sizeof gc);
        close(p[1]);
        exit(0);
    }

    close(p[1]);
    int gc = -1;
    if (read(p[0], &gc, sizeof gc) != (long)sizeof gc || gc <= 0) {
        printf("[orphantest] FAILED: did not receive grandchild pid\n");
        return 1;
    }
    close(p[0]);

    int st;
    wait4(child, &st, 0, 0);            // reap our direct child

    // The child is gone; the grandchild is still spinning. Its parent
    // must now be PID 1.
    int pp = -1;
    for (int tries = 0; tries < 200000; tries++) {
        pp = neoos_test_parent_pid(gc);
        if (pp == 1 || pp < 0) { break; }
        yield();
    }
    if (pp == -38 /* -ENOSYS: production build, hook unavailable */) {
        printf("[orphantest] skipped parent_pid check (no test hook)\n");
    } else if (pp != 1 && pp != -3 /* -ESRCH: reparented then reaped by init */) {
        printf("[orphantest] FAILED: grandchild parent_pid is %d, expected 1\n", pp);
        return 1;
    }

    // The grandchild is not ours, so wait4(-1) has nothing to return.
    int w = wait4(-1, &st, 0, 0);
    if (w >= 0) {
        printf("[orphantest] FAILED: wait4(-1) returned pid %d, expected -ECHILD\n", w);
        return 1;
    }

    printf("[orphantest] ALL PASSED\n");
    return 0;
}
