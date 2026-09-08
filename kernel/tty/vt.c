#include "tty/vt.h"
#include "tty/kvt.h"
#include "tty/tty.h"
#include "tty/con_driver.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "mm/heap.h"
#include "smp/smp.h"
#include "errno.h"

struct vt_console {
    struct tty tty;
    struct kvt scr;
    int kd_mode;                       // KD_TEXT or KD_GRAPHICS
    int kd_owner_pid;                  // who claimed it; 0 when KD_TEXT
    struct waitq wait_active;          // VT_WAITACTIVE sleepers
    struct vc_cell shown[VC_MAX_ROWS][VC_MAX_COLS];   // last painted (diff)
    int shown_valid;
};

static struct vt_console vts[VT_COUNT];
// vt_active is volatile and read without vt_lock by vt_active_tty,
// vt_tty and vt_active_index on purpose: those resolve "whichever VT is
// active right now", which is inherently a snapshot. An aligned int
// cannot tear, so the worst case is routing to the VT that was active a
// moment ago -- which is what /dev/tty0 means anyway.
static volatile int vt_active;
static int g_cols, g_rows;

// CS0. Guards vt_active, each VT's diff cache (shown / shown_valid) and
// kd_mode. Rank VT sits above TTY and below FBCON: the write path
// arrives here already holding t->lock (tty_obj_write -> backend
// output) and calls into the con_driver underneath, so the chain
// TTY -> VT -> FBCON is ascending on every path. vt_switch and
// vt_scroll come from the keyboard IRQ holding nothing and take
// t->lock first, in that same order.
static struct spinlock vt_lock;

// One-way, set by the panic path: it may hold any lock, so it takes
// none. Same reasoning as fbcon_panicking.
static volatile int vt_panicking;

// pack a cell to the con_driver attr byte (fg | bg<<4), honouring reverse
static uint8_t pack(const struct vc_cell *c) {
    uint8_t fg = c->fg & 15, bg = c->bg & 15;
    if (c->attr & VC_REVERSE) { uint8_t t = fg; fg = bg; bg = t; }
    return (uint8_t)(fg | (bg << 4));
}

// CS0 race detector. render_diff must only ever paint the VT that is
// active *at the moment it paints*: vt_backend_output guards on it,
// vt_switch sets vt_active=n before rendering vts[n], vt_scroll and
// KDSETMODE render only the active one, and vt_panic_reset sets
// vt_active=0 first. So a non-zero count here means a switch landed
// between one of those guards and the paint -- which is the bug: the
// screen now belongs to another VT and we painted this one's cells
// onto it, while marking them clean in this VT's diff cache, so they
// are never repainted.
static volatile uint64_t vt_race_hits;

// Callers must hold vt_lock, or be the panic path.
static void render_diff_locked(struct vt_console *vc) {
    struct con_driver *cd = con_driver_active();
    if (!cd || !cd->putc_at) { return; }
    if (vc != &vts[vt_active]) { vt_race_hits++; }
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            const struct vc_cell *cell = kvt_cell(&vc->scr, r, c);
            struct vc_cell *prev = &vc->shown[r][c];
            if (!vc->shown_valid ||
                cell->ch != prev->ch || cell->fg != prev->fg ||
                cell->bg != prev->bg || cell->attr != prev->attr) {
                cd->putc_at(r, c, (char)cell->ch, pack(cell));
                *prev = *cell;
            }
        }
    }
    // Checked again on the way out: the switch that breaks this can land
    // anywhere in the loop above, so the exit check has a window
    // thousands of cells wide where the entry check has only a few
    // instructions.
    if (vc != &vts[vt_active]) { vt_race_hits++; }
    vc->shown_valid = 1;
    int x, y, vis;
    kvt_cursor(&vc->scr, &x, &y, &vis);
    if (cd->cursor) { cd->cursor(y, x, vis); }
}

static void render_full_locked(struct vt_console *vc) {
    vc->shown_valid = 0;
    render_diff_locked(vc);
}

// --- tty backend: cooked output feeds the parser ---------------------

static uint32_t vt_backend_output(struct tty *t, const char *s, uint32_t n) {
    struct vt_console *vc = t->backend_priv;
    kvt_feed(&vc->scr, s, n);                  // under t->lock already
    uint64_t f = vt_panicking ? 0 : spin_lock_irqsave(&vt_lock);
    if (vc == &vts[vt_active]) {
        serial_write_raw_n(s, n);              // the visible VT mirrors to serial
        if (vc->kd_mode == KD_TEXT) { render_diff_locked(vc); }
    }
    if (!vt_panicking) { spin_unlock_irqrestore(&vt_lock, f); }
    return n;   // the screen has no bounded queue: it always takes all of it
}// The screen has no bounded queue: it always takes everything offered.
static uint32_t vt_backend_space(struct tty *t) { (void)t; return 0xFFFFFFFFu; }

