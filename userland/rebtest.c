#include <unistd.h>
#include <stdio.h>
#include <sys/reboot.h>

// rebtest is not PID 1, so every reboot command must be refused with no
// effect. If the kernel failed to gate the call, POWER_OFF would halt
// the machine here and the ALL PASSED marker would never print -- so a
// missing marker is itself the failure signal.
int main(void) {
    if (reboot(LINUX_REBOOT_CMD_POWER_OFF) != -1) {
        printf("[rebtest] FAILED: POWER_OFF from a non-init process was not refused\n");
        return 1;
    }
    if (reboot(LINUX_REBOOT_CMD_RESTART) != -1) {
        printf("[rebtest] FAILED: RESTART from a non-init process was not refused\n");
        return 1;
    }
    if (reboot(0x1234) != -1) {
        printf("[rebtest] FAILED: bad command from a non-init process was not refused\n");
        return 1;
    }
    printf("[rebtest] ALL PASSED\n");
    return 0;
}
