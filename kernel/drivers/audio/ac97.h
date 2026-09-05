#ifndef NEOOS_AC97_H
#define NEOOS_AC97_H

#include <stdint.h>

#define VECTOR_AC97 0x23

// One BDL, 4 fixed-size segments, 4096 frames each (16-bit stereo:
// 4096 frames * 2 channels * 2 bytes = 16384 bytes/segment, ~85ms;
// ~341ms total buffer -- see spec section 3.3).
#define AC97_BDL_ENTRIES     4
#define AC97_FRAMES_PER_SEG  4096
#define AC97_BYTES_PER_SEG   (AC97_FRAMES_PER_SEG * 2 /*channels*/ * 2 /*bytes/sample*/)

struct ac97_bdl_entry {
    uint32_t buf_phys;
    uint16_t length;   // SAMPLES (16-bit words): frames * channels
    uint16_t flags;    // bit15 IOC (0x8000), bit14 BUP (0x4000)
} __attribute__((packed));
_Static_assert(sizeof(struct ac97_bdl_entry) == 8, "AC97 BDL ABI");

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

struct file_descriptor;
struct file_ops;
extern const struct file_ops ac97_control_file_ops;
extern const struct file_ops ac97_pcm_file_ops;
int ac97_control_open(struct file_descriptor *f);
int ac97_pcm_open(struct file_descriptor *f);

// "[ac97] /dev/snd selftest ..." -- exercises the file_ops surface
// directly (open/hw_params/write) in-kernel. Does not prove a real
// userland ALSA-shaped client works end-to-end through actual
// syscalls; see the plan's closing note on that scope boundary.
void ac97_snd_selftest(void);

#endif