static const struct tty_backend vt_backend = { vt_backend_output, vt_backend_space };

// --- public ------------------------------------------------------

void vt_init(void) {
    spin_init(&vt_lock, LOCK_RANK_VT, "vt");
    con_driver_geometry(&g_cols, &g_rows);
    if (g_cols <= 0) { g_cols = 80; }
    if (g_rows <= 0) { g_rows = 25; }
    if (g_cols > VC_MAX_COLS) { g_cols = VC_MAX_COLS; }
    if (g_rows > VC_MAX_ROWS) { g_rows = VC_MAX_ROWS; }

    for (int i = 0; i < VT_COUNT; i++) {
        kvt_init(&vts[i].scr, g_cols, g_rows);
        vts[i].kd_mode = KD_TEXT;
        vts[i].shown_valid = 0;
        waitq_init(&vts[i].wait_active);
        tty_obj_init(&vts[i].tty, &vt_backend, &vts[i]);
        vts[i].tty.win.ws_col = (uint16_t)g_cols;
        vts[i].tty.win.ws_row = (uint16_t)g_rows;
    }
    vt_active = 0;
    serial_write_string("[vt] virtual terminals ready\n");
}

struct tty *vt_active_tty(void)   { return &vts[vt_active].tty; }
int  vt_active_index(void)        { return vt_active; }
void vt_active_geometry(int *cols, int *rows) {
    if (cols) { *cols = g_cols; }
    if (rows) { *rows = g_rows; }
}

struct tty *vt_tty(int vt_index) {
    if (vt_index <= 0) { return &vts[vt_active].tty; }
    if (vt_index > VT_COUNT) { return 0; }
    return &vts[vt_index - 1].tty;
}

void vt_switch(int n) {
    if (n < 0 || n >= VT_COUNT) { return; }
    // The target VT's tty lock first, so render_full_locked's read of
    // vts[n].scr cannot race a kvt_feed on the same VT, then vt_lock.
    struct tty *t = &vts[n].tty;
    uint64_t tf = spin_lock_irqsave(&t->lock);
    uint64_t f  = spin_lock_irqsave(&vt_lock);
    // Tested under the lock: read outside it and two CPUs can both
    // decide they are the one switching.
    if (n != vt_active) {
        vt_active = n;
        struct con_driver *cd = con_driver_active();
        if (cd && cd->clear) { cd->clear(); }
        if (vts[n].kd_mode == KD_TEXT) { render_full_locked(&vts[n]); }
    }
    spin_unlock_irqrestore(&vt_lock, f);
    spin_unlock_irqrestore(&t->lock, tf);
    // Outside both: WAITQ ranks below VT, so waking under vt_lock would
    // be a descending acquire.
    waitq_wake_all(&vts[n].wait_active);
}

void vt_scroll(int delta_lines) {
    uint64_t f = spin_lock_irqsave(&vt_lock);
    int n = vt_active;
    spin_unlock_irqrestore(&vt_lock, f);

    // kvt_scroll_view mutates the same struct kvt that kvt_feed does,
    // and that is protected by t->lock -- so the scroll needs it too.
    struct tty *t = &vts[n].tty;
    uint64_t tf = spin_lock_irqsave(&t->lock);
    f = spin_lock_irqsave(&vt_lock);
    if (n == vt_active) {                    // still current after the re-lock
        kvt_scroll_view(&vts[n].scr, delta_lines);
        render_full_locked(&vts[n]);
    }
    spin_unlock_irqrestore(&vt_lock, f);
    spin_unlock_irqrestore(&t->lock, tf);
}

void vt_write_active(const char *s, unsigned n) {
    tty_obj_write(&vts[vt_active].tty, s, n);
}

// Set by the exception handler, never by vt_panic_reset itself --
// vt_selftest calls that reset in ordinary context, and latching the
// flag there would silently disable VT locking for the rest of the boot.
void vt_enter_panic(void) { vt_panicking = 1; }

