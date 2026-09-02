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
#include <auxv.h>

#define BB "/BIN/BUSYBOX.ELF"

static int run(const char *label, char *const argv[]) {
    // spawnVE, not spawnv: the environment is the point of half these
    // invocations, and spawnv passes an empty one. bbspike's own
    // environment came from init, so forwarding it is what makes
    // `sh -c 'echo $PATH'` a test of the whole chain.
    int pid = spawnve(BB, argv, environ);
    if (pid < 0) {
        printf("[bbspike] %s: spawnv FAILED (%d)\n", label, pid);
        return -1;
    }
    int st = 0;
    if (waitpid(pid, &st, 0) < 0) {
        printf("[bbspike] %s: waitpid FAILED\n", label);
        return -1;
    }
    if (WIFSIGNALED(st)) {
        printf("[bbspike] %s: KILLED by signal %d\n", label, WTERMSIG(st));
        return -1;
    }
    if (WIFEXITED(st)) {
        printf("[bbspike] %s: exit %d\n", label, WEXITSTATUS(st));
        return WEXITSTATUS(st);
    }
    printf("[bbspike] %s: ended in an unrecognised state %d\n", label, st);
    return -1;
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

    // BB3: the environment must survive both spawn and exec, and be
    // visible to the shell as shell variables. `sh -c 'echo $PATH'` is
    // the end-to-end check -- it goes kernel entry stack -> musl envp ->
    // ash's variable table -> expansion.
    char *a_env[]  = { (char *)"busybox", (char *)"sh", (char *)"-c",
                       (char *)"echo PATH=$PATH HOME=$HOME", 0 };
    // ...and again through an exec, so a lost environment at the exec
    // boundary shows up separately from a lost one at spawn.
    char *a_env2[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                       (char *)"busybox env", 0 };
    // Four probes, each isolating one hop, and each TAGGING its own
    // output. Untagged probes were unreadable: `env` prints the same
    // four lines whoever ran it, serial output from several processes
    // interleaves, and "which probe printed this" became guesswork.
    //
    //   spawnve  -- the kernel's spawn path carries the environment
    //   ash      -- ash read its own envp and can expand from it
    //   execve   -- the kernel's exec path carries it (execve's 3rd arg)
    //   ash-exec -- ash passes it on when IT execs something
    char *p_spawn[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"echo TAG-spawnve PATH=$PATH", 0 };
    char *p_exec[]  = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"echo TAG-execve PATH=$PATH", 0 };
    char *p_ashex[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"busybox sh -c 'echo TAG-ashexec PATH=$PATH'", 0 };

    // Is the loss PATH-specific, or the whole environment? ash keeps
    // PATH as a special variable with a built-in default, so those are
    // very different diagnoses.
    char *p_expo[]  = { (char *)"busybox", (char *)"sh", (char *)"-c",
                        (char *)"export FOO=bar; busybox sh -c 'echo TAG-nested FOO=$FOO HOME=$HOME PATH=$PATH'", 0 };

    run("env via spawnve", p_spawn);
    run("env via ash-exec", p_ashex);
    run("nested exports",  p_expo);

    // fork WITHOUT exec, which is what BusyBox does to run an applet in
    // a child (FEATURE_SH_STANDALONE). fork+exec is already known to
    // work -- `busybox echo external` above proves it -- so if this
    // fails the difference is the child continuing to run musl code in
    // the inherited address space rather than replacing it.
    char *p_pipe[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                       (char *)"echo one two three | wc -w", 0 };
    run("pipeline",        p_pipe);

    // BB5. `ps` is the only thing that has asked for /proc, and it asked
    // by name: "ps: can't open '/proc': No such file or directory".
    // Piping it through grep asserts the CONTENT rather than the exit
    // status -- ps exits 0 whether or not it found anything, so the exit
    // code alone would pass against an empty /proc.
    char *p_ps[] = { (char *)"busybox", (char *)"ps", 0 };
    char *p_ps1[] = { (char *)"busybox", (char *)"sh", (char *)"-c",
                      (char *)"busybox ps | grep -q INIT.ELF && echo PS-FOUND-INIT", 0 };
    run("ps",              p_ps);
    // This one IS asserted: `grep -q` exits non-zero when it finds
    // nothing, and ps exits 0 whether or not it listed anything, so the
    // exit status of the pipeline is the only thing that distinguishes
    // a working /proc from an empty one.
    if (run("ps | grep init", p_ps1) != 0) {
        printf("[bbspike] FAILED: ps did not list init -- /proc is empty "
               "or unreadable\n");
        return 1;
    }

    int ek = fork();
    if (ek == 0) {
        execve(BB, p_exec, environ);
        printf("[bbspike] execve returned -- it should not have\n");
        exit(9);
    } else if (ek > 0) {
        int st = 0; waitpid(ek, &st, 0);
        printf("[bbspike] env via execve: exit %d\n",
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    }

    printf("[bbspike] --- end of first contact ---\n");
    printf("[bbspike] ALL PASSED\n");
    return 0;
}
