#include "mm/mmio.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "sync/lock.h"
#include "drivers/char/serial.h"

// Bump allocator over the window: device BARs are mapped once, at probe
// time, and never unmapped, so nothing is ever given back.
static uint64_t next_virt;
static struct spinlock mmio_lock;

void mmio_init(void) {
    spin_init(&mmio_lock, LOCK_RANK_DRIVER, "mmio");
    next_virt = MMIO_VIRT_BASE;
}

volatile void *mmio_map(uint64_t phys, uint64_t len) {
    uint64_t base = phys & ~0xFFFULL;
    uint64_t span = ((phys + len + 0xFFF) & ~0xFFFULL) - base;
    if (len == 0) { return 0; }

    uint64_t f = spin_lock_irqsave(&mmio_lock);
    if (span > MMIO_VIRT_BASE + MMIO_VIRT_SIZE - next_virt) {
        spin_unlock_irqrestore(&mmio_lock, f);
        serial_write_string("[mmio] window exhausted\n");
        return 0;
    }
    uint64_t virt = next_virt;
    next_virt += span;
    spin_unlock_irqrestore(&mmio_lock, f);

    // Outside the lock: paging_map_range allocates page tables (pmm_lock
    // is rank PMM, above DRIVER, so holding mmio_lock would be legal, but
    // there is nothing to protect -- the range is already ours).
    if (paging_map_range(virt, base, span,
                         PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_PCD | PAGE_PWT) != 0) {
        serial_write_string("[mmio] page table allocation failed\n");
        return 0;
    }
    return (volatile void *)(uintptr_t)(virt + (phys - base));
}

// Proves the three things a driver depends on: the alias really reaches
// the physical page, the leaf entry really is uncached, and a sub-page
// offset survives the rounding.
void mmio_selftest(void) {
    const char *why = 0;
    uint64_t pa = pmm_alloc(0);
    if (!pa) { why = "pmm_alloc"; goto out; }
    volatile uint64_t *v = (volatile uint64_t *)mmio_map(pa, 4096);
    if (!v) { why = "map"; goto out; }
    *v = 0xA5A5DEADBEEF5A5AULL;
    if (*(volatile uint64_t *)phys_to_virt(pa) != 0xA5A5DEADBEEF5A5AULL) { why = "alias does not reach the page"; goto out; }
    if ((paging_leaf_flags((uint64_t)(uintptr_t)v) & (PAGE_PCD | PAGE_PWT)) != (PAGE_PCD | PAGE_PWT)) {
        why = "leaf entry is cacheable"; goto out;
    }
    volatile uint8_t *o = (volatile uint8_t *)mmio_map(pa + 0x10, 8);
    if (!o || ((uint64_t)(uintptr_t)o & 0xFFF) != 0x10) { why = "sub-page offset lost"; goto out; }
    if (*(volatile uint64_t *)o != *(volatile uint64_t *)phys_to_virt(pa + 0x10)) { why = "offset alias"; goto out; }
out:
    // The frame is not freed: its uncached aliases are permanent, and a
    // frame reused as ordinary memory under a UC alias is a trap.
    if (why) {
        serial_write_string("[mmio] selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[mmio] selftest passed\n");
}
