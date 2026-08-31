#ifndef NEOOS_TTY_H
#define NEOOS_TTY_H

#include <stdint.h>

// Linux's KERNEL `struct termios` -- the one TCGETS/TCSETS move, which
// is 36 bytes and has NCCS 19. Userland's struct (musl's) is larger:
// it appends c_ispeed/c_ospeed and declares c_cc[32]. That is fine and
// deliberate on Linux's part -- the leading 36 bytes are identical, so
// the kernel writes a prefix of the caller's struct and leaves the
// rest alone. Copying userland's 44-byte version here instead would
// scribble 8 bytes past what Linux writes.
#define NCCS_K 19

struct termios_k {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS_K];
};

// c_cc indices, Linux's order.
#define VINTR  0
#define VQUIT  1
#define VERASE 2
#define VKILL  3
#define VEOF   4
#define VTIME  5
#define VMIN   6
#define VSTART 8
#define VSTOP  9
#define VSUSP  10

// c_iflag
#define ICRNL  0000400
#define INLCR  0000100
#define IGNCR  0000200
#define IXON   0002000

// c_oflag
#define OPOST  0000001
#define ONLCR  0000004

// c_cflag
#define B38400 0000017
#define CS8    0000060
#define CREAD  0000200
#define CLOCAL 0004000

// c_lflag
#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

// The ioctls a terminal answers. Linux's numbers.
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

struct winsize_k {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

#include "sync/spinlock_types.h"
#include "sync/waitq.h"

#define TTY_BUF 1024

struct tty;

// Where a tty's cooked output goes. The console backend writes serial +
// framebuffer; the pty backend appends to the master's read buffer.
struct tty_backend {
    void (*output)(struct tty *t, const char *s, uint32_t n);
};

struct tty {
    struct spinlock lock;
    struct waitq    readers;        // blocked in the slave/console read()

    char     edit[TTY_BUF];  uint32_t edit_len;        // canonical assembly
    char     ready[TTY_BUF]; uint32_t ready_head, ready_len;  // readable
    int      saw_eof;

    struct termios_k tio;
    struct winsize_k win;
    int      fg_pgid;
    int      sid;
    int      hung_up;              // pty master closed -> slave reads see EOF

    const struct tty_backend *backend;
    void *backend_priv;

    // pty master side: what the slave wrote, waiting for the master read.
    char     outq[TTY_BUF]; uint32_t out_head, out_len;
    struct waitq out_readers;
};

void tty_init(void);
struct tty *tty_console(void);

// Bring a freshly allocated struct tty up with default (canonical,
// echoing) termios and the given backend.
void tty_obj_init(struct tty *t, const struct tty_backend *b, void *priv);

// The line discipline, now taking an explicit tty. tty_input_char is
// called from the keyboard IRQ (console) and from a pty master write().
void    tty_input_char(struct tty *t, char c);
int64_t tty_obj_read(struct tty *t, void *buf, uint32_t len, int nonblock);
int64_t tty_obj_write(struct tty *t, const void *buf, uint32_t len);
int64_t tty_obj_ioctl(struct tty *t, uint64_t request, void *arg);
int     tty_obj_poll(struct tty *t, int events);

// File operations for /dev/CONSOLE and /dev/TTY (bound to tty_console()).
struct file_ops;
extern const struct file_ops tty_file_ops;

// Selftest helpers for tracking input delivery (test-only)
void tty_selftest_reset(void);
int  tty_selftest_saw(char c);

void tty_selftest(void);

#endif
