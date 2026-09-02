#include "drivers/video/fb.h"
#include "drivers/video/fb_device.h"
#include "drivers/video/vesafb_internal.h"
#include "drivers/char/serial.h"
#include "mm/paging.h"
#include "mm/vma.h"
#include "fs/file.h"
#include "sync/poll_head.h"
#include "sched/proc.h"
#include "errno.h"

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

void vesafb_parse(void *multiboot_info) {
    if (fb.present) { return; }   // idempotent: probe may run more than once
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
        // The MB2 tag's channel positions have been seen inconsistent
        // (r==g, or > 24) under GRUB + QEMU stdvga. Every 32bpp mode
        // NeoOS supports is XRGB8888, and fbcon already assumes it.
        int bad = (fb.r.pos == fb.g.pos) || (fb.g.pos == fb.b.pos) ||
                  fb.r.pos > 24 || fb.g.pos > 24 || fb.b.pos > 24;
        if (bad) {
            serial_write_string("[fb] MB2 channel positions bogus (r=");
            serial_write_hex64(fb.r.pos); serial_write_string(" g=");
            serial_write_hex64(fb.g.pos); serial_write_string(" b=");
            serial_write_hex64(fb.b.pos);
            serial_write_string("), assuming XRGB8888\n");
            fb.r.pos = 16; fb.g.pos = 8; fb.b.pos = 0;
            fb.r.size = fb.g.size = fb.b.size = 8;
        }

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

// ---------------------------------------------------------------- fb_device

static int vesafb_probe(void *mbi) {
    vesafb_parse(mbi);
    return fb.present;
}

static void vesafb_current(struct fb_mode *m, uint64_t *phys, uint64_t *size) {
    m->width  = fb.width;
    m->height = fb.height;
    m->bpp    = fb.bpp;
    m->pitch  = fb.pitch;
    m->r = (struct fb_channel){ fb.r.pos, fb.r.size };
    m->g = (struct fb_channel){ fb.g.pos, fb.g.size };
    m->b = (struct fb_channel){ fb.b.pos, fb.b.size };
    if (phys) { *phys = fb.phys; }
    if (size) { *size = fb.size; }
}

static int vesafb_modes(struct fb_mode *out, int max) {
    if (max < 1) { return 0; }
    vesafb_current(&out[0], 0, 0);
    return 1;
}

static int vesafb_set_mode(const struct fb_mode *m) {
    struct fb_mode cur;
    vesafb_current(&cur, 0, 0);
    return (m->width == cur.width && m->height == cur.height &&
            m->bpp == cur.bpp) ? 0 : -EINVAL;
}

static void vesafb_flush_rect(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;      // linear framebuffer: scanout is live
}

struct fb_device vesafb_drv = {
    .name = "vesafb", .priority = 100,
    .probe = vesafb_probe, .current = vesafb_current,
    .modes = vesafb_modes, .set_mode = vesafb_set_mode,
    .flush_rect = vesafb_flush_rect, .cursor = 0,
};

// ---------------------------------------------------------------- /dev/fb0

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    struct fb_bitfield { uint32_t offset, length, msb_right; } red, green, blue, transp;
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin,
             hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};

struct fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

static void bzero_local(void *p, uint64_t n) {
    for (uint64_t i = 0; i < n; i++) { ((uint8_t *)p)[i] = 0; }
}

static int64_t fb_mmap(struct file_descriptor *f, struct mmap_req *r) {
    (void)f;
    if (!fb.present) { return -ENODEV; }
    if (r->len == 0 || r->off + r->len < r->off) { return -EINVAL; }
    if (r->off + r->len > fb.size) { return -EINVAL; }
    if (r->prot & PROT_EXEC) { return -EINVAL; }        // W^X
    int64_t rc = vma_map_phys(current_proc(), fb.phys + r->off, r->len, (uint32_t)r->prot);
    if (rc < 0) { return rc; }
    r->out_addr = (uint64_t)rc;
    return 0;
}

static int64_t fb_read(struct file_descriptor *f, void *buf, uint64_t n) {
    if (!fb.present) { return -ENODEV; }
    if ((uint64_t)f->position >= fb.size) { return 0; }
    uint64_t k = fb.size - (uint64_t)f->position;
    if (k > n) { k = n; }
    for (uint64_t i = 0; i < k; i++) { ((uint8_t *)buf)[i] = fb.virt[f->position + i]; }
    f->position += (uint32_t)k;
    return (int64_t)k;
}

