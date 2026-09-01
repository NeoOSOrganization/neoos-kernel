#include "arch/cpu_local.h"
#include "arch/msr.h"
#include "drivers/char/serial.h"
#include "drivers/irq/lapic.h"


struct cpu cpus[MAX_CPUS];


static void cpu_local_install(int index) {
    struct cpu *c = &cpus[index];
    c->self             = c;
    c->current          = 0;
    c->idle             = 0;
    c->tss              = &tss[index];
    c->user_rsp_scratch = 0;
    c->kernel_stack     = 0;
    c->lapic_id         = 0;
    c->held_depth       = 0;
    c->ready_head       = 0;
    c->ready_tail       = 0;
    c->ready_count      = 0;
    spin_init(&c->ready_lock, LOCK_RANK_RUNQUEUE, "runqueue");

    // Must run AFTER gdt_flush: loading a GS SELECTOR (`mov gs, ax`)
    // zeroes IA32_GS_BASE as a side effect, so any base installed
    // before the GDT reload is silently destroyed. The same hazard is
    // why the ring-3 trampolines swapgs only after their segment
    // loads.
    //
    // While executing in the kernel, GS_BASE names this block and
    // KERNEL_GS_BASE holds userland's GS value (0 -- NeoOS gives
    // userland no GS base). Every kernel entry swapgs's them into that
    // arrangement and every exit swaps them back.
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

void cpu_local_init_bsp(void) {
    cpu_local_install(0);
    serial_write_string("[cpu] per-CPU block installed (bsp)\n");
}

void cpu_local_init_ap(int index) {
    cpu_local_install(index);
    cpus[index].lapic_id = lapic_get_id();
}
