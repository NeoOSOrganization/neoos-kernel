#include "arch/isr.h"
#include "drivers/char/serial.h"
#include "drivers/video/fbcon.h"
#include "tty/console.h"
#include "tty/tty.h"
#include "tty/vt.h"
#include "drivers/char/timer.h"
#include "smp/smp.h"
#include "smp/tlb.h"
#include "drivers/irq/lapic.h"
#include "drivers/input/keyboard.h"
#include "mm/paging.h"
#include "mm/vma.h"
#include "sched/proc.h"
#include "ipc/signal.h"

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
    // Reclaim the screen from any userland terminal so this dump paints.
    fbcon_enter_panic();          // drop the rank check: we may hold anything
    vt_enter_panic();             // ditto for the VT layer
    console_set_fb_owned(0);
    tty_set_active(0);
    vt_panic_reset();

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

    console_write("EXCEPTION - HALTED\n", 18);

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void unhandled_interrupt(uint64_t vector) {
    serial_write_string("[isr] unhandled interrupt vector=");
    serial_write_hex64(vector);
    serial_write_string("\n");

    console_write("UNHANDLED IRQ - HALTED\n", 22);

    __asm__ volatile ("cli");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}

static void isr_handler_inner(struct registers *regs) {
    // FIRST, and above all the exception handling below, because the
    // NMI's vector is 2 -- inside the `vector_number < 32` range that
    // block claims. It used to sit AFTER that block and was therefore
    // dead code: every NMI was handled as a CPU exception instead.
    //
    // That was not merely cosmetic. smp_panic_stop_others() stops the
    // other CPUs by NMI, so a CPU that took the stop while running RING
    // 3 fell into the exception path's user branch, which delivers
    // SIGSEGV to the current process and RETURNS. The panic-stop
    // therefore killed a random user process and let the machine carry
    // on running instead of freezing it -- so a panic on one CPU
    // surfaced as an unrelated test failure somewhere else (observed:
    // a [lock] PANIC on one CPU, and sigtest reporting its child dead
    // of signal 11 while the boot ran happily to completion).
    if (regs->vector_number == 2) {
        nmi_handler();   // never returns
        return;
    }

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

    if (regs->vector_number == 14) {
        // Order matters. COW (above) handles a WRITE to a page that is
        // present but read-only. This handles the FIRST TOUCH of a
        // mapping that has no frame yet -- mmap records the region and
        // maps nothing. A fault matching neither is a genuine SIGSEGV,
        // which the block below delivers.
        if (!(regs->error_code & 1) && (regs->cs & 3) == 3) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            struct process *p = current_proc();
            if (p && p->pml4_phys &&
                vma_fault(p, cr2, (regs->error_code & 2) != 0)) {
                return;
            }
        }
    }

    if (regs->vector_number < 32) {
        // A fault from ring 0 is a kernel bug and must still stop the
        // machine loudly. A fault from ring 3 kills only its process --
        // which is what finally lets faulter.c into the standard boot.
        if ((regs->cs & 3) != 3 || !current_proc()) {
            exception_dump_and_halt(regs);
            return;
        }

        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        int sig = SIGSEGV, code = SI_KERNEL;
        uint64_t addr = 0;
        switch (regs->vector_number) {
        case 0:  sig = SIGFPE;  code = FPE_INTDIV;  addr = regs->rip; break;
        case 6:  sig = SIGILL;  code = ILL_ILLOPC;  addr = regs->rip; break;
        case 13: sig = SIGSEGV; code = SI_KERNEL;                     break;
        case 14: sig = SIGSEGV;
                 code = (regs->error_code & 1) ? SEGV_ACCERR : SEGV_MAPERR;
                 addr = cr2;
                 break;
        case 16: case 19: sig = SIGFPE; code = FPE_FLTINV; addr = regs->rip; break;
        case 17: sig = SIGBUS;  code = BUS_ADRERR; addr = cr2; break;
        default: break;
        }
        signal_raise_fault(regs, sig, code, addr);
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

    if (regs->vector_number == VECTOR_IPI_TLB) {
        ipi_tlb_handler();
        return;
    }

    if (regs->vector_number == VECTOR_IPI_RESCHEDULE) {
        ipi_reschedule_handler();
        return;
    }

    if (regs->vector_number == VECTOR_KEYBOARD) {
        keyboard_handler();
        lapic_send_eoi();
        return;
    }

    unhandled_interrupt(regs->vector_number);
}

void isr_handler(struct registers *regs) {
    isr_handler_inner(regs);

    // Deliver pending signals on the way back to ring 3. Without this a
    // signal could never interrupt a compute loop -- only a thread that
    // happened to make a syscall would notice one.
    //
    // This runs AFTER the inner handler, never before: a fatal signal
    // terminates the process without returning, and doing that ahead of
    // lapic_send_eoi() would leave the EOI unsent. The LAPIC then
    // withholds every further timer interrupt and preemption deadlocks
    // system-wide -- the same trap documented at the EOI call itself.
    if ((regs->cs & 3) == 3) {
        signal_deliver_from_interrupt(regs);
    }
}
