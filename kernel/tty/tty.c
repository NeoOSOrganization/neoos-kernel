#include "tty/tty.h"
#include "drivers/char/serial.h"
#include "tty/console.h"
#include "tty/vt.h"
#include "fs/file.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "sched/proc.h"
#include "ipc/signal.h"
#include "errno.h"

// The terminal: a line discipline between an input source (the keyboard,
// or a pty master's write) and a reading process.
//
// Everything that makes a terminal a terminal rather than a byte pipe
// lives here -- canonical line assembly with editing, echo, the signal
// characters, and the output translation that turns "\n" into "\r\n".
// struct tty is now an allocatable object (see tty.h); the console is
// one instance, a pty allocates more.

// The console is now the active virtual terminal (kernel/tty/vt.c).
struct tty *tty_console(void) { return vt_active_tty(); }

// Cooked keyboard input is delivered here (see input.c). NULL means
// "the active VT"; a pty master points this at its slave (M1c-4).
static struct tty *active_input_tty;

void tty_set_active(struct tty *t) { active_input_tty = t; }
struct tty *tty_active(void) {
    return active_input_tty ? active_input_tty : vt_active_tty();
}

static void tty_set_defaults(struct tty *t) {
    struct termios_k *o = &t->tio;
    for (unsigned i = 0; i < sizeof(*o); i++) { ((uint8_t *)o)[i] = 0; }

    // The settings a Linux console comes up with: canonical, echoing,
    // signal-generating, CR translated to NL on input and NL expanded
    // to CRNL on output.
    o->c_iflag = ICRNL;
    o->c_oflag = OPOST | ONLCR;
    o->c_cflag = B38400 | CS8 | CREAD | CLOCAL;
    o->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK | IEXTEN;

    o->c_cc[VINTR]  = 3;    // ^C
    o->c_cc[VQUIT]  = 28;   // ctrl-backslash
    o->c_cc[VERASE] = 127;  // DEL
    o->c_cc[VKILL]  = 21;   // ^U
    o->c_cc[VEOF]   = 4;    // ^D
    o->c_cc[VSUSP]  = 26;   // ^Z
    o->c_cc[VSTART] = 17;   // ^Q
    o->c_cc[VSTOP]  = 19;   // ^S
    o->c_cc[VMIN]   = 1;
    o->c_cc[VTIME]  = 0;

    // 80x25 is what the VGA text console actually is. Reporting a real
    // size rather than zeroes is what makes isatty() and every "how
    // wide is my terminal" check give a usable answer.
    t->win.ws_row = 25;
    t->win.ws_col = 80;
}

void tty_obj_init(struct tty *t, const struct tty_backend *b, void *priv) {
    for (unsigned i = 0; i < sizeof(*t); i++) { ((uint8_t *)t)[i] = 0; }
    spin_init(&t->lock, LOCK_RANK_TTY, priv ? "tty-pts" : "tty-con");
    waitq_init(&t->readers);
    waitq_init(&t->out_readers);
    poll_head_init(&t->poll, "tty-poll");
    t->backend = b;
    t->backend_priv = priv;
    tty_set_defaults(t);
}

// CS5.2. Every readiness change on a tty goes through one of these two,
// so the poll head is notified everywhere the waitq is -- and only this
// tty's pollers are woken, rather than every poller in the system.
void tty_wake_readers(struct tty *t) {
    waitq_wake_all(&t->readers);
    poll_head_notify(&t->poll);
}

void tty_wake_out_readers(struct tty *t) {
    waitq_wake_all(&t->out_readers);
    poll_head_notify(&t->poll);
}

void tty_init(void) {
    vt_init();
    serial_write_string("[tty] console line discipline ready\n");
}

// ---- output ---------------------------------------------------------

