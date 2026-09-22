// wmwait -- sleeps a few seconds, then exits. Inserted as a `wait`
// inittab entry between `spawn /wm.nex` and `wait /wmdemo.nex` in the
// wm-* dev-convenience Makefile targets (`wm`, `wm-glass`, `wm-cursor`,
// `wm-cursor-fallback`, `wm-taskbar`): WM.ELF is now dynamically
// linked (real Mesa/OSMesa, see
// docs/superpowers/specs/2026-09-22-wm-mesa-everywhere-design.md) and
// needs several seconds to load ~16.5MB of runtime .so's off NeoOS's
// ATA-PIO disk driver before it reaches listen() -- wmdemo's own
// connect() is a single, un-retried attempt (wire protocol/client
// code, out of scope for that migration), so without a head start it
// races wm.nex's startup and loses. This buys that head start without
// touching wmclient.c/wmdemo.c or leaving the boot to hang on
// BOOT_TIMEOUT (see the design spec's "Deviations found during
// implementation" section for the measurement behind the 5s figure).
#include <time.h>

int main(void) {
    struct timespec req = { .tv_sec = 5, .tv_nsec = 0 };
    nanosleep(&req, 0);
    return 0;
}
