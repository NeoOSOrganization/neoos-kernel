#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "serial.h"
#include "../lock.h"

// One lock over the heap's free lists and page arrays. Takes pmm_lock
// beneath it when it needs more pages -- HEAP (12) then PMM (13) is
// ascending, which is the order the rank table has always declared.
// Non-static only so heap_selftest can assert its rank.
struct spinlock heap_lock;

static const uint32_t heap_size_classes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048 };
#define HEAP_NUM_CLASSES (sizeof(heap_size_classes) / sizeof(heap_size_classes[0]))
#define HEAP_LARGE_MARKER 0xFFFFFFFFu

struct heap_free_slot {
    struct heap_free_slot *next;
};

// 64-byte aligned so that sizeof() -- and therefore the offset of the
// first slot in every page -- is a multiple of 64. Slots are carved at
// multiples of their size class starting from that offset, so this is
// what makes every kmalloc() pointer suitably aligned for the SSE and
// FPU state buffers stored in them: fxsave/fxrstor #GP on an address
// that is not 16-byte aligned, and XSAVE will want 64. Without the
// attribute the header is 24 bytes and every slot lands on an 8-mod-16
// address (observed: #GP in schedule()'s fxrstor of a heap-allocated
// struct thread).
struct heap_page {
    struct heap_page *next;
    struct heap_free_slot *free_list;
    uint32_t size_class; // bytes per slot, or HEAP_LARGE_MARKER for a large (multi-page) allocation
    uint32_t meta;        // size-class pages: free slot count. Large allocations: the pmm buddy order.
} __attribute__((aligned(64)));

static struct heap_page *class_pages[HEAP_NUM_CLASSES];

