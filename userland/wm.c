// userland/wm.c -- neoos-wm, the NeoOS window compositor.
//
// Owns /dev/fb0, composites client surfaces onto it, and routes input.
// Written in C rather than in a managed language on purpose: it is the
// process that must not stall, and it holds the screen for everything
// else on the machine.
//
// Clients hand over surfaces as memfds passed by descriptor over an
// abstract AF_UNIX socket, so the compositor maps the same physical
// pages the application draws into -- a commit costs a copy of the
// damaged region and nothing else.
//
// What it deliberately is NOT: a Wayland server, a widget toolkit, or a
// text renderer. Windows get a solid title bar and a border; anything
// inside them is the application's business (LVGL's, for the C# demo).

#include "wmproto.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>

// libneoos predates AF_UNIX sockets, so it does not export the
// constant yet. Linux's value; see docs/stdlib.md.
#ifndef AF_UNIX
#define AF_UNIX 1
#endif

#define SYS_RECVMSG 90

static long neo6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall" : "=a"(r)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return r;
}

struct k_iovec { void *base; uint64_t len; };
struct k_msghdr_u {
    void *name; uint32_t namelen, _p0;
    struct k_iovec *iov; uint64_t iovlen;
    void *control; uint64_t controllen;
    uint32_t flags, _p1;
};
struct k_cmsghdr_u { uint64_t len; int level, type; };

// ---- framebuffer ------------------------------------------------------

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602
#define KDSETMODE 0x4B3A
#define KD_TEXT 0
#define KD_GRAPHICS 1

struct fb_var_screeninfo {
    uint32_t xres, yres, xres_virtual, yres_virtual, xoffset, yoffset;
    uint32_t bits_per_pixel, grayscale;
    uint32_t red[3], green[3], blue[3], transp[3];
    uint32_t nonstd, activate, height, width, accel_flags;
    uint32_t pixclock, left_margin, right_margin, upper_margin, lower_margin;
    uint32_t hsync_len, vsync_len, sync, vmode, rotate, colorspace, reserved[4];
};
struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len, type, type_aux, visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len, accel;
    uint16_t capabilities, reserved[2];
};

static int       fb_fd = -1, tty_fd = -1;
static uint32_t *fb;                 // the scanned-out framebuffer
static uint32_t *back;               // where compositing actually happens
static uint32_t  fb_w, fb_h, fb_stride_px, fb_bytes;

// The accumulated damage rectangle, in screen coordinates, as a
// bounding box. Compositing happens in RAM and only this region is
// copied out to the framebuffer -- writes to the real framebuffer are
// by far the expensive part, and copying 4 MB to move a cursor is what
// makes a software compositor feel slow.
static int dmg_x0, dmg_y0, dmg_x1, dmg_y1;   // x1/y1 exclusive
static int dmg_any;

static void damage(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) { return; }
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) { w = (int)fb_w - x; }
    if (y + h > (int)fb_h) { h = (int)fb_h - y; }
    if (w <= 0 || h <= 0) { return; }

    if (!dmg_any) { dmg_x0 = x; dmg_y0 = y; dmg_x1 = x + w; dmg_y1 = y + h; dmg_any = 1; return; }
    if (x < dmg_x0) { dmg_x0 = x; }
    if (y < dmg_y0) { dmg_y0 = y; }
    if (x + w > dmg_x1) { dmg_x1 = x + w; }
    if (y + h > dmg_y1) { dmg_y1 = y + h; }
}

static void damage_all(void) { damage(0, 0, (int)fb_w, (int)fb_h); }

// ---- input ------------------------------------------------------------

struct input_event_u {
    int64_t tv_sec, tv_usec;
    uint16_t type, code;
    int32_t value;
};
#define EV_KEY 1
#define EV_REL 2
#define REL_X 0
#define REL_Y 1
#define BTN_LEFT 0x110

static int kbd_fd = -1, mouse_fd = -1;
static int cur_x, cur_y;
#define CURSOR_W 10
#define CURSOR_H 14

// ---- surfaces ---------------------------------------------------------

#define MAX_CLIENTS 8
#define TITLE_H 18
#define BORDER  2

struct client {
    int       fd;             // -1 when the slot is free
    uint32_t  id;
    int       has_surface;
    uint32_t  w, h;           // content size
    int       x, y;           // content origin on screen
    uint32_t *px;             // the client's memfd, mapped here
    uint64_t  bytes;
    uint32_t  stride_px;
    char      title[64];
    int       mapped;         // committed at least once
    int       is_shell;       // undecorated, at the origin, behind all
};

