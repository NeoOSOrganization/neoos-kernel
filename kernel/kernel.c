#include "kernel.h"
#include "dev/vga.h"
#include "dev/fb.h"
#include "dev/fbcon.h"
#include "dev/pty.h"
#include "dev/console.h"
#include "dev/serial.h"
#include "arch/tss.h"
#include "arch/gdt.h"
#include "arch/idt.h"
#include "dev/acpi.h"
#include "dev/pic.h"
#include "dev/lapic.h"
#include "dev/ioapic.h"
#include "dev/timer.h"
#include "dev/rtc.h"
#include "dev/tty.h"
#include "dev/keyboard.h"
#include "dev/input.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "mm/vma.h"
#include "dev/ata.h"
#include "fs/blkcache.h"
#include "fs/fatfs.h"
#include "fs/vfs.h"
#include "sched/proc.h"
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
#include "lib/rand.h"

void kernel_shutdown(void) {
    serial_write_string("\n[kernel] last user process exited, powering off\n");
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

    // Parse the framebuffer tag now (identity-mapped MBI, no allocation);
    // the mapping waits for pmm + the physmap in paging_init below.
    fb_init(multiboot_info);

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
    fbcon_init();
    fbcon_selftest();
    console_clear();
    console_write("NeoOS booted\n", 13);

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

    // Hold one reference across the whole spawn sequence: a test that
    // exits fast on another CPU must not drive the live-process count to
    // zero (which powers the machine off) before the boot has finished
    // launching everything. Released just before schedule().
    user_proc_started();

    struct process *parent_task = spawn("/BIN/PARENT.ELF");
    if (!parent_task) {
        serial_write_string("[process] spawn FAILED for /BIN/PARENT.ELF\n");
    }
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/LOOPER.ELF");
    spawn("/BIN/YIELDER.ELF");
    spawn("/BIN/VFSTEST.ELF");
    spawn("/BIN/CWDTEST.ELF");
    spawn("/BIN/STATTEST.ELF");
    spawn("/BIN/DIRTEST.ELF");
    spawn("/BIN/LFNTEST.ELF");
    spawn("/BIN/TIER0.ELF");
    spawn("/BIN/MUSLHELO.ELF");
    spawn("/BIN/TTYTEST.ELF");
    spawn("/BIN/THRDTEST.ELF");
    spawn("/BIN/SIGTEST.ELF");
    spawn("/BIN/FAULTER.ELF");
    spawn("/BIN/AVXTEST.ELF");
    spawn("/BIN/MMAPTEST.ELF");
    spawn("/BIN/REBTEST.ELF");
    spawn("/BIN/ORPHANTEST.ELF");
    spawn("/BIN/FBTEST.ELF");
    spawn("/BIN/POLLTEST.ELF");
    spawn("/BIN/PTYTEST.ELF");
    spawn("/BIN/SMPTEST.ELF");
    spawn("/BIN/EVTEST.ELF");
    spawn("/BIN/IPCTEST.ELF");
    spawn("/BIN/PIPETEST.ELF");
    spawn("/BIN/TLSTEST.ELF");
    spawn("/BIN/NETTEST.ELF");
    spawn("/BIN/MPITEST.ELF");

    // After the spawns so the selftest's own kernel threads draw ids
    // above the real processes', keeping pids stable across boots.
    waitq_selftest_start();
    signal_selftest_start();
    futex_selftest();
    smp_parallel_selftest_start();
    smp_steal_selftest_start();

    serial_write_string("NeoOS: interrupts enabled, starting scheduler\n");
    __asm__ volatile ("sti");

    // Drop kmain's spawn-sequence reference. If every test somehow
    // already finished, this is the drop that powers off; normally the
    // real processes are still running and it just balances the count.
    user_proc_exited();

    schedule(); // never returns in practice -- control passes permanently into the task system
    for (;;) {
        __asm__ volatile ("hlt");
    }
}