// Cooked output: expand LF -> CRLF when OPOST|ONLCR, then one call into
// the backend so a userland write cannot interleave with kernel output.
static void tty_out_cooked(struct tty *t, const char *s, uint32_t n) {
    int post = (t->tio.c_oflag & OPOST) && (t->tio.c_oflag & ONLCR);
    if (!post) { t->backend->output(t, s, n); return; }
    char buf[64];
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (k >= sizeof(buf) - 2) { t->backend->output(t, buf, k); k = 0; }
        if (s[i] == '\n') { buf[k++] = '\r'; }
        buf[k++] = s[i];
    }
    if (k) { t->backend->output(t, buf, k); }
}

static void tty_echo(struct tty *t, char c) {
    tty_out_cooked(t, &c, 1);
}

int64_t tty_obj_write(struct tty *t, const void *buf, uint32_t len) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    tty_out_cooked(t, (const char *)buf, len);
    spin_unlock_irqrestore(&t->lock, f);
    // A pty backend just appended to outq under the lock; wake the
    // master reader now that it is dropped. Harmless for the console
    // (out_readers is always empty there).
    (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
    return (int64_t)len;
}

// ---- input ----------------------------------------------------------

static void ready_push(struct tty *t, char c) {
    if (t->ready_len >= TTY_BUF) { return; }   // overflow: drop, as Linux does
    uint32_t idx = (t->ready_head + t->ready_len) % TTY_BUF;
    t->ready[idx] = c;
    t->ready_len++;
}

// Moves the completed line into the readable queue.
static void line_commit(struct tty *t) {
    for (uint32_t i = 0; i < t->edit_len; i++) { ready_push(t, t->edit[i]); }
    t->edit_len = 0;
}

void tty_input_char(struct tty *t, char c) {
    uint64_t f = spin_lock_irqsave(&t->lock);

    struct termios_k *o = &t->tio;

    if (o->c_iflag & ICRNL) { if (c == '\r') { c = '\n'; } }
    else if (o->c_iflag & IGNCR) { if (c == '\r') { spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0); return; } }
    if (o->c_iflag & INLCR) { if (c == '\n') { c = '\r'; } }

    // Signal characters are checked BEFORE the canonical/raw split:
    // ^C must work while a line is half-typed, which is the whole
    // point of it.
    if (o->c_lflag & ISIG) {
        int sig = 0;
        if (c == (char)o->c_cc[VINTR]) { sig = SIGINT; }
        else if (c == (char)o->c_cc[VQUIT]) { sig = SIGQUIT; }
        else if (c == (char)o->c_cc[VSUSP]) { sig = SIGTSTP; }
        if (sig) {
            int pgid = t->fg_pgid;
            // The half-typed line is discarded, as on Linux: what was
            // being typed was for the program just interrupted.
            t->edit_len = 0;
            if (o->c_lflag & ECHO) { tty_echo(t, '^'); tty_echo(t, (char)('@' + c)); tty_echo(t, '\n'); }
            spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
            // Signalling is done with the tty lock DROPPED: delivery
            // takes the proc_table bucket lock and per-process p->lock,
            // both of which rank above this one, and a wake can spin
            // waiting for a thread to leave its CPU.
            if (pgid > 0) { signal_kill(-pgid, sig, 0); }
            tty_wake_readers(t);
            return;
        }
    }

    if (!(o->c_lflag & ICANON)) {
        // Raw mode: every byte is immediately readable, no editing.
        ready_push(t, c);
        if (o->c_lflag & ECHO) { tty_echo(t, c); }
        spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
        tty_wake_readers(t);
        return;
    }

    // ---- canonical mode: assemble and edit a line ----
    if (c == (char)o->c_cc[VERASE] || c == '\b') {
        if (t->edit_len > 0) {
            t->edit_len--;
            // ECHOE: rub the character off the screen rather than
            // leaving it there with the cursor sitting on top.
            if (o->c_lflag & ECHOE) { tty_echo(t, '\b'); tty_echo(t, ' '); tty_echo(t, '\b'); }
        }
        spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
        return;
    }

    if (c == (char)o->c_cc[VKILL]) {
        if (o->c_lflag & ECHOK) {
            while (t->edit_len > 0) { tty_echo(t, '\b'); tty_echo(t, ' '); tty_echo(t, '\b'); t->edit_len--; }
        }
        t->edit_len = 0;
        spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
        return;
    }

    if (c == (char)o->c_cc[VEOF]) {
        // ^D commits what has been typed. On an EMPTY line that is a
        // zero-length commit, which is exactly how end-of-file reaches
        // a reader.
        if (t->edit_len == 0) { t->saw_eof = 1; }
        else { line_commit(t); }
        spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
        tty_wake_readers(t);
        return;
    }

    if (c == '\n') {
        if (t->edit_len < TTY_BUF) { t->edit[t->edit_len++] = c; }
        if (o->c_lflag & ECHO) { tty_echo(t, '\n'); }
        line_commit(t);
        spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
        tty_wake_readers(t);
        return;
    }

    if (t->edit_len < TTY_BUF) {
        t->edit[t->edit_len++] = c;
        if (o->c_lflag & ECHO) { tty_echo(t, c); }
    }
    spin_unlock_irqrestore(&t->lock, f); (t->backend_priv ? tty_wake_out_readers(t) : (void)0);
}

