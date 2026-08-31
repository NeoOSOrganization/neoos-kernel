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

#endif
