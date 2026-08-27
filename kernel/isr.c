#include "isr.h"
#include "serial.h"
#include "vga.h"
#include "timer.h"
#include "lapic.h"
#include "keyboard.h"
#include "mm/paging.h"
#include "sched/proc.h"

static const char *exception_names[32] = {
    "Divide Error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
    "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS",
    "Segment Not Present", "Stack-Segment Fault", "General Protection Fault",
    "Page Fault", "Reserved", "x87 Floating-Point Exception", "Alignment Check",
    "Machine Check", "SIMD Floating-Point Exception", "Virtualization Exception",
    "Control Protection Exception", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Injection Exception",
    "VMM Communication Exception", "Security Exception", "Reserved",
};

static void exception_dump_and_halt(struct registers *regs) {
    serial_write_string("\n[exception] ");
    serial_write_string(exception_names[regs->vector_number]);
    serial_write_string(" (vector=");
    serial_write_hex64(regs->vector_number);
    serial_write_string(", error_code=");
    serial_write_hex64(regs->error_code);
    serial_write_string(")\n  rip="); serial_write_hex64(regs->rip);
    serial_write_string(" cs=");      serial_write_hex64(regs->cs);
    serial_write_string(" rflags=");  serial_write_hex64(regs->rflags);
    serial_write_string("\n  rax="); serial_write_hex64(regs->rax);
    serial_write_string(" rbx=");    serial_write_hex64(regs->rbx);
    serial_write_string(" rcx=");    serial_write_hex64(regs->rcx);
    serial_write_string(" rdx=");    serial_write_hex64(regs->rdx);
    serial_write_string("\n  rsi="); serial_write_hex64(regs->rsi);
    serial_write_string(" rdi=");    serial_write_hex64(regs->rdi);
    serial_write_string(" rbp=");    serial_write_hex64(regs->rbp);

    if (regs->vector_number == 14) {
        uint64_t cr2;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
        serial_write_string("\n  cr2="); serial_write_hex64(cr2);
    }
    serial_write_string("\n");

    vga_print_string("EXCEPTION - HALTED");

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void unhandled_interrupt(uint64_t vector) {
    serial_write_string("[isr] unhandled interrupt vector=");
    serial_write_hex64(vector);
    serial_write_string("\n");

    vga_print_string("UNHANDLED IRQ - HALTED");

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

void isr_handler(struct registers *regs) {
    if (regs->vector_number == 14) {
        // A #PF that was present + write + user can only be a write to a
        // read-only user page, and fork() is the only thing that ever
        // creates one. The current_proc()/pml4_phys guard is belt-and-
        // braces: user=1 means the fault came from user mode, which
        // implies a running thread with a process address space -- but
        // a null deref here would surface as an unrelated double fault
        // and bury whatever the real bug was.
        uint64_t present_write_user = 0x7; // P=1, W=1, U=1
        struct process *p = current_proc();
        if ((regs->error_code & present_write_user) == present_write_user && p && p->pml4_phys) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            if (paging_handle_cow_fault(p->pml4_phys, cr2)) {
                return;
            }
        }
    }

    if (regs->vector_number < 32) {
        exception_dump_and_halt(regs);
        return;
    }

    if (regs->vector_number == VECTOR_TIMER) {
        // EOI must go out BEFORE timer_handler(), not after: timer_handler
        // may call schedule(), which can switch to a different task via a
        // bare `ret` that never "returns" here in the traditional sense
        // until the task we just preempted is itself resumed later. EOI'ing
        // after the call would defer it indefinitely, and the LAPIC
        // withholds all further timer interrupts until it arrives --
        // deadlocking preemption entirely (confirmed: without this, tick
        // logging stops completely the moment a switch happens mid-handler).
        lapic_send_eoi();
        timer_handler();
        return;
    }

    if (regs->vector_number == VECTOR_KEYBOARD) {
        keyboard_handler();
        lapic_send_eoi();
        return;
    }

    unhandled_interrupt(regs->vector_number);
}
