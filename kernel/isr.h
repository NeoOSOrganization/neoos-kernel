#ifndef NEOOS_ISR_H
#define NEOOS_ISR_H

#include <stdint.h>

// Layout mirrors isr.asm's push order exactly (low to high address).
// No rsp/ss fields: every interrupt is taken at CPL0, so the CPU
// never pushes them (see Global Constraints in the plan).
struct registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector_number;
    uint64_t error_code;
    uint64_t rip, cs, rflags;
    // In long mode the CPU pushes SS:RSP at EVERY privilege level, so
    // these have always been on the stack -- the struct simply stopped
    // at rflags. Signal delivery from a fault needs the user RSP to
    // build a frame on. isr.asm needs no change: it never touched them,
    // and its `add rsp, 16` + iretq already skip the CPU-pushed frame.
    uint64_t rsp, ss;
} __attribute__((packed));

void isr_handler(struct registers *regs);

#endif