int64_t tty_obj_read(struct tty *t, void *buf, uint32_t len, int nonblock) {
    char *out = (char *)buf;
    if (len == 0) { return 0; }

    uint64_t f = spin_lock_irqsave(&t->lock);
    for (;;) {
        if (t->ready_len > 0) { break; }
        if (t->saw_eof) { t->saw_eof = 0; spin_unlock_irqrestore(&t->lock, f); return 0; }
        if (t->hung_up) { spin_unlock_irqrestore(&t->lock, f); return 0; }
        if (nonblock) { spin_unlock_irqrestore(&t->lock, f); return -EAGAIN; }

        // waitq_sleep drops t->lock, sleeps, and returns HOLDING it again
        // -- both on a normal wake and after a signal. So the loop does
        // NOT retake it; a -EINTR just needs one unlock. (Same shape as
        // pipe_read; the old console-only tty_read got this wrong and
        // re-locked, which was a latent self-deadlock a pty read hits.)
        int rc = waitq_sleep(&t->readers, &t->lock);
        if (rc != 0) { spin_unlock_irqrestore(&t->lock, f); return rc; }
    }

    uint32_t n = 0;
    while (n < len && t->ready_len > 0) {
        out[n++] = t->ready[t->ready_head];
        t->ready_head = (t->ready_head + 1) % TTY_BUF;
        t->ready_len--;
        // Canonical reads stop at the line terminator even if the
        // caller asked for more: that is what "line at a time" means.
        if ((t->tio.c_lflag & ICANON) && out[n - 1] == '\n') { break; }
    }
    spin_unlock_irqrestore(&t->lock, f);
    return (int64_t)n;
}

// ---- ioctl ----------------------------------------------------------

