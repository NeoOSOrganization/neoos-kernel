#include "drivers/audio/ac97.h"
#include "drivers/audio/snd_abi.h"
#include "drivers/pci/pci.h"
#include "drivers/char/serial.h"
#include "arch/io.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "errno.h"
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

static struct ac97_bdl_entry *bdl;     // 4 entries, one page, never freed
static uint64_t bdl_phys;
static uint8_t *seg_virt[AC97_BDL_ENTRIES];
static uint64_t seg_phys[AC97_BDL_ENTRIES];
static volatile uint32_t next_write_seg;   // next segment software may fill
static volatile uint32_t irq_count;        // observed by the selftest
static struct waitq  space_waitq;
static struct spinlock ac97_lock;

static uint16_t nam_read16(uint16_t reg)  { return inw((uint16_t)(nam_base + reg)); }
static void     nam_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nam_base + reg), v); }

static uint32_t nabm_read32(uint16_t reg) { return inl((uint16_t)(nabm_base + reg)); }
static void     nabm_write32(uint16_t reg, uint32_t v) { outl((uint16_t)(nabm_base + reg), v); }
static uint16_t nabm_read16(uint16_t reg) { return inw((uint16_t)(nabm_base + reg)); }
static void     nabm_write16(uint16_t reg, uint16_t v) { outw((uint16_t)(nabm_base + reg), v); }
static uint8_t  nabm_read8(uint16_t reg)  { return inb((uint16_t)(nabm_base + reg)); }
static void     nabm_write8(uint16_t reg, uint8_t v) { outb((uint16_t)(nabm_base + reg), v); }

uint8_t ac97_irq_line(void) { return ac97_irq_line_val; }

// Programs the PCM-OUT DMA engine with the BDL and starts it.
static void ac97_start_dma(void) {
    nabm_write32(NABM_PCM_OUT_BASE + NABM_BDBAR, (uint32_t)bdl_phys);
    nabm_write8(NABM_PCM_OUT_BASE + NABM_LVI, AC97_BDL_ENTRIES - 1);
    nabm_write8(NABM_PCM_OUT_BASE + NABM_CR, CR_RPBM | CR_IOCE);
}

// Fills segment `seg` with silence (used to arm buffers before the
// first real write, so DMA has valid data to play immediately).
static void ac97_fill_silence(uint32_t seg) {
    uint8_t *p = seg_virt[seg];
    for (uint32_t i = 0; i < AC97_BYTES_PER_SEG; i++) { p[i] = 0; }
}

