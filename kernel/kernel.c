#include "kernel.h"
#include "vga.h"
#include "serial.h"
#include "tss.h"
#include "gdt.h"
#include "idt.h"
#include "acpi.h"
#include "pic.h"
#include "lapic.h"
#include "ioapic.h"
#include "timer.h"
#include "keyboard.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/vma.h"
#include "ata.h"
#include "fs/blkcache.h"
#include "fs/fatfs.h"
#include "fs/vfs.h"
#include "sched/proc.h"
#include "syscall.h"
#include "cpu.h"
#include "lock.h"
#include "waitq.h"
#include "signal.h"
#include "cpu_local.h"
#include "tlb.h"
#include "smp.h"

void kmain(void *multiboot_info) {
    serial_init();
    serial_write_string("NeoOS booting (milestone 4: storage)\n");
    serial_write_string("[boot] kmain address=");
    serial_write_hex64((uint64_t)(uintptr_t)kmain);
    serial_write_string("\n");

    // BEFORE pmm_init: every rank-checked spinlock calls this_cpu(),
    // which reads gs:0. Until cpu_local_init() installs a GS base that
    // read returns 0 and the lock dereferences physical address 0. pmm
    // and the heap are locked subsystems now, so their init must not be
    // the first thing to run.
    //
    // tss_init, gdt_init and cpu_local_init touch only static memory --
    // no allocator, no paging -- so nothing is lost by hoisting them.
    tss_init();
    gdt_init();
    // AFTER gdt_init: gdt_flush reloads the segment registers, and
    // `mov gs, ax` ZEROES IA32_GS_BASE as a side effect. Installing the
    // per-CPU pointer any earlier would have it wiped a few
    // instructions later, and every this_cpu() would then dereference
    // physical address 0.
    cpu_local_init_bsp();
    cpu_local_selftest();
    lock_selftest();
    waitq_lock_selftest();

    pmm_init(multiboot_info);
    pmm_selftest();

    paging_init();
    paging_selftest();

    vga_clear();
    vga_print_string("NeoOS booted");

    idt_init();
    serial_write_string("[idt] loaded\n");

    struct acpi_info acpi;
    acpi_find_madt(&acpi);

    smp_topology_init(&acpi);
    smp_topology_selftest();

    pic_disable();
    serial_write_string("[pic] disabled\n");

    lapic_init(acpi.lapic_address);
    serial_write_string("[lapic] enabled, id="); serial_write_hex64(lapic_get_id());
    serial_write_string("\n");

    ioapic_init(acpi.ioapic_address);
    serial_write_string("[ioapic] initialized\n");

    timer_init();

    uint8_t keyboard_pin = acpi.irq1_gsi - acpi.ioapic_gsi_base;
    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");

    heap_init();
    heap_selftest();
    tlb_init();
    // AFTER heap_init: vma_insert allocates, so this cannot run against
    // an uninitialised heap. It used to sit before heap_init and worked
    // by accident -- class_pages is BSS-zero either way -- until
    // heap_lock made "uninitialised" mean rank 0 with no name, an
    // instant inversion under the vma lock.
    vma_selftest();

    ata_init();   // before the first ata_* call
    struct ata_identify_info ata_info;
    ata_identify(0, &ata_info);

    // Before the first sector read of the boot: every filesystem read
    // below goes through it.
    blkcache_init();
    blkcache_selftest();

    fat16_mount();
    fat16_selftest();
    fat16_write_selftest();

    vfs_init();
    vfs_mount_fs("hd0", "/",    "fat");
    vfs_mount_fs(0,     "/dev", "devfs");
    vfs_mount_fs(0,     "/tmp", "ramfs");
    vfs_mount_fs("hd1", "/mnt", "fat");
    vfs_selftest();

    // Hits are sector reads the drive never saw. The ratio is the whole
    // point of the cache, so it goes in the boot log where a regression
    // in it is visible.
    uint64_t bc_hits, bc_misses;
    blkcache_stats(&bc_hits, &bc_misses);
    serial_write_string("[blkcache] hits=");
    serial_write_hex64(bc_hits);
    serial_write_string(" misses=");
    serial_write_hex64(bc_misses);
    serial_write_string("\n");

    cpu_init();
    cpu_state_selftest();

    process_init();
    runqueue_lock_selftest();
    syscall_init();

    struct process *parent_task = spawn("/BIN/PARENT.ELF");
    if (!parent_task) {
        serial_write_string("[process] spawn FAILED for /BIN/PARENT.ELF\n");
    }
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/YIELDER.ELF");
    spawn("/BIN/VFSTEST.ELF");
    spawn("/BIN/THRDTEST.ELF");
    spawn("/BIN/SIGTEST.ELF");
    spawn("/BIN/FAULTER.ELF");
    spawn("/BIN/AVXTEST.ELF");
    spawn("/BIN/MMAPTEST.ELF");
    spawn("/BIN/SMPTEST.ELF");

    // After the spawns so the selftest's own kernel threads draw ids
    // above the real processes', keeping pids stable across boots.
    waitq_selftest_start();
    signal_selftest_start();

    smp_start_aps();
    smp_online_selftest();
    smp_reschedule_ipi_selftest();
    tlb_shootdown_selftest();
    panic_stop_selftest();
    smp_parallel_selftest_start();

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
