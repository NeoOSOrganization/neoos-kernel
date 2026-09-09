// userland/wmclient.c -- client library for neoos-wm.

#include "wmclient.h"
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

#define SYS_SENDMSG      89
#define SYS_MEMFD_CREATE 135
#define SYS_FTRUNCATE    134

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

struct k_iovec  { void *base; uint64_t len; };
struct k_msghdr_u {
    void    *name;   uint32_t namelen, _p0;
    struct k_iovec *iov; uint64_t iovlen;
    void    *control; uint64_t controllen;
    uint32_t flags, _p1;
};
struct k_cmsghdr_u { uint64_t len; int level, type; };

struct wm_conn {
    int       fd;
    int       memfd;
    uint32_t  id;
    uint32_t  w, h;
    uint32_t  screen_w, screen_h;
    uint32_t *px;
    int       alive;
};

static struct wm_conn g_conn;

static int send_msg(struct wm_conn *c, int type, uint32_t sid,
                    const void *body, int blen) {
    uint8_t buf[sizeof(struct wm_header) + WM_MAX_BODY];
    struct wm_header *h = (struct wm_header *)buf;
    h->type = (uint16_t)type;
    h->length = (uint16_t)blen;
    h->surface_id = sid;
    if (blen) { memcpy(buf + sizeof(*h), body, (uint64_t)blen); }
    int total = (int)sizeof(*h) + blen;
    return write(c->fd, buf, (unsigned)total) == total ? 0 : -1;
}

struct wm_conn *wm_connect(void) {
    struct wm_conn *c = &g_conn;
    c->fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (c->fd < 0) { return 0; }

    // Abstract namespace: a leading NUL, then the name, addrlen
    // measuring exactly that -- no terminator.
    struct sockaddr_un { uint16_t family; char path[108]; } addr;
    addr.family = AF_UNIX;
    addr.path[0] = 0;
    int n = (int)strlen(WM_SOCKET_NAME);
    memcpy(addr.path + 1, WM_SOCKET_NAME, (uint64_t)n);
    if (connect(c->fd, (struct sockaddr *)&addr,
                (socklen_t)(sizeof(uint16_t) + 1 + n)) != 0) {
        close(c->fd); c->fd = -1; return 0;
    }

    struct wm_hello hello = { WM_PROTO_VERSION };
    if (send_msg(c, WM_HELLO, 0, &hello, sizeof hello) != 0) {
        close(c->fd); c->fd = -1; return 0;
    }

    // The compositor answers a hello with the screen geometry, on
    // surface 0. A shell needs it before it can size itself, and every
    // client benefits from knowing it up front rather than guessing.
    struct wm_header h;
    struct wm_configure cfg = { 0, 0 };
    if (read(c->fd, &h, sizeof h) == (long)sizeof h &&
        h.type == WM_CONFIGURE && h.length == sizeof cfg) {
        if (read(c->fd, &cfg, sizeof cfg) == (long)sizeof cfg) {
            c->screen_w = cfg.width;
            c->screen_h = cfg.height;
        }
    }

    c->alive = 1;
    return c;
}

uint32_t wm_screen_width(struct wm_conn *c)  { return c ? c->screen_w : 0; }
uint32_t wm_screen_height(struct wm_conn *c) { return c ? c->screen_h : 0; }

