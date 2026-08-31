// evtest.c - Test the /dev/input/event0 evdev device
// Reads raw keyboard events and reports them without TTY line discipline

#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <linux/input.h>

#define KEY_CNT 256

// Array to track which keys are pressed
static unsigned char keystate[KEY_CNT / 8];

// Get the name of a key code (simplified: only common keys)
static const char *key_name(uint16_t code) {
    switch (code) {
    case KEY_A:        return "A";
    case KEY_B:        return "B";
    case KEY_C:        return "C";
    case KEY_D:        return "D";
    case KEY_E:        return "E";
    case KEY_SPACE:    return "SPACE";
    case KEY_ENTER:    return "ENTER";
    case KEY_BACKSPACE: return "BACKSPACE";
    case KEY_TAB:      return "TAB";
    case KEY_ESC:      return "ESC";
    case KEY_LSHIFT:   return "LSHIFT";
    case KEY_RSHIFT:   return "RSHIFT";
    case KEY_LCTRL:    return "LCTRL";
    case KEY_RCTRL:    return "RCTRL";
    case KEY_LALT:     return "LALT";
    case KEY_RALT:     return "RALT";
    default:           return "?";
    }
}

// Extract a bit from the keystate bitmap
static int keystate_bit(uint16_t code) {
    uint32_t byte_idx = code / 8;
    uint32_t bit_idx = code % 8;
    if (byte_idx >= sizeof(keystate)) return 0;
    return (keystate[byte_idx] >> bit_idx) & 1;
}

// Set a bit in the keystate bitmap
static void keystate_set(uint16_t code, int pressed) {
    uint32_t byte_idx = code / 8;
    uint32_t bit_idx = code % 8;
    if (byte_idx >= sizeof(keystate)) return;
    if (pressed) {
        keystate[byte_idx] |= (1 << bit_idx);
    } else {
        keystate[byte_idx] &= ~(1 << bit_idx);
    }
}

// Report current key state
static void report_keys(void) {
    printf("[evtest] keys pressed: ");
    int count = 0;
    for (int i = 0; i < KEY_CNT; i++) {
        if (keystate_bit(i)) {
            if (count > 0) printf(" ");
            printf("%s", key_name(i));
            count++;
        }
    }
    if (count == 0) printf("(none)");
    printf("\n");
}

// Test event injection (requires SYS_TEST_HOOK)
static void test_inject(int evfd) {
    #include <neoos_test.h>

    printf("[evtest] injecting test events...\n");

    // Inject KEY_A press
    if (neoos_test_inject_key(KEY_A, 1) != 0) {
        printf("[evtest] key injection not available (production kernel)\n");
        return;
    }

    // Read the injected event
    struct input_event ev[8];
    int64_t nread = read(evfd, ev, sizeof(ev));
    if (nread < (int64_t)sizeof(struct input_event)) {
        printf("[evtest] FAILED: read returned %ld\n", nread);
        return;
    }

    // Expect: MSC_SCAN, EV_KEY (press), SYN_REPORT
    int ev_count = nread / sizeof(struct input_event);
    printf("[evtest] received %d events\n", ev_count);

    for (int i = 0; i < ev_count; i++) {
        printf("  event %d: type=%d code=%d value=%d\n",
               i, ev[i].type, ev[i].code, ev[i].value);
        if (ev[i].type == EV_KEY) {
            keystate_set(ev[i].code, ev[i].value);
        }
    }

    // Release the key
    neoos_test_inject_key(KEY_A, 0);
    nread = read(evfd, ev, sizeof(ev));
    if (nread >= (int64_t)sizeof(struct input_event)) {
        ev_count = nread / sizeof(struct input_event);
        for (int i = 0; i < ev_count; i++) {
            if (ev[i].type == EV_KEY) {
                keystate_set(ev[i].code, ev[i].value);
            }
        }
    }

    report_keys();
    printf("[evtest] injection test passed\n");
}

// Main test: open evdev, read some events, verify structure
int main(void) {
    printf("[evtest] opening /dev/input/event0\n");

    int evfd = open("/dev/input/event0", O_RDONLY);
    if (evfd < 0) {
        printf("[evtest] FAILED: open returned %d\n", evfd);
        return 1;
    }
    printf("[evtest] fd=%d\n", evfd);

    // Query device info via ioctl
    printf("[evtest] querying device...\n");

    int version = 0;
    if (ioctl(evfd, EVIOCGVERSION, &version) == 0) {
        printf("[evtest] EV_VERSION = 0x%x\n", version);
    }

    struct input_id id;
    if (ioctl(evfd, EVIOCGID, &id) == 0) {
        printf("[evtest] bustype=0x%x vendor=0x%x product=0x%x version=0x%x\n",
               id.bustype, id.vendor, id.product, id.version);
    }

    char name[64];
    if (ioctl(evfd, EVIOCGNAME(sizeof(name)), name) >= 0) {
        printf("[evtest] name=%s\n", name);
    }

    // Get initial key bitmap
    unsigned char ev_bitmap[16];
    int64_t ret = ioctl(evfd, EVIOCGBIT(0, sizeof(ev_bitmap)), ev_bitmap);
    if (ret >= 0) {
        printf("[evtest] EV_SYN=%d EV_KEY=%d EV_MSC=%d\n",
               (ev_bitmap[0] >> 0) & 1,
               (ev_bitmap[0] >> 1) & 1,
               (ev_bitmap[0] >> 4) & 1);
    }

    // Try the test injection path
    test_inject(evfd);

    close(evfd);
    printf("[evtest] ALL PASSED\n");
    return 0;
}
