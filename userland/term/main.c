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
#include <auxv.h>

#include "vt.h"
#include "render.h"
#include "palette.h"
#include "font_term.h"
#include "neoos_logo.h"

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 0x01

#define TIOCGPTN           0x80045430
#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define NEOOS_TIOCSACTIVE  0x4E454F01
#define TIOCSCTTY          0x540E

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

// With no arguments TERM runs /BIN/TERMCHILD.ELF and finishes with the
// render self-check that makes it a test. Given arguments it runs those
// instead and skips the self-check, because the check looks for
// TERMCHILD's red 'R' at cell (0,0) and any other program would rightly
// fail it.
//
// The shape is execve's, deliberately: argv[1] is the PATH and argv[2..]
// is the child's whole argv, argv[0] included.
//
//   wait /BIN/TERM.ELF /BIN/BUSYBOX.ELF busybox sh
//                      ^path            ^argv[0] ^argv[1]
//
// Separating them is not ceremony. BusyBox chooses its applet from
// argv[0], and the path on a FAT volume is `/BIN/BUSYBOX.ELF`, whose
// basename is not an applet name -- given the path as argv[0] it prints
// "applet not found" and exits 127, which is exactly what the first
// version of `make shell` did. With argv[2..] supplied, argv[0] can be
// `busybox` while the kernel still loads the file it can actually find.
//
// Omitting argv[2..] falls back to argv[0] = path, which is right for
// any program that does not care.
// Paints the shared logo art at the top of the framebuffer and returns
// how many text ROWS it used, including the blank separator line. The
// colours match the kernel banner's: 'P' cells purple, the rest red.
static int logo_draw(const struct term_fb *fb) {
    const uint32_t purple = vt_palette[13];   // bright magenta
    const uint32_t red    = vt_palette[9];    // bright red
    const uint32_t bg     = VT_DEFAULT_BG;

    int max_rows = fb->h / TERM_GLYPH_H;
    int max_cols = fb->w / TERM_GLYPH_W;

    int r = 0;
    for (; neoos_logo_art[r]; r++) {
        // A framebuffer too short for the whole logo gets as much as
        // fits and no terminal at all would be worse -- stop early and
        // leave the rest of the screen to the shell.
        if (r + 2 >= max_rows) { break; }
        const char *a = neoos_logo_art[r];
        const char *c = neoos_logo_col[r];
        int clen = 0;
        while (c && c[clen]) { clen++; }
        for (int i = 0; a[i] && i < max_cols; i++) {
            uint32_t fg = (i < clen && c[i] == 'P') ? purple : red;
            render_glyph_at(fb, r, i, (unsigned char)a[i], fg, bg);
        }
    }

    if (r < max_rows) {
        // Wordmark only, no version string: NEOOS_VERSION lives in
        // kernel/version.h, and dragging a kernel header onto the
        // userland include path to print four characters is a worse
        // trade than leaving them out. The kernel banner already
        // reports the version at boot.
        render_text_at(fb, r, 3, "NeoOS", purple, bg);
        r++;
    }
    if (r < max_rows) { r++; }        // one blank row between logo and shell
    return r;
}

int main(int argc, char **argv) {
    char *default_child[] = { (char *)"/BIN/TERMCHILD.ELF", 0 };
    char *path_only[]     = { 0, 0 };
    const char *child_path;
    char **child_argv;
    if (argc > 2) {
        child_path = argv[1];
        child_argv = &argv[2];
    } else if (argc > 1) {
        child_path = argv[1];
        path_only[0] = argv[1];
        child_argv = path_only;
    } else {
        child_path = default_child[0];
        child_argv = default_child;
    }
    int is_selftest = (argc <= 1);
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

    palette_init();

    // The logo header.
    //
    // Taking the framebuffer over means clearing it, and that erased the
    // kernel's boot banner -- logo and all. So the terminal repaints the
    // logo itself, from the same art the kernel draws
    // (shared/neoos_logo.h, included by both so the two cannot drift).
    //
    // It is painted straight into the framebuffer rather than fed to the
    // VT, because it is not terminal content: it must not scroll away,
    // must not enter the scrollback, and must survive the shell running
    // `clear`. The terminal is then confined to the rows BELOW it by
    // moving the framebuffer origin down -- after which every render_*
    // call is already relative to FB.pix and needs no changes at all.
    // Claim the screen BEFORE painting anything on it.
    //
    // The claim is what sets fb_owned and stops the kernel console
    // painting (kernel/tty/console.c). Clearing and drawing first, as
    // this used to, leaves a window in which late kernel output lands on
    // top of what was just drawn. No such collision has actually been
    // observed -- this is ordering put right on the argument, not on a
    // reproduction.
    ioctl(m, NEOOS_TIOCSACTIVE, (void *)1);

    render_clear(&FB, VT_DEFAULT_BG);
    int header_rows = logo_draw(&FB);

    FB.pix += (long)header_rows * TERM_GLYPH_H * FB.pitch;
    FB.h   -= header_rows * TERM_GLYPH_H;

    int cols = FB.w / TERM_GLYPH_W;
    int rows = FB.h / TERM_GLYPH_H;
    if (cols > VT_MAX_COLS) cols = VT_MAX_COLS;
    if (rows > VT_MAX_ROWS) rows = VT_MAX_ROWS;

    vt_init(&V, cols, rows);

    // Open the slave ourselves and hold it across the fork, so the pty
    // never momentarily looks hung up (slave_refs == 0) while the child
    // is still setting up -- that would make our first poll() return
    // POLLHUP and the render loop bail before any output arrives.
    int slave = open(pts, O_RDWR);
    if (slave < 0) { fail("pts open"); }

    // --- child -----------------------------------------------------
    int child = fork();
    if (child < 0) { ioctl(m, NEOOS_TIOCSACTIVE, (void *)0); fail("fork"); }
    if (child == 0) {
        close(m);                            // the child has no use for the master
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        if (slave > 2) { close(slave); }
        setsid();                    // the child leads its own session
        // ...and takes this pty as its controlling terminal. Without
        // it a shell finds a foreground process group that is not its
        // own and turns job control off (see docs/stdlib.md).
        ioctl(0, TIOCSCTTY, 0);
        int rc = execve(child_path, child_argv, environ);
        // Reported to /dev/kmsg, not stdout: stdout is the pty slave by
        // now, so anything printed here would be rendered to the screen
        // by a terminal that is about to find out its child died -- and
        // would never reach the serial log where a failure is read.
        int k = open("/dev/kmsg", O_WRONLY);
        if (k >= 0) {
            char msg[96];
            int n = 0;
            const char *pre = "[term] execve failed: ";
            while (pre[n]) { msg[n] = pre[n]; n++; }
            int v = rc < 0 ? -rc : rc;
            if (rc < 0) { msg[n++] = '-'; }
            char d[8]; int dn = 0;
            do { d[dn++] = (char)('0' + (v % 10)); v /= 10; } while (v && dn < 8);
            while (dn) { msg[n++] = d[--dn]; }
            msg[n++] = ' ';
            for (int i = 0; child_path[i] && n < 90; i++) { msg[n++] = child_path[i]; }
            msg[n++] = '\n';
            write(k, msg, (unsigned long)n);
        }
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

    // Running someone else's program: there is no render self-check to
    // do, and the exit status that matters is the child's.
    if (!is_selftest) {
        ioctl(m, NEOOS_TIOCSACTIVE, (void *)0);
        printf("[term] child exited, status %d\n", st);
        return 0;
    }

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
