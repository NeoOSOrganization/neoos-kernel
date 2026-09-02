// bbspike.c -- BB1: run BusyBox and record what it actually needs.
//
// The roadmap predicted ~25 syscalls BusyBox would want and sequenced
// the build last, after implementing all of them. That is backwards for
// a port: the cheapest way to learn what a binary needs is to run it.
// This runs a handful of BusyBox invocations, in increasing order of
// ambition, and reports how each one ended.
//
// The DELIVERABLE IS THE FAILURE LIST, not a working shell. An applet
// that dies is a result, not a test failure -- so this always reports
// ALL PASSED as long as the spike itself ran. What it must never do is
// hide an outcome: every invocation's exit status is printed, including
// the signal that killed it.
//
// The shim answers an unmapped syscall number with -ENOSYS rather than
// crashing, so a missing call surfaces as a applet-level error message
// or a clean non-zero exit rather than as a fault.

#include <unistd.h>
#include <stdio.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#define BB "/BIN/BUSYBOX.ELF"

static void run(const char *label, char *const argv[]) {
    int pid = spawnv(BB, argv);
    if (pid < 0) {
        printf("[bbspike] %s: spawnv FAILED (%d)\n", label, pid);
        return;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        printf("[bbspike] %s: waitpid FAILED\n", label);
        return;
    }
    if (WIFSIGNALED(st)) {
        printf("[bbspike] %s: KILLED by signal %d\n", label, WTERMSIG(st));
    } else if (WIFEXITED(st)) {
        printf("[bbspike] %s: exit %d\n", label, WEXITSTATUS(st));
    } else {
        printf("[bbspike] %s: ended in an unrecognised state %d\n", label, st);
    }
}

int main(void) {
    printf("[bbspike] --- BusyBox on NeoOS, first contact ---\n");

    // In increasing order of what they demand of the kernel: an applet
    // that does nothing, one that writes, one that reads the
    // filesystem, then the shell itself.
    char *a_true[]  = { (char *)"busybox", (char *)"true", 0 };
    char *a_echo[]  = { (char *)"busybox", (char *)"echo", (char *)"hello from busybox", 0 };
    char *a_uname[] = { (char *)"busybox", (char *)"uname", (char *)"-a", 0 };
    char *a_pwd[]   = { (char *)"busybox", (char *)"pwd", 0 };
    char *a_ls[]    = { (char *)"busybox", (char *)"ls", (char *)"/", 0 };
    char *a_cat[]   = { (char *)"busybox", (char *)"cat", (char *)"/ETC/INITTAB", 0 };
    char *a_shc[]   = { (char *)"busybox", (char *)"sh", (char *)"-c", (char *)":", 0 };
    char *a_shec[]  = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"echo shell works", 0 };
    char *a_shext[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"busybox echo external", 0 };

    run("true",          a_true);
    run("echo",          a_echo);
    run("uname -a",      a_uname);
    run("pwd",           a_pwd);
    run("ls /",          a_ls);
    run("cat INITTAB",   a_cat);
    run("sh -c :",       a_shc);
    run("sh -c echo",    a_shec);
    run("sh -c external",a_shext);

    // BB2's three, asserted rather than eyeballed. uname is the one
    // with a visible symptom: before it existed the shim returned
    // -ENOSYS and BusyBox printed a blank line rather than an error.
    struct utsname u;
    if (uname(&u) != 0) {
        printf("[bbspike] FAILED: uname returned non-zero\n");
        return 1;
    }
    if (u.sysname[0] == 0 || u.machine[0] == 0 || u.release[0] == 0) {
        printf("[bbspike] FAILED: uname left fields empty\n");
        return 1;
    }
    printf("[bbspike] uname: %s %s %s\n", u.sysname, u.release, u.machine);

    int ppid = getppid();
    if (ppid <= 0) {
        printf("[bbspike] FAILED: getppid returned %d\n", ppid);
        return 1;
    }
    printf("[bbspike] getppid: %d (init is 1)\n", ppid);

    printf("[bbspike] --- end of first contact ---\n");
    printf("[bbspike] ALL PASSED\n");
    return 0;
}
