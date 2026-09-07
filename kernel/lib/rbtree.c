// kernel/lib/rbtree.c -- CLRS red-black tree, intrusive, for NeoOS.
// See rbtree.h for the API contract and the colour convention
// (0 = RED, 1 = BLACK, packed in __rb_parent_color bit 0).

#include "lib/rbtree.h"

// ---------------------------------------------------------------- colour

static inline void rb_set_parent(struct rb_node *n, struct rb_node *p) {
    n->__rb_parent_color = (n->__rb_parent_color & 3) | (uintptr_t)p;
}
static inline void rb_set_parent_color(struct rb_node *n, struct rb_node *p,
                                       int color) {
    n->__rb_parent_color = (uintptr_t)p | (unsigned)color;
}
static inline void rb_set_black(struct rb_node *n) {
    n->__rb_parent_color |= RB_BLACK;
}
static inline void rb_set_red(struct rb_node *n) {
    n->__rb_parent_color &= ~(uintptr_t)1;
}
// A NIL (absent) child counts as black.
static inline int node_is_black(const struct rb_node *n) {
    return !n || rb_is_black(n);
}

// ---------------------------------------------------------------- rotate
//
// Rotations never touch colours -- the insert/erase fix-ups own those.
// They do fix every parent link and the root pointer, and hand the
// (old-root, new-root) pair to the augment callback so a subtree
// aggregate can be repaired.

static void __rb_rotate_left(struct rb_node *node, struct rb_root *root,
                             const struct rb_augment_callbacks *aug) {
    struct rb_node *pivot  = node->rb_right;
    struct rb_node *parent = rb_parent(node);

    node->rb_right = pivot->rb_left;
    if (pivot->rb_left) { rb_set_parent(pivot->rb_left, node); }

    pivot->rb_left = node;
    rb_set_parent(node, pivot);

    rb_set_parent(pivot, parent);
    if (parent) {
        if (parent->rb_left == node) { parent->rb_left  = pivot; }
        else                         { parent->rb_right = pivot; }
    } else {
        root->rb_node = pivot;
    }

    if (aug && aug->rotate) { aug->rotate(node, pivot); }
}

static void __rb_rotate_right(struct rb_node *node, struct rb_root *root,
                              const struct rb_augment_callbacks *aug) {
    struct rb_node *pivot  = node->rb_left;
    struct rb_node *parent = rb_parent(node);

    node->rb_left = pivot->rb_right;
    if (pivot->rb_right) { rb_set_parent(pivot->rb_right, node); }

    pivot->rb_right = node;
    rb_set_parent(node, pivot);

    rb_set_parent(pivot, parent);
    if (parent) {
        if (parent->rb_left == node) { parent->rb_left  = pivot; }
        else                         { parent->rb_right = pivot; }
    } else {
        root->rb_node = pivot;
    }

    if (aug && aug->rotate) { aug->rotate(node, pivot); }
}

// ---------------------------------------------------------------- insert

// CLRS RB-INSERT-FIXUP. `node` is already linked as a RED leaf.
static void __rb_insert(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_callbacks *aug) {
    struct rb_node *parent, *gparent, *uncle;

    for (;;) {
        parent = rb_parent(node);
        if (!parent) {                    // node is the root
            rb_set_parent_color(node, 0, RB_BLACK);
            break;
        }
        if (rb_is_black(parent)) {
            break;                        // no red-red violation
        }
        gparent = rb_parent(parent);      // exists: parent is red => not root

        if (parent == gparent->rb_left) {
            uncle = gparent->rb_right;
            if (uncle && rb_is_red(uncle)) {
                // Case 1: recolour and continue from the grandparent.
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }
            if (node == parent->rb_right) {
                // Case 2: reduce to case 3 by left-rotating the parent.
                __rb_rotate_left(parent, root, aug);
                parent = node;            // 'parent' now names the node
                                          // adjacent to the grandparent
            }
            // Case 3.
            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_right(gparent, root, aug);
            break;
        } else {
            uncle = gparent->rb_left;
            if (uncle && rb_is_red(uncle)) {
                rb_set_black(uncle);
                rb_set_black(parent);
                rb_set_red(gparent);
                node = gparent;
                continue;
            }
            if (node == parent->rb_left) {
                __rb_rotate_right(parent, root, aug);
                parent = node;
            }
            rb_set_black(parent);
            rb_set_red(gparent);
            __rb_rotate_left(gparent, root, aug);
            break;
        }
    }
}

