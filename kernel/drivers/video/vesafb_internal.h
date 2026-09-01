#ifndef NEOOS_VESAFB_INTERNAL_H
#define NEOOS_VESAFB_INTERNAL_H

#include <stdint.h>

// vesafb's own state -- the Multiboot2 linear framebuffer. Not a public
// interface: everything outside kernel/drivers/video/ goes through
// fb_device.h (the abstraction) or fb.h (the /dev/fb0 surface). fbcon
// is the one exception -- it draws straight into fb.virt.

struct fb_info {
    uint64_t phys;                 // framebuffer physical base
    uint64_t size;                 // pitch * height, rounded up to a page
    uint32_t pitch;                // bytes per scanline
    uint32_t width, height;        // pixels
    uint8_t  bpp;                  // 32 only
    struct { uint8_t pos, size; } r, g, b;   // channel bit positions
    volatile uint8_t *virt;        // kernel VA once fb_map() runs, else 0
    int      present;              // 0 => VGA text fallback
};

extern struct fb_info fb;

// Scan the Multiboot2 framebuffer info tag; set fb.present + the fields
// above. Allocates nothing; safe before pmm/paging. Sanitises bogus
// channel positions to XRGB8888.
void vesafb_parse(void *multiboot_info);

// Map the framebuffer into the kernel higher half (physmap when it
// reaches, else FB_VIRT_BASE) and set fb.virt. Needs pmm + the physmap.
void fb_map(void);

#endif
