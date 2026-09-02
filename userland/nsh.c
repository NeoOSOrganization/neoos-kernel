// nsh -- the NeoOS shell.
//
// Deliberately minimal (N4). BusyBox's ash is present and is a far
// better shell; nsh exists to be NeoOS's OWN, to be what a login session
// lands in, and to be small enough to read in one sitting.
//
// What it does: a prompt, one line at a time, split on whitespace.
// Builtins cd / pwd / exit / echo / help / env. Everything else is
// looked up along PATH, spawned, waited for, and its exit status
// reported when non-zero.
//
// What it deliberately does NOT do: quoting, pipes, redirection,
// variables, globbing, job control, scripting. An argument containing a
// space cannot be written. That is a documented limit, not an oversight
// -- `busybox sh` is one word away whenever a session needs a real
// shell, and duplicating ash badly would be worse than not duplicating
// it at all.

#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <auxv.h>
#include <sys/wait.h>

// nsh writes to STDOUT, never through printf.
//
// libneoos's printf targets /dev/kmsg -- the serial diagnostic channel
// -- not fd 1 (see lib/stdio.c). That is right for a test reporting
// into the boot log and wrong for a shell, whose entire output is
// supposed to reach the terminal its user is looking at. The first
// version used printf and produced a shell that appeared completely
// silent on its own tty while narrating itself into the kernel log.
static void out(const char *s) {
    uint64_t n = 0;
    while (s[n]) { n++; }
    if (n) { write(1, s, n); }
}

static void out_int(long v) {
    char d[24];
    int n = 0;
    if (v < 0) { out("-"); v = -v; }
    do { d[n++] = (char)('0' + (v % 10)); v /= 10; } while (v && n < 24);
    char buf[24];
    int k = 0;
    while (n > 0) { buf[k++] = d[--n]; }
    write(1, buf, (unsigned long)k);
}

#define LINE_MAX  512
#define MAX_ARGS  32
#define PATH_MAX  256

static char line[LINE_MAX];
static char *argv_buf[MAX_ARGS + 1];

// The environment value for `name`, or 0. There is no getenv in
// libneoos -- musl has one, but nsh is a NeoOS-native program and lib/
// keeps only what has no POSIX analogue.
static const char *env_get(const char *name) {
    uint64_t n = strlen(name);
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=') { return *e + n + 1; }
    }
    return 0;
}

// Reads one line from stdin. Returns its length, 0 for an empty line, or
// -1 at end of input.
//
// One byte at a time, deliberately: the terminal is in canonical mode,
// so a read returns at most one line, but a BLOCK read would happily
// return the next line too if one had already been typed ahead. Reading
// to the newline keeps the leftover in the tty rather than in a buffer
// this shell would then have to manage.
static int read_line(void) {
    int n = 0;
    for (;;) {
        char c;
        long r = read(0, &c, 1);
        if (r <= 0) { return n > 0 ? n : -1; }
        if (c == '\n' || c == '\r') { line[n] = 0; return n; }
        if (n < LINE_MAX - 1) { line[n++] = c; }
        // Past LINE_MAX the rest of the line is DISCARDED rather than
        // silently starting a second command.
    }
}