static void ac97_setup_bdl(void) {
    bdl_phys = pmm_alloc(0);      // one 4KiB page: 4 * 8-byte entries fits easily
    bdl = (struct ac97_bdl_entry *)phys_to_virt(bdl_phys);

    // One contiguous pool for all 4 segments, never freed -- same
    // style as virtio_net's rx_pool_phys.
    uint64_t need = (uint64_t)AC97_BDL_ENTRIES * AC97_BYTES_PER_SEG;
    unsigned order = 0;
    while (((uint64_t)4096 << order) < need) { order++; }
    uint64_t pool_phys = pmm_alloc(order);
    uint8_t *pool_virt = (uint8_t *)phys_to_virt(pool_phys);

    for (int i = 0; i < AC97_BDL_ENTRIES; i++) {
        seg_phys[i] = pool_phys + (uint64_t)i * AC97_BYTES_PER_SEG;
        seg_virt[i] = pool_virt + (uint64_t)i * AC97_BYTES_PER_SEG;
        bdl[i].buf_phys = (uint32_t)seg_phys[i];
        bdl[i].length   = AC97_FRAMES_PER_SEG * 2;   // samples, not bytes
        bdl[i].flags    = 0x8000;                    // IOC on every segment
        ac97_fill_silence((uint32_t)i);
    }

    waitq_init(&space_waitq);
    spin_init(&ac97_lock, LOCK_RANK_AC97, "ac97");
    next_write_seg = 0;
    irq_count = 0;

    ac97_start_dma();
}

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

    ac97_setup_bdl();

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

    // Write one segment's worth of a simple 480 Hz square wave (100
    // samples high, 100 low, at 48kHz -- integer-only, no FPU: this
    // kernel is built -mno-sse) and wait for its completion IRQ. This
    // is the structural proof DMA actually moves data, matching
    // fb_device_selftest's "prove the driver's real path works, not
    // just that a function pointer is non-NULL" standard.
    uint32_t before = irq_count;
    uint16_t *samples = (uint16_t *)seg_virt[0];
    for (uint32_t i = 0; i < AC97_FRAMES_PER_SEG; i++) {
        int16_t v = ((i / 100) % 2) ? 8000 : -8000;
        samples[i * 2 + 0] = (uint16_t)v;   // left
        samples[i * 2 + 1] = (uint16_t)v;   // right
    }

    // This runs before kernel.c's global `sti` (the scheduler is not
    // driving thread wakeups yet), so waiting via waitq_sleep would
    // never actually be woken by the completion IRQ -- schedule()
    // would switch away with no interrupt able to ever switch back.
    // Bracket a narrow interrupts-enabled polling window instead, the
    // same pattern kernel/smp/tlb.c's shootdown wait uses for the
    // identical problem (needing one hardware interrupt to arrive
    // during early boot, before the scheduler is live).
    uint64_t caller_flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(caller_flags) :: "memory");
    __asm__ volatile ("sti");
    int spins = 0;
    while (irq_count == before) {
        __asm__ volatile ("pause");
        if (++spins > 50000000) { break; }
    }
    if (!(caller_flags & (1ULL << 9))) { __asm__ volatile ("cli"); }

    if (irq_count == before) {
        serial_write_string("[ac97] selftest FAILED: no completion interrupt\n");
        return;
    }
    serial_write_string("[ac97] selftest passed\n");
}

void ac97_irq(void) {
    uint16_t sr = nabm_read16(NABM_PCM_OUT_BASE + NABM_SR);
    if (sr & SR_BCIS) {
        nabm_write16(NABM_PCM_OUT_BASE + NABM_SR, SR_BCIS);   // write-1-to-clear
        irq_count++;
        waitq_wake_all(&space_waitq);
    }
    if (sr & SR_FIFOE) {
        nabm_write16(NABM_PCM_OUT_BASE + NABM_SR, SR_FIFOE);
    }
}

// ---- /dev/snd/controlC0 ---------------------------------------------
// Opens cleanly, answers PVERSION/CARD_INFO with real values, -ENOTTY
// otherwise -- there is no mixer tree to expose yet (documented
// limitation, see docs/stdlib.md).

static int64_t ctl_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f;
    if (request == SNDRV_CTL_IOCTL_PVERSION) {
        *(int *)arg = 0x02000108;   // ALSA protocol 2.0.8, matches real cards' typical value
        return 0;
    }
    if (request == SNDRV_CTL_IOCTL_CARD_INFO) {
        struct snd_ctl_card_info *ci = (struct snd_ctl_card_info *)arg;
        for (uint64_t i = 0; i < sizeof(*ci); i++) { ((uint8_t *)ci)[i] = 0; }
        ci->card = 0;
        const char id[] = "AC97";
        const char driver[] = "neoos-ac97";
        const char name[] = "AC97 (NeoOS)";
        for (unsigned i = 0; i < sizeof(id); i++) { ci->id[i] = (unsigned char)id[i]; }
        for (unsigned i = 0; i < sizeof(driver); i++) { ci->driver[i] = (unsigned char)driver[i]; }
        for (unsigned i = 0; i < sizeof(name); i++) { ci->name[i] = (unsigned char)name[i]; }
        return 0;
    }
    return -ENOTTY;
}