static struct client clients[MAX_CLIENTS];
static int focus_slot = -1;
static int screen_dirty = 1;

// ---- painting ---------------------------------------------------------

// Everything below composites into `back`, never into `fb`. Drawing
// straight into the scanned-out framebuffer is what made the screen
// flicker: repaint clears to the desktop colour first, so every cursor
// move showed a blank frame before the windows came back.
static void fill(int x, int y, int w, int h, uint32_t colour) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) { w = (int)fb_w - x; }
    if (y + h > (int)fb_h) { h = (int)fb_h - y; }
    for (int row = 0; row < h; row++) {
        uint32_t *dst = back + (uint64_t)(y + row) * fb_w + x;
        for (int col = 0; col < w; col++) { dst[col] = colour; }
    }
}

// The cursor: a filled arrow, drawn from a bitmap so it stays visible
// over any window colour.
static const uint16_t cursor_bits[CURSOR_H] = {
    0x0001, 0x0003, 0x0007, 0x000F, 0x001F, 0x003F, 0x007F,
    0x00FF, 0x01FF, 0x003F, 0x0067, 0x00C3, 0x00C0, 0x0180,
};

static void draw_cursor(void) {
    for (int row = 0; row < CURSOR_H; row++) {
        int y = cur_y + row;
        if (y < 0 || y >= (int)fb_h) { continue; }
        uint32_t *dst = back + (uint64_t)y * fb_w;
        for (int col = 0; col < CURSOR_W; col++) {
            int x = cur_x + col;
            if (x < 0 || x >= (int)fb_w) { continue; }
            if (cursor_bits[row] & (1u << col)) { dst[x] = 0x00FFFFFF; }
        }
    }
}

static void draw_window(struct client *c, int focused) {
    if (!c->mapped || !c->px) { return; }

    // The shell draws its own everything -- a title bar on the desktop
    // would be absurd.
    if (!c->is_shell) {
        uint32_t frame = focused ? 0x003A6EA5 : 0x00505050;
        fill(c->x - BORDER, c->y - TITLE_H - BORDER,
             (int)c->w + 2 * BORDER, TITLE_H + BORDER, frame);
        fill(c->x - BORDER, c->y, BORDER, (int)c->h, frame);
        fill(c->x + (int)c->w, c->y, BORDER, (int)c->h, frame);
        fill(c->x - BORDER, c->y + (int)c->h, (int)c->w + 2 * BORDER, BORDER, frame);
    }

    // The content, straight out of the client's own pages.
    for (uint32_t row = 0; row < c->h; row++) {
        int y = c->y + (int)row;
        if (y < 0 || y >= (int)fb_h) { continue; }
        uint32_t *dst = back + (uint64_t)y * fb_w + c->x;
        const uint32_t *src = c->px + (uint64_t)row * c->stride_px;
        int w = (int)c->w;
        if (c->x + w > (int)fb_w) { w = (int)fb_w - c->x; }
        for (int col = 0; col < w; col++) { dst[col] = src[col]; }
    }
}

// A window's full extent including its decoration.
static void damage_window(struct client *c) {
    if (!c->mapped) { return; }
    if (c->is_shell) { damage(c->x, c->y, (int)c->w, (int)c->h); return; }
    damage(c->x - BORDER, c->y - TITLE_H - BORDER,
           (int)c->w + 2 * BORDER, (int)c->h + TITLE_H + 2 * BORDER);
}

static void repaint(void) {
    if (!dmg_any) { screen_dirty = 0; return; }

    // Compose the whole scene in RAM. This is cheap next to touching the
    // framebuffer, and it means the visible screen never shows a
    // partially drawn frame.
    fill(0, 0, (int)fb_w, (int)fb_h, 0x00202830);      // desktop
    // The shell is the backdrop: it goes down first and everything else
    // floats above it, whatever order the clients happened to connect in.
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && clients[i].is_shell) { draw_window(&clients[i], 0); }
    }
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0 && !clients[i].is_shell) {
            draw_window(&clients[i], i == focus_slot);
        }
    }
    draw_cursor();

    // Copy out only what changed.
    for (int y = dmg_y0; y < dmg_y1; y++) {
        const uint32_t *src = back + (uint64_t)y * fb_w + dmg_x0;
        uint32_t *dst = fb + (uint64_t)y * fb_stride_px + dmg_x0;
        for (int x = 0; x < dmg_x1 - dmg_x0; x++) { dst[x] = src[x]; }
    }
    dmg_any = 0;
    screen_dirty = 0;
}