void rb_insert_augmented(struct rb_node *node, struct rb_root *root,
                         const struct rb_augment_callbacks *augment) {
    // Contract: the caller has already set `node`'s own subtree
    // aggregate (a fresh leaf's aggregate == its own value). Propagate
    // "one more node in the subtree" up from the parent BEFORE the
    // fix-up; the fix-up's rotations then repair themselves via
    // augment->rotate. (This is the "suboptimal but correct" shape:
    // propagate always walks to the root.)
    if (augment && augment->propagate) {
        augment->propagate(rb_parent(node), 0);   // callback must accept 0
    }
    __rb_insert(node, root, augment);
}

void rb_insert_color(struct rb_node *node, struct rb_root *root) {
    __rb_insert(node, root, 0);
}

// ---------------------------------------------------------------- erase

// Replace the subtree rooted at `u` with the one rooted at `v` (may be
// 0). Does NOT set v's parent when v == 0 -- the caller tracks that.
static void transplant(struct rb_root *root, struct rb_node *u,
                       struct rb_node *v) {
    struct rb_node *up = rb_parent(u);
    if (!up)                    { root->rb_node   = v; }
    else if (u == up->rb_left)  { up->rb_left     = v; }
    else                        { up->rb_right    = v; }
    if (v) { rb_set_parent(v, up); }
}

static struct rb_node *subtree_min(struct rb_node *n) {
    while (n->rb_left) { n = n->rb_left; }
    return n;
}

// CLRS RB-DELETE-FIXUP. `x` (possibly 0) is doubly-black; `parent` is
// its parent. Rebalances upward.
static void __rb_erase_fixup(struct rb_node *x, struct rb_node *parent,
                             struct rb_root *root,
                             const struct rb_augment_callbacks *aug) {
    struct rb_node *w;

    while (x != root->rb_node && node_is_black(x)) {
        if (x == parent->rb_left) {
            w = parent->rb_right;                  // sibling (never NIL here)
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(parent);
                __rb_rotate_left(parent, root, aug);
                w = parent->rb_right;
            }
            if (node_is_black(w->rb_left) && node_is_black(w->rb_right)) {
                rb_set_red(w);
                x = parent;
                parent = rb_parent(x);
            } else {
                if (node_is_black(w->rb_right)) {
                    if (w->rb_left) { rb_set_black(w->rb_left); }
                    rb_set_red(w);
                    __rb_rotate_right(w, root, aug);
                    w = parent->rb_right;
                }
                rb_set_parent_color(w, rb_parent(w), rb_color(parent));
                rb_set_black(parent);
                if (w->rb_right) { rb_set_black(w->rb_right); }
                __rb_rotate_left(parent, root, aug);
                x = root->rb_node;
                break;
            }
        } else {
            w = parent->rb_left;
            if (rb_is_red(w)) {
                rb_set_black(w);
                rb_set_red(parent);
                __rb_rotate_right(parent, root, aug);
                w = parent->rb_left;
            }
            if (node_is_black(w->rb_left) && node_is_black(w->rb_right)) {
                rb_set_red(w);
                x = parent;
                parent = rb_parent(x);
            } else {
                if (node_is_black(w->rb_left)) {
                    if (w->rb_right) { rb_set_black(w->rb_right); }
                    rb_set_red(w);
                    __rb_rotate_left(w, root, aug);
                    w = parent->rb_left;
                }
                rb_set_parent_color(w, rb_parent(w), rb_color(parent));
                rb_set_black(parent);
                if (w->rb_left) { rb_set_black(w->rb_left); }
                __rb_rotate_right(parent, root, aug);
                x = root->rb_node;
                break;
            }
        }
    }
    if (x) { rb_set_black(x); }
}