static int64_t fb_write(struct file_descriptor *f, const void *buf, uint64_t n) {
    if (!fb.present) { return -ENODEV; }
    if ((uint64_t)f->position >= fb.size) { return -ENOSPC; }
    uint64_t k = fb.size - (uint64_t)f->position;
    if (k > n) { k = n; }
    for (uint64_t i = 0; i < k; i++) { fb.virt[f->position + i] = ((const uint8_t *)buf)[i]; }
    f->position += (uint32_t)k;
    return (int64_t)k;
}

static int64_t fb_lseek(struct file_descriptor *f, int64_t off, int whence) {
    int64_t base = whence == 1 ? (int64_t)f->position
                 : whence == 2 ? (int64_t)fb.size : 0;
    int64_t pos = base + off;
    if (pos < 0) { return -EINVAL; }
    if (pos > (int64_t)fb.size) { pos = (int64_t)fb.size; }
    f->position = (uint32_t)pos;
    return pos;
}

static int64_t fb_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    (void)f;
    if (!fb.present) { return -ENODEV; }
    if (req == FBIOGET_FSCREENINFO) {
        if (!user_range_writable((uint64_t)(uintptr_t)arg, sizeof(struct fb_fix_screeninfo))) { return -EFAULT; }
        struct fb_fix_screeninfo x; bzero_local(&x, sizeof x);
        const char id[] = "neoosfb";
        for (unsigned i = 0; i < sizeof(id); i++) { x.id[i] = id[i]; }
        x.smem_start  = fb.phys;
        x.smem_len    = (uint32_t)fb.size;
        x.type        = 0;      // FB_TYPE_PACKED_PIXELS
        x.visual      = 2;      // FB_VISUAL_TRUECOLOR
        x.line_length = fb.pitch;
        for (unsigned i = 0; i < sizeof x; i++) { ((uint8_t *)arg)[i] = ((uint8_t *)&x)[i]; }
        return 0;
    }
    if (req == FBIOGET_VSCREENINFO) {
        if (!user_range_writable((uint64_t)(uintptr_t)arg, sizeof(struct fb_var_screeninfo))) { return -EFAULT; }
        struct fb_var_screeninfo x; bzero_local(&x, sizeof x);
        x.xres = x.xres_virtual = fb.width;
        x.yres = x.yres_virtual = fb.height;
        x.bits_per_pixel = fb.bpp;
        x.red   = (struct fb_bitfield){ fb.r.pos, fb.r.size, 0 };
        x.green = (struct fb_bitfield){ fb.g.pos, fb.g.size, 0 };
        x.blue  = (struct fb_bitfield){ fb.b.pos, fb.b.size, 0 };
        for (unsigned i = 0; i < sizeof x; i++) { ((uint8_t *)arg)[i] = ((uint8_t *)&x)[i]; }
        return 0;
    }
    if (req == FBIOPUT_VSCREENINFO) { return -EINVAL; }   // no mode setting
    return -ENOTTY;
}

static int64_t fb_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }
static int     fb_poll(struct file_descriptor *f, int e) { (void)f; return e & (POLLIN | POLLOUT); }
static void    fb_dup(struct file_descriptor *f) { (void)f; }
static void    fb_close(struct file_descriptor *f) { (void)f; }

// CS5.2. The framebuffer is always ready, so its readiness never
// changes and there is nothing to be woken about. The shared
// never-notified head says so, which is what keeps a poller holding only
// such fds off the global broadcast entirely.
static struct poll_head *fb_poll_head(struct file_descriptor *f) {
    (void)f;
    return poll_head_always_ready();
}

const struct file_ops fb_file_ops = {
    .name = "fb", .read = fb_read, .write = fb_write, .lseek = fb_lseek,
    .getdents = fb_getdents, .ioctl = fb_ioctl, .poll = fb_poll, .poll_head = fb_poll_head,
    .mmap = fb_mmap, .dup = fb_dup, .close = fb_close,
};

int fb_open(struct file_descriptor *f) {
    if (!fb.present) { return -ENODEV; }
    f->ops = &fb_file_ops;
    f->priv = 0;
    return 0;
}
