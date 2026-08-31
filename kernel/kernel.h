#ifndef NEOOS_KERNEL_H
#define NEOOS_KERNEL_H

void kmain(void *multiboot_info);

// Powers the machine off via ACPI (QEMU exits). Called when the last
// user process exits -- see user_proc_started/user_proc_exited in
// kernel/sched/proc.c. M2's reboot(2) + init will replace the internal
// trigger; the function itself stays.
void kernel_shutdown(void);

// Bump on every successful spawn()/fork(); drop in process_exit(). The
// drop that reaches zero powers the machine off. kmain() holds one
// reference across its own spawn sequence so an early exit on another
// CPU cannot trip the shutdown before the boot is done launching.
void user_proc_started(void);
void user_proc_exited(void);

#endif