// Splits `line` in place. Returns argc.
static int split(void) {
    int argc = 0;
    char *p = line;
    while (*p && argc < MAX_ARGS) {
        while (*p == ' ' || *p == '\t') { *p++ = 0; }
        if (!*p) { break; }
        argv_buf[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') { p++; }
    }
    argv_buf[argc] = 0;
    return argc;
}

static int has_slash(const char *s) {
    for (int i = 0; s[i]; i++) { if (s[i] == '/') { return 1; } }
    return 0;
}

// Resolves `cmd` against PATH into `out`. A name containing a slash is
// used as given, as every shell does -- that is how ./prog works.
static int resolve(const char *cmd, char *out) {
    if (has_slash(cmd)) {
        uint64_t n = strlen(cmd);
        if (n >= PATH_MAX) { return 0; }
        memcpy(out, cmd, n + 1);
        return 1;
    }
    const char *path = env_get("PATH");
    if (!path) { path = "/bin"; }

    while (*path) {
        int k = 0;
        while (*path && *path != ':' && k < PATH_MAX - 2) { out[k++] = *path++; }
        while (*path && *path != ':') { path++; }          // overlong element
        if (*path == ':') { path++; }
        if (k == 0) { continue; }
        if (out[k - 1] != '/') { out[k++] = '/'; }
        uint64_t n = strlen(cmd);
        if (k + n >= PATH_MAX) { continue; }
        memcpy(out + k, cmd, n + 1);

        // Probing with open() rather than stat() keeps this to one
        // syscall and answers the question that matters: can it be read?
        int fd = open(out, O_RDONLY);
        if (fd >= 0) { close(fd); return 1; }

        // ...and again with `.nex` appended. NeoOS executables carry the
        // extension, and making a person type `busybox.nex` every time
        // would be noise -- the shell knows what an executable is called
        // here. Tried SECOND so an extensionless file of that exact name
        // still wins, which is what a shell must do for `./prog`.
        uint64_t e = strlen(out);
        if (e + 4 < PATH_MAX) {
            out[e] = '.'; out[e+1] = 'n'; out[e+2] = 'e'; out[e+3] = 'x'; out[e+4] = 0;
            fd = open(out, O_RDONLY);
            if (fd >= 0) { close(fd); return 1; }
            out[e] = 0;
        }
    }
    return 0;
}

static void builtin_help(void) {
    out("nsh -- the NeoOS shell. Builtins:\n");
    out("  cd [dir]   change directory (no argument: $HOME)\n");
    out("  pwd        print the working directory\n");
    out("  echo ...   print the arguments\n");
    out("  cat FILE   print a file\n");
    out("  clear      clear the screen\n");
    out("  env        print the environment\n");
    out("  help       this list\n");
    out("  exit [n]   leave the shell\n");
    out("Anything else is looked up along $PATH and run.\n");
    out("No quoting, pipes or redirection -- run `busybox sh` for those.\n");
}

static int to_int(const char *s) {
    int v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) { v = v * 10 + (s[i] - '0'); }
    return v;
}

// Copies a file to stdout. A builtin rather than a program because the
// startup file needs it before anything else is guaranteed to exist,
// and because a shell with no pipes should at least be able to show you
// a file.
static int builtin_cat(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            out("nsh: cat: "); out(argv[i]); out(": cannot open\n");
            continue;
        }
        char buf[512];
        long n;
        while ((n = read(fd, buf, sizeof buf)) > 0) { write(1, buf, (unsigned long)n); }
        close(fd);
    }
    return 1;
}

// Returns 1 if the command was a builtin (and *quit is set to leave).
static int run_builtin(int argc, char **argv, int *quit, int *status) {
    if (strcmp(argv[0], "exit") == 0) {
        *quit = 1;
        *status = (argc > 1) ? to_int(argv[1]) : 0;
        return 1;
    }
    if (strcmp(argv[0], "help") == 0) { builtin_help(); return 1; }
    if (strcmp(argv[0], "pwd") == 0) {
        char buf[PATH_MAX];
        if (getcwd(buf, sizeof buf)) { out(buf); out("\n"); }
        else { out("nsh: pwd: cannot read the working directory\n"); }
        return 1;
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *dir = (argc > 1) ? argv[1] : env_get("HOME");
        if (!dir) { dir = "/"; }
        int rc = chdir(dir);
        if (rc != 0) { out("nsh: cd: "); out(dir); out(": error "); out_int(rc); out("\n"); }
        return 1;
    }
    if (strcmp(argv[0], "echo") == 0) {
        for (int i = 1; i < argc; i++) {
            out(argv[i]);
            if (i + 1 < argc) { out(" "); }
        }
        out("\n");
        return 1;
    }
    if (strcmp(argv[0], "cat") == 0) { return builtin_cat(argc, argv); }
    if (strcmp(argv[0], "clear") == 0) {
        // The terminal's own escape: home the cursor and erase the
        // screen INCLUDING the scrollback. The logo is ordinary shell
        // output, printed by the startup file, so clear removes it like
        // anything else -- which is the point of printing it here rather
        // than painting it outside the terminal.
        out("\033[H\033[2J\033[3J");
        return 1;
    }
    if (strcmp(argv[0], "env") == 0) {
        for (char **e = environ; e && *e; e++) { out(*e); out("\n"); }
        return 1;
    }
    return 0;
}

