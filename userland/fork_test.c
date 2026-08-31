#include <unistd.h>
#include <stdio.h>
#include <signal.h>

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    // PID visibility: a child must be findable by pid the instant fork
    // returns and gone the instant it is reaped. Guards proc_table
    // becoming the sole process store -- there is no proc_list shadow
    // to drift out of sync. kill(pid, 0) is an existence probe.
    int vpid = fork();
    if (vpid == 0) { exit(7); }
    if (vpid > 0) {
        if (kill(vpid, 0) != 0) {
            printf("[fork_test] FAILED: child not visible by pid after fork\n");
        }
        wait(vpid);
        if (kill(vpid, 0) == 0) {
            printf("[fork_test] FAILED: reaped child still visible by pid\n");
        } else {
            printf("[fork_test] pid visibility passed\n");
        }
    }

    volatile int shared_until_written = 100;

    int pid = fork();
    if (pid < 0) {
        printf("[fork_test] fork FAILED\n");
        return 1;
    }

    if (pid == 0) {
        shared_until_written = 200;
        printf("[fork_test child pid=%d] wrote 200, read back %d\n", getpid(), shared_until_written);
        if (shared_until_written != 200) {
            printf("[fork_test child pid=%d] FAILED: readback mismatch\n", getpid());
            return 1;
        }
        printf("[fork_test child pid=%d] passed\n", getpid());

        int exec_result = exec("/BIN/EXECTARG.ELF");
        printf("[fork_test child pid=%d] exec FAILED, result=%d\n", getpid(), exec_result);
        return 1; // only reached if exec() failed
    }

    shared_until_written = 300;
    printf("[fork_test parent pid=%d, child=%d] wrote 300, read back %d\n", getpid(), pid, shared_until_written);
    if (shared_until_written != 300) {
        printf("[fork_test parent pid=%d] FAILED: readback mismatch\n", getpid());
        return 1;
    }
    int status = wait(pid);
    printf("[fork_test parent pid=%d] child exited code=%d, passed\n", getpid(), status);
    return 0;
}
