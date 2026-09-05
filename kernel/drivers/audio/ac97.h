#ifndef NEOOS_AC97_H
#define NEOOS_AC97_H

#include <stdint.h>

// AC97 (Intel ICH 82801AA) playback driver. QEMU's -device AC97
// emulates exactly this controller -- see
// docs/superpowers/specs/2026-09-05-ac97-audio-driver-design.md.
//
// Returns 0 if a device was found and brought up (codec responded),
// -1 if no device is present (a machine without one is valid -- not
// a FAILED condition).
int ac97_init(void);

// "[ac97] selftest ..." -- structural checks only (register state,
// codec responded, DMA completed), never an audible check.
void ac97_selftest(void);

// The PCI IRQ line this device is on, valid only after ac97_init()
// returns 0. Same shape as virtio_net_irq_line() -- kernel.c reads
// this to program the IOAPIC redirection itself; the driver does not
// touch the IOAPIC.
uint8_t ac97_irq_line(void);

// Called from kernel/arch/isr.c's VECTOR_AC97 branch.
void ac97_irq(void);

#endif