// ---- client protocol --------------------------------------------------

static void send_to(struct client *c, int type, const void *body, int blen) {
    uint8_t buf[sizeof(struct wm_header) + WM_MAX_BODY];
    struct wm_header *h = (struct wm_header *)buf;
    h->type = (uint16_t)type; h->length = (uint16_t)blen; h->surface_id = c->id;
    if (blen) { memcpy(buf + sizeof(*h), body, (uint64_t)blen); }
    (void)write(c->fd, buf, sizeof(*h) + (unsigned)blen);
}

static void drop_client(struct client *c) {
    if (c->fd < 0) { return; }
    printf("[wm] client gone\n");
    damage_window(c);                      // the hole it leaves behind
    if (c->px) { munmap(c->px, c->bytes); c->px = 0; }
    close(c->fd);
    c->fd = -1;
    c->mapped = 0;
    c->has_surface = 0;
    screen_dirty = 1;
}

// Read exactly n bytes, or fail. A stream socket coalesces: the client's
// hello, create, attach and commit all arrive in one recvmsg if it sent
// them back to back, so a reader that takes "whatever arrived" and
// handles the first message silently drops the rest. Framing has to be
// exact.
static int read_exact(int fd, void *buf, int n) {
    uint8_t *p = buf;
    int got = 0;
    while (got < n) {
        long r = read(fd, p + got, (unsigned)(n - got));
        if (r <= 0) { return -1; }
        got += (int)r;
    }
    return 0;
}

// Read exactly n bytes AND collect any descriptor sent with them.
//
// The control buffer is supplied only on the body of a message whose
// header already said it carries one. That is not fastidiousness: NeoOS
// queues passed descriptors per direction in FIFO order and hands the
// oldest to the next recvmsg that offers a control buffer, so a reader
// that always offered one would collect the attach's fd while reading
// some earlier message's bytes.
static int read_exact_fd(int fd, void *buf, int n, int *passed_fd) {
    uint8_t cbuf[sizeof(struct k_cmsghdr_u) + 4 * sizeof(int)];
    memset(cbuf, 0, sizeof cbuf);
    struct k_iovec iov = { buf, (uint64_t)n };
    struct k_msghdr_u m;
    memset(&m, 0, sizeof m);
    m.iov = &iov; m.iovlen = 1;
    m.control = cbuf; m.controllen = sizeof cbuf;

    long r = neo6(SYS_RECVMSG, fd, (long)&m, 0, 0, 0, 0);
    if (r <= 0) { return -1; }

    if (m.controllen >= sizeof(struct k_cmsghdr_u)) {
        struct k_cmsghdr_u *cm = (struct k_cmsghdr_u *)cbuf;
        if (cm->level == 1 && cm->type == 1 &&
            cm->len >= sizeof(*cm) + sizeof(int)) {
            *passed_fd = *(int *)(cbuf + sizeof(*cm));
        }
    }
    // A short recvmsg still has to be completed, or the next header
    // starts mid-message.
    if (r < n) { return read_exact(fd, (uint8_t *)buf + r, n - (int)r); }
    return 0;
}

static int read_message(struct client *c, struct wm_header *h, uint8_t *body, int *passed_fd) {
    *passed_fd = -1;
    if (read_exact(c->fd, h, (int)sizeof *h) != 0) { return -1; }
    if (h->length > WM_MAX_BODY) { return -1; }
    if (h->length == 0) { return 1; }

    if (h->type == WM_ATTACH_BUFFER) {
        if (read_exact_fd(c->fd, body, h->length, passed_fd) != 0) { return -1; }
    } else {
        if (read_exact(c->fd, body, h->length) != 0) { return -1; }
    }
    return 1;
}

static void place_window(struct client *c) {
    // Cascade, so a second window is not hidden exactly behind the first.
    static int next_x = 60, next_y = 60;
    c->x = next_x;
    c->y = next_y;
    next_x += 40; next_y += 40;
    if (c->x + (int)c->w > (int)fb_w)  { next_x = 60; c->x = 60; }
    if (c->y + (int)c->h > (int)fb_h)  { next_y = 60; c->y = 60; }
}