static int64_t ctl_read(struct file_descriptor *f, void *b, uint64_t n) { (void)f;(void)b;(void)n; return 0; }
static int64_t ctl_write(struct file_descriptor *f, const void *b, uint64_t n) { (void)f;(void)b; return (int64_t)n; }
static int64_t ctl_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t ctl_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }
static int     ctl_poll(struct file_descriptor *f, int e) { (void)f; return e; }
static void    ctl_dup(struct file_descriptor *f) { (void)f; }
static void    ctl_close(struct file_descriptor *f) { (void)f; }

const struct file_ops ac97_control_file_ops = {
    .name = "ac97-control", .read = ctl_read, .write = ctl_write,
    .lseek = ctl_lseek, .getdents = ctl_getdents, .ioctl = ctl_ioctl,
    .poll = ctl_poll, .dup = ctl_dup, .close = ctl_close,
};

int ac97_control_open(struct file_descriptor *f) {
    if (!ac97_present) { return -ENODEV; }
    f->ops = &ac97_control_file_ops;
    f->priv = 0;
    return 0;
}

// ---- /dev/snd/pcmC0D0p ----------------------------------------------
// Fixed format only: S16_LE, 2 channels, 48000 Hz. hw_params for
// anything else is -EINVAL -- see Global Constraints.

static int pcm_started;

static int64_t pcm_hw_params(struct snd_pcm_hw_params *hp) {
    struct snd_mask *fmt_mask = &hp->masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    if (!(fmt_mask->bits[0] & (1u << SNDRV_PCM_FORMAT_S16_LE))) { return -EINVAL; }

    struct snd_interval *ch = &hp->intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (ch->min > 2 || ch->max < 2) { return -EINVAL; }

    struct snd_interval *rate = &hp->intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    if (rate->min > 48000 || rate->max < 48000) { return -EINVAL; }

    // Pin every negotiated value to the one fixed format -- no
    // interval-refinement algorithm, just "this is the only answer".
    fmt_mask->bits[0] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    ch->min = ch->max = 2; ch->integer = 1;
    rate->min = rate->max = 48000; rate->integer = 1;
    hp->fifo_size = AC97_FRAMES_PER_SEG;
    return 0;
}

static int64_t pcm_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    (void)f;
    if (request == SNDRV_PCM_IOCTL_PVERSION) { *(int *)arg = 0x02000108; return 0; }
    if (request == SNDRV_PCM_IOCTL_HW_PARAMS) { return pcm_hw_params((struct snd_pcm_hw_params *)arg); }
    if (request == SNDRV_PCM_IOCTL_SW_PARAMS) { return 0; }   // fixed buffer/period sizes, nothing to negotiate
    if (request == SNDRV_PCM_IOCTL_PREPARE) { next_write_seg = 0; return 0; }
    if (request == SNDRV_PCM_IOCTL_START) { pcm_started = 1; return 0; }
    if (request == SNDRV_PCM_IOCTL_DROP) { pcm_started = 0; return 0; }
    return -ENOTTY;
}

static int64_t pcm_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    if (!ac97_present) { return -ENODEV; }
    const uint8_t *src = (const uint8_t *)buf;
    uint64_t written = 0;

    uint64_t fl = spin_lock_irqsave(&ac97_lock);
    while (written < len) {
        uint64_t chunk = len - written;
        if (chunk > AC97_BYTES_PER_SEG) { chunk = AC97_BYTES_PER_SEG; }

        // Block until the segment we are about to overwrite is not the
        // one DMA is currently reading from -- CIV is read-only
        // hardware state, so "safe to write" means "not CIV".
        while (next_write_seg == nabm_read8(NABM_PCM_OUT_BASE + NABM_CIV)) {
            if (waitq_sleep(&space_waitq, &ac97_lock) < 0) {
                spin_unlock_irqrestore(&ac97_lock, fl);
                return written ? (int64_t)written : -EINTR;
            }
        }

        for (uint64_t i = 0; i < chunk; i++) { seg_virt[next_write_seg][i] = src[written + i]; }
        next_write_seg = (next_write_seg + 1) % AC97_BDL_ENTRIES;
        written += chunk;
    }
    spin_unlock_irqrestore(&ac97_lock, fl);
    return (int64_t)written;
}

