#include "drivers/video/fb_device.h"
#include "drivers/char/serial.h"

#define FB_DEVICE_MAX 4

static struct fb_device *registry[FB_DEVICE_MAX];
static int               registry_n;
static struct fb_device *active;

extern struct fb_device vesafb_drv;

void fb_device_register(struct fb_device *d) {
    if (registry_n < FB_DEVICE_MAX) { registry[registry_n++] = d; }
}

void fb_device_register_builtin(void) {
    fb_device_register(&vesafb_drv);
}

void fb_device_probe_all(void *multiboot_info) {
    active = 0;
    for (int i = 0; i < registry_n; i++) {
        struct fb_device *d = registry[i];
        if (d->probe && d->probe(multiboot_info)) {
            if (!active || d->priority > active->priority) { active = d; }
        }
    }
    if (active) {
        serial_write_string("[fbdev] ");
        serial_write_string(active->name);
        serial_write_string(" selected\n");
    } else {
        serial_write_string("[fbdev] no framebuffer device\n");
    }
}

struct fb_device *fb_device_active(void) { return active; }

void fb_device_selftest(void) {
    if (!active) {
        serial_write_string("[fbdev] selftest skipped (no framebuffer)\n");
        return;
    }
    struct fb_mode m;
    uint64_t phys = 0, size = 0;
    active->current(&m, &phys, &size);
    if (m.width == 0 || m.height == 0 || m.pitch < m.width || phys == 0) {
        serial_write_string("[fbdev] selftest FAILED: bad current() mode\n");
        return;
    }
    if (active->flush_rect) { active->flush_rect(0, 0, 1, 1); }
    serial_write_string("[fbdev] selftest passed\n");
}
