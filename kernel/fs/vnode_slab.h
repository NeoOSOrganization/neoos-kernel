#ifndef NEOOS_VNODE_SLAB_H
#define NEOOS_VNODE_SLAB_H

#include <stdint.h>
#include "vfs.h"

/*
 * Vnode slab allocator
 *
 * Replaces static vnodes[MAX_VNODES] array with dynamic slab pools.
 * Each slab contains VNODE_SLAB_SIZE vnodes allocated together.
 * Multiple slabs are linked for growth.
 *
 * Benefits:
 * - Better cache locality (objects allocated together)
 * - Efficient memory usage (allocate only what's needed)
 * - Simpler freelist management
 * - No fixed MAX_VNODES limit
 *
 * Current: 64 vnodes in static array
 * With slab: Start with 16/slab, grow as needed
 * Typical system: 8-16 vnodes in use, 1-2 slabs allocated
 */

#define VNODE_SLAB_SIZE 16   // vnodes per slab

struct vnode_slab {
    struct vnode_slab *next;           // linked list of slabs
    struct vnode slots[VNODE_SLAB_SIZE];
    int in_use_count;                  // count of allocated vnodes in this slab
};

struct vnode_slab_pool {
    struct vnode_slab *slabs;          // head of slab list
    int slab_count;                    // total slabs allocated
    int total_in_use;                  // total vnodes allocated across all slabs
};

// Initialize vnode slab pool
void vnode_slab_init(void);

// Allocate a vnode from the slab pool
struct vnode *vnode_slab_alloc(void);

// Free a vnode back to the slab pool
void vnode_slab_free(struct vnode *vn);

// Get count of vnodes in use
int vnode_slab_in_use_count(void);

#endif