int64_t tty_obj_ioctl(struct tty *t, uint64_t request, void *arg) {

    switch (request) {
    case TCGETS: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&t->lock);
        struct termios_k copy = t->tio;
        spin_unlock_irqrestore(&t->lock, f);
        // Exactly sizeof(struct termios_k) bytes: userland's struct is
        // longer, and writing the whole of it would clobber fields
        // Linux leaves alone.
        *(struct termios_k *)arg = copy;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        if (!arg) { return -EFAULT; }
        struct termios_k in = *(const struct termios_k *)arg;
        uint64_t f = spin_lock_irqsave(&t->lock);
        t->tio = in;
        // TCSETSF discards unread input; TCSETSW would drain output,
        // which is instantaneous here.
        if (request == TCSETSF) {
            t->ready_len = 0; t->ready_head = 0; t->edit_len = 0;
        }
        spin_unlock_irqrestore(&t->lock, f);
        return 0;
    }
    case TIOCGWINSZ: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&t->lock);
        struct winsize_k copy = t->win;
        spin_unlock_irqrestore(&t->lock, f);
        *(struct winsize_k *)arg = copy;
        return 0;
    }
    case TIOCSWINSZ: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&t->lock);
        t->win = *(const struct winsize_k *)arg;
        spin_unlock_irqrestore(&t->lock, f);
        return 0;
    }
    case TIOCGPGRP: {
        if (!arg) { return -EFAULT; }
        uint64_t f = spin_lock_irqsave(&t->lock);
        int pg = t->fg_pgid;
        spin_unlock_irqrestore(&t->lock, f);
        // No foreground group set yet: report the caller's own, which
        // is what a session leader expects to see before it sets one.
        if (pg == 0) { struct process *p = current_proc(); pg = p ? p->pgid : 0; }
        *(int *)arg = pg;
        return 0;
    }
    case TIOCSCTTY: {
        // Make this tty the caller's controlling terminal. Only a
        // SESSION LEADER may, which is Linux's rule and the reason the
        // standard sequence is setsid() then TIOCSCTTY on the inherited
        // descriptor.
        //
        // Without this a shell could never take over a pty it inherited
        // across fork: pts_devfs_open records the session of whoever
        // opened the slave FIRST, which for the usual arrangement is the
        // parent that set the pty up, not the shell. BusyBox's ash then
        // saw tcgetpgrp() report a group that was not its own and
        // signalled itself with SIGTTIN until it stopped.
        struct process *p = current_proc();
        if (!p) { return -ESRCH; }
        if (p->pid != p->sid) { return -EPERM; }   // not a session leader
        uint64_t f = spin_lock_irqsave(&t->lock);
        t->sid     = p->sid;
        t->fg_pgid = p->pgid;
        spin_unlock_irqrestore(&t->lock, f);
        return 0;
    }
    case TIOCSPGRP: {
        if (!arg) { return -EFAULT; }
        int pg = *(const int *)arg;
        if (pg <= 0) { return -EINVAL; }
        uint64_t f = spin_lock_irqsave(&t->lock);
        t->fg_pgid = pg;
        spin_unlock_irqrestore(&t->lock, f);
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

int tty_obj_poll(struct tty *t, int events) {
    uint64_t f = spin_lock_irqsave(&t->lock);
    int mask = POLLOUT;
    if (t->ready_len > 0 || t->saw_eof) { mask |= POLLIN; }
    if (t->hung_up) { mask |= POLLIN | POLLHUP; }
    spin_unlock_irqrestore(&t->lock, f);
    return mask & events;
}

// Selftest helpers: track what characters were delivered to the TTY
// without consuming them (so a reader can still get them).

void tty_selftest_reset(void) {
    struct tty *t = tty_console();
    uint64_t f = spin_lock_irqsave(&t->lock);
    t->ready_len = 0;
    t->ready_head = 0;
    t->edit_len = 0;  // Also clear the edit buffer
    spin_unlock_irqrestore(&t->lock, f);
}

int tty_selftest_saw(char c) {
    struct tty *t = tty_console();
    uint64_t f = spin_lock_irqsave(&t->lock);
    int found = 0;

    // Check the ready buffer
    for (uint32_t i = 0; i < t->ready_len; i++) {
        uint32_t idx = (t->ready_head + i) % TTY_BUF;
        if (t->ready[idx] == c) {
            found = 1;
            break;
        }
    }

    // Also check the edit buffer (canonical mode characters waiting for line terminator)
    if (!found) {
        for (uint32_t i = 0; i < t->edit_len; i++) {
            if (t->edit[i] == c) {
                found = 1;
                break;
            }
        }
    }

    spin_unlock_irqrestore(&t->lock, f);
    return found;
}

void tty_selftest(void) {
    struct tty *t = tty_console();

    // The layout TCGETS moves is fixed by Linux and compiled into every
    // caller, so it is asserted rather than assumed.
    if (sizeof(struct termios_k) != 36) {
        serial_write_string("[tty] selftest FAILED: struct termios is not 36 bytes\n");
        return;
    }
    if (t->tio.c_cc[VINTR] != 3 || t->tio.c_cc[VEOF] != 4 || t->tio.c_cc[VERASE] != 127) {
        serial_write_string("[tty] selftest FAILED: control characters wrong\n");
        return;
    }
    if (!(t->tio.c_lflag & ICANON) || !(t->tio.c_lflag & ECHO) || !(t->tio.c_lflag & ISIG)) {
        serial_write_string("[tty] selftest FAILED: default line discipline wrong\n");
        return;
    }
    if (t->win.ws_row == 0 || t->win.ws_col == 0) {
        serial_write_string("[tty] selftest FAILED: window size not reported\n");
        return;
    }

    // Drive the line discipline directly: type a line with a mistake,
    // erase it, and check the reader sees the corrected line and
    // nothing before the terminator.
    uint64_t f = spin_lock_irqsave(&t->lock);
    int saved_lflag = (int)t->tio.c_lflag;
    t->tio.c_lflag &= ~(uint32_t)ECHO;   // no echo: this is a test, not output
    t->ready_len = 0; t->ready_head = 0; t->edit_len = 0; t->saw_eof = 0;
    spin_unlock_irqrestore(&t->lock, f);

    tty_input_char(t, 'h'); tty_input_char(t, 'i'); tty_input_char(t, 'x');
    tty_input_char(t, 127);            // erase the 'x'
    tty_input_char(t, '\n');

    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    int64_t n = tty_obj_read(t, buf, sizeof(buf), 0);

    f = spin_lock_irqsave(&t->lock);
    t->tio.c_lflag = (uint32_t)saved_lflag;
    spin_unlock_irqrestore(&t->lock, f);

    if (n != 3 || buf[0] != 'h' || buf[1] != 'i' || buf[2] != '\n') {
        serial_write_string("[tty] selftest FAILED: canonical editing, n=");
        serial_write_hex64((uint64_t)n);
        serial_write_string("\n");
        return;
    }
    serial_write_string("[tty] selftest passed\n");
}

// File operations for /dev/CONSOLE and /dev/TTY -- always the console tty.
static int64_t tty_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    return tty_obj_read(tty_console(), buf, (uint32_t)len, f->nonblock);
}

