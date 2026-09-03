#include "kernel.h"
#include "drivers/video/fb.h"
#include "drivers/video/fb_device.h"
#include "drivers/video/fbcon.h"
#include "tty/pty.h"
#include "tty/console.h"
#include "tty/con_driver.h"
#include "tty/banner.h"
#include "tty/kvt.h"
#include "tty/vt.h"
#include "drivers/char/serial.h"
#include "arch/tss.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "drivers/acpi/acpi.h"
#include "drivers/irq/pic.h"
#include "drivers/irq/lapic.h"
#include "drivers/irq/ioapic.h"
#include "drivers/pci/pci.h"
#include "drivers/char/timer.h"
#include "drivers/char/rtc.h"
#include "tty/tty.h"
#include "drivers/input/keyboard.h"
#include "drivers/input/input.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/vma.h"
#include "drivers/block/ata.h"
#include "fs/blkcache.h"
#include "fs/fatfs.h"
#include "fs/vfs.h"
#include "sched/proc.h"
#include "sched/pid_alloc.h"
#include "syscall/syscall.h"
#include "arch/cpu.h"
#include "sync/lock.h"
#include "sync/waitq.h"
#include "ipc/signal.h"
#include "arch/cpu_local.h"
#include "smp/tlb.h"
#include "smp/smp.h"
#include "ipc/futex.h"
#include "fs/file.h"
#include "fs/devfs.h"
#include "ipc/pipe.h"
#include "net/net.h"
#include "net/socket.h"
#include "net/netrx.h"
#include "drivers/net/virtio_net.h"
#include "lib/rand.h"

void kernel_shutdown(void) {
    {   // CS3: the poll-broadcast baseline for CS5.2. Whole-boot totals
        // over the same suite are directly comparable before and after a
        // per-object poll redesign -- unlike any windowed ratio, which
        // ambient traffic here makes meaningless (see pollstorm.c).
        uint64_t pev = 0, pwk = 0;
        waitq_poll_stats(&pev, &pwk);
        serial_write_string("[pmm] free frames at shutdown=");
        serial_write_hex64(pmm_free_frame_count());
        serial_write_string("\n");
        serial_write_string("[poll] broadcasts=");
        serial_write_hex64(pev);
        serial_write_string(" wakeups=");
        serial_write_hex64(pwk);
        serial_write_string(" wasted=");
        serial_write_hex64(waitq_poll_wasted());
        serial_write_string("\n");
    }
    lock_stats_dump();   // no-op unless DEBUG_LOCKSTAT
    serial_write_string("\n[kernel] shutdown requested, powering off\n");
    cli();

    // ACPI S5 (soft-off): SLP_EN | SLP_TYP=0 written as a WORD to
    // PM1a_CNT. QEMU's default PIIX4/ICH9 chipset puts PM1a_CNT at
    // 0x604 and treats this exact write as poweroff; 0xB004 is the
    // older Bochs/QEMU port kept as a fallback. The previous byte
    // write of 0x06 to 0x604 did nothing, which is why every headless
    // run used to sit until the timeout killed it.
    __asm__ volatile ("outw %0, %1" : : "a"((unsigned short)0x2000), "d"((unsigned short)0x604));
    __asm__ volatile ("outw %0, %1" : : "a"((unsigned short)0x2000), "d"((unsigned short)0xB004));

    // Fallback if neither port powered the machine off.
    for (;;) { __asm__ volatile ("hlt"); }
}

