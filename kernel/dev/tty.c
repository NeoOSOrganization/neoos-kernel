#include "dev/tty.h"
#include "dev/serial.h"
#include "dev/console.h"
#include "fs/file.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "sched/proc.h"
#include "ipc/signal.h"
#include "errno.h"

// The terminal: a line discipline between the keyboard and a reading
// process.
//
// Everything that makes a terminal a terminal rather than a byte pipe
// lives here -- canonical line assembly with editing, echo, the signal
// characters, and the output translation that turns "\n" into "\r\n"
// for a device that would otherwise leave the cursor in column 40.

#define TTY_BUF 1024

struct tty {
    struct spinlock lock;       // guards everything below
    struct waitq    readers;

    // Raw bytes accepted but not yet part of a completed line. In
    // canonical mode a reader cannot see these: an unterminated line is
    // still being edited.
    char     edit[TTY_BUF];
    uint32_t edit_len;

    // Bytes a reader may take. In canonical mode a whole line is moved
    // here at once when the terminator arrives; in raw mode every byte
    // goes straight in.
    char     ready[TTY_BUF];
    uint32_t ready_head;
    uint32_t ready_len;

    int      saw_eof;           // VEOF on an empty line: read returns 0

    struct termios_k  tio;
    struct winsize_k  win;
    int      fg_pgid;           // foreground process group, 0 = none
};

static struct tty console_tty;

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

void tty_init(void) {
    spin_init(&console_tty.lock, LOCK_RANK_DRIVER, "tty");
    waitq_init(&console_tty.readers);
    console_tty.edit_len = 0;
    console_tty.ready_head = 0;
    console_tty.ready_len = 0;
    console_tty.saw_eof = 0;
    console_tty.fg_pgid = 0;
    tty_set_defaults(&console_tty);
    serial_write_string("[tty] console line discipline ready, 80x25\n");
}

// ---- output ---------------------------------------------------------

// One character out, with ONLCR applied. Goes through the LOCKED serial
// primitive: serial_lock is what stops a userland write from
// interleaving byte-for-byte with kernel serial output. It used to call
// the unlocked serial_putc directly, and ~1 boot in 10 a userland
// printf landed in the middle of a kernel line -- e.g. the
// "interrupts enabled, starting scheduler" marker split as
// "...enabled, sta[looper pid=7] tick\ning scheduler", which then made
// `make test` report a boot that had in fact completed.
// serial_write_string_n inserts the CR before a LF itself.
static void tty_emit(char c) {
    serial_write_string_n(&c, 1);
    console_putc(c);
}