// Runs one line: split, builtin, or PATH lookup + spawn. Returns 1 if
// the shell was asked to leave.
static int run_line(int *status) {
    int argc = split();
    if (argc == 0) { return 0; }

    int quit = 0;
    if (run_builtin(argc, argv_buf, &quit, status)) { return quit; }

    char path[PATH_MAX];
    if (!resolve(argv_buf[0], path)) {
        out("nsh: "); out(argv_buf[0]); out(": not found\n");
        *status = 127;
        return 0;
    }

    int pid = spawnve(path, argv_buf, environ);
    if (pid < 0) {
        out("nsh: "); out(path); out(": cannot run ("); out_int(pid); out(")\n");
        *status = 126;
        return 0;
    }
    int st = 0;
    waitpid(pid, &st, 0);
    if (WIFSIGNALED(st)) {
        out("nsh: "); out(argv_buf[0]); out(": killed by signal ");
        out_int(WTERMSIG(st)); out("\n");
        *status = 128 + WTERMSIG(st);
    } else if (WIFEXITED(st)) {
        *status = WEXITSTATUS(st);
        // Reported only when non-zero: a shell that announced every
        // success would be unusable.
        if (*status != 0) {
            out("nsh: "); out(argv_buf[0]); out(": exit "); out_int(*status); out("\n");
        }
    }
    return 0;
}

// The startup file, in the spirit of .bashrc: every non-blank,
// non-comment line is a command nsh runs before the first prompt. That
// is the whole format -- there is no separate configuration language,
// because "a list of commands" already covers setting a prompt, showing
// a banner, or changing directory, and a second syntax would be a
// second thing to learn and to get wrong.
//
// Missing is not an error: a system without one simply starts quiet.
static void run_rc(const char *path, int *status) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { return; }

    // Read the whole file, then walk it line by line. The lines are
    // copied into `line` because run_line SPLITS ITS BUFFER IN PLACE.
    static char rc[4096];
    int n = 0, r;
    while (n < (int)sizeof rc - 1 && (r = (int)read(fd, rc + n, sizeof rc - 1 - n)) > 0) {
        n += r;
    }
    close(fd);
    rc[n] = 0;

    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && rc[j] != '\n') { j++; }
        int len = j - i;
        if (len > LINE_MAX - 1) { len = LINE_MAX - 1; }
        memcpy(line, rc + i, (uint64_t)len);
        line[len] = 0;
        i = j + 1;

        // Skip blanks and comments.
        int k = 0;
        while (line[k] == ' ' || line[k] == '\t') { k++; }
        if (line[k] == 0 || line[k] == '#') { continue; }

        if (run_line(status)) { break; }   // `exit` in the rc file
    }
}

int main(void) {
    const char *ps1 = env_get("PS1");
    if (!ps1) { ps1 = "nsh$ "; }

    int status = 0;

    // A per-user file would go here too once N5 gives users homes; for
    // now there is one system-wide file.
    run_rc("/etc/nshrc", &status);

    for (;;) {
        out(ps1);

        int n = read_line();
        if (n < 0) { out("\n"); break; }      // end of input: leave
        if (n == 0) { continue; }

        if (run_line(&status)) { break; }
    }
    return status;
}
