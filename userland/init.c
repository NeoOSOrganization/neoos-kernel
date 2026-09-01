// /SBIN/INIT -- NeoOS PID 1.
//
// Parses /ETC/INITTAB, launches every entry, then reaps children
// forever. When wait4() reports -ECHILD -- every launched process and
// every orphan reparented to us has exited -- it powers the machine
// off with reboot(2).
//
// INITTAB grammar: one entry per line, "<mode> <path>". Blank lines and
// lines beginning with '#' are ignored.
//   spawn    launch and do not wait
//   wait     launch and block until it exits before the next entry
//   respawn  launch, and relaunch whenever it exits

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

static struct { char path[64]; int mode; } ents[MAX_ENTRIES];
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

static void parse_inittab(void) {
    int fd = open("/ETC/INITTAB", O_RDONLY);
    if (fd < 0) { printf("[init] no /ETC/INITTAB (%d)\n", fd); return; }

    static char buf[8192];
    int n = 0, r;
    while (n < (int)sizeof buf && (r = read(fd, buf + n, sizeof buf - n)) > 0) {
        n += r;
    }
    close(fd);

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

        if (m < 0 || le <= ls || (le - ls) >= (int)sizeof ents[0].path) {
            printf("[init] bad inittab line skipped\n");
            continue;
        }
        memcpy(ents[nents].path, buf + ls, le - ls);
        ents[nents].path[le - ls] = 0;
        ents[nents].mode = m;
        nents++;
    }
}

static int launch(int e) {
    char *argv[2] = { ents[e].path, 0 };
    int pid = spawnv(ents[e].path, argv);
    if (pid < 0) {
        printf("[init] spawn %s failed (%d)\n", ents[e].path, pid);
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
    printf("[init] %d entries launched, reaping\n", launched);

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
