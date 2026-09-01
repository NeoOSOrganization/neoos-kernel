#ifndef NEOOS_PTY_H
#define NEOOS_PTY_H

#include <stdint.h>

struct file_descriptor;

// Bring the pty pool up (call once at boot).
void pty_init(void);

// devfs open hook for /dev/ptmx: allocates a master/slave pair, wires
// f to the master, and registers /dev/pts/N for the slave.
int  ptmx_open(struct file_descriptor *f);

// Linux ioctl numbers used by the master.
#define TIOCGPTN   0x80045430
#define TIOCSPTLCK 0x40045431

// NeoOS extension, no Linux equivalent (deliberately outside the 0x54xx
// TIOC range): on a pty master, arg != 0 routes cooked keyboard input
// to this master's slave and hands the framebuffer to userland; arg ==
// 0 releases both. Released automatically when the master is closed;
// forcibly reclaimed by a kernel panic. See docs/stdlib.md.
#define NEOOS_TIOCSACTIVE 0x4E454F01

#endif