static void handle_message(struct client *c, struct wm_header *h,
                           uint8_t *body, int passed_fd) {
    switch (h->type) {
    case WM_HELLO: {
        printf("[wm] client hello, version %u\n", ((struct wm_hello *)body)->version);
        // Answer with the screen geometry on surface 0: a shell needs it
        // before it can size itself.
        struct wm_configure screen = { fb_w, fb_h };
        uint32_t saved = c->id;
        c->id = 0;
        send_to(c, WM_CONFIGURE, &screen, sizeof screen);
        c->id = saved;
        break;
    }

    case WM_CREATE_SURFACE: {
        struct wm_create_surface *cs = (struct wm_create_surface *)body;
        if (cs->width == 0 || cs->height == 0 ||
            cs->width > fb_w || cs->height > fb_h) { break; }
        c->id = h->surface_id;
        c->w = cs->width; c->h = cs->height;
        c->has_surface = 1;
        c->is_shell = (cs->flags & WM_SURFACE_SHELL) != 0;
        if (c->is_shell) {
            c->x = 0; c->y = 0;         // the shell owns the whole screen
        } else {
            place_window(c);
        }
        struct wm_configure cfg = { c->w, c->h };
        send_to(c, WM_CONFIGURE, &cfg, sizeof cfg);
        printf("[wm] surface %ux%u\n", c->w, c->h);
        break;
    }

    case WM_ATTACH_BUFFER: {
        struct wm_attach_buffer *ab = (struct wm_attach_buffer *)body;
        if (!c->has_surface || passed_fd < 0) { break; }
        if (ab->format != WM_FORMAT_XRGB8888) { close(passed_fd); break; }
        if (c->px) { munmap(c->px, c->bytes); c->px = 0; }
        c->stride_px = ab->stride / 4;
        c->bytes = (uint64_t)c->stride_px * c->h * 4;
        void *p = mmap(0, c->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, passed_fd, 0);
        close(passed_fd);                 // the mapping keeps the object alive
        if (!p || (long)p < 0) { printf("[wm] buffer map failed\n"); break; }
        c->px = p;
        printf("[wm] buffer attached\n");
        break;
    }

    case WM_DAMAGE: {
        // The client's rectangle is surface-relative; the compositor
        // works in screen coordinates.
        struct wm_damage *d = (struct wm_damage *)body;
        if (c->mapped) {
            damage(c->x + d->x, c->y + d->y, (int)d->w, (int)d->h);
            screen_dirty = 1;
        }
        break;
    }

    case WM_COMMIT:
        if (c->px) {
            if (!c->mapped) {
                printf("[wm] surface mapped\n");
                c->mapped = 1;
                damage_window(c);          // decoration included, once
            }
            screen_dirty = 1;
        }
        break;

    case WM_SET_TITLE: {
        struct wm_set_title *st = (struct wm_set_title *)body;
        unsigned n = st->len < sizeof(c->title) - 1 ? st->len : sizeof(c->title) - 1;
        memcpy(c->title, st->text, n);
        c->title[n] = 0;
        printf("[wm] title \"%s\"\n", c->title);
        break;
    }

    case WM_DESTROY_SURFACE:
        drop_client(c);
        break;

    default:
        break;
    }
}

// ---- input routing ----------------------------------------------------

// Topmost surface under the point. Ordinary windows are searched
// first, in reverse connection order; the shell answers only where no
// window covers, matching what is actually drawn.
static int slot_at(int x, int y) {
    for (int pass = 0; pass < 2; pass++) {
        for (int i = MAX_CLIENTS - 1; i >= 0; i--) {
            struct client *c = &clients[i];
            if (c->fd < 0 || !c->mapped) { continue; }
            if ((pass == 0) == (c->is_shell != 0)) { continue; }
            if (x >= c->x && x < c->x + (int)c->w &&
                y >= c->y && y < c->y + (int)c->h) { return i; }
        }
    }
    return -1;
}