int64_t tty_write(const void *buf, uint32_t len) {
    const char *s = (const char *)buf;
    uint64_t f = spin_lock_irqsave(&console_tty.lock);
    int post = (console_tty.tio.c_oflag & OPOST) && (console_tty.tio.c_oflag & ONLCR);
    spin_unlock_irqrestore(&console_tty.lock, f);

    // One locked serial call for the whole write either way, so a
    // userland printf cannot be split down the middle by kernel output
    // on another CPU. The cooked path expands LF -> CRLF; the raw path
    // (OPOST off) writes the bytes verbatim.
    if (post) {
        serial_write_string_n(s, len);
    } else {
        serial_write_raw_n(s, len);
    }
    for (uint32_t i = 0; i < len; i++) { console_putc(s[i]); }
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

void tty_input_char(char c) {
    struct tty *t = &console_tty;
    uint64_t f = spin_lock_irqsave(&t->lock);

    struct termios_k *o = &t->tio;

    if (o->c_iflag & ICRNL) { if (c == '\r') { c = '\n'; } }
    else if (o->c_iflag & IGNCR) { if (c == '\r') { spin_unlock_irqrestore(&t->lock, f); return; } }
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
            if (o->c_lflag & ECHO) { tty_emit('^'); tty_emit((char)('@' + c)); tty_emit('\n'); }
            spin_unlock_irqrestore(&t->lock, f);
            // Signalling is done with the tty lock DROPPED: delivery
            // takes the proc_table bucket lock and per-process p->lock,
            // both of which rank above this one, and a wake can spin
            // waiting for a thread to leave its CPU.
            if (pgid > 0) { signal_kill(-pgid, sig, 0); }
            waitq_wake_all(&t->readers);
            return;
        }
    }

    if (!(o->c_lflag & ICANON)) {
        // Raw mode: every byte is immediately readable, no editing.
        ready_push(t, c);
        if (o->c_lflag & ECHO) { tty_emit(c); }
        spin_unlock_irqrestore(&t->lock, f);
        waitq_wake_all(&t->readers);
        return;
    }

    // ---- canonical mode: assemble and edit a line ----
    if (c == (char)o->c_cc[VERASE] || c == '\b') {
        if (t->edit_len > 0) {
            t->edit_len--;
            // ECHOE: rub the character off the screen rather than
            // leaving it there with the cursor sitting on top.
            if (o->c_lflag & ECHOE) { tty_emit('\b'); tty_emit(' '); tty_emit('\b'); }
        }
        spin_unlock_irqrestore(&t->lock, f);
        return;
    }

    if (c == (char)o->c_cc[VKILL]) {
        if (o->c_lflag & ECHOK) {
            while (t->edit_len > 0) { tty_emit('\b'); tty_emit(' '); tty_emit('\b'); t->edit_len--; }
        }
        t->edit_len = 0;
        spin_unlock_irqrestore(&t->lock, f);
        return;
    }

    if (c == (char)o->c_cc[VEOF]) {
        // ^D commits what has been typed. On an EMPTY line that is a
        // zero-length commit, which is exactly how end-of-file reaches
        // a reader.
        if (t->edit_len == 0) { t->saw_eof = 1; }
        else { line_commit(t); }
        spin_unlock_irqrestore(&t->lock, f);
        waitq_wake_all(&t->readers);
        return;
    }

    if (c == '\n') {
        if (t->edit_len < TTY_BUF) { t->edit[t->edit_len++] = c; }
        if (o->c_lflag & ECHO) { tty_emit('\n'); }
        line_commit(t);
        spin_unlock_irqrestore(&t->lock, f);
        waitq_wake_all(&t->readers);
        return;
    }

    if (t->edit_len < TTY_BUF) {
        t->edit[t->edit_len++] = c;
        if (o->c_lflag & ECHO) { tty_emit(c); }
    }
    spin_unlock_irqrestore(&t->lock, f);
}

int64_t tty_read(void *buf, uint32_t len) {
    struct tty *t = &console_tty;
    char *out = (char *)buf;
    if (len == 0) { return 0; }

    uint64_t f = spin_lock_irqsave(&t->lock);
    for (;;) {
        if (t->ready_len > 0) { break; }
        if (t->saw_eof) { t->saw_eof = 0; spin_unlock_irqrestore(&t->lock, f); return 0; }

        // Nothing to read: block. waitq_sleep releases the lock for us
        // and returns holding nothing, which is why the loop retakes it.
        int rc = waitq_sleep(&t->readers, &t->lock);
        if (rc != 0) { return rc; }   // interrupted by a signal -> -EINTR
        f = spin_lock_irqsave(&t->lock);
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

int64_t tty_ioctl(uint64_t request, void *arg) {
    struct tty *t = &console_tty;

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

// Selftest helpers: track what characters were delivered to the TTY
// without consuming them (so a reader can still get them).

void tty_selftest_reset(void) {
    struct tty *t = &console_tty;
    uint64_t f = spin_lock_irqsave(&t->lock);
    t->ready_len = 0;
    t->ready_head = 0;
    t->edit_len = 0;  // Also clear the edit buffer
    spin_unlock_irqrestore(&t->lock, f);
}

int tty_selftest_saw(char c) {
    struct tty *t = &console_tty;
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
    struct tty *t = &console_tty;

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
    if (t->win.ws_row != 25 || t->win.ws_col != 80) {
        serial_write_string("[tty] selftest FAILED: window size wrong\n");
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

    tty_input_char('h'); tty_input_char('i'); tty_input_char('x');
    tty_input_char(127);            // erase the 'x'
    tty_input_char('\n');

    char buf[16];
    for (int i = 0; i < 16; i++) { buf[i] = 0; }
    int64_t n = tty_read(buf, sizeof(buf));

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

// File operations for /dev/CONSOLE and /dev/TTY
static int64_t tty_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    (void)f;
    return tty_read(buf, (uint32_t)len);
}

static int64_t tty_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    return tty_write(buf, (uint32_t)len);
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
    return tty_ioctl(request, arg);
}

static int tty_fop_poll(struct file_descriptor *f, int events) {
    (void)f;
    // For now, always return POLLIN | POLLOUT
    // A real implementation would check if a line is ready for reading
    return (events & (POLLIN | POLLOUT));
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
    .dup      = tty_fop_dup,
    .close    = tty_fop_close,
};
