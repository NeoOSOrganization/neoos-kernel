// fbtest.c -- exercise /dev/fb0: fbdev ioctls, mmap, pixel round-trip.
// On a text-mode kernel (no framebuffer) it prints SKIPPED and passes.

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>

#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 0x01

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

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

int main(void) {
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { printf("[fbtest] SKIPPED: /dev/fb0 open %d\n", fd); return 0; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) != 0) { printf("[fbtest] FAILED: GET_VSCREENINFO\n"); return 1; }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &f) != 0) { printf("[fbtest] FAILED: GET_FSCREENINFO\n"); return 1; }

    if (v.bits_per_pixel != 32 || v.xres == 0 || v.yres == 0 ||
        f.line_length < v.xres * 4) {
        printf("[fbtest] FAILED: geometry %ux%ux%u pitch %u\n",
               v.xres, v.yres, v.bits_per_pixel, f.line_length);
        return 1;
    }

    unsigned long len = (unsigned long)f.line_length * v.yres;
    long m = mmap_fd_raw(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m < 0) { printf("[fbtest] FAILED: mmap rc %d\n", (int)m); return 1; }

    // The framebuffer is shared with the kernel's console, which is
    // actively rendering boot/test output onto it -- so any pixel we
    // write can be overwritten a moment later. The test is "a write
    // through the mapping is visible through the mapping (and the fd)
    // when nothing else touches that spot in the window" -- retried,
    // because sometimes something does.
    volatile unsigned *px = (volatile unsigned *)(unsigned long)m;
    unsigned long off = (len / 4) - 4;    // last row, well away from the cursor's usual path
    int mapped_ok = 0, fd_ok = 0;
    for (int try = 0; try < 200 && !(mapped_ok && fd_ok); try++) {
        px[off] = 0x00abcdefu;
        if (px[off] == 0x00abcdefu) { mapped_ok = 1; }

        unsigned back = 0;
        px[off + 1] = 0x00123456u;
        if (lseek(fd, (long)((off + 1) * 4), 0) == (long)((off + 1) * 4) &&
            read(fd, &back, 4) == 4 && back == 0x00123456u) { fd_ok = 1; }
    }
    if (!mapped_ok) { printf("[fbtest] FAILED: write not visible through the mapping\n"); return 1; }
    if (!fd_ok)     { printf("[fbtest] FAILED: write not visible through the fd\n"); return 1; }

    printf("[fbtest] ALL PASSED\n");
    return 0;
}