static void pump_mouse(void) {
    struct input_event_u ev[16];
    long n = read(mouse_fd, ev, sizeof ev);
    if (n <= 0) { return; }
    int old_x = cur_x, old_y = cur_y;
    int count = (int)(n / (long)sizeof(struct input_event_u));

    for (int i = 0; i < count; i++) {
        if (ev[i].type == EV_REL && ev[i].code == REL_X) { cur_x += ev[i].value; }
        else if (ev[i].type == EV_REL && ev[i].code == REL_Y) { cur_y += ev[i].value; }
        else if (ev[i].type == EV_KEY && ev[i].code == BTN_LEFT) {
            int hit = slot_at(cur_x, cur_y);
            if (ev[i].value == 1 && hit >= 0 && hit != focus_slot) {
                // The old and new focus both change colour.
                if (focus_slot >= 0) { damage_window(&clients[focus_slot]); }
                focus_slot = hit;
                damage_window(&clients[hit]);
                screen_dirty = 1;
            }
            if (hit >= 0) {
                struct wm_pointer_button pb = { (uint16_t)ev[i].code,
                                                (uint16_t)ev[i].value };
                send_to(&clients[hit], WM_POINTER_BUTTON, &pb, sizeof pb);
            }
        }
    }
    if (cur_x < 0) { cur_x = 0; }
    if (cur_y < 0) { cur_y = 0; }
    if (cur_x >= (int)fb_w) { cur_x = (int)fb_w - 1; }
    if (cur_y >= (int)fb_h) { cur_y = (int)fb_h - 1; }

    int hit = slot_at(cur_x, cur_y);
    if (hit >= 0) {
        struct wm_pointer_motion pm = { cur_x - clients[hit].x, cur_y - clients[hit].y };
        send_to(&clients[hit], WM_POINTER_MOTION, &pm, sizeof pm);
    }

    // Only the two cursor positions changed, so only they need copying
    // out. Repainting the whole screen for a mouse move is what made
    // this expensive as well as ugly.
    if (cur_x != old_x || cur_y != old_y) {
        damage(old_x, old_y, CURSOR_W, CURSOR_H);
        damage(cur_x, cur_y, CURSOR_W, CURSOR_H);
        screen_dirty = 1;
    }
}

static void pump_keyboard(void) {
    struct input_event_u ev[16];
    long n = read(kbd_fd, ev, sizeof ev);
    if (n <= 0) { return; }
    int count = (int)(n / (long)sizeof(struct input_event_u));
    for (int i = 0; i < count; i++) {
        if (ev[i].type != EV_KEY) { continue; }
        if (focus_slot < 0 || clients[focus_slot].fd < 0) { continue; }
        struct wm_key k = { ev[i].code, (uint16_t)ev[i].value };
        send_to(&clients[focus_slot], WM_KEY, &k, sizeof k);
    }
}

// ---- setup ------------------------------------------------------------

static int screen_open(void) {
    fb_fd = open("/dev/fb0", 2 /* O_RDWR */);
    if (fb_fd < 0) { printf("[wm] cannot open /dev/fb0\n"); return -1; }

    struct fb_var_screeninfo v;
    struct fb_fix_screeninfo f;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &v) < 0 ||
        ioctl(fb_fd, FBIOGET_FSCREENINFO, &f) < 0) {
        printf("[wm] FBIOGET_*SCREENINFO failed\n"); return -1;
    }
    if (v.bits_per_pixel != 32) {
        printf("[wm] need a 32bpp framebuffer, got %u\n", v.bits_per_pixel);
        return -1;
    }
    fb_w = v.xres; fb_h = v.yres;
    fb_stride_px = f.line_length / 4;
    fb_bytes = f.smem_len;

    // Claim the screen BEFORE mapping: the kernel refuses fb writes and
    // mmap from a process that does not own the display.
    tty_fd = open("/dev/tty0", 2);
    if (tty_fd >= 0) { ioctl(tty_fd, KDSETMODE, (void *)(long)KD_GRAPHICS); }

    fb = mmap(0, fb_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (!fb || (long)fb < 0) { printf("[wm] mmap(/dev/fb0) failed\n"); return -1; }

    // The compositing buffer. Anonymous memory, tightly packed at fb_w
    // stride -- the framebuffer's own stride may be wider, and that
    // padding is not something the compositor should carry around.
    uint64_t back_bytes = (uint64_t)fb_w * fb_h * 4;
    back = mmap(0, back_bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (!back || (long)back < 0) { printf("[wm] back buffer alloc failed\n"); return -1; }
    damage_all();

    cur_x = (int)fb_w / 2; cur_y = (int)fb_h / 2;
    printf("[wm] screen %ux%u\n", fb_w, fb_h);
    return 0;
}

static void screen_release(void) {
    if (tty_fd >= 0) { ioctl(tty_fd, KDSETMODE, (void *)(long)KD_TEXT); close(tty_fd); }
}

static int listen_open(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { printf("[wm] socket failed\n"); return -1; }
    struct sockaddr_un { uint16_t family; char path[108]; } addr;
    addr.family = AF_UNIX;
    addr.path[0] = 0;
    int n = (int)strlen(WM_SOCKET_NAME);
    memcpy(addr.path + 1, WM_SOCKET_NAME, (uint64_t)n);
    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)(sizeof(uint16_t) + 1 + n)) != 0) {
        printf("[wm] bind failed\n"); return -1;
    }
    if (listen(fd, MAX_CLIENTS) != 0) { printf("[wm] listen failed\n"); return -1; }
    printf("[wm] listening on @%s\n", WM_SOCKET_NAME);
    return fd;
}

