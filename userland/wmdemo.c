// userland/wmdemo.c -- the compositor's smoke test.
//
// A client with no toolkit at all: connect, make a window, fill it with
// a known pattern, commit, log what the compositor sends back. It
// exists so that "does the compositor work" and "does LVGL work" are
// never the same question.

#include "wmclient.h"
#include "wmproto.h"
#include <stdio.h>
#include <unistd.h>

// argv[1], when present, is how many poll iterations to hold the window
// open for. The screenshot run needs it alive long enough to be caught.
int main(int argc, char **argv) {
    int hold = 120;
    if (argc > 1) {
        hold = 0;
        for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++) {
            hold = hold * 10 + (*p - '0');
        }
    }
    struct wm_conn *c = wm_connect();
    if (!c) { printf("[wmdemo] cannot reach the compositor\n"); return 1; }
    printf("[wmdemo] connected\n");

    int32_t id = wm_create_window(c, 320, 200, "wmdemo");
    if (id < 0) { printf("[wmdemo] create window failed\n"); return 1; }
    printf("[wmdemo] surface %d\n", id);

    uint32_t *px = wm_pixels(c);
    uint32_t stride = wm_stride_px(c);
    if (!px) { printf("[wmdemo] no pixels\n"); return 1; }

    // A gradient with a solid border: wrong strides and off-by-one
    // blits are obvious in it, which a flat fill would hide.
    for (uint32_t y = 0; y < 200; y++) {
        for (uint32_t x = 0; x < 320; x++) {
            uint32_t v = (x < 3 || y < 3 || x >= 317 || y >= 197)
                       ? 0x00FFFFFF
                       : (((x * 255 / 320) << 16) | ((y * 255 / 200) << 8) | 0x80);
            px[y * stride + x] = v;
        }
    }
    wm_damage(c, 0, 0, 320, 200);
    wm_commit(c);
    printf("[wmdemo] committed\n");

    // Drain events briefly, then leave -- this is a smoke test, not an
    // application.
    for (int i = 0; i < hold && wm_alive(c); i++) {
        int t, a, b;
        while (wm_poll_event(c, &t, &a, &b) == 1) {
            printf("[wmdemo] event type=%d a=%d b=%d\n", t, a, b);
        }
        struct timespec { long s, ns; } ts = { 0, 16000000 };
        (void)ts;
        for (volatile int spin = 0; spin < 200000; spin++) { }
    }
    printf("[wmdemo] done\n");
    wm_disconnect(c);
    return 0;
}