static int64_t pcm_read(struct file_descriptor *f, void *b, uint64_t n) { (void)f;(void)b;(void)n; return -EINVAL; }
static int64_t pcm_lseek(struct file_descriptor *f, int64_t o, int w) { (void)f;(void)o;(void)w; return -ESPIPE; }
static int64_t pcm_getdents(struct file_descriptor *f, void *b, int n) { (void)f;(void)b;(void)n; return -ENOTDIR; }
static int     pcm_poll(struct file_descriptor *f, int e) { (void)f; return e & 0x4 /* POLLOUT */; }
static void    pcm_dup(struct file_descriptor *f) { (void)f; }
static void    pcm_close(struct file_descriptor *f) { (void)f; pcm_started = 0; }

const struct file_ops ac97_pcm_file_ops = {
    .name = "ac97-pcm", .read = pcm_read, .write = pcm_write,
    .lseek = pcm_lseek, .getdents = pcm_getdents, .ioctl = pcm_ioctl,
    .poll = pcm_poll, .dup = pcm_dup, .close = pcm_close,
};

int ac97_pcm_open(struct file_descriptor *f) {
    if (!ac97_present) { return -ENODEV; }
    f->ops = &ac97_pcm_file_ops;
    f->priv = 0;
    return 0;
}

void ac97_snd_selftest(void) {
    if (!ac97_present) {
        serial_write_string("[ac97] /dev/snd selftest skipped (no device)\n");
        return;
    }

    struct snd_pcm_hw_params hp;
    for (uint64_t i = 0; i < sizeof(hp); i++) { ((uint8_t *)&hp)[i] = 0; }
    hp.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1u << SNDRV_PCM_FORMAT_S16_LE);
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_CHANNELS - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 2;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min = 48000;
    hp.intervals[SNDRV_PCM_HW_PARAM_RATE - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].max = 48000;

    if (pcm_hw_params(&hp) != 0) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: hw_params rejected the fixed format\n");
        return;
    }

    // Wrong format must be rejected -- proves this isn't a rubber-stamp accept.
    struct snd_pcm_hw_params bad = hp;
    bad.masks[SNDRV_PCM_HW_PARAM_FORMAT - SNDRV_PCM_HW_PARAM_FIRST_MASK].bits[0] = (1u << 0 /* S8 */);
    if (pcm_hw_params(&bad) == 0) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: hw_params accepted an unsupported format\n");
        return;
    }

    // pcm_write() blocks (via waitq_sleep) if next_write_seg == CIV --
    // correct once the scheduler is live, but this selftest runs during
    // early boot, before kernel.c's global `sti` (the same problem
    // ac97_selftest() hit and fixed with a bounded sti-poll instead of
    // waitq_sleep). Guarantee CIV has moved off segment 0 first, the
    // same way, so pcm_write's internal check passes immediately and
    // never actually needs to block.
    uint64_t caller_flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(caller_flags) :: "memory");
    __asm__ volatile ("sti");
    int spins = 0;
    while (nabm_read8(NABM_PCM_OUT_BASE + NABM_CIV) == next_write_seg) {
        __asm__ volatile ("pause");
        if (++spins > 50000000) { break; }
    }
    if (!(caller_flags & (1ULL << 9))) { __asm__ volatile ("cli"); }

    uint16_t tone[AC97_FRAMES_PER_SEG * 2];
    for (uint32_t i = 0; i < AC97_FRAMES_PER_SEG; i++) {
        int16_t v = ((i / 100) % 2) ? 8000 : -8000;
        tone[i * 2 + 0] = (uint16_t)v; tone[i * 2 + 1] = (uint16_t)v;
    }
    int64_t rc = pcm_write(0, tone, sizeof(tone));
    if (rc != (int64_t)sizeof(tone)) {
        serial_write_string("[ac97] /dev/snd selftest FAILED: write() short or errored\n");
        return;
    }

    serial_write_string("[ac97] /dev/snd selftest passed\n");
}
