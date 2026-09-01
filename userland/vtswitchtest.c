// Kernel virtual terminals: /dev/tty1..6 are independent terminals,
// /dev/tty0 follows whichever is active, the VT_*/KD* ioctls work, and
// Alt+Fn switches from the keyboard.

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <neoos_test.h>

#define VT_OPENQRY   0x5600
#define VT_GETSTATE  0x5603
#define VT_ACTIVATE  0x5606
#define KDSETMODE    0x4B3A
#define KDGETMODE    0x4B3B
#define KD_TEXT      0
#define KD_GRAPHICS  1
#define TIOCGWINSZ   0x5413

#define KEY_LALT 56
#define KEY_F1   59
#define KEY_F2   60

struct vt_stat { unsigned short v_active, v_signal, v_state; };
struct winsize { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };

static int t0, t2;

static int active(void) {
    struct vt_stat st;
    st.v_active = 0;
    if (ioctl(t0, VT_GETSTATE, &st) != 0) { return -1; }
    return st.v_active;
}

int main(void) {
    // A background VT is a real terminal: opening and writing to it
    // must succeed even though nothing of it is on screen.
    t2 = open("/dev/tty2", O_RDWR);
    if (t2 < 0) { printf("[vtswitchtest] FAILED: open /dev/tty2 %d\n", t2); return 1; }
    if (!isatty(t2)) { printf("[vtswitchtest] FAILED: /dev/tty2 is not a tty\n"); return 1; }
    if (write(t2, "on VT2\n", 7) != 7) {
        printf("[vtswitchtest] FAILED: write to a background VT\n"); return 1;
    }

    t0 = open("/dev/tty0", O_RDWR);
    if (t0 < 0) { printf("[vtswitchtest] FAILED: open /dev/tty0 %d\n", t0); return 1; }

    // An ioctl outside the VT_*/KD* set must fall through to the
    // ordinary terminal handler rather than failing.
    struct winsize ws;
    ws.ws_col = 0;
    if (ioctl(t2, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
        printf("[vtswitchtest] FAILED: TIOCGWINSZ through a VT fd (col=%d)\n", ws.ws_col);
        return 1;
    }
    printf("[vtswitchtest] tty device + ioctl fall-through passed\n");

    if (active() != 1) { printf("[vtswitchtest] FAILED: boot VT is %d, want 1\n", active()); return 1; }

    if (ioctl(t0, VT_ACTIVATE, (void *)2) != 0) {
        printf("[vtswitchtest] FAILED: VT_ACTIVATE 2\n"); return 1;
    }
    if (active() != 2) { printf("[vtswitchtest] FAILED: v_active=%d after activate\n", active()); return 1; }

    int q = 0;
    if (ioctl(t0, VT_OPENQRY, &q) != 0 || q < 1 || q > 6) {
        printf("[vtswitchtest] FAILED: VT_OPENQRY gave %d\n", q); return 1;
    }
    printf("[vtswitchtest] VT_ACTIVATE / VT_GETSTATE / VT_OPENQRY passed\n");

    // KD_GRAPHICS stops the kernel painting this VT; it must round-trip
    // and must not disturb which VT is active.
    int mode = -1;
    if (ioctl(t0, KDSETMODE, (void *)KD_GRAPHICS) != 0 ||
        ioctl(t0, KDGETMODE, &mode) != 0 || mode != KD_GRAPHICS) {
        printf("[vtswitchtest] FAILED: KDSETMODE/KDGETMODE gave %d\n", mode); return 1;
    }
    ioctl(t0, KDSETMODE, (void *)KD_TEXT);
    if (ioctl(t0, KDGETMODE, &mode) != 0 || mode != KD_TEXT || active() != 2) {
        printf("[vtswitchtest] FAILED: KD_TEXT restore\n"); return 1;
    }
    printf("[vtswitchtest] KDSETMODE / KDGETMODE passed\n");

    // Alt+F1 from the keyboard switches back. Test-hook builds only:
    // a production kernel has no way to inject a keystroke.
    if (neoos_test_inject_key(KEY_LALT, 0) == 0) {
        neoos_test_inject_key(KEY_LALT, 1);
        neoos_test_inject_key(KEY_F1, 1);
        neoos_test_inject_key(KEY_F1, 0);
        neoos_test_inject_key(KEY_LALT, 0);
        if (active() != 1) {
            printf("[vtswitchtest] FAILED: Alt+F1 -> VT %d\n", active()); return 1;
        }
        // ...and F2 with no Alt held is an ordinary keystroke, not a
        // switch. It reaches the line discipline instead.
        neoos_test_inject_key(KEY_F2, 1);
        neoos_test_inject_key(KEY_F2, 0);
        if (active() != 1) {
            printf("[vtswitchtest] FAILED: bare F2 switched to VT %d\n", active()); return 1;
        }
        printf("[vtswitchtest] Alt+F1 intercept passed\n");
    } else {
        printf("[vtswitchtest] switch-key check skipped (production kernel)\n");
        ioctl(t0, VT_ACTIVATE, (void *)1);
    }

    if (active() != 1) { printf("[vtswitchtest] FAILED: did not end on VT 1\n"); return 1; }

    close(t2);
    close(t0);
    printf("[vtswitchtest] ALL PASSED\n");
    return 0;
}
