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
    int rc = chdir("/usr/share/test/dir");
    if (rc != 0) { fail("chdir(/usr/share/test/dir)", rc); return; }

    char buf[128];
    if (!getcwd(buf, sizeof(buf))) { fail("getcwd after chdir", -1); return; }
    if (!str_eq(buf, "/usr/share/test/dir")) {
        printf("[cwdtest] FAILED: cwd is \"%s\", expected \"/usr/share/test/dir\"\n", buf);
        failures++;
        return;
    }
    printf("[cwdtest] chdir + getcwd passed\n");
}

// The point of the cwd: a bare name resolves against it. /usr/share/test/dir/nested.txt
// is placed on the FAT16 image by the Makefile.
static void check_relative_open(void) {
    int fd = open("nested.txt", O_RDONLY);
    if (fd < 0) { fail("open(nested.txt) relative to /usr/share/test/dir", fd); return; }
    char buf[8];
    int64_t n = read(fd, buf, 6);
    close(fd);
    if (n != 6) { fail("read from relative open", (int)n); return; }
    printf("[cwdtest] relative open passed\n");
}

static void check_dot_and_dotdot(void) {
    if (chdir("./") != 0) { fail("chdir(./)", -1); return; }
    char buf[128];
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/usr/share/test/dir")) {
        fail("\".\" changed the cwd", -1);
        return;
    }

    // ".." from /usr/share/test/dir is its PARENT, not the root. The
    // test used to sit in /DIR and could not tell the two apart -- one
    // level up from a top-level directory is the root either way. The
    // clean layout gives it a real parent to check against.
    if (chdir("..") != 0) { fail("chdir(..)", -1); return; }
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/usr/share/test")) {
        printf("[cwdtest] FAILED: after \"..\" cwd is \"%s\", expected \"/usr/share/test\"\n", buf);
        failures++;
        return;
    }

    // ".." at the root stays at the root rather than escaping it. Go
    // there explicitly first: the test now starts three levels down
    // (/usr/share/test/dir), so a single ".." no longer arrives at the
    // root the way it did when the fixture sat in /DIR.
    if (chdir("/") != 0) { fail("chdir(/)", -1); return; }
    if (chdir("..") != 0) { fail("chdir(..) at root", -1); return; }
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/")) {
        fail("\"..\" escaped the root", -1);
        return;
    }
    printf("[cwdtest] . and .. passed\n");
}

static void check_errors(void) {
    int rc = chdir("/no_such_dir");
    if (rc != -ENOENT) { fail("chdir to a missing directory should be -ENOENT", rc); return; }

    // A FILE is not a directory, and chdir must refuse it rather than
    // leave the process with a cwd nothing can resolve against.
    rc = chdir("/usr/share/test/hello.txt");
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
    if (chdir("/usr/share/test/dir") != 0) { fail("chdir(/usr/share/test/dir) before fork", -1); return; }

    int pid = fork();
    if (pid < 0) { fail("fork", pid); return; }
    if (pid == 0) {
        char buf[128];
        if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/usr/share/test/dir")) { exit(1); }
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
    if (!getcwd(buf, sizeof(buf)) || !str_eq(buf, "/usr/share/test/dir")) {
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
