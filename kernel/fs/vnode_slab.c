#include "vnode_slab.h"
#include "../mm/heap.h"
#include "../lock.h"

static struct vnode_slab_pool pool;
static struct spinlock pool_lock;

static void slab_init(struct vnode_slab *slab) {
    slab->next = 0;
    slab->used_mask = 0;
    slab->in_use_count = 0;
    for (int i = 0; i < VNODE_SLAB_SIZE; i++) {
        slab->slots[i].refcount = 0;
        slab->slots[i].mount = 0;
        slab->slots[i].fs_private = 0;
        slab->slots[i].next = 0;
    }
}

void vnode_slab_init(void) {
    pool.slabs = 0;
    pool.slab_count = 0;
    pool.total_in_use = 0;

    spin_init(&pool_lock, LOCK_RANK_VNODEHASH, "vnode_slab");

    // Allocate first slab
    struct vnode_slab *slab = (struct vnode_slab *)kmalloc(sizeof(struct vnode_slab));
    if (!slab) return;

    slab_init(slab);
    pool.slabs = slab;
    pool.slab_count = 1;
}

// Claims slots[i] in `slab` and returns it. Caller holds pool_lock.
static struct vnode *slab_take(struct vnode_slab *slab, int i) {
    slab->used_mask |= (uint16_t)(1u << i);
    slab->in_use_count++;
    pool.total_in_use++;

    struct vnode *vn = &slab->slots[i];
    vn->refcount = 0;    // filled in by the caller
    vn->mount = 0;
    vn->fs_private = 0;
    vn->next = 0;
    return vn;
}

struct vnode *vnode_slab_alloc(void) {
    uint64_t flags = spin_lock_irqsave(&pool_lock);

    for (struct vnode_slab *slab = pool.slabs; slab; slab = slab->next) {
        if (slab->in_use_count == VNODE_SLAB_SIZE) { continue; }
        for (int i = 0; i < VNODE_SLAB_SIZE; i++) {
            if (slab->used_mask & (1u << i)) { continue; }
            struct vnode *vn = slab_take(slab, i);
            spin_unlock_irqrestore(&pool_lock, flags);
            return vn;
        }
    }

    // Every slab full -- grow the pool.
    struct vnode_slab *new_slab = (struct vnode_slab *)kmalloc(sizeof(struct vnode_slab));
    if (!new_slab) {
        spin_unlock_irqrestore(&pool_lock, flags);
        return 0;  // OOM
    }

    slab_init(new_slab);
    new_slab->next = pool.slabs;
    pool.slabs = new_slab;
    pool.slab_count++;

    struct vnode *vn = slab_take(new_slab, 0);

    spin_unlock_irqrestore(&pool_lock, flags);
    return vn;
}

void vnode_slab_free(struct vnode *vn) {
    if (!vn) return;

    uint64_t flags = spin_lock_irqsave(&pool_lock);

    for (struct vnode_slab *slab = pool.slabs; slab; slab = slab->next) {
        if (vn < &slab->slots[0] || vn >= &slab->slots[VNODE_SLAB_SIZE]) {
            continue;
        }
        int i = (int)(vn - &slab->slots[0]);
        if (slab->used_mask & (1u << i)) {
            slab->used_mask &= (uint16_t)~(1u << i);
            slab->in_use_count--;
            pool.total_in_use--;
        }
        break;
    }

    spin_unlock_irqrestore(&pool_lock, flags);
}

int vnode_slab_in_use_count(void) {
    uint64_t flags = spin_lock_irqsave(&pool_lock);
    int count = pool.total_in_use;
    spin_unlock_irqrestore(&pool_lock, flags);
    return count;
}
