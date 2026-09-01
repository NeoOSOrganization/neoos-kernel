#ifndef NEOOS_KERNEL_H
#define NEOOS_KERNEL_H

void kmain(void *multiboot_info);

// Powers the machine off via ACPI (QEMU exits). Reached from
// reboot(LINUX_REBOOT_CMD_POWER_OFF), which /SBIN/INIT calls once every
// process it launched (and every orphan reparented to it) has exited.
void kernel_shutdown(void);

#endif
