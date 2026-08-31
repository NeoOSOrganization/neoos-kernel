#ifndef NEOOS_FB_H
#define NEOOS_FB_H

#include <stdint.h>

// The linear framebuffer GRUB handed us via the Multiboot2 framebuffer
// tag. `present` is 0 when GRUB left the machine in VGA text mode (no
// tag, or a mode we do not support) -- everything framebuffer degrades
// gracefully in that case (see fb.c and dev/console.c).
struct fb_info {
    uint64_t phys;                 // framebuffer physical base
    uint64_t size;                 // pitch * height, rounded up to a page
    uint32_t pitch;                // bytes per scanline
    uint32_t width, height;        // pixels
    uint8_t  bpp;                  // 32 only in M1a
    struct { uint8_t pos, size; } r, g, b;   // channel bit positions
    volatile uint8_t *virt;        // kernel VA once fb_map() runs, else 0
    int      present;              // 0 => VGA text fallback
};

extern struct fb_info fb;

// Parse the Multiboot2 framebuffer info tag. Allocates nothing; safe to
// call before pmm/paging are up. Sets fb.present.
void fb_init(void *multiboot_info);

// Map [fb.phys, fb.phys + fb.size) into the kernel higher half at
// FB_VIRT_BASE and set fb.virt. Needs pmm + the physmap, so call after
// paging_init(). No-op when !fb.present.
void fb_map(void);

#endif