static void __rb_erase(struct rb_node *z, struct rb_root *root,
                       const struct rb_augment_callbacks *aug) {
    struct rb_node *x, *x_parent;
    struct rb_node *y = z;
    int y_black = rb_is_black(y);
    // The lowest node whose subtree composition changed -- augment
    // propagation starts here.
    struct rb_node *aug_from;

    if (!z->rb_left) {
        x = z->rb_right;
        x_parent = rb_parent(z);
        transplant(root, z, z->rb_right);
        aug_from = x_parent;
    } else if (!z->rb_right) {
        x = z->rb_left;
        x_parent = rb_parent(z);
        transplant(root, z, z->rb_left);
        aug_from = x_parent;
    } else {
        y = subtree_min(z->rb_right);
        y_black = rb_is_black(y);
        x = y->rb_right;
        if (rb_parent(y) == z) {
            // y is z's right child; x (maybe 0) hangs directly off y.
            x_parent = y;
            aug_from = y;
        } else {
            x_parent = rb_parent(y);
            transplant(root, y, y->rb_right);
            y->rb_right = z->rb_right;
            rb_set_parent(y->rb_right, y);
            aug_from = x_parent;
        }
        transplant(root, z, y);
        y->rb_left = z->rb_left;
        rb_set_parent(y->rb_left, y);
        rb_set_parent_color(y, rb_parent(y), rb_color(z));
        if (aug && aug->copy) { aug->copy(z, y); }
    }

    // Fix subtree aggregates along the changed path before any fix-up
    // rotations, then again after (the rotations call aug->rotate
    // themselves and __rb_erase_fixup can move the changed frontier up).
    if (aug && aug->propagate && aug_from) { aug->propagate(aug_from, 0); }

    if (y_black) {
        __rb_erase_fixup(x, x_parent, root, aug);
    }

    if (aug && aug->propagate && aug_from) {
        // aug_from may have been rotated out of the tree; walk from
        // wherever it now sits (its parent chain still terminates at
        // the root).
        aug->propagate(aug_from, 0);
    }
}

void rb_erase(struct rb_node *node, struct rb_root *root) {
    __rb_erase(node, root, 0);
}
void rb_erase_augmented(struct rb_node *node, struct rb_root *root,
                        const struct rb_augment_callbacks *augment) {
    __rb_erase(node, root, augment);
}

// ---------------------------------------------------------------- replace

void rb_replace_node(struct rb_node *victim, struct rb_node *newn,
                     struct rb_root *root) {
    struct rb_node *parent = rb_parent(victim);

    newn->__rb_parent_color = victim->__rb_parent_color;
    newn->rb_left  = victim->rb_left;
    newn->rb_right = victim->rb_right;

    if (victim->rb_left)  { rb_set_parent(victim->rb_left,  newn); }
    if (victim->rb_right) { rb_set_parent(victim->rb_right, newn); }

    if (!parent)                        { root->rb_node   = newn; }
    else if (victim == parent->rb_left) { parent->rb_left  = newn; }
    else                                { parent->rb_right = newn; }
}

// --------------------------------------------------------------- iterate

struct rb_node *rb_first(const struct rb_root *root) {
    struct rb_node *n = root->rb_node;
    if (!n) { return 0; }
    while (n->rb_left) { n = n->rb_left; }
    return n;
}

struct rb_node *rb_last(const struct rb_root *root) {
    struct rb_node *n = root->rb_node;
    if (!n) { return 0; }
    while (n->rb_right) { n = n->rb_right; }
    return n;
}

struct rb_node *rb_next(const struct rb_node *node) {
    if (RB_EMPTY_NODE(node)) { return 0; }
    if (node->rb_right) {
        node = node->rb_right;
        while (node->rb_left) { node = node->rb_left; }
        return (struct rb_node *)node;
    }
    struct rb_node *parent;
    while ((parent = rb_parent(node)) && node == parent->rb_right) {
        node = parent;
    }
    return parent;
}

struct rb_node *rb_prev(const struct rb_node *node) {
    if (RB_EMPTY_NODE(node)) { return 0; }
    if (node->rb_left) {
        node = node->rb_left;
        while (node->rb_right) { node = node->rb_right; }
        return (struct rb_node *)node;
    }
    struct rb_node *parent;
    while ((parent = rb_parent(node)) && node == parent->rb_left) {
        node = parent;
    }
    return parent;
}

// ---------------------------------------------------------------- cached

void rb_insert_color_cached(struct rb_node *node, struct rb_root_cached *root,
                            int leftmost) {
    if (leftmost) { root->rb_leftmost = node; }
    __rb_insert(node, &root->rb_root, 0);
}

void rb_insert_augmented_cached(struct rb_node *node,
                                struct rb_root_cached *root, int leftmost,
                                const struct rb_augment_callbacks *augment) {
    if (leftmost) { root->rb_leftmost = node; }
    rb_insert_augmented(node, &root->rb_root, augment);
}

void rb_erase_cached(struct rb_node *node, struct rb_root_cached *root) {
    if (root->rb_leftmost == node) { root->rb_leftmost = rb_next(node); }
    __rb_erase(node, &root->rb_root, 0);
}

void rb_erase_augmented_cached(struct rb_node *node, struct rb_root_cached *root,
                               const struct rb_augment_callbacks *augment) {
    if (root->rb_leftmost == node) { root->rb_leftmost = rb_next(node); }
    __rb_erase(node, &root->rb_root, augment);
}
