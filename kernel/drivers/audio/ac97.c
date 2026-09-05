#include "drivers/audio/ac97.h"
#include "drivers/pci/pci.h"
#include "drivers/char/serial.h"
#include "arch/io.h"
#include <stdint.h>
#include <stddef.h>

// QEMU's -device AC97 emulates Intel's 82801AA ICH AC'97 controller.
#define AC97_VENDOR 0x8086
#define AC97_DEVICE 0x2415

// ---- NAM (Native Audio Mixer) registers -- BAR0, 16-bit each -------
#define NAM_RESET            0x00
#define NAM_MASTER_VOLUME    0x02
#define NAM_PCM_OUT_VOLUME   0x18
#define NAM_EXT_AUDIO_ID     0x2C

// ---- NABM (Native Audio Bus Master) registers -- BAR1 --------------
// Three 16-byte-wide DMA engine blocks; only PCM-OUT (offset 0x10) is
// used (this is a playback-only driver -- spec section 2).
#define NABM_PCM_OUT_BASE    0x10
#define NABM_BDBAR           0x00   // +engine base, 4 bytes
#define NABM_CIV             0x04   // +engine base, 1 byte, read-only
#define NABM_LVI             0x05   // +engine base, 1 byte
#define NABM_SR              0x06   // +engine base, 2 bytes
#define NABM_PICB            0x08   // +engine base, 2 bytes, read-only
#define NABM_PIV             0x0A   // +engine base, 1 byte, read-only
#define NABM_CR              0x0B   // +engine base, 1 byte

#define SR_DCH    0x01   // DMA controller halted
#define SR_CELV   0x02
#define SR_LVBCI  0x04   // last valid buffer completion interrupt
#define SR_BCIS   0x08   // buffer completion interrupt status (write-1-clear)
#define SR_FIFOE  0x10   // FIFO error

#define CR_RPBM   0x01   // run/pause bus master
#define CR_RR     0x02   // reset registers
#define CR_LVBIE  0x04   // last-valid-buffer interrupt enable
#define CR_FEIE   0x08   // FIFO error interrupt enable
#define CR_IOCE   0x10   // interrupt-on-completion enable

#define NABM_GLOB_CNT  0x2C   // 4 bytes
#define NABM_GLOB_STA  0x30   // 4 bytes
#define NABM_CAS       0x34   // 1 byte

#define GLOB_CNT_COLD_RESET  0x02   // set = codec released from reset
#define GLOB_STA_PCR         0x100  // primary codec ready

static uint16_t nam_base, nabm_base;   // BAR0/BAR1, I/O port space
static uint8_t  ac97_present;
static uint8_t  ac97_irq_line_val;

static uint16_t nam_read16(uint16_t reg)  { return inw((uint16_t)(nam_base + reg)); }
static void     nam_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nam_base + reg), v); }

static uint32_t nabm_read32(uint16_t reg) { return inl((uint16_t)(nabm_base + reg)); }
static void     nabm_write32(uint16_t reg, uint32_t v) { outl((uint16_t)(nabm_base + reg), v); }
static uint16_t nabm_read16(uint16_t reg) { return inw((uint16_t)(nabm_base + reg)); }
static void     nabm_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nabm_base + reg), v); }
static uint8_t  nabm_read8(uint16_t reg)  { return inb((uint16_t)(nabm_base + reg)); }
static void     nabm_write8(uint16_t reg, uint8_t v) { outb((uint16_t)(nabm_base + reg), v); }

uint8_t ac97_irq_line(void) { return ac97_irq_line_val; }

int ac97_init(void) {
    ac97_present = 0;

    const struct pci_device *pci = pci_find_id(AC97_VENDOR, AC97_DEVICE);
    if (!pci) {
        serial_write_string("[ac97] no device (not a FAILED: a machine "
                            "without one is a valid machine)\n");
        return -1;
    }

    // BAR0 = NAM (mixer), BAR1 = NABM (bus master), both I/O space.
    // pci_find_id already decoded and sized every BAR (struct pci_bar
    // has the type bits already masked off) -- use that directly
    // rather than re-reading raw config space.
    if (!pci->bar[0].is_io || !pci->bar[1].is_io) {
        serial_write_string("[ac97] FAILED: expected I/O-space BARs\n");
        return -1;
    }
    nam_base  = (uint16_t)pci->bar[0].addr;
    nabm_base = (uint16_t)pci->bar[1].addr;
    ac97_irq_line_val = pci->irq_line;

    pci_enable_bus_master(pci);

    // Cold reset: release the codec from reset by setting GLOB_CNT bit1.
    uint32_t glob_cnt = nabm_read32(NABM_GLOB_CNT);
    nabm_write32(NABM_GLOB_CNT, glob_cnt | GLOB_CNT_COLD_RESET);

    // Poll GLOB_STA.PCR (primary codec ready) -- bounded, not infinite:
    // a codec that never becomes ready is a real failure, not a hang.
    int ready = 0;
    for (int i = 0; i < 100000; i++) {
        if (nabm_read32(NABM_GLOB_STA) & GLOB_STA_PCR) { ready = 1; break; }
    }
    if (!ready) {
        serial_write_string("[ac97] FAILED: codec never became ready\n");
        return -1;
    }

    uint16_t ext_id = nam_read16(NAM_EXT_AUDIO_ID);
    serial_write_string("[ac97] codec ready, ext_audio_id=");
    serial_write_hex64(ext_id);
    serial_write_string("\n");

    // Unmute + full volume (0x0000 = loudest, bit15 = mute).
    nam_write16(NAM_MASTER_VOLUME, 0x0000);
    nam_write16(NAM_PCM_OUT_VOLUME, 0x0000);

    ac97_present = 1;
    serial_write_string("[ac97] device found\n");
    return 0;
}

void ac97_selftest(void) {
    if (!ac97_present) {
        serial_write_string("[ac97] selftest skipped (no device)\n");
        return;
    }
    uint32_t sta = nabm_read32(NABM_GLOB_STA);
    if (!(sta & GLOB_STA_PCR)) {
        serial_write_string("[ac97] selftest FAILED: codec no longer ready\n");
        return;
    }
    serial_write_string("[ac97] selftest passed\n");
}

void ac97_irq(void) {
    // Populated in Task 2, once there is a stream to acknowledge.
}
