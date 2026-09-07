// kernel/lib/rbtree_selftest.c -- boot-time proof of kernel/lib/rbtree.
//
// Builds a 4096-node tree from random keys, verifies the red-black
// invariants continuously, exercises rb_erase churn, the cached
// leftmost, and an augmented subtree-sum callback set, cross-checking
// everything against brute force. Prints exactly one of
//   [rbtree] selftest passed
//   [rbtree] selftest FAILED: <reason>

#include "lib/rbtree.h"
#include "drivers/char/serial.h"
#include <stdint.h>
#include <stddef.h>

// A private xorshift PRNG so this test does not depend on rand.c's
// init order.
static uint64_t prng_state = 0x2545F4914F6CDD1DULL;
static uint32_t prng(void) {
    uint64_t x = prng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    prng_state = x;
    return (uint32_t)(x >> 32);
}

static void fail(const char *why) {
    serial_write_string("[rbtree] selftest FAILED: ");
    serial_write_string(why);
    serial_write_string("\n");
}

#define POOL 4096

// ---------------------------------------------------------------- plain

struct tnode {
    struct rb_node rb;
    uint32_t       key;
};

static struct tnode t_pool[POOL];
static int          t_in[POOL];

static void t_insert(struct rb_root *root, uint32_t key, int idx) {
    struct rb_node **link = &root->rb_node, *parent = 0;
    while (*link) {
        parent = *link;
        struct tnode *t = rb_entry(parent, struct tnode, rb);
        link = (key < t->key) ? &parent->rb_left : &parent->rb_right;
    }
    t_pool[idx].key = key;
    rb_link_node(&t_pool[idx].rb, parent, link);
    rb_insert_color(&t_pool[idx].rb, root);
}

// Returns the subtree black-height, or -1 on any invariant violation
// (sets *ok = 0).
static int check(struct rb_node *n, int *ok) {
    if (!*ok) { return -1; }
    if (!n)   { return 1; }   // NIL leaves: black, height 1

    struct rb_node *l = n->rb_left, *r = n->rb_right;

    if (rb_is_red(n)) {
        if ((l && rb_is_red(l)) || (r && rb_is_red(r))) { *ok = 0; return -1; }
    }
    if (l && rb_parent(l) != n) { *ok = 0; return -1; }
    if (r && rb_parent(r) != n) { *ok = 0; return -1; }
    if (l && rb_entry(l, struct tnode, rb)->key >
             rb_entry(n, struct tnode, rb)->key) { *ok = 0; return -1; }
    if (r && rb_entry(r, struct tnode, rb)->key <
             rb_entry(n, struct tnode, rb)->key) { *ok = 0; return -1; }

    int bl = check(l, ok), br = check(r, ok);
    if (!*ok || bl != br) { *ok = 0; return -1; }
    return bl + (rb_is_black(n) ? 1 : 0);
}

static int invariants_ok(struct rb_root *root) {
    if (root->rb_node && rb_is_red(root->rb_node)) { return 0; }
    int ok = 1;
    check(root->rb_node, &ok);
    return ok;
}

static int plain_phase(void) {
    struct rb_root root = RB_ROOT;

    // Build.
    for (int i = 0; i < POOL; i++) {
        t_insert(&root, prng(), i);
        t_in[i] = 1;
        if ((i & 0x1ff) == 0x1ff && !invariants_ok(&root)) {
            fail("invariant violated during insert"); return 0;
        }
    }
    if (!invariants_ok(&root)) { fail("invariant after full insert"); return 0; }

    // Forward traversal: sorted, every node once.
    {
        uint32_t prev = 0; int first = 1, seen = 0;
        for (struct rb_node *n = rb_first(&root); n; n = rb_next(n)) {
            uint32_t k = rb_entry(n, struct tnode, rb)->key;
            if (!first && k < prev) { fail("rb_next not sorted"); return 0; }
            prev = k; first = 0; seen++;
        }
        if (seen != POOL) { fail("rb_next visited wrong count"); return 0; }
    }
    // Reverse traversal.
    {
        int seen = 0;
        for (struct rb_node *n = rb_last(&root); n; n = rb_prev(n)) { seen++; }
        if (seen != POOL) { fail("rb_prev visited wrong count"); return 0; }
    }

    // Delete / re-insert churn.
    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < POOL; i++) {
            if (t_in[i] && (prng() & 3) == 0) {
                rb_erase(&t_pool[i].rb, &root);
                t_in[i] = 0;
            }
        }
        if (!invariants_ok(&root)) { fail("invariant violated during erase"); return 0; }
        for (int i = 0; i < POOL; i++) {
            if (!t_in[i] && (prng() & 1)) {
                t_insert(&root, prng(), i);
                t_in[i] = 1;
            }
        }
        if (!invariants_ok(&root)) { fail("invariant violated during re-insert"); return 0; }
    }

    // Count matches the bookkeeping.
    {
        int want = 0, have = 0;
        for (int i = 0; i < POOL; i++) { want += t_in[i]; }
        for (struct rb_node *n = rb_first(&root); n; n = rb_next(n)) { have++; }
        if (have != want) { fail("node count drifted after churn"); return 0; }
    }

    // Erase everything.
    for (int i = 0; i < POOL; i++) {
        if (t_in[i]) { rb_erase(&t_pool[i].rb, &root); t_in[i] = 0; }
    }
    if (!RB_EMPTY_ROOT(&root)) { fail("tree not empty after erasing all"); return 0; }
    return 1;
}

