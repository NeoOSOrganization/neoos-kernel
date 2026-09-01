#include "tty/con_driver.h"

// The console driver of last resort: probes true, discards everything.
// A boot with no framebuffer and no usable VGA text buffer still has a
// non-NULL con_driver_active().

static int  dummy_probe(void) { return 1; }
static void dummy_init(int *cols, int *rows) { *cols = 80; *rows = 25; }
static void dummy_putc(char c, uint8_t fg) { (void)c; (void)fg; }
static void dummy_clear(void) {}

struct con_driver dummycon_drv = {
    .name = "dummycon", .priority = 0,
    .probe = dummy_probe, .init = dummy_init,
    .putc_attr = dummy_putc, .clear = dummy_clear,
};
