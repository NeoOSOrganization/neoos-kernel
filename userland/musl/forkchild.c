// forkchild.c -- what a musl program can still do after fork().
//
// BusyBox runs an applet in a forked child WITHOUT exec'ing
// (FEATURE_SH_STANDALONE), so `echo one two three | wc -w` is two forks
// and no exec. That segfaults, while fork+exec works -- `busybox echo
// external` runs fine. The difference is the child CONTINUING to run
// musl code in the inherited address space instead of replacing it.
//
// This narrows that to a step. The child does progressively more, and
// reports before each one, so the last line printed names what broke.
// Nothing here is BusyBox-specific: it is ordinary musl.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static void say(const char *s) { write(1, s, strlen(s)); }

int main(void) {
    say("[muslfork] parent alive\n");

    int pid = fork();
    if (pid < 0) { say("[muslfork] FAILED: fork\n"); return 1; }

    if (pid == 0) {
        // Raw write first: no libc state involved beyond the syscall.
        say("[muslfork] child: write works\n");

        // Stack and static memory.
        volatile char buf[256];
        for (int i = 0; i < 256; i++) { buf[i] = (char)i; }
        if (buf[100] != 100) { say("[muslfork] child: FAILED stack\n"); _exit(2); }
        say("[muslfork] child: stack works\n");

        // The allocator, which gets its arenas from mmap -- the thing
        // BB0 found fork was not inheriting properly.
        char *p = malloc(64 * 1024);
        if (!p) { say("[muslfork] child: FAILED malloc\n"); _exit(3); }
        memset(p, 0x5a, 64 * 1024);
        if (p[65535] != 0x5a) { say("[muslfork] child: FAILED malloc readback\n"); _exit(4); }
        say("[muslfork] child: malloc works\n");
        free(p);

        // A fresh, larger allocation the parent never touched.
        char *q = malloc(1024 * 1024);
        if (!q) { say("[muslfork] child: FAILED big malloc\n"); _exit(5); }
        memset(q, 0x33, 1024 * 1024);
        say("[muslfork] child: big malloc works\n");
        free(q);

        // stdio, which has its own buffers and locks.
        printf("[muslfork] child: printf works\n");
        fflush(stdout);

        _exit(0);
    }

    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        printf("[muslfork] FAILED: child killed by signal %d\n", WTERMSIG(st));
        return 1;
    }
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        printf("[muslfork] FAILED: child exit %d\n",
               WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        return 1;
    }
    printf("[muslfork] ALL PASSED\n");
    return 0;
}
