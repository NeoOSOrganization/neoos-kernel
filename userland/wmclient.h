#ifndef NEOOS_WMCLIENT_H
#define NEOOS_WMCLIENT_H
// Client library for neoos-wm.
//
// Per CLAUDE.md this is a NeoOS-native facility with no POSIX analogue,
// so it gets a real wrapper rather than leaving callers to hand-roll
// the wire protocol. The C# bindings reach these same functions through
// NativeAOT's DirectPInvoke, so the signatures stay blittable: no
// structs by value, no callbacks, plain integers and pointers.

#include <stdint.h>

struct wm_conn;

// Connect to the running compositor. Returns 0 on failure.
struct wm_conn *wm_connect(void);
void            wm_disconnect(struct wm_conn *c);

// Create a window and its backing buffer in one step: the buffer is a
// memfd, sized w*h*4, mapped writable here and handed to the compositor
// by descriptor. Returns the surface id, or a negative errno.
int32_t  wm_create_window(struct wm_conn *c, uint32_t w, uint32_t h, const char *title);

// Like wm_create_window, but for the desktop shell: undecorated, at the
// origin, behind everything else. Pass 0 for w/h to fill the screen.
int32_t  wm_create_shell(struct wm_conn *c, uint32_t w, uint32_t h);

// The screen size, valid after wm_connect. A shell uses it to lay
// itself out.
uint32_t wm_screen_width(struct wm_conn *c);
uint32_t wm_screen_height(struct wm_conn *c);

// The pixels. XRGB8888, w*4 bytes per row, valid until wm_disconnect.
uint32_t *wm_pixels(struct wm_conn *c);
uint32_t  wm_stride_px(struct wm_conn *c);

// Mark a rectangle changed and push it. Damage without commit does
// nothing; the compositor repaints only what it is told about.
void wm_damage(struct wm_conn *c, int32_t x, int32_t y, uint32_t w, uint32_t h);
void wm_commit(struct wm_conn *c);

// Events, drained without blocking. Returns 1 and fills the out
// parameters when one was waiting, 0 when none was, -1 if the
// compositor is gone. `type` is a wm_msg_type; a and b are the body's
// two fields, whatever they mean for that type.
int wm_poll_event(struct wm_conn *c, int *type, int *a, int *b);

// 1 while the compositor is still there. A client loop is
// `while (wm_alive(c)) { ... }`.
int wm_alive(struct wm_conn *c);

#endif
