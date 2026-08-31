#include "dev/fb.h"
#include "dev/serial.h"
#include "mm/paging.h"

struct fb_info fb;

// Multiboot2 framebuffer info tag (type 8). Layout from the spec:
// common header, then addr/pitch/width/height, then bpp + type + a
// reserved byte, then a type-specific colour-info block. For a direct
// RGB framebuffer (type 1) the colour block is six bytes: the position
// and size of each of the red, green and blue channels.
#define MB2_TAG_FRAMEBUFFER 8
#define MB2_FB_TYPE_RGB     1

struct mb2_tag {
    uint32_t type;
    uint32_t size;
};

struct mb2_fb_tag {
    uint32_t type;
    uint32_t size;
    uint64_t addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint8_t  reserved;
    uint8_t  r_pos, r_size;
    uint8_t  g_pos, g_size;
    uint8_t  b_pos, b_size;
} __attribute__((packed));

void fb_init(void *multiboot_info) {
    fb.present = 0;

    uint32_t total_size = *(uint32_t *)multiboot_info;
    uint8_t *ptr = (uint8_t *)multiboot_info + 8;   // skip total_size + reserved
    uint8_t *end = (uint8_t *)multiboot_info + total_size;

    while (ptr < end) {
        struct mb2_tag *tag = (struct mb2_tag *)ptr;
        if (tag->type == 0) { break; }             // end tag
        if (tag->type == MB2_TAG_FRAMEBUFFER) {
            struct mb2_fb_tag *ft = (struct mb2_fb_tag *)tag;
            if (ft->fb_type == MB2_FB_TYPE_RGB && ft->bpp == 32) {
                fb.phys   = ft->addr;
                fb.pitch  = ft->pitch;
                fb.width  = ft->width;
                fb.height = ft->height;
                fb.bpp    = ft->bpp;
                fb.r.pos = ft->r_pos; fb.r.size = ft->r_size;
                fb.g.pos = ft->g_pos; fb.g.size = ft->g_size;
                fb.b.pos = ft->b_pos; fb.b.size = ft->b_size;
                fb.size  = (((uint64_t)fb.pitch * fb.height) + 0xFFF) & ~0xFFFULL;
                fb.virt  = 0;
                fb.present = 1;
            }
            break;
        }
        ptr += (tag->size + 7) & ~7u;
    }

    if (fb.present) {
        serial_write_string("[fb] framebuffer ");
        serial_write_hex64(fb.width);
        serial_write_string("x");
        serial_write_hex64(fb.height);
        serial_write_string("x");
        serial_write_hex64(fb.bpp);
        serial_write_string(" pitch=");
        serial_write_hex64(fb.pitch);
        serial_write_string(" @ ");
        serial_write_hex64(fb.phys);
        serial_write_string("\n");
    } else {
        serial_write_string("[fb] no framebuffer tag -- VGA text fallback\n");
    }
}

void fb_map(void) {
    if (!fb.present) { return; }

    // The physmap (PML4[256]) already covers the first 4 GiB of physical
    // memory and -- unlike a fresh higher-half mapping -- it is copied
    // into every process's PML4, so fbcon works from any context (a
    // panic while a user process is current, console mirroring during a
    // syscall). QEMU's framebuffer BAR is well below 4 GiB, so this is
    // the path that is taken; the explicit FB_VIRT_BASE mapping is the
    // fallback for a framebuffer the physmap does not reach.
    if (fb.phys + fb.size <= (4ULL * 1024 * 1024 * 1024)) {
        fb.virt = (volatile uint8_t *)phys_to_virt(fb.phys);
        serial_write_string("[fb] via physmap at ");
        serial_write_hex64((uint64_t)(uintptr_t)fb.virt);
        serial_write_string("\n");
        return;
    }

    if (paging_map_range(FB_VIRT_BASE, fb.phys, fb.size,
                         PAGE_PRESENT | PAGE_WRITABLE | PAGE_NO_EXECUTE) != 0) {
        serial_write_string("[fb] FAILED to map framebuffer -- disabling\n");
        fb.present = 0;
        return;
    }
    fb.virt = (volatile uint8_t *)FB_VIRT_BASE;
    serial_write_string("[fb] mapped at ");
    serial_write_hex64(FB_VIRT_BASE);
    serial_write_string("\n");
}
