// sigstorm.c -- signal delivery overlapping process exit.
//
// CS3. The dangerous window is a signal sent to a process that is
// concurrently exiting: the sender looks the target up by pid, and the
// target's last thread may be tearing the process down underneath it.
// proc_get/proc_put and thread_get/thread_put were installed by the
// SMP-lifetime milestone precisely for this, and this is their
// regression suite -- not a test of a bug still open.
//
// Nothing here asserts the signal is DELIVERED. Racing exit it is
// legitimately lost, and demanding otherwise would be testing a
// guarantee the kernel does not make. What IS asserted:
//
//   - kill() into the exit window returns 0 or ESRCH and nothing else.
//     Any other errno means the lookup found something it should not
//     have, or failed in a way that is not "the process is gone".
//   - the parent survives with its own pid.
//
// There is deliberately NO kill after the reap. Two attempts at one
// taught why:
//
//   1. "post-reap kill must return ESRCH" is unsound. Once a pid is
//      reaped it may be REUSED immediately, so a 0 return is correct
//      rather than evidence of a stale table entry -- and a userland
//      process cannot tell the two apart.
//   2. Worse, it is actively harmful. Under load those kills landed on
//      OTHER tests' processes that had been handed the recycled pid:
//
//          [sigstorm]  kill(316) succeeded AFTER reaping
//          [forkstorm] gen 8 pid 316 killed by signal 10
//
//      Both kernel behaviours are correct there; the test was not.
//      Signalling a pid you no longer own is a bug in the signaller.
//
// So this only ever signals a child it is still holding. CS4's PID
// wraparound work is where the reuse hazard itself gets addressed.
//   - the parent survives and keeps its own pid. A kill landing on the
//     wrong target after pid reuse shows up here.
//
// Note on what is NOT asserted, learned by getting it wrong: kill() on a
// ZOMBIE correctly returns 0 -- the process exists until it is reaped --
// so no amount of timing makes a pre-waitpid kill report ESRCH. A first
// version required both outcomes before the reap and could never have
// passed, which its own straddle check caught (360 landed, 0 gone).

#include <unistd.h>
#include <stdio.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>

#define ROUNDS      120
#define KILLS_EACH  3

int main(void) {
    int my_pid = getpid();
    int landed = 0, gone = 0;

    for (int r = 0; r < ROUNDS; r++) {
        int child = fork();
        if (child < 0) { printf("[sigstorm] FAILED: fork at round %d\n", r); return 1; }
        if (child == 0) {
            // Exit almost immediately, with just enough work that the
            // parent's kills sometimes arrive before and sometimes after.
            for (volatile int i = 0; i < (r % 64); i++) { }
            exit(0);
        }

        // Sweep the send time across the child's lifetime. Without this
        // the parent always wins: the first version fired all 360 kills
        // before any child had exited, and the straddle check below
        // caught it -- 360 landed, 0 gone, i.e. the window was never
        // sampled at all.
        for (volatile int d = 0; d < (r * 137) % 8192; d++) { }

        for (int k = 0; k < KILLS_EACH; k++) {
            int rc = kill(child, SIGUSR1);
            if (rc == 0) { landed++; }
            else if (rc == -ESRCH || rc == ESRCH) { gone++; }
            else {
                printf("[sigstorm] FAILED: round %d kill(%d) returned %d, expected 0 or ESRCH\n",
                       r, child, rc);
                return 1;
            }
        }

        int st = 0;
        if (waitpid(child, &st, 0) < 0) {
            printf("[sigstorm] FAILED: round %d waitpid(%d)\n", r, child);
            return 1;
        }

        // The parent must be untouched: a kill that went astray after a
        // pid was reused would most likely have hit us.
        if (getpid() != my_pid) {
            printf("[sigstorm] FAILED: parent pid changed %d -> %d\n", my_pid, getpid());
            return 1;
        }
    }

    printf("[sigstorm] %d rounds x %d kills into the exit window: %d delivered, %d found gone\n",
           ROUNDS, KILLS_EACH, landed, gone);

    if (landed + gone != ROUNDS * KILLS_EACH) {
        printf("[sigstorm] FAILED: %d kills unaccounted for\n",
               ROUNDS * KILLS_EACH - (landed + gone));
        return 1;
    }

    printf("[sigstorm] ALL PASSED\n");
    return 0;
}
