#include "vnode_slab.h"
#include "../mm/heap.h"
#include "../lock.h"

static struct vnode_slab_pool pool;
static struct spinlock pool_lock;

void vnode_slab_init(void) {
    pool.slabs = 0;
    pool.slab_count = 0;
    pool.total_in_use = 0;

    spin_init(&pool_lock, LOCK_RANK_VNODEHASH, "vnode_slab");

    // Allocate first slab
    struct vnode_slab *slab = (struct vnode_slab *)kmalloc(sizeof(struct vnode_slab));
    if (!slab) return;

    // Initialize slab
    slab->next = 0;
    slab->in_use_count = 0;
    for (int i = 0; i < VNODE_SLAB_SIZE; i++) {
        slab->slots[i].refcount = 0;
        slab->slots[i].mount = 0;
        slab->slots[i].fs_private = 0;
        slab->slots[i].next = 0;
    }

    pool.slabs = slab;
    pool.slab_count = 1;
}

struct vnode *vnode_slab_alloc(void) {
    uint64_t flags = spin_lock_irqsave(&pool_lock);

    // Search existing slabs for free vnode
    for (struct vnode_slab *slab = pool.slabs; slab; slab = slab->next) {
        for (int i = 0; i < VNODE_SLAB_SIZE; i++) {
            if (slab->slots[i].refcount == 0 && slab->slots[i].mount == 0) {
                // Found free slot
                slab->slots[i].refcount = 0;  // Will be set by caller
                slab->slots[i].mount = 0;     // Will be set by caller
                slab->slots[i].fs_private = 0;
                slab->in_use_count++;
                pool.total_in_use++;

                spin_unlock_irqrestore(&pool_lock, flags);
                return &slab->slots[i];
            }
        }
    }

    // No free slot in existing slabs, allocate new slab
    struct vnode_slab *new_slab = (struct vnode_slab *)kmalloc(sizeof(struct vnode_slab));
    if (!new_slab) {
        spin_unlock_irqrestore(&pool_lock, flags);
        return 0;  // OOM
    }

    // Initialize new slab
    new_slab->next = pool.slabs;
    new_slab->in_use_count = 0;
    for (int i = 0; i < VNODE_SLAB_SIZE; i++) {
        new_slab->slots[i].refcount = 0;
        new_slab->slots[i].mount = 0;
        new_slab->slots[i].fs_private = 0;
        new_slab->slots[i].next = 0;
    }

    pool.slabs = new_slab;
    pool.slab_count++;

    // Allocate from new slab
    struct vnode *vn = &new_slab->slots[0];
    new_slab->in_use_count++;
    pool.total_in_use++;

    spin_unlock_irqrestore(&pool_lock, flags);
    return vn;
}

void vnode_slab_free(struct vnode *vn) {
    if (!vn) return;

    uint64_t flags = spin_lock_irqsave(&pool_lock);

    // Find which slab this vnode belongs to and update counts
    for (struct vnode_slab *slab = pool.slabs; slab; slab = slab->next) {
        if (vn >= &slab->slots[0] && vn < &slab->slots[VNODE_SLAB_SIZE]) {
            // Found the slab
            if (slab->in_use_count > 0) {
                slab->in_use_count--;
            }
            if (pool.total_in_use > 0) {
                pool.total_in_use--;
            }
            spin_unlock_irqrestore(&pool_lock, flags);
            return;
        }
    }

    spin_unlock_irqrestore(&pool_lock, flags);
}

int vnode_slab_in_use_count(void) {
    uint64_t flags = spin_lock_irqsave(&pool_lock);
    int count = pool.total_in_use;
    spin_unlock_irqrestore(&pool_lock, flags);
    return count;
}