void vt_panic_reset(void) {
    // Locked normally, EXCEPT on the panic path: a panicking CPU may
    // hold any lock, and a checked acquire would call lock_panic() from
    // inside a panic and recurse. fbcon splits this the same way.
    uint64_t tf = 0, f = 0;
    if (!vt_panicking) {
        tf = spin_lock_irqsave(&vts[0].tty.lock);
        f  = spin_lock_irqsave(&vt_lock);
    }
    vt_active = 0;
    vts[0].kd_mode = KD_TEXT;
    struct con_driver *cd = con_driver_active();
    if (cd && cd->clear) { cd->clear(); }
    render_full_locked(&vts[0]);
    if (!vt_panicking) {
        spin_unlock_irqrestore(&vt_lock, f);
        spin_unlock_irqrestore(&vts[0].tty.lock, tf);
    }
}

int64_t vt_ioctl(int vt_index, uint64_t request, void *arg) {
    int idx = (vt_index <= 0) ? vt_active : (vt_index - 1);
    if (idx < 0 || idx >= VT_COUNT) { return -EINVAL; }
    long a = (long)(intptr_t)arg;

    switch (request) {
    case VT_ACTIVATE:
        if (a < 1 || a > VT_COUNT) { return -EINVAL; }
        vt_switch((int)a - 1);
        return 0;
    case VT_WAITACTIVE:
        if (a < 1 || a > VT_COUNT) { return -EINVAL; }
        while (vt_active != (int)a - 1) {
            waitq_sleep(&vts[a - 1].wait_active, 0);
        }
        return 0;
    case VT_GETSTATE: {
        if (!arg) { return -EFAULT; }
        struct vt_stat *st = arg;
        st->v_active = (uint16_t)(vt_active + 1);
        st->v_signal = 0;
        st->v_state  = (uint16_t)(((1u << VT_COUNT) - 1) << 1);   // VTs 1..N allocated
        return 0;
    }
    case VT_OPENQRY:
        if (!arg) { return -EFAULT; }
        *(int *)arg = vt_active + 1;    // no free-VT pool; the active one
        return 0;
    case VT_GETMODE:
    case VT_SETMODE:
    case VT_RELDISP:
        return 0;                       // accepted, inert until M1c-4
    case KDSETMODE: {
        uint64_t f = spin_lock_irqsave(&vt_lock);
        vts[idx].kd_mode = (a == KD_GRAPHICS) ? KD_GRAPHICS : KD_TEXT;
        if (idx == vt_active && vts[idx].kd_mode == KD_TEXT) {
            render_full_locked(&vts[idx]);
        }
        spin_unlock_irqrestore(&vt_lock, f);
        return 0;
    }
    case KDGETMODE: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&vt_lock);
        *(int *)arg = vts[idx].kd_mode;
        spin_unlock_irqrestore(&vt_lock, f);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

// --- /dev/tty0 .. /dev/ttyN -------------------------------------------
//
// One set of ops for all seven nodes; the node's VT index is in
// f->priv, put there by devfs's vt_dev_open (which is the only place
// that knows the device names). Index 0 is /dev/tty0 -- "whatever is
// active right now", re-resolved on every call rather than bound at
// open, exactly like Linux's.
//
// Only ioctl differs from /dev/console: the VT_*/KD* set is tried
// first, and vt_ioctl returns -ENOTTY for anything outside it so the
// ordinary terminal ioctls (TCGETS, TIOCGWINSZ, ...) still work on the
// same fd.

// What a /dev/ttyN fd's priv points at. It was a bare int cast through
// a pointer, which was enough while nothing had to be undone on the way
// out. It is not enough now: a process that puts its VT into
// KD_GRAPHICS and then dies must not leave the machine with no console,
// so the fd has to remember that IT made the claim, and the claim has
// to outlive dup/fork -- hence the refcount.
struct vt_fd {
    int index;          // 0 == "whichever VT is active"
    int claimed;        // this fd put its VT into KD_GRAPHICS
    int refs;           // dup/fork share one vt_fd
};

// devfs is the only place that knows the device names, so it opens
// these; vt.c owns the layout.
void *vt_fd_new(int index) {
    struct vt_fd *v = kmalloc(sizeof(struct vt_fd));
    if (!v) { return 0; }
    v->index = index;
    v->claimed = 0;
    v->refs = 1;
    return v;
}

int vt_selftest_kd_mode(int vt_index) {
    if (vt_index < 0 || vt_index >= VT_COUNT) { return -1; }
    uint64_t f = spin_lock_irqsave(&vt_lock);
    int m = vts[vt_index].kd_mode;
    spin_unlock_irqrestore(&vt_lock, f);
    return m;
}

// May the CALLING process paint the framebuffer right now?
//
// The rule is about the caller's own claim, not merely about whether
// anyone is claiming:
//
//   - A process that has claimed a VT with KDSETMODE(KD_GRAPHICS) may
//     paint only while that VT is the one on display. This is the case
//     that matters: a compositor on VT 0 must go quiet the instant the
//     user hits Alt+F2, or it paints over VT 1's console.
//   - A process that has claimed nothing may paint only while nobody
//     else owns the screen. That keeps existing framebuffer programs
//     working -- neoos-tinygl opens and mmaps /dev/fb0 without ever
//     calling KDSETMODE -- while still shutting them out the moment a
//     compositor takes over.
//
// LIMITATION, deliberate and recorded in docs/stdlib.md: this gates the
// mmap CALL, not subsequent faults on an established mapping. A process
// that mapped the framebuffer while it owned the screen keeps a usable
// mapping across a VT switch. Revoking that needs the switch path to
// unmap every fb mapping, which is a bigger change than this milestone
// wants; the compositor cooperates by stopping on a Focus event.
// Split from vt_process_owns_screen so the policy can be tested with a
// pid of the test's choosing: the interesting cases are "a DIFFERENT
// process owns the screen" and "the owner is on a VT that is not on
// display", and boot-time selftests run with no current_proc at all.
int vt_screen_gate_check(int pid) {
    uint64_t fl = spin_lock_irqsave(&vt_lock);
    int owned_by_caller = -1;
    for (int i = 0; i < VT_COUNT; i++) {
        if (vts[i].kd_mode == KD_GRAPHICS && vts[i].kd_owner_pid == pid && pid != 0) {
            owned_by_caller = i;
            break;
        }
    }
    int active = vt_active;
    int active_is_claimed = (vts[active].kd_mode == KD_GRAPHICS);
    spin_unlock_irqrestore(&vt_lock, fl);

    if (owned_by_caller >= 0) { return owned_by_caller == active; }
    return !active_is_claimed;
}

int vt_process_owns_screen(void) {
    struct process *p = current_proc();
    return vt_screen_gate_check(p ? p->pid : 0);
}

static int vt_index_of(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    return v ? v->index : 0;
}

static struct tty *vt_of(struct file_descriptor *f) {
    return vt_tty(vt_index_of(f));
}

static int64_t vt_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct tty *t = vt_of(f);
    if (!t) { return -ENODEV; }
    return tty_obj_read(t, buf, (uint32_t)len, f->nonblock);
}

