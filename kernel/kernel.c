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
#include "ata.h"
#include "fs/fatfs.h"
#include "fs/vfs.h"
#include "sched/proc.h"
#include "syscall.h"
#include "cpu.h"
#include "lock.h"
#include "waitq.h"
#include "signal.h"
#include "cpu_local.h"

void kmain(void *multiboot_info) {
    serial_init();
    serial_write_string("NeoOS booting (milestone 4: storage)\n");
    serial_write_string("[boot] kmain address=");
    serial_write_hex64((uint64_t)(uintptr_t)kmain);
    serial_write_string("\n");

    pmm_init(multiboot_info);
    pmm_selftest();

    paging_init();
    paging_selftest();

    vga_clear();
    vga_print_string("NeoOS booted");

    tss_init();
    gdt_init();
    // AFTER gdt_init: gdt_flush reloads the segment registers, and
    // `mov gs, ax` ZEROES IA32_GS_BASE as a side effect. Installing the
    // per-CPU pointer any earlier would have it wiped a few
    // instructions later, and every this_cpu() would then dereference
    // physical address 0.
    cpu_local_init();
    lock_selftest();
    serial_write_string("[gdt] loaded, tss_selector=0x18\n");

    idt_init();
    serial_write_string("[idt] loaded\n");

    struct acpi_info acpi;
    acpi_find_madt(&acpi);

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

    struct ata_identify_info ata_info;
    ata_identify(0, &ata_info);

    fat16_mount();
    fat16_selftest();
    fat16_write_selftest();

    vfs_init();
    vfs_mount_fs("hd0", "/",    "fat");
    vfs_mount_fs(0,     "/dev", "devfs");
    vfs_mount_fs(0,     "/tmp", "ramfs");
    vfs_mount_fs("hd1", "/mnt", "fat");
    vfs_selftest();

    cpu_init();

    process_init();
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

    // After the spawns so the selftest's own kernel threads draw ids
    // above the real processes', keeping pids stable across boots.
    waitq_selftest_start();
    signal_selftest_start();

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