static int64_t tty_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    return tty_obj_write(tty_console(), buf, (uint32_t)len);
}

static int64_t tty_fop_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int64_t tty_fop_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f;
    (void)buf;
    (void)bytes;
    return -ENOTDIR;
}

static int64_t tty_fop_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f;
    return tty_obj_ioctl(tty_console(), request, arg);
}

// This file_ops is the CONSOLE's, and every fd on it refers to the one
// console tty -- tty_fop_poll ignores `f` for the same reason.
static struct poll_head *tty_fop_poll_head(struct file_descriptor *f) {
    (void)f;
    struct tty *t = tty_console();
    return t ? &t->poll : 0;
}

static int tty_fop_poll(struct file_descriptor *f, int events) {
    (void)f;
    return tty_obj_poll(tty_console(), events);
}

static void tty_fop_dup(struct file_descriptor *f) {
    (void)f;
    // TTY doesn't need special dup handling
}

static void tty_fop_close(struct file_descriptor *f) {
    (void)f;
    // TTY doesn't need special close handling
}

const struct file_ops tty_file_ops = {
    .name     = "tty",
    .read     = tty_fop_read,
    .write    = tty_fop_write,
    .lseek    = tty_fop_lseek,
    .getdents = tty_fop_getdents,
    .ioctl    = tty_fop_ioctl,
    .poll     = tty_fop_poll,
    .poll_head = tty_fop_poll_head,
    .dup      = tty_fop_dup,
    .close    = tty_fop_close,
};