static int64_t vt_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct tty *t = vt_of(f);
    if (!t) { return -ENODEV; }
    return tty_obj_write(t, buf, (uint32_t)len);
}

static int64_t vt_fop_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return -ESPIPE;
}

static int64_t vt_fop_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f; (void)buf; (void)bytes;
    return -ENOTDIR;
}

static int64_t vt_fop_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    int64_t rc = vt_ioctl(vt_index_of(f), request, arg);
    if (rc != -ENOTTY) {
        // Remember which fd claimed the screen, so releasing it can hand
        // the screen back even if the process never got the chance to.
        if (request == KDSETMODE && rc == 0 && f->priv) {
            struct vt_fd *v = f->priv;
            v->claimed = ((long)(intptr_t)arg == KD_GRAPHICS);
            int idx = v->index ? v->index - 1 : vt_active;
            if (idx >= 0 && idx < VT_COUNT) {
                struct process *p = current_proc();
                uint64_t fl = spin_lock_irqsave(&vt_lock);
                vts[idx].kd_owner_pid = v->claimed && p ? p->pid : 0;
                spin_unlock_irqrestore(&vt_lock, fl);
            }
        }
        return rc;
    }
    struct tty *t = vt_of(f);
    if (!t) { return -ENODEV; }
    return tty_obj_ioctl(t, request, arg);
}

static struct poll_head *vt_fop_poll_head(struct file_descriptor *f) {
    struct tty *t = vt_of(f);
    return t ? &t->poll : 0;
}

static int vt_fop_poll(struct file_descriptor *f, int events) {
    struct tty *t = vt_of(f);
    if (!t) { return 0; }
    return tty_obj_poll(t, events);
}

static void vt_fop_dup(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    if (v) { __atomic_add_fetch(&v->refs, 1, __ATOMIC_SEQ_CST); }
}