void kmain(void *multiboot_info) {
    serial_init();
    serial_write_string("NeoOS booting (milestone 4: storage)\n");
    serial_write_string("[boot] kmain address=");
    serial_write_hex64((uint64_t)(uintptr_t)kmain);
    serial_write_string("\n");

    // Probe for a framebuffer device now (identity-mapped MBI, no
    // allocation); the mapping waits for pmm + the physmap below.
    fb_device_register_builtin();
    fb_device_probe_all(multiboot_info);

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
    waitq_global_init();

    pmm_init(multiboot_info);
    pmm_selftest();

    paging_init();
    paging_selftest();

    // Framebuffer console up as early as possible: from here on a panic
    // renders through fbcon (fb.virt lives in the physmap, so it works
    // from any address space).
    fb_map();
    fb_device_selftest();
    con_driver_register_builtin();
    con_driver_select();
    fbcon_selftest();
    con_driver_selftest();
    kvt_selftest();

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
    // AFTER timer_init: the RTC anchor is stored as "epoch at tick
    // zero", so it needs a tick counter that already means something.
    tty_init();
    tty_selftest();
    vt_selftest();
    pty_init();
    keyboard_decode_selftest();
    rtc_init();
    rtc_selftest();

    // AFTER rtc_init: rand_init uses rtc_boot_epoch() as part of the seed.
    rand_init();

    // Initialize the input subsystem BEFORE the keyboard IRQ is unmasked
    input_init();
    input_selftest();

    uint8_t keyboard_pin = acpi.irq1_gsi - acpi.ioapic_gsi_base;
    ioapic_set_redirection(keyboard_pin, VECTOR_KEYBOARD, acpi.irq1_polarity,
                            acpi.irq1_trigger, (uint8_t)lapic_get_id());
    serial_write_string("[ioapic] keyboard routed: gsi=");
    serial_write_hex64(acpi.irq1_gsi);
    serial_write_string(" vector=0x21\n");

    heap_init();
    heap_selftest();
    pid_alloc_selftest();   // AFTER heap_init: the allocator kmallocs free-list entries
    tlb_init();

    // W^X the kernel address space: .text read-only, everything else
    // NX. AFTER heap_init (the huge-page split allocates PT frames) and
    // on the BSP only -- no AP has a TLB yet, so a CR3 reload suffices.
    paging_protect_kernel();
    wxorx_selftest();
    // AFTER heap_init: vma_insert allocates, so this cannot run against
    // an uninitialised heap. It used to sit before heap_init and worked
    // by accident -- class_pages is BSS-zero either way -- until
    // heap_lock made "uninitialised" mean rank 0 with no name, an
    // instant inversion under the vma lock.
    vma_selftest();

    // D0. Before any driver that needs to find its device, and after
    // heap_init only because the log goes out over an initialised
    // serial port -- enumeration itself allocates nothing.
    pci_init();
    pci_selftest();

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
    // BB5: synthetic, read-only, and mounted unconditionally -- `ps`
    // looks for /proc by name and says so when it is missing.
    vfs_mount_fs(0,     "/proc", "procfs");
    vfs_mount_fs("hd1", "/mnt", "fat");
    vfs_selftest();
    devfs_selftest();

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
    syscall_table_selftest();
    rand_selftest();
    futex_init();
    file_selftest();
    pipe_selftest();
    net_init();
    net_selftest();
    socket_init();
    socket_selftest();

    // D1. netrx BEFORE the driver: the device may raise its first
    // interrupt the instant DRIVER_OK is set, and that interrupt posts
    // into this queue.
    netrx_init();
    if (virtio_net_init() == 0) {
        // PCI interrupts are level-triggered and active-low, always --
        // unlike the ISA lines above, which need the MADT's overrides to
        // say so. The line the firmware programmed into the device's
        // config space IS the GSI; the IOAPIC pin is that minus the
        // controller's base.
        uint8_t nic_pin = (uint8_t)(virtio_net_irq_line() - acpi.ioapic_gsi_base);
        ioapic_set_redirection(nic_pin, VECTOR_VIRTIO_NET,
                               1 /* active-low */, 1 /* level */,
                               (uint8_t)lapic_get_id());
        serial_write_string("[ioapic] virtio-net routed: gsi=");
        serial_write_hex64(virtio_net_irq_line());
        serial_write_string(" vector=0x22\n");
    }
    // AFTER process_init: it starts a kernel thread. The queue would
    // simply fill and drop without one, which is why the driver may be
    // brought up first.
    netrx_start();

    // BEFORE the spawns, and before any kernel thread exists.
    //
    // Bringing the APs up afterwards meant each one came online into a
    // system that already had a run queue full of work, and -- now that
    // idle CPUs steal -- immediately started running user processes
    // while the BSP was still bringing up the NEXT AP and running the
    // selftests below. tlb_shootdown_selftest in particular asserts an
    // exact free-frame count across a shootdown, which is only a
    // meaningful statement while nothing else is allocating. Started
    // here, every AP parks in an idle loop with an empty queue and the
    // work arrives afterwards.
    smp_start_aps();
    smp_online_selftest();
    syscall_msr_selftest();   // asserts every AP programmed its own MSRs
    smp_reschedule_ipi_selftest();
    tlb_shootdown_selftest();
    panic_stop_selftest();

    // Concurrency stress that needs a second core, run HERE: after the
    // APs are up, but before init spawns the userland workload. Both
    // perturb state the workload observes -- vt_stress_selftest clears
    // the screen and flips VTs, which raced TERM's render check when it
    // ran after the spawn -- and the banner repaints afterwards anyway.
#ifndef NEOOS_QUIET_BOOT
    vt_stress_selftest();     // CS0: VT switch vs. console write
    waitq_churn_selftest();   // CS2: thread exit vs. the kzombies drain
#else
    // Quiet boot skips these two specifically: vt_stress_selftest
    // CLEARS THE SCREEN and flips VTs, which is invisible in a test run
    // and unacceptable underneath an interactive shell. The rest of the
    // selftests only write to the serial log, which is a file here, so
    // they are left alone -- a boot that skipped every check would be a
    // different kernel, not a quieter one.
#endif

    // LAST of the selftests, and the only one that needs interrupts: the
    // ARP reply it waits for arrives in the NIC's interrupt and is
    // delivered by the netrx thread, so it opens an interrupt window of
    // its own and closes it again. Everything above it -- the APs, the
    // scheduler, the netrx thread -- has to exist first.
    virtio_net_selftest();

    // Everything the banner reports is now known: framebuffer/console up,
    // pmm seeded, CPU probed, every AP online.
    banner_show();

    // PID 1. /sbin/init.nex reads /etc/inittab, launches the workload, reaps
    // every child and orphan, and powers the machine off via reboot(2)
    // when they have all exited. The kernel no longer knows the test
    // list -- it lives in the disk image's /etc/inittab (see Makefile).
    struct process *init_task = spawn("/sbin/init.nex");
    if (!init_task || init_task->pid != 1) {
        serial_write_string("[init] PANIC: /sbin/init.nex did not start as PID 1 (pid=");
        serial_write_hex64(init_task ? (uint64_t)init_task->pid : 0);
        serial_write_string(")\n");
        for (;;) { __asm__ volatile ("hlt"); }
    }

    // After the spawns so the selftest's own kernel threads draw ids
    // above the real processes', keeping pids stable across boots.
    waitq_selftest_start();
    signal_selftest_start();
    futex_selftest();
    smp_parallel_selftest_start();
    smp_steal_selftest_start();

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
