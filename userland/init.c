// /sbin/init.nex -- NeoOS PID 1.
//
// Parses /etc/inittab, launches every entry, then reaps children
// forever. When wait4() reports -ECHILD -- every launched process and
// every orphan reparented to us has exited -- it powers the machine
// off with reboot(2).
//
// INITTAB grammar: one entry per line, "<mode> <path> [args...]".
// Blank lines and lines beginning with '#' are ignored.
//   spawn    launch and do not wait
//   wait     launch and block until it exits before the next entry
//   respawn  launch, and relaunch whenever it exits
//
// Arguments after the path are passed to the program as argv[1..].
// Without them an entry can only ever run a program that needs no
// configuration, which is why `make shell` needs them: it launches the
// framebuffer terminal with the shell to run inside it.
//
// Splitting is on whitespace ONLY -- there is no quoting. An argument
// containing a space cannot be written here, and a line using quotes
// gets them as literal characters (a `sh -c "a; b"` entry reaches the
// shell as three broken arguments and it exits 2). A program needing
// that should be given a script to run instead.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/reboot.h>

#define MAX_ENTRIES 64
#define MODE_SPAWN   0
#define MODE_WAIT    1
#define MODE_RESPAWN 2

#define MAX_ARGS 8
#define LINE_MAX 192

static struct {
    // The whole "<path> [args...]" remainder, split IN PLACE: each
    // separator becomes a NUL and argv points into this buffer. One
    // allocation-free split per entry, and argv[0] is the path.
    char  line[LINE_MAX];
    char *argv[MAX_ARGS + 1];
    int   argc;
    int   mode;
} ents[MAX_ENTRIES];
static int nents;

// pid -> entry, so a respawn entry's exit can be matched and relaunched.
static int  rp_pid[MAX_ENTRIES];
static int  rp_ent[MAX_ENTRIES];
static int  nrp;

static int mode_of(const char *w, int len) {
    if (len == 5 && !memcmp(w, "spawn",   5)) { return MODE_SPAWN; }
    if (len == 4 && !memcmp(w, "wait",    4)) { return MODE_WAIT; }
    if (len == 7 && !memcmp(w, "respawn", 7)) { return MODE_RESPAWN; }
    return -1;
}

// Bytes actually read from /etc/inittab, reported alongside the entry
// count so a boot that parsed less than the whole file says so.
static int inittab_bytes;

static void parse_inittab(void) {
    int fd = open("/etc/inittab", O_RDONLY);
    if (fd < 0) { printf("[init] no /etc/inittab (%d)\n", fd); return; }

    // A SHORT read here used to be indistinguishable from end-of-file,
    // and init would go on to launch a prefix of the system in silence
    // -- the machine boots, most things work, and the only symptom is
    // that some services were never started. A read error is reported,
    // and so is a full buffer, because both mean the entry list below is
    // not the file on disk.
    static char buf[8192];
    int n = 0, r = 0;
    while (n < (int)sizeof buf && (r = read(fd, buf + n, sizeof buf - n)) > 0) {
        n += r;
    }
    if (r < 0) { printf("[init] FAILED: read error %d on /etc/inittab after %d bytes\n", r, n); }
    if (n == (int)sizeof buf) { printf("[init] FAILED: /etc/inittab is larger than %d bytes\n", n); }
    close(fd);
    inittab_bytes = n;

    int i = 0;
    while (i < n && nents < MAX_ENTRIES) {
        int ls = i;
        while (i < n && buf[i] != '\n') { i++; }
        int le = i;
        if (i < n) { i++; }                 // step past '\n'

        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t')) { ls++; }
        while (le > ls && (buf[le-1] == ' ' || buf[le-1] == '\t' || buf[le-1] == '\r')) { le--; }
        if (ls >= le || buf[ls] == '#') { continue; }

        int ws = ls;
        while (ls < le && buf[ls] != ' ' && buf[ls] != '\t') { ls++; }
        int m = mode_of(buf + ws, ls - ws);
        while (ls < le && (buf[ls] == ' ' || buf[ls] == '\t')) { ls++; }

        if (m < 0 || le <= ls || (le - ls) >= LINE_MAX) {
            printf("[init] bad inittab line skipped\n");
            continue;
        }
        memcpy(ents[nents].line, buf + ls, le - ls);
        ents[nents].line[le - ls] = 0;

        // Split on runs of spaces and tabs.
        int argc = 0;
        char *p = ents[nents].line;
        while (*p && argc < MAX_ARGS) {
            while (*p == ' ' || *p == '\t') { *p++ = 0; }
            if (!*p) { break; }
            ents[nents].argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') { p++; }
        }
        ents[nents].argv[argc] = 0;
        if (argc == 0) { printf("[init] bad inittab line skipped\n"); continue; }
        ents[nents].argc = argc;
        ents[nents].mode = m;
        nents++;
    }
}

// The environment every process on the machine inherits (BB3). init is
// the only place it can come from: there is nothing above PID 1 to
// inherit one from, so if init does not supply it, nothing has one.
//
// PATH names /bin, where programs live, and /usr/tests so the suite's
// binaries can be run by name from a shell. HOME is / until N5 gives
// users real home directories.
static char *const base_env[] = {
    (char *)"PATH=/bin:/usr/tests",
    (char *)"HOME=/",
    (char *)"TERM=linux",
    (char *)"PS1=neoos$ ",
    0
};

static int launch(int e) {
    int pid = spawnve(ents[e].argv[0], ents[e].argv, base_env);
    if (pid < 0) {
        printf("[init] spawn %s failed (%d)\n", ents[e].argv[0], pid);
        return -1;
    }
    if (ents[e].mode == MODE_RESPAWN && nrp < MAX_ENTRIES) {
        rp_pid[nrp] = pid;
        rp_ent[nrp] = e;
        nrp++;
    }
    return pid;
}

int main(void) {
    parse_inittab();
    if (nents == 0) {
        printf("[init] empty inittab -- powering off\n");
        reboot(LINUX_REBOOT_CMD_POWER_OFF);
        for (;;) { }
    }

    int launched = 0;
    for (int e = 0; e < nents; e++) {
        if (ents[e].mode == MODE_WAIT) {
            int pid = launch(e);
            if (pid > 0) { int st; wait4(pid, &st, 0, 0); }
        } else if (launch(e) > 0) {
            launched++;
        }
    }
    printf("[init] %d entries launched, %d parsed from %d bytes of INITTAB, reaping\n",
           launched, nents, inittab_bytes);

    for (;;) {
        int st;
        int pid = wait4(-1, &st, 0, 0);
        if (pid < 0) { break; }             // -ECHILD: nothing left
        for (int k = 0; k < nrp; k++) {
            if (rp_pid[k] == pid) { rp_pid[k] = launch(rp_ent[k]); break; }
        }
    }

    printf("[init] all entries exited -- powering off\n");
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    for (;;) { }
}
