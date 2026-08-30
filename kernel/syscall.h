#ifndef NEOOS_SYSCALL_H
#define NEOOS_SYSCALL_H

void syscall_init(void);
void syscall_init_this_cpu(void);
void syscall_msr_selftest(void);

#endif
