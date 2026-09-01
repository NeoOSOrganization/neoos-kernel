#ifndef NEOOS_SYS_REBOOT_H
#define NEOOS_SYS_REBOOT_H

// reboot(2). Command words match Linux's magic-2 values so a ported
// program's constants keep working. DIVERGENCE from Linux: NeoOS gates
// on "caller is PID 1" rather than CAP_SYS_BOOT (there are no uids or
// capabilities). A non-init caller gets -1. See docs/stdlib.md.

#define LINUX_REBOOT_CMD_RESTART   0x01234567
#define LINUX_REBOOT_CMD_HALT      0xcdef0123
#define LINUX_REBOOT_CMD_POWER_OFF 0x4321fedc

// Returns 0 on success for a command that (somehow) returned; -1 if the
// kernel refused it. The three real commands do not return at all.
int reboot(int cmd);

#endif
