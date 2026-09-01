#ifndef NEOOS_FB_DEVICE_H
#define NEOOS_FB_DEVICE_H

#include <stdint.h>

// A linear-framebuffer provider. One implementation today (vesafb, the
// Multiboot2 framebuffer); a bochs-drm or virtio-gpu driver could add
// itself later without touching any caller. The console driver
// (con_driver, fbcon) and userland (/dev/fb0) reach the framebuffer
// only through the active fb_device.

struct fb_channel { uint8_t offset, length; };

struct fb_mode {
    uint32_t width, height, bpp, pitch;
    struct fb_channel r, g, b;
};

struct fb_device {
    const char *name;
    int priority;                            // higher wins the probe

    int  (*probe)(void *multiboot_info);     // 1 if it can drive this machine
    void (*current)(struct fb_mode *m, uint64_t *phys, uint64_t *size);
    int  (*modes)(struct fb_mode *out, int max);
    int  (*set_mode)(const struct fb_mode *m);
    void (*flush_rect)(int x, int y, int w, int h);   // may be NULL
    int  (*cursor)(int x, int y, int visible);        // may be NULL
};

void              fb_device_register(struct fb_device *d);
void              fb_device_register_builtin(void);   // registers vesafb
void              fb_device_probe_all(void *multiboot_info);
struct fb_device *fb_device_active(void);   // NULL on a text-only boot
void              fb_device_selftest(void); // "[fbdev] ..."

#endif