int main(int argc, char **argv) {
    // A bounded run keeps the headless test from hanging the build; the
    // interactive case passes no argument and runs until killed.
    int max_frames = 0;
    if (argc > 1) {
        max_frames = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            max_frames = max_frames * 10 + (*p - '0');
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) { clients[i].fd = -1; }
    if (screen_open() != 0) { return 1; }

    int lfd = listen_open();
    if (lfd < 0) { screen_release(); return 1; }

    kbd_fd   = open("/dev/input/event0", 0 /* O_RDONLY */);
    mouse_fd = open("/dev/input/event1", 0);
    printf("[wm] input kbd=%d mouse=%d\n", kbd_fd, mouse_fd);
    printf("[wm] ready\n");

    int frames = 0;
    for (;;) {
        struct pollfd p[MAX_CLIENTS + 3];
        int np = 0, cidx[MAX_CLIENTS];
        p[np].fd = lfd; p[np].events = POLLIN; p[np].revents = 0; np++;
        int kslot = -1, mslot = -1;
        if (kbd_fd >= 0)   { kslot = np; p[np].fd = kbd_fd;   p[np].events = POLLIN; p[np].revents = 0; np++; }
        if (mouse_fd >= 0) { mslot = np; p[np].fd = mouse_fd; p[np].events = POLLIN; p[np].revents = 0; np++; }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd < 0) { continue; }
            cidx[np] = i;
            p[np].fd = clients[i].fd; p[np].events = POLLIN; p[np].revents = 0; np++;
        }

        int ready = poll(p, (unsigned long)np, 16);       // ~60 Hz ceiling

        if (ready > 0) {
            if (p[0].revents & POLLIN) {
                int cfd = accept(lfd, 0, 0);
                if (cfd >= 0) {
                    int slot = -1;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].fd < 0) { slot = i; break; }
                    }
                    if (slot < 0) { close(cfd); }
                    else {
                        memset(&clients[slot], 0, sizeof clients[slot]);
                        clients[slot].fd = cfd;
                        if (focus_slot < 0) { focus_slot = slot; }
                        printf("[wm] client connected\n");
                    }
                }
            }
            if (kslot >= 0 && (p[kslot].revents & POLLIN)) { pump_keyboard(); }
            if (mslot >= 0 && (p[mslot].revents & POLLIN)) { pump_mouse(); }

            for (int i = (kslot < 0 ? 1 : 0) + 1; i < np; i++) {
                if (!(p[i].revents & (POLLIN | POLLHUP))) { continue; }
                if (p[i].fd == lfd || p[i].fd == kbd_fd || p[i].fd == mouse_fd) { continue; }
                struct client *c = &clients[cidx[i]];
                // Drain: poll reports readable once for a whole batch.
                for (;;) {
                    struct wm_header h;
                    uint8_t body[WM_MAX_BODY];
                    int passed = -1;
                    int rc = read_message(c, &h, body, &passed);
                    if (rc < 0) { drop_client(c); break; }
                    handle_message(c, &h, body, passed);
                    struct pollfd more = { c->fd, POLLIN, 0 };
                    if (poll(&more, 1, 0) <= 0 || !(more.revents & POLLIN)) { break; }
                }
            }
        }

        if (screen_dirty) { repaint(); frames++; }
        if (max_frames && frames >= max_frames) {
            printf("[wm] %d frames composited, exiting\n", frames);
            break;
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) { drop_client(&clients[i]); }
    close(lfd);
    screen_release();
    return 0;
}