static int size_class_for(size_t size) {
    for (unsigned i = 0; i < HEAP_NUM_CLASSES; i++) {
        if (size <= heap_size_classes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static struct heap_page *heap_new_page(uint32_t size_class) {
    uint64_t phys = pmm_alloc(0);
    if (!phys) {
        return 0;
    }

    struct heap_page *page = (struct heap_page *)phys_to_virt(phys);
    page->size_class = size_class;
    page->free_list = 0;
    page->next = 0;

    uint8_t *area = (uint8_t *)page + sizeof(struct heap_page);
    uint32_t slot_count = (uint32_t)((PMM_FRAME_SIZE - sizeof(struct heap_page)) / size_class);
    for (uint32_t i = 0; i < slot_count; i++) {
        struct heap_free_slot *slot = (struct heap_free_slot *)(area + i * size_class);
        slot->next = page->free_list;
        page->free_list = slot;
    }
    page->meta = slot_count;
    return page;
}

void heap_init(void) {
    spin_init(&heap_lock, LOCK_RANK_HEAP, "heap");
    for (unsigned i = 0; i < HEAP_NUM_CLASSES; i++) {
        class_pages[i] = 0;
    }
    serial_write_string("[heap] initialized\n");
}

// Unlocked. Caller must hold heap_lock.
static void *kmalloc_locked(size_t size) {
    if (size == 0) {
        return 0;
    }
    if (size < sizeof(struct heap_free_slot)) {
        size = sizeof(struct heap_free_slot);
    }

    int class_index = size_class_for(size);
    if (class_index >= 0) {
        // Scan the class's pages for one with a free slot. The original
        // version checked only the FRONT page and allocated a fresh one
        // whenever it was full, which permanently stranded every slot
        // freed back into a non-front page. That was harmless while
        // kmalloc was used for a handful of long-lived objects, but the
        // threads milestone allocates a struct thread per thread -- at
        // 3 slots per page, a process creating 8 threads strands two
        // pages per run (measured: ~1 frame leaked per spawn/wait
        // cycle, growing linearly with iteration count).
        struct heap_page *page = class_pages[class_index];
        while (page && !page->free_list) {
            page = page->next;
        }
        if (!page) {
            page = heap_new_page(heap_size_classes[class_index]);
            if (!page) {
                return 0;
            }
            page->next = class_pages[class_index];
            class_pages[class_index] = page;
        }

        struct heap_free_slot *slot = page->free_list;
        page->free_list = slot->next;
        page->meta--;
        return (void *)slot;
    }

    // Large allocation: header lives on the first page; the block spans
    // ceil((size + header) / 4096) frames, rounded up to a buddy order.
    uint64_t needed = size + sizeof(struct heap_page);
    uint64_t frames_needed = (needed + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    unsigned order = 0;
    while ((1ULL << order) < frames_needed) {
        order++;
    }

    uint64_t phys = pmm_alloc(order);
    if (!phys) {
        return 0;
    }
    struct heap_page *page = (struct heap_page *)phys_to_virt(phys);
    page->size_class = HEAP_LARGE_MARKER;
    page->meta = order;
    page->free_list = 0;
    page->next = 0;
    return (void *)((uint8_t *)page + sizeof(struct heap_page));
}

// Unlocked. Caller must hold heap_lock.
static void kfree_locked(void *ptr) {
    if (!ptr) {
        return;
    }

    struct heap_page *page = (struct heap_page *)((uintptr_t)ptr & ~(uint64_t)0xFFF);
    if (page->size_class == HEAP_LARGE_MARKER) {
        pmm_free(virt_to_phys_physmap((uint64_t)(uintptr_t)page), page->meta);
        return;
    }

    struct heap_free_slot *slot = (struct heap_free_slot *)ptr;
    slot->next = page->free_list;
    page->free_list = slot;
    page->meta++;
}

// The locked public entry points. Only these take heap_lock; the
// statics above are called from within an already-locked region, and
// locking them too would be a same-rank self-deadlock.
void *kmalloc(size_t size) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    void *p = kmalloc_locked(size);
    spin_unlock_irqrestore(&heap_lock, flags);
    return p;
}

void kfree(void *ptr) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    kfree_locked(ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}

void heap_selftest(void) {
    void *ptrs[16];
    for (int i = 0; i < 16; i++) {
        size_t size = heap_size_classes[i % HEAP_NUM_CLASSES];
        ptrs[i] = kmalloc(size);
        if (!ptrs[i]) {
            serial_write_string("[heap] selftest FAILED: kmalloc returned NULL\n");
            return;
        }
        uint8_t pattern = (uint8_t)(i + 1);
        for (size_t b = 0; b < size; b++) {
            ((uint8_t *)ptrs[i])[b] = pattern;
        }
    }

    void *large = kmalloc(9000); // exercises the multi-page path
    if (!large) {
        serial_write_string("[heap] selftest FAILED: large kmalloc returned NULL\n");
        return;
    }
    for (size_t b = 0; b < 9000; b++) {
        ((uint8_t *)large)[b] = 0xAB;
    }

    for (int i = 0; i < 16; i++) {
        size_t size = heap_size_classes[i % HEAP_NUM_CLASSES];
        uint8_t pattern = (uint8_t)(i + 1);
        for (size_t b = 0; b < size; b++) {
            if (((uint8_t *)ptrs[i])[b] != pattern) {
                serial_write_string("[heap] selftest FAILED: pattern mismatch\n");
                return;
            }
        }
    }
    for (size_t b = 0; b < 9000; b++) {
        if (((uint8_t *)large)[b] != 0xAB) {
            serial_write_string("[heap] selftest FAILED: large pattern mismatch\n");
            return;
        }
    }

    for (int i = 0; i < 16; i++) {
        kfree(ptrs[i]);
    }
    kfree(large);

    if (heap_lock.rank != LOCK_RANK_HEAP) {
        serial_write_string("[heap] selftest FAILED: lock rank wrong\n");
        return;
    }
    // The heap calls pmm when it needs more pages, so PMM must be
    // legally acquirable while HEAP is held -- strictly ascending.
    uint64_t f = spin_lock_irqsave(&heap_lock);
    int pmm_ok = lock_rank_ok(LOCK_RANK_PMM);
    spin_unlock_irqrestore(&heap_lock, f);
    if (!pmm_ok) {
        serial_write_string("[heap] selftest FAILED: pmm not acquirable under heap\n");
        return;
    }
    void *probe = kmalloc(64);
    if (lock_held_depth() != 0) {
        serial_write_string("[heap] selftest FAILED: kmalloc leaked the lock\n");
        return;
    }
    kfree(probe);
    if (lock_held_depth() != 0) {
        serial_write_string("[heap] selftest FAILED: kfree leaked the lock\n");
        return;
    }

    serial_write_string("[heap] selftest passed\n");
}
