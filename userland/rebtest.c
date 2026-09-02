#include <unistd.h>
#include <stdio.h>
#include <sys/reboot.h>

// Rebooting is god's alone (N3). It used to be PID 1's, which is NOT
// Linux's rule -- Linux gates reboot on privilege, not on being init --
// and the difference is why `make shell` had no way to power the machine
// off: nothing but init could ask.
//
// So this drops to an ordinary uid FIRST and then asserts every reboot
// command is refused. Dropping is essential to the test, not decoration:
// every process on NeoOS inherits god today, so a test that stayed god
// would POWER THE MACHINE OFF here. It did exactly that when the gate
// moved and this file still assumed the old rule -- the boot ended
// mid-suite with three tests reported and no clue why.
//
// If the gate ever fails open again, POWER_OFF halts the machine and the
// ALL PASSED marker never prints, so a missing marker is itself the
// failure signal.
#define UID_NOBODY 65534

int main(void) {
    if (getuid() != 0) {
        printf("[rebtest] FAILED: expected to start as god, got uid %d\n", getuid());
        return 1;
    }
    if (setgid(UID_NOBODY) != 0 || setuid(UID_NOBODY) != 0) {
        printf("[rebtest] FAILED: could not drop privilege\n");
        return 1;
    }

    if (reboot(LINUX_REBOOT_CMD_POWER_OFF) != -1) {
        printf("[rebtest] FAILED: POWER_OFF from an ordinary uid was not refused\n");
        return 1;
    }
    if (reboot(LINUX_REBOOT_CMD_RESTART) != -1) {
        printf("[rebtest] FAILED: RESTART from an ordinary uid was not refused\n");
        return 1;
    }
    if (reboot(0x1234) != -1) {
        printf("[rebtest] FAILED: bad command from an ordinary uid was not refused\n");
        return 1;
    }
    printf("[rebtest] reboot refused for uid %d; god keeps it\n", UID_NOBODY);
    printf("[rebtest] ALL PASSED\n");
    return 0;
}