// -------------------------------------------------------- augmented / cached

struct anode {
    struct rb_node rb;
    uint32_t       key;
    uint64_t       subtree_sum;   // sum of `key` over this node's subtree
};

static uint64_t brute_sum(struct rb_node *n) {
    if (!n) { return 0; }
    struct anode *a = rb_entry(n, struct anode, rb);
    return a->key + brute_sum(n->rb_left) + brute_sum(n->rb_right);
}

static uint64_t node_sum(struct rb_node *n) {
    struct anode *a = rb_entry(n, struct anode, rb);
    uint64_t s = a->key;
    if (n->rb_left)  { s += rb_entry(n->rb_left,  struct anode, rb)->subtree_sum; }
    if (n->rb_right) { s += rb_entry(n->rb_right, struct anode, rb)->subtree_sum; }
    return s;
}

static void a_propagate(struct rb_node *node, struct rb_node *stop) {
    while (node != stop) {                 // handles node == 0, stop == 0
        struct anode *a = rb_entry(node, struct anode, rb);
        uint64_t s = node_sum(node);
        if (a->subtree_sum == s) { break; } // unchanged here => unchanged above
        a->subtree_sum = s;
        node = rb_parent(node);
    }
}
static void a_copy(struct rb_node *old, struct rb_node *newn) {
    rb_entry(newn, struct anode, rb)->subtree_sum =
        rb_entry(old, struct anode, rb)->subtree_sum;
}
static void a_rotate(struct rb_node *old, struct rb_node *newn) {
    a_copy(old, newn);
    rb_entry(old, struct anode, rb)->subtree_sum = node_sum(old);
}
static const struct rb_augment_callbacks A_CB = { a_propagate, a_copy, a_rotate };

static struct anode a_pool[POOL];
static int          a_in[POOL];

static int a_insert(struct rb_root_cached *root, uint32_t key, int idx) {
    struct rb_node **link = &root->rb_root.rb_node, *parent = 0;
    int leftmost = 1;
    while (*link) {
        parent = *link;
        struct anode *a = rb_entry(parent, struct anode, rb);
        if (key < a->key) { link = &parent->rb_left; }
        else              { link = &parent->rb_right; leftmost = 0; }
    }
    a_pool[idx].key = key;
    a_pool[idx].subtree_sum = key;          // leaf: own value (the contract)
    rb_link_node(&a_pool[idx].rb, parent, link);
    rb_insert_augmented_cached(&a_pool[idx].rb, root, leftmost, &A_CB);
    return leftmost;
}

static int aug_ok(struct rb_root_cached *root) {
    for (struct rb_node *n = rb_first(&root->rb_root); n; n = rb_next(n)) {
        if (rb_entry(n, struct anode, rb)->subtree_sum != brute_sum(n)) {
            return 0;
        }
    }
    return rb_first_cached(root) == rb_first(&root->rb_root);
}

static int augmented_phase(void) {
    struct rb_root_cached root = RB_ROOT_CACHED;

    for (int i = 0; i < POOL; i++) {
        a_insert(&root, prng(), i);
        a_in[i] = 1;
        if ((i & 0x1ff) == 0x1ff) {
            if (!invariants_ok(&root.rb_root)) { fail("augmented: rb invariant on insert"); return 0; }
            if (!aug_ok(&root))                { fail("augmented: subtree_sum or leftmost wrong on insert"); return 0; }
        }
    }
    if (!invariants_ok(&root.rb_root)) { fail("augmented: rb invariant after build"); return 0; }
    if (!aug_ok(&root))                { fail("augmented: subtree_sum or leftmost wrong after build"); return 0; }

    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < POOL; i++) {
            if (a_in[i] && (prng() & 3) == 0) {
                rb_erase_augmented_cached(&a_pool[i].rb, &root, &A_CB);
                a_in[i] = 0;
            }
        }
        if (!invariants_ok(&root.rb_root)) { fail("augmented: rb invariant on erase"); return 0; }
        if (!aug_ok(&root))                { fail("augmented: subtree_sum or leftmost wrong on erase"); return 0; }
        for (int i = 0; i < POOL; i++) {
            if (!a_in[i] && (prng() & 1)) {
                a_insert(&root, prng(), i);
                a_in[i] = 1;
            }
        }
        if (!invariants_ok(&root.rb_root)) { fail("augmented: rb invariant on re-insert"); return 0; }
        if (!aug_ok(&root))                { fail("augmented: subtree_sum or leftmost wrong on re-insert"); return 0; }
    }

    for (int i = 0; i < POOL; i++) {
        if (a_in[i]) { rb_erase_augmented_cached(&a_pool[i].rb, &root, &A_CB); a_in[i] = 0; }
    }
    if (!RB_EMPTY_ROOT(&root.rb_root)) { fail("augmented: not empty after erasing all"); return 0; }
    if (rb_first_cached(&root) != 0)   { fail("augmented: cached leftmost not cleared"); return 0; }
    return 1;
}

void rbtree_selftest(void) {
    prng_state = 0x2545F4914F6CDD1DULL;
    if (!plain_phase())     { return; }
    if (!augmented_phase()) { return; }
    serial_write_string("[rbtree] selftest passed\n");
}
