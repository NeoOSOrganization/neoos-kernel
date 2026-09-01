#ifndef NEOOS_FB_H
#define NEOOS_FB_H

#include <stdint.h>

// The /dev/fb0 surface. The framebuffer itself is an fb_device (see
// fb_device.h); vesafb's own state and the MB2 parse live in
// vesafb_internal.h. This header is just the character device.

// Map the active framebuffer into the kernel higher half and set
// fb.virt (physmap when it reaches, else FB_VIRT_BASE). Needs pmm +
// the physmap, so call after paging_init(). No-op when no framebuffer.
void fb_map(void);

// /dev/fb0. fb_open is the devfs open hook; it wires fb_file_ops.
struct file_descriptor;
struct file_ops;
extern const struct file_ops fb_file_ops;
int fb_open(struct file_descriptor *f);

// Linux fbdev ioctl numbers (asm-generic values).
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOPUT_VSCREENINFO 0x4601
#define FBIOGET_FSCREENINFO 0x4602

#endif
