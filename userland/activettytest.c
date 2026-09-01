// NEOOS_TIOCSACTIVE routing: a pty master that claims the active tty
// receives cooked keyboard input (echoed to the master); before the
// claim and after the release it does not.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <neoos_test.h>

#define NEOOS_TIOCSACTIVE 0x4E454F01
#define TIOCGPTN          0x80045430
#define KEY_A             30            // Linux KEY_A -> 'a'

static int m;

static void inject_a(void) {
    neoos_test_inject_key(KEY_A, 1);
    neoos_test_inject_key(KEY_A, 0);
}

static int drained(void) {              // returns 1 if the master has no data
    fcntl(m, F_SETFL, O_NONBLOCK);
    char b[8];
    int r = read(m, b, sizeof b);
    fcntl(m, F_SETFL, 0);
    return r <= 0;
}

int main(void) {
    m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { printf("[activetty] FAILED: ptmx %d\n", m); return 1; }

    int n = -1;
    ioctl(m, TIOCGPTN, &n);

    char path[24];
    const char *pre = "/dev/pts/";
    int p = 0;
    while (pre[p]) { path[p] = pre[p]; p++; }
    if (n >= 10) { path[p++] = '0' + n / 10; }
    path[p++] = '0' + n % 10;
    path[p] = 0;

    int s = open(path, O_RDWR);
    if (s < 0) { printf("[activetty] FAILED: pts open %d\n", s); return 1; }

    // Key injection needs the test-hook syscall; a production kernel
    // returns -ENOSYS. Fall back to checking the ioctls alone.
    int have_hook = (neoos_test_inject_key(KEY_A, 0) == 0);

    if (have_hook) {
        // Not claimed: injected keys go to the console, not this master.
        inject_a();
        if (!drained()) { printf("[activetty] FAILED: key leaked before claim\n"); return 1; }
    }

    // Claim.
    if (ioctl(m, NEOOS_TIOCSACTIVE, (void *)1) != 0) {
        printf("[activetty] FAILED: claim\n");
        return 1;
    }

    if (have_hook) {
        inject_a();
        char b[8];
        int r = read(m, b, sizeof b);        // blocking: the echo will arrive
        if (r < 1 || b[0] != 'a') {
            printf("[activetty] FAILED: no echo after claim (r=%d)\n", r);
            return 1;
        }
    }

    // Release.
    if (ioctl(m, NEOOS_TIOCSACTIVE, (void *)0) != 0) {
        printf("[activetty] FAILED: release\n");
        return 1;
    }

    if (have_hook) {
        inject_a();
        if (!drained()) { printf("[activetty] FAILED: key leaked after release\n"); return 1; }
    } else {
        printf("[activetty] routing check skipped (production kernel)\n");
    }

    printf("[activetty] ALL PASSED\n");
    return 0;
}
