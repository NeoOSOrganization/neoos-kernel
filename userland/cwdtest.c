#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

static int failures;

static void fail(const char *what, int rc) {
    printf("[cwdtest] FAILED: %s rc=%d\n", what, rc);
    failures++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Every process must have a cwd the moment it is launched, with no
// chdir needed first. That is the property this whole milestone is
// about, so it is checked before anything else.
static void check_inherited_cwd(void) {
    char buf[128];
    if (!getcwd(buf, sizeof(buf))) { fail("getcwd on a fresh process", -1); return; }
    if (!str_eq(buf, "/")) {
        printf("[cwdtest] FAILED: fresh cwd is \"%s\", expected \"/\"\n", buf);
        failures++;
        return;
    }
    printf("[cwdtest] launched with a cwd: %s\n", buf);
}

static void check_chdir_and_getcwd(void) {
    int rc = chdir("/DIR");
    if (rc != 0) { fail("chdir(/DIR)", rc); return; }

    char buf[128];
    if (!getcwd(buf, sizeof(buf))) { fail("getcwd after chdir", -1); return; }
    if (!str_eq(buf, "/DIR")) {
        printf("[cwdtest] FAILED: cwd is \"%s\", expected \"/DIR\"\n", buf);
        failures++;
        return;
    }
    printf("[cwdtest] chdir + getcwd passed\n");
}

// The point of the cwd: a bare name resolves against it. /DIR/NESTED.TXT
// is placed on the FAT16 image by the Makefile.
static void check_relative_open(void) {
    int fd = open("NESTED.TXT", O_RDONLY);
    if (fd < 0) { fail("open(NESTED.TXT) relative to /DIR", fd); return; }
    char buf[8];
    int64_t n = read(fd, buf, 6);
    close(fd);
    if (n != 6) { fail("read from relative open", (int)n); return; }
    printf("[cwdtest] relative open passed\n");
}

static void check_dot_and_dotdot(void) {
    if (chdir("./") != 0) { fail("chdir(./)", -1); return; }
    char buf[128];
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/DIR")) {
        fail("\".\" changed the cwd", -1);
        return;
    }

    if (chdir("..") != 0) { fail("chdir(..)", -1); return; }
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/")) {
        printf("[cwdtest] FAILED: after \"..\" cwd is \"%s\", expected \"/\"\n", buf);
        failures++;
        return;
    }

    // ".." at the root stays at the root rather than escaping it.
    if (chdir("..") != 0) { fail("chdir(..) at root", -1); return; }
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/")) {
        fail("\"..\" escaped the root", -1);
        return;
    }
    printf("[cwdtest] . and .. passed\n");
}

static void check_errors(void) {
    int rc = chdir("/NO_SUCH_DIR");
    if (rc != -ENOENT) { fail("chdir to a missing directory should be -ENOENT", rc); return; }

    // A FILE is not a directory, and chdir must refuse it rather than
    // leave the process with a cwd nothing can resolve against.
    rc = chdir("/HELLO.TXT");
    if (rc != -ENOTDIR) { fail("chdir to a file should be -ENOTDIR", rc); return; }

    char buf[128];
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/")) {
        fail("a failed chdir changed the cwd", -1);
        return;
    }

    // Linux's getcwd reports -ERANGE through a NULL return when the
    // buffer cannot hold the path plus its NUL.
    char tiny[2];
    if (getcwd(tiny, sizeof(tiny))) {
        // "/" plus NUL is exactly 2, so this one must SUCCEED.
        if (!str_eq(tiny, "/")) { fail("getcwd into an exactly-sized buffer", -1); return; }
    } else {
        fail("getcwd into an exactly-sized buffer returned NULL", -1);
        return;
    }
    char too_small[1];
    if (getcwd(too_small, sizeof(too_small))) {
        fail("getcwd into an undersized buffer should fail", -1);
        return;
    }
    printf("[cwdtest] error cases passed\n");
}

// The cwd is inherited across fork, as on Linux.
static void check_fork_inherits(void) {
    if (chdir("/DIR") != 0) { fail("chdir(/DIR) before fork", -1); return; }

    int pid = fork();
    if (pid < 0) { fail("fork", pid); return; }
    if (pid == 0) {
        char buf[128];
        if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/DIR")) { exit(1); }
        // A child's chdir must not reach back into the parent.
        chdir("/");
        exit(0);
    }

    int st = 0;
    wait4(pid, &st, 0, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fail("child did not inherit the cwd", st);
        return;
    }

    char buf[128];
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/DIR")) {
        printf("[cwdtest] FAILED: child's chdir changed the parent's cwd to \"%s\"\n", buf);
        failures++;
        return;
    }
    printf("[cwdtest] fork inheritance passed\n");
}

int main(void) {
    check_inherited_cwd();
    check_chdir_and_getcwd();
    check_relative_open();
    check_dot_and_dotdot();
    check_errors();
    check_fork_inherits();

    if (failures) {
        printf("[cwdtest] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[cwdtest] ALL PASSED\n");
    return 0;
}
