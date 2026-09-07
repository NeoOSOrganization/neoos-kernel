#include "drivers/char/pit.h"
#include "arch/io.h"
#include "arch/cpu.h"
#include "drivers/irq/lapic.h"

#define PIT_CHANNEL0_DATA 0x40
#define PIT_COMMAND       0x43
#define PIT_FREQUENCY     1193182

// One-shot stopwatch: program PIT channel 0 for a known ~10ms interval,
// start the LAPIC timer counting down from its max, and measure how far
// it counted down by the time the PIT interval elapses. The PIT is
// never touched again after this function returns.
uint32_t pit_calibrate_lapic_ticks_per_10ms(uint64_t *tsc_per_10ms_out) {
    uint16_t divisor = (uint16_t)(PIT_FREQUENCY / 100); // ~10ms

    outb(PIT_COMMAND, 0x30); // channel 0, lobyte/hibyte access, mode 0, binary
    outb(PIT_CHANNEL0_DATA, divisor & 0xFF);
    outb(PIT_CHANNEL0_DATA, (divisor >> 8) & 0xFF);

    lapic_timer_start_oneshot_max();
    uint64_t tsc0 = rdtsc();

    uint8_t status;
    do {
        outb(PIT_COMMAND, 0xE2); // read-back command: latch status for channel 0
        status = inb(PIT_CHANNEL0_DATA);
    } while (!(status & 0x80)); // bit 7 = OUT pin; goes high at terminal count (mode 0)

    uint64_t tsc1 = rdtsc();
    uint32_t remaining = lapic_timer_stop_and_read();
    if (tsc_per_10ms_out) { *tsc_per_10ms_out = tsc1 - tsc0; }
    return 0xFFFFFFFF - remaining;
}