static int32_t create_surface(struct wm_conn *c, uint32_t w, uint32_t h,
                              const char *title, uint32_t flags) {
    if (!c || !c->alive) { return -1; }
    c->w = w; c->h = h;
    c->id = 1;

    struct wm_create_surface cs = { w, h, flags };
    if (send_msg(c, WM_CREATE_SURFACE, c->id, &cs, sizeof cs) != 0) { return -1; }

    const char *nm = "wm-surface";
    c->memfd = (int)neo6(SYS_MEMFD_CREATE, (long)nm, (long)strlen(nm), 0, 0, 0, 0);
    if (c->memfd < 0) { return -1; }
    uint64_t bytes = (uint64_t)w * h * 4;
    if (neo6(SYS_FTRUNCATE, c->memfd, (long)bytes, 0, 0, 0, 0) != 0) { return -1; }
    c->px = mmap(0, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, c->memfd, 0);
    if (!c->px || (long)c->px < 0) { return -1; }

    // The descriptor rides a one-byte payload: the compositor reads the
    // ATTACH_BUFFER header first and takes the fd from that recvmsg.
    struct wm_attach_buffer ab = { w * 4, WM_FORMAT_XRGB8888 };
    uint8_t hdr[sizeof(struct wm_header) + sizeof(ab)];
    struct wm_header *hh = (struct wm_header *)hdr;
    hh->type = WM_ATTACH_BUFFER; hh->length = sizeof(ab); hh->surface_id = c->id;
    memcpy(hdr + sizeof(*hh), &ab, sizeof ab);

    uint8_t cbuf[sizeof(struct k_cmsghdr_u) + sizeof(int)];
    struct k_cmsghdr_u *cm = (struct k_cmsghdr_u *)cbuf;
    cm->len = sizeof(*cm) + sizeof(int);
    cm->level = 1;                      // SOL_SOCKET
    cm->type  = 1;                      // SCM_RIGHTS
    *(int *)(cbuf + sizeof(*cm)) = c->memfd;

    struct k_iovec iov = { hdr, sizeof hdr };
    struct k_msghdr_u m;
    memset(&m, 0, sizeof m);
    m.iov = &iov; m.iovlen = 1;
    m.control = cbuf; m.controllen = sizeof cbuf;
    if (neo6(SYS_SENDMSG, c->fd, (long)&m, 0, 0, 0, 0) != (long)sizeof hdr) { return -1; }

    if (title && *title) {
        struct wm_set_title st;
        memset(&st, 0, sizeof st);
        int tl = (int)strlen(title);
        if (tl > (int)sizeof(st.text) - 1) { tl = (int)sizeof(st.text) - 1; }
        st.len = (uint16_t)tl;
        memcpy(st.text, title, (uint64_t)tl);
        send_msg(c, WM_SET_TITLE, c->id, &st, sizeof st);
    }
    return (int32_t)c->id;
}

int32_t wm_create_window(struct wm_conn *c, uint32_t w, uint32_t h, const char *title) {
    return create_surface(c, w, h, title, WM_SURFACE_NORMAL);
}

int32_t wm_create_shell(struct wm_conn *c, uint32_t w, uint32_t h) {
    if (!c) { return -1; }
    if (w == 0) { w = c->screen_w; }
    if (h == 0) { h = c->screen_h; }
    return create_surface(c, w, h, 0, WM_SURFACE_SHELL);
}

uint32_t *wm_pixels(struct wm_conn *c)   { return c ? c->px : 0; }
uint32_t  wm_stride_px(struct wm_conn *c){ return c ? c->w : 0; }
int       wm_alive(struct wm_conn *c)    { return c && c->alive; }

void wm_damage(struct wm_conn *c, int32_t x, int32_t y, uint32_t w, uint32_t h) {
    if (!c || !c->alive) { return; }
    struct wm_damage d = { x, y, w, h };
    send_msg(c, WM_DAMAGE, c->id, &d, sizeof d);
}

void wm_commit(struct wm_conn *c) {
    if (!c || !c->alive) { return; }
    if (send_msg(c, WM_COMMIT, c->id, 0, 0) != 0) { c->alive = 0; }
}

int wm_poll_event(struct wm_conn *c, int *type, int *a, int *b) {
    if (!c || !c->alive) { return -1; }
    struct pollfd p = { c->fd, POLLIN, 0 };
    if (poll(&p, 1, 0) <= 0) { return 0; }

    struct wm_header h;
    long n = read(c->fd, &h, sizeof h);
    if (n == 0) { c->alive = 0; return -1; }
    if (n != (long)sizeof h) { return 0; }

    uint8_t body[WM_MAX_BODY];
    if (h.length) {
        if (h.length > WM_MAX_BODY) { c->alive = 0; return -1; }
        if (read(c->fd, body, h.length) != h.length) { return 0; }
    }
    *type = h.type;
    *a = *b = 0;
    switch (h.type) {
    case WM_POINTER_MOTION: { struct wm_pointer_motion *m = (void *)body; *a = m->x; *b = m->y; break; }
    case WM_POINTER_BUTTON: { struct wm_pointer_button *m = (void *)body; *a = m->button; *b = m->state; break; }
    case WM_KEY:            { struct wm_key *m = (void *)body; *a = m->keycode; *b = m->state; break; }
    case WM_FOCUS:          { struct wm_focus *m = (void *)body; *a = (int)m->focused; break; }
    case WM_CONFIGURE:      { struct wm_configure *m = (void *)body; *a = (int)m->width; *b = (int)m->height; break; }
    case WM_CLOSE:          c->alive = 0; break;
    default: break;
    }
    return 1;
}

void wm_disconnect(struct wm_conn *c) {
    if (!c) { return; }
    if (c->px) { munmap(c->px, (uint64_t)c->w * c->h * 4); c->px = 0; }
    if (c->memfd >= 0) { close(c->memfd); c->memfd = -1; }
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    c->alive = 0;
}
