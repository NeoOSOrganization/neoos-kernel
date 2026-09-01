#ifndef NEOOS_VT_H
#define NEOOS_VT_H

#include <stdint.h>

// The kernel virtual terminals: VT_COUNT text consoles multiplexed onto
// the active con_driver. /dev/tty1..VT_COUNT, /dev/tty0 == active.
// Alt+F1..Fn switches (input.c). The kernel log and panic land on VT 1.

#define VT_COUNT 6

struct tty;
struct vt_console;
struct file_ops;

void  vt_init(void);                        // build all VTs, wire their ttys
struct tty *vt_active_tty(void);            // for tty_console()
int   vt_active_index(void);                // 0-based
void  vt_active_geometry(int *cols, int *rows);

void  vt_switch(int n);                     // 0-based; VT_AUTO, instant
void  vt_scroll(int delta_lines);           // active VT scrollback

void  vt_write_active(const char *s, unsigned n);   // kernel console output
void  vt_panic_reset(void);                 // force VT0, KD_TEXT, repaint

// ioctl on /dev/ttyN (vt_index 0 == active). Handles VT_* / KD*; returns
// -ENOTTY for anything else so the caller can fall through to the tty.
int64_t vt_ioctl(int vt_index, uint64_t request, void *arg);

// devfs helpers: map /dev/ttyN -> the VT's struct tty.
struct tty *vt_tty(int vt_index);           // 0 == active, 1..VT_COUNT

// /dev/tty0../dev/ttyN. The VT index lives in f->priv (0 = "whichever
// is active"), which devfs's open sets from the device name; every
// operation then routes to that VT's struct tty, with ioctl trying the
// VT_*/KD* set first.
extern const struct file_ops vt_file_ops;

void  vt_selftest(void);

// Linux VT / KD ioctl numbers.
#define VT_OPENQRY   0x5600
#define VT_GETMODE   0x5601
#define VT_SETMODE   0x5602
#define VT_GETSTATE  0x5603
#define VT_RELDISP   0x5605
#define VT_ACTIVATE  0x5606
#define VT_WAITACTIVE 0x5607
#define KDGETMODE    0x4B3B
#define KDSETMODE    0x4B3A
#define KD_TEXT      0
#define KD_GRAPHICS  1

struct vt_stat { uint16_t v_active, v_signal, v_state; };

#endif
