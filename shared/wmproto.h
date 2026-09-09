#ifndef NEOOS_WMPROTO_H
#define NEOOS_WMPROTO_H
// The neoos-wm wire protocol.
//
// A NeoOS extension with no POSIX analogue, shared verbatim by the
// compositor, the C client library, and (through P/Invoke) the C#
// bindings. Deliberately NOT Wayland: that protocol is enormous and
// none of its ecosystem can reach NeoOS anyway. This is the smallest
// thing that lets one process draw into a window another process
// composites.
//
// Every message is a fixed 8-byte header followed by a fixed body, all
// little-endian. Fixed sizes mean the reader never has to parse a
// length it has not validated, which is the usual way a display server
// gets an out-of-bounds read.

#include <stdint.h>

#define WM_SOCKET_NAME  "neoos-wm"     // abstract AF_UNIX name
#define WM_PROTO_VERSION 1

// The only pixel format G4 speaks: 32bpp, opaque, no alpha, no
// blending -- the same layout /dev/fb0 uses, so compositing is a copy.
#define WM_FORMAT_XRGB8888 1

enum wm_msg_type {
    // client -> server
    WM_HELLO           = 1,
    WM_CREATE_SURFACE  = 2,
    WM_ATTACH_BUFFER   = 3,   // carries one memfd via SCM_RIGHTS
    WM_DAMAGE          = 4,
    WM_COMMIT          = 5,
    WM_SET_TITLE       = 6,
    WM_DESTROY_SURFACE = 7,
    // server -> client
    WM_CONFIGURE       = 128,
    WM_POINTER_MOTION  = 129,
    WM_POINTER_BUTTON  = 130,
    WM_KEY             = 131,
    WM_FOCUS           = 132,
    WM_CLOSE           = 133,
};

struct wm_header {
    uint16_t type;
    uint16_t length;        // bytes AFTER this header
    uint32_t surface_id;
};

struct wm_hello          { uint32_t version; };
// WM_SURFACE_SHELL marks the desktop shell: no decoration, positioned
// at the origin, and kept at the BOTTOM of the stack so ordinary
// windows float above it. There is exactly one; a second request for
// it is treated as an ordinary window.
#define WM_SURFACE_NORMAL 0
#define WM_SURFACE_SHELL  1

struct wm_create_surface { uint32_t width, height, flags; };
struct wm_attach_buffer  { uint32_t stride, format; };
struct wm_damage         { int32_t x, y; uint32_t w, h; };
struct wm_set_title      { uint16_t len; char text[62]; };
struct wm_configure      { uint32_t width, height; };
struct wm_pointer_motion { int32_t x, y; };     // surface-relative
struct wm_pointer_button { uint16_t button, state; };
struct wm_key            { uint16_t keycode, state; };
struct wm_focus          { uint32_t focused; };

// Largest body, so a reader can size one buffer and be done.
#define WM_MAX_BODY 64

#endif
