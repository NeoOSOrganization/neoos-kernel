#ifndef NEOOS_TERMIOS_H
#define NEOOS_TERMIOS_H

#include <stdint.h>

// USERLAND's struct, which is LONGER than the one TCGETS moves: Linux's
// kernel struct has NCCS 19 and no speed fields, and the ioctl writes
// only that 36-byte prefix. The trailing fields exist so the layout
// matches what a program compiled against Linux headers expects; they
// are not filled in by the kernel.
#define NCCS 32

typedef unsigned int  tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int  speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_line;
    cc_t     c_cc[NCCS];
    speed_t  __c_ispeed;
    speed_t  __c_ospeed;
};

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

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

#define ICRNL  0000400
#define INLCR  0000100
#define IGNCR  0000200
#define IXON   0002000

#define OPOST  0000001
#define ONLCR  0000004

#define B38400 0000017
#define CS8    0000060
#define CREAD  0000200
#define CLOCAL 0004000

#define ISIG   0000001
#define ICANON 0000002
#define ECHO   0000010
#define ECHOE  0000020
#define ECHOK  0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410

// POSIX's spellings. TCSANOW/DRAIN/FLUSH map onto the three TCSETS
// variants, exactly as they do on Linux.
#define TCSANOW   0
#define TCSADRAIN 1
#define TCSAFLUSH 2

int tcgetattr(int fd, struct termios *t);
int tcsetattr(int fd, int actions, const struct termios *t);

#endif