static void vt_fop_close(struct file_descriptor *f) {
    struct vt_fd *v = f->priv;
    if (!v) { return; }
    if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_SEQ_CST) != 0) { return; }

    // Last reference. If this fd claimed graphics mode, hand the screen
    // back. This runs on the exit path of a process that crashed just as
    // much as one that tidied up, which is the whole point: nothing here
    // may depend on the application having asked.
    if (v->claimed) {
        int idx = v->index ? v->index - 1 : vt_active;
        if (idx >= 0 && idx < VT_COUNT) {
            uint64_t fl = spin_lock_irqsave(&vt_lock);
            vts[idx].kd_mode = KD_TEXT;
            vts[idx].kd_owner_pid = 0;
            if (idx == vt_active) { render_full_locked(&vts[idx]); }
            spin_unlock_irqrestore(&vt_lock, fl);
        }
    }
    f->priv = 0;
    kfree(v);
}

const struct file_ops vt_file_ops = {
    .name     = "vt",
    .read     = vt_fop_read,
    .write    = vt_fop_write,
    .lseek    = vt_fop_lseek,
    .getdents = vt_fop_getdents,
    .ioctl    = vt_fop_ioctl,
    .poll     = vt_fop_poll,
    .poll_head = vt_fop_poll_head,
    .dup      = vt_fop_dup,
    .close    = vt_fop_close,
};

// CS0: the switch-vs-write race. A background kernel thread flips
// between VT 0 and VT 1 while the caller writes to VT 0. Without a
// lock, render_full (from the switch) and render_diff (from the write)
// interleave on vts[0].shown, and shown_valid ends up 1 with cells that
// were never painted -- so a later diff skips them forever. The check
// is that the diff cache agrees with the grid once both settle.
static volatile int vtstress_run;
static volatile int vtstress_done;

static void vt_stress_thread(void) {
    while (vtstress_run) {
        vt_switch(1);
        vt_switch(0);
    }
    vtstress_done = 1;
    thread_exit_self(0);
}

