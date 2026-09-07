// kernel/lib/rbtree.h -- a generic, intrusive red-black tree.
//
// Intrusive: the caller embeds a `struct rb_node` in its own struct
// and recovers the container with rb_entry(). The tree never
// allocates.
//
// The API mirrors the Linux kernel idiom (rb_link_node + rb_insert_*
// two-step insert, rb_erase, rb_first/next/... iteration) because that
// is the shape kernel code expects -- but the implementation is a
// plain CLRS red-black tree written for NeoOS, not a port.
//
// Colour lives in bit 0 of __rb_parent_color. **0 = RED, 1 = BLACK**,
// deliberately: rb_link_node writes a bare parent pointer (bit 0
// clear), so a just-linked node is RED, which is what the insert
// fix-up assumes. Bit 1 is reserved (kept clear) so an augmented
// caller could pack a flag there later.
//
// Users: the scheduler (SCH-1 EEVDF cfs_rq, SCH-4 dl_rq) and, later, a
// timer wheel. See docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md.

#ifndef NEOOS_RBTREE_H
#define NEOOS_RBTREE_H

#include <stdint.h>
#include <stddef.h>

struct rb_node {
    uintptr_t       __rb_parent_color;  // [ptr .. : 2] parent | [1]:rsvd | [0]:colour
    struct rb_node *rb_right;
    struct rb_node *rb_left;
} __attribute__((aligned(sizeof(uintptr_t))));

struct rb_root {
    struct rb_node *rb_node;
};

// Caches the leftmost (minimum) node for O(1) rb_first.
struct rb_root_cached {
    struct rb_root  rb_root;
    struct rb_node *rb_leftmost;
};

// The augment callbacks maintain a per-node subtree aggregate through
// rotations and splices. All three may be 0 for a plain tree.
//   propagate(node, stop): recompute node's aggregate from its
//     children, walking up to but not including `stop` (0 = the root);
//     an implementation may stop early once a level is unchanged.
//   copy(old, new): new takes old's tree slot verbatim -- copy the
//     aggregate across.
//   rotate(old, new): a rotation moved `new` above `old`; recompute
//     `old` (now a child) and then `new`.
struct rb_augment_callbacks {
    void (*propagate)(struct rb_node *node, struct rb_node *stop);
    void (*copy)(struct rb_node *old, struct rb_node *newn);
    void (*rotate)(struct rb_node *old, struct rb_node *newn);
};

#define RB_ROOT          ((struct rb_root){ 0 })
#define RB_ROOT_CACHED   ((struct rb_root_cached){ { 0 }, 0 })
#define RB_EMPTY_ROOT(r) ((r)->rb_node == 0)
// A detached node points __rb_parent_color at itself.
#define RB_EMPTY_NODE(n) ((n)->__rb_parent_color == (uintptr_t)(n))
#define RB_CLEAR_NODE(n) ((n)->__rb_parent_color = (uintptr_t)(n))

#define rb_entry(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))
#define rb_entry_safe(ptr, type, member) \
    ({ __typeof__(ptr) ____p = (ptr); \
       ____p ? rb_entry(____p, type, member) : 0; })

#define RB_RED    0
#define RB_BLACK  1

static inline struct rb_node *rb_parent(const struct rb_node *n) {
    return (struct rb_node *)(n->__rb_parent_color & ~(uintptr_t)3);
}
static inline int rb_color(const struct rb_node *n) {
    return (int)(n->__rb_parent_color & 1);
}
static inline int rb_is_red(const struct rb_node *n)   { return !rb_color(n); }
static inline int rb_is_black(const struct rb_node *n) { return  rb_color(n); }

// Step 1 of an insert: the caller has descended the tree to the empty
// child slot `rb_link` under `parent`; link `node` there as a RED
// leaf. Step 2 is one of the rb_insert_* functions below.
static inline void rb_link_node(struct rb_node *node, struct rb_node *parent,
                                struct rb_node **rb_link) {
    node->__rb_parent_color = (uintptr_t)parent;   // colour bit clear => RED
    node->rb_left = node->rb_right = 0;
    *rb_link = node;
}

// ---- plain tree ----
void rb_insert_color(struct rb_node *node, struct rb_root *root);
void rb_erase(struct rb_node *node, struct rb_root *root);
void rb_replace_node(struct rb_node *victim, struct rb_node *newn,
                     struct rb_root *root);

struct rb_node *rb_first(const struct rb_root *root);
struct rb_node *rb_last(const struct rb_root *root);
struct rb_node *rb_next(const struct rb_node *node);
struct rb_node *rb_prev(const struct rb_node *node);

// ---- cached-leftmost variant ----
// `leftmost` != 0 iff the caller's BST descent went left at every step
// (i.e. `node` becomes the new minimum).
void rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root,
                            int leftmost);
void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root);

static inline struct rb_node *rb_first_cached(const struct rb_root_cached *root) {
    return root->rb_leftmost;
}

// ---- augmented variants ----
void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_callbacks *augment);
void rb_insert_augmented_cached(struct rb_node *node,
                                struct rb_root_cached *root, int leftmost,
                                const struct rb_augment_callbacks *augment);
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_callbacks *augment);
void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
                              const struct rb_augment_callbacks *augment);

void rbtree_selftest(void);

#endif // NEOOS_RBTREE_H
