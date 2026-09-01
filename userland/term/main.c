// /BIN/TERM -- the userland framebuffer terminal.
//
// Owns a PTY, runs a child on the slave, mmaps /dev/fb0, and paints the
// VT grid with the Spleen glyph table. M1b-3 scope: render the child's
// output; the cooked keyboard path already reaches the slave through
// NEOOS_TIOCSACTIVE. Arrow-key translation and scrollback are M1b-4.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <poll.h>
#include <sys/wait.h>

#include "vt.h"
#include "render.h"
#include "palette.h"
#include "font_term.h"

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 0x01

#define TIOCGPTN           0x80045430
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define NEOOS_TIOCSACTIVE  0x4E454F01

extern long mmap_fd_raw(unsigned long addr, unsigned long len, int prot,
                        int flags, int fd, long offset);

struct fb_bitfield { unsigned offset, length, msb_right; };
struct fb_var_screeninfo {
    unsigned xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    unsigned bits_per_pixel, grayscale;
    struct fb_bitfield red, green, blue, transp;
    unsigned nonstd, activate, height, width, accel_flags;
    unsigned pixclock, left_margin, right_margin, upper_margin, lower_margin,
             hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    unsigned long smem_start;
    unsigned smem_len, type, type_aux, visual;
    unsigned short xpanstep, ypanstep, ywrapstep;
    unsigned line_length;
    unsigned long mmio_start;
    unsigned mmio_len, accel;
    unsigned short capabilities, reserved[2];
};

static struct vt V;

static void fail(const char *what) {
    printf("[term] FAILED: %s\n", what);
    exit(1);
}

// pack an 0xRRGGBB into the framebuffer's channel layout
static unsigned fb_pack(const struct term_fb *fb, unsigned rgb) {
    unsigned r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (r << fb->ro) | (g << fb->go) | (b << fb->bo);
}
static unsigned fb_px(const struct term_fb *fb, int x, int y) {
    volatile unsigned *p = (volatile unsigned *)(fb->pix + y * fb->pitch) + x;
    return *p;
}

int main(void) {
    // --- PTY ---------------------------------------------------------
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0) { fail("ptmx open"); }
    int n = -1;
    ioctl(m, TIOCGPTN, &n);
    char pts[24];
    {
        const char *pre = "/dev/pts/";
        int i = 0;
        while (pre[i]) { pts[i] = pre[i]; i++; }
        if (n >= 10) { pts[i++] = '0' + n / 10; }
        pts[i++] = '0' + n % 10;
        pts[i] = 0;
    }

    // --- framebuffer ----------------------------------------------------
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        printf("[term] SKIPPED: no framebuffer\n");
        return 0;
    }
    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;
    if (ioctl(fbfd, FBIOGET_VSCREENINFO, &vinfo) != 0) { fail("GET_VSCREENINFO"); }
    if (ioctl(fbfd, FBIOGET_FSCREENINFO, &finfo) != 0) { fail("GET_FSCREENINFO"); }

    unsigned long len = (unsigned long)finfo.line_length * vinfo.yres;
    len = (len + 0xfff) & ~0xfffUL;
    long map = mmap_fd_raw(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (map < 0) { fail("fb mmap"); }

    // QEMU stdvga / the GRUB VBE mode is XRGB8888; fbcon assumes the
    // same. The MB2 tag's channel positions have been seen bogus, so
    // trust the format the kernel already renders through unless the
    // ioctl reports something equally plausible.
    int ro = 16, go = 8, bo = 0;
    if (vinfo.red.offset != vinfo.green.offset &&
        vinfo.green.offset != vinfo.blue.offset &&
        vinfo.red.offset <= 24 && vinfo.blue.offset <= 24) {
        ro = (int)vinfo.red.offset;
        go = (int)vinfo.green.offset;
        bo = (int)vinfo.blue.offset;
    }

    struct term_fb FB = {
        (volatile unsigned char *)map,
        (int)finfo.line_length,
        (int)vinfo.xres, (int)vinfo.yres,
        ro, go, bo,
    };

    int cols = FB.w / TERM_GLYPH_W;
    int rows = FB.h / TERM_GLYPH_H;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;

    palette_init();
    vt_init(&V, cols, rows);
    render_clear(&FB, VT_DEFAULT_BG);

    // Open the slave ourselves and hold it across the fork, so the pty
    // never momentarily looks hung up (slave_refs == 0) while the child
    // is still setting up -- that would make our first poll() return
    // POLLHUP and the render loop bail before any output arrives.
    int slave = open(pts, O_RDWR);
    if (slave < 0) { fail("pts open"); }

    // --- claim the screen + keyboard --------------------------------
    ioctl(m, NEOOS_TIOCSACTIVE, (void *)1);

    // --- child -----------------------------------------------------
    int child = fork();
    if (child < 0) { ioctl(m, NEOOS_TIOCSACTIVE, (void *)0); fail("fork"); }
    if (child == 0) {
        close(m);                            // the child has no use for the master
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setsid();
        exec("/BIN/TERMCHILD.ELF");
        exit(127);
    }

    close(slave);                            // parent: the child holds 0/1/2 now

    // --- render loop ----------------------------------------------
    for (;;) {
        struct pollfd pf = { m, POLLIN, 0 };
        int pr = poll(&pf, 1, 1000);
        if (pr > 0 && (pf.revents & POLLIN)) {
            unsigned char buf[1024];
            int r = read(m, buf, sizeof buf);
            if (r <= 0) { break; }
            vt_feed(&V, buf, (size_t)r);
            struct vt_span sp[VT_MAX_ROWS];
            int nd = vt_take_dirty(&V, sp, VT_MAX_ROWS);
            for (int i = 0; i < nd; i++) {
                render_span(&FB, &V, sp[i].row, sp[i].col0, sp[i].col1);
            }
            render_cursor(&FB, &V);
        }
        if (pf.revents & (POLLHUP | POLLERR)) { break; }
    }

    int st;
    wait4(child, &st, 0, 0);

    // --- self-check: TERMCHILD's red 'R' at cell (0,0), blank at (0,20)
    unsigned want_red = fb_pack(&FB, vt_palette[1]);
    unsigned want_bg  = fb_pack(&FB, VT_DEFAULT_BG);
    int red_seen = 0, blank_ok = 1;
    for (int y = 0; y < TERM_GLYPH_H; y++) {
        for (int x = 0; x < TERM_GLYPH_W; x++) {
            if (fb_px(&FB, x, y) == want_red) { red_seen = 1; }
        }
        for (int x = 20 * TERM_GLYPH_W; x < 21 * TERM_GLYPH_W; x++) {
            if (fb_px(&FB, x, y) != want_bg) { blank_ok = 0; }
        }
    }

    ioctl(m, NEOOS_TIOCSACTIVE, (void *)0);

    if (red_seen && blank_ok) {
        printf("[term] render ALL PASSED\n");
        return 0;
    }
    printf("[term] render FAILED: red_seen=%d blank_ok=%d\n", red_seen, blank_ok);
    return 1;
}