// Runs AFTER smp_start_aps(): the switcher must land on a real second
// core, and vt_selftest() runs long before the APs exist.
void vt_stress_selftest(void) {
    if (smp_online_count() < 2) {
        serial_write_string("[vt] stress SKIPPED: single CPU\n");
        return;
    }
    vtstress_run = 1;
    vtstress_done = 0;
    vt_race_hits = 0;
    vt_switch(0);
    // _on(cpu 1): the race needs the switcher on a DIFFERENT core from
    // the writer.
    if (!thread_alloc_kernel_on(vt_stress_thread, 1)) {
        serial_write_string("[vt] stress SKIPPED: no thread\n");
        return;
    }
    for (int i = 0; i < 2000; i++) {
        tty_obj_write(&vts[0].tty, "x", 1);
    }
    vtstress_run = 0;
    while (!vtstress_done) { __asm__ volatile("pause"); }
    vt_switch(0);

    // The writer's 2000 characters mirrored to serial (the VT was
    // active, which is the point). Break the line so the result is
    // greppable.
    serial_write_string("\n");
    if (vt_race_hits) {
        serial_write_string("[vt] stress FAILED: painted a background VT, hits=");
        serial_write_hex64(vt_race_hits);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[vt] stress passed\n");
}

// A VT left in KD_GRAPHICS by a process that died must come back to
// KD_TEXT when the last fd that claimed it is released. Otherwise a
// crashed graphical app leaves the machine with no console, and the
// only way out is a reboot.
//
// VT 1 (the second one) is used throughout: it is not the active VT, so
// nothing here repaints the screen the test is running on.
static void vt_kd_release_selftest(void) {
    struct file_descriptor f = { 0 };
    struct file_descriptor a = { 0 }, b = { 0 };
    const int devidx = 2;              // /dev/tty2 -> vts[1]
    const int vtidx  = 1;

    f.priv = vt_fd_new(devidx);
    if (!f.priv) {
        serial_write_string("[vt] kd-release selftest FAILED: alloc\n");
        return;
    }

    if (vt_file_ops.ioctl(&f, KDSETMODE, (void *)(long)KD_GRAPHICS) != 0 ||
        vt_selftest_kd_mode(vtidx) != KD_GRAPHICS) {
        serial_write_string("[vt] kd-release selftest FAILED: set\n");
        return;
    }

    // Releasing the claiming fd restores text mode -- this is the path a
    // crashing process takes, so it must not depend on the process
    // having done anything on the way out.
    vt_file_ops.close(&f);
    if (vt_selftest_kd_mode(vtidx) != KD_TEXT) {
        serial_write_string("[vt] kd-release selftest FAILED: not restored\n");
        vts[vtidx].kd_mode = KD_TEXT;              // do not poison the rest of boot
        return;
    }

    // A dup'd fd holds the claim until the LAST reference goes.
    a.priv = vt_fd_new(devidx);
    if (!a.priv) {
        serial_write_string("[vt] kd-release selftest FAILED: alloc2\n");
        return;
    }
    vt_file_ops.ioctl(&a, KDSETMODE, (void *)(long)KD_GRAPHICS);
    b.priv = a.priv;
    vt_file_ops.dup(&b);
    vt_file_ops.close(&a);
    if (vt_selftest_kd_mode(vtidx) != KD_GRAPHICS) {
        serial_write_string("[vt] kd-release selftest FAILED: dup released early\n");
        vts[vtidx].kd_mode = KD_TEXT;
        return;
    }
    vt_file_ops.close(&b);
    if (vt_selftest_kd_mode(vtidx) != KD_TEXT) {
        serial_write_string("[vt] kd-release selftest FAILED: dup never released\n");
        vts[vtidx].kd_mode = KD_TEXT;
        return;
    }

    // An fd that never claimed graphics mode must not restore anything
    // on close -- otherwise any process closing /dev/ttyN would yank the
    // screen out from under whoever legitimately owns it.
    vts[vtidx].kd_mode = KD_GRAPHICS;
    struct file_descriptor bystander = { 0 };
    bystander.priv = vt_fd_new(devidx);
    vt_file_ops.close(&bystander);
    if (vt_selftest_kd_mode(vtidx) != KD_GRAPHICS) {
        serial_write_string("[vt] kd-release selftest FAILED: bystander stole the release\n");
        vts[vtidx].kd_mode = KD_TEXT;
        return;
    }
    vts[vtidx].kd_mode = KD_TEXT;

    serial_write_string("[vt] kd-release selftest passed\n");
}

// The screen gate: who may paint /dev/fb0, and when.
//
// Poking kd_owner_pid directly is the point -- the interesting cases
// are "a DIFFERENT process owns the screen" and "the owner is on a VT
// that is not on display", and neither can be reached from a single
// kernel thread through the ioctl path.
static void vt_screen_gate_selftest(void) {
    const int me = 4242;               // synthetic: boot has no current_proc
    int saved = vt_active;
    const char *why = 0;

    vt_switch(0);

    // Nobody has claimed anything: an unclaimed caller may paint. This
    // is what keeps neoos-tinygl, which never calls KDSETMODE, working.
    if (!vt_screen_gate_check(me)) { why = "unclaimed screen refused"; goto done; }

    // We own the active VT.
    vts[0].kd_mode = KD_GRAPHICS;
    vts[0].kd_owner_pid = me;
    if (!vt_screen_gate_check(me)) { why = "owner on the active VT refused"; goto done; }

    // Still ours, but the user switched away. This is the case the gate
    // exists for: a compositor must go quiet on Alt+F2.
    vt_switch(1);
    if (vt_screen_gate_check(me)) { why = "owner painted from a background VT"; goto done; }

    // Someone else owns the VT on display; we claimed nothing.
    vt_switch(0);
    vts[0].kd_owner_pid = me + 1000;
    if (vt_screen_gate_check(me)) { why = "painted over another process's screen"; goto done; }

done:
    vts[0].kd_mode = KD_TEXT;
    vts[0].kd_owner_pid = 0;
    vt_switch(saved);

    if (why) {
        serial_write_string("[vt] screen-gate selftest FAILED: ");
        serial_write_string(why);
        serial_write_string("\n");
    } else {
        serial_write_string("[vt] screen-gate selftest passed\n");
    }
}

void vt_selftest(void) {
    int fail = 0;

    // a write to a background VT updates its grid, paints nothing
    vt_switch(0);
    vts[2].shown_valid = 0;
    tty_obj_write(&vts[2].tty, "probe\n", 6);
    if (kvt_cell(&vts[2].scr, 0, 0)->ch != 'p') { fail = 1; }
    if (vts[2].shown_valid) { fail = 1; }   // never rendered

    vt_switch(2);
    if (vt_active != 2 || !vts[2].shown_valid) { fail = 1; }
    vt_switch(0);
    vt_panic_reset();
    if (vt_active != 0) { fail = 1; }

    serial_write_string(fail ? "[vt] selftest FAILED\n" : "[vt] selftest passed\n");

    vt_kd_release_selftest();
    vt_screen_gate_selftest();
}
