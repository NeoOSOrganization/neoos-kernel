// AHCI host bus adapters: PCI discovery, BIOS/OS handoff, HBA reset, and
// bringing up each implemented port far enough to know what is on it
// (AHCI 1.3.1 section 10.1, in libata's order).
#include "drivers/block/ahci/ahci.h"
#include "drivers/pci/pci.h"
#include "mm/mmio.h"
#include "time/ktime.h"
#include "drivers/char/serial.h"
#include "errno.h"

static struct ahci_hba  hbas[AHCI_MAX_HBAS];
static struct ahci_port ports[AHCI_MAX_HBAS][AHCI_MAX_PORTS];
// Directly attached devices take link (port, 0); a port multiplier's
// devices take (port, n). 4 HBAs x 32 ports is the ceiling, but a PMP
// port fans out to 15, so links are pooled rather than one per port.
#define AHCI_MAX_LINKS 64
static struct ahci_link links[AHCI_MAX_LINKS];
static int nlinks;
static int nhbas;

static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }

static int wait_hba(struct ahci_hba *h, uint32_t off, uint32_t mask, uint32_t value, uint64_t ns) {
    uint64_t deadline = ktime_after_ns(ns);
    for (;;) {
        if ((hba_r32(h, off) & mask) == value) { return 1; }
        if (ktime_get_ns() >= deadline) { return (hba_r32(h, off) & mask) == value; }
        cpu_pause();
    }
}

static void log_hba(struct ahci_hba *h, const char *what) {
    serial_write_string("[ahci] hba");
    ahci_log_dec((uint64_t)h->index);
    serial_write_string(": ");
    serial_write_string(what);
}

struct ahci_link *ahci_link_alloc(struct ahci_port *p, uint8_t pmp) {
    if (nlinks == AHCI_MAX_LINKS) { return 0; }
    struct ahci_link *l = &links[nlinks++];
    l->port = p;
    l->pmp = pmp;
    l->ncq_depth = 0;
    return l;
}

// Firmware may still own the controller (CAP2.BOH): ask for it, and if
// the BIOS says it is busy, give it the 2 s the spec allows. On timeout,
// carry on -- libata does too; the reset below takes it regardless.
static void bios_handoff(struct ahci_hba *h) {
    if (!(h->cap2 & AHCI_CAP2_BOH)) { return; }
    hba_w32(h, AHCI_BOHC, hba_r32(h, AHCI_BOHC) | AHCI_BOHC_OOS);
    if (wait_hba(h, AHCI_BOHC, AHCI_BOHC_BOS, 0, 25000000ULL)) { return; }
    if ((hba_r32(h, AHCI_BOHC) & AHCI_BOHC_BB) &&
        wait_hba(h, AHCI_BOHC, AHCI_BOHC_BOS, 0, 2000000000ULL)) { return; }
    log_hba(h, "BIOS handoff timed out, continuing\n");
}

static int hba_reset(struct ahci_hba *h) {
    // AE before anything else: some HBAs ignore every other GHC bit
    // until AHCI mode is on. CAP and PI are saved first because they are
    // write-once on some firmware and the reset can clear them.
    hba_w32(h, AHCI_GHC, hba_r32(h, AHCI_GHC) | AHCI_GHC_AE);
    uint32_t cap = hba_r32(h, AHCI_CAP), pi = hba_r32(h, AHCI_PI);
    hba_w32(h, AHCI_GHC, hba_r32(h, AHCI_GHC) | AHCI_GHC_HR);
    if (!wait_hba(h, AHCI_GHC, AHCI_GHC_HR, 0, 1000000000ULL)) {
        log_hba(h, "reset FAILED\n");
        return -EIO;
    }
    hba_w32(h, AHCI_GHC, hba_r32(h, AHCI_GHC) | AHCI_GHC_AE);
    if (!hba_r32(h, AHCI_CAP)) { hba_w32(h, AHCI_CAP, cap); }
    if (!hba_r32(h, AHCI_PI))  { hba_w32(h, AHCI_PI, pi); }
    // Polled driver: interrupts stay off at the HBA and at every port.
    hba_w32(h, AHCI_GHC, hba_r32(h, AHCI_GHC) & ~AHCI_GHC_IE);
    hba_w32(h, AHCI_IS, 0xFFFFFFFFu);
    return 0;
}

static void classify_and_attach(struct ahci_port *p) {
    uint32_t sig = px_r32(p, PX_SIG);
    if (p->hba->spm) {
        // With a multiplier possibly attached, the plain signature is not
        // trustworthy: pmp.c soft-resets PMP 15 and decides.
        int rc = ahci_pmp_attach(p);
        if (rc != -ENODEV) { return; }
        sig = px_r32(p, PX_SIG);
    }
    struct ahci_link *l;
    switch (sig) {
    case SIG_ATA:
        if ((l = ahci_link_alloc(p, 0))) { ahci_disk_attach(l); }
        break;
    case SIG_ATAPI:
        if ((l = ahci_link_alloc(p, 0))) { ahci_atapi_attach(l); }
        break;
    case SIG_PMP:
        ahci_log_port(p);
        serial_write_string("port multiplier on an HBA without CAP.SPM, skipped\n");
        break;
    default:
        ahci_log_port(p);
        serial_write_string(sig == SIG_SEMB ? "enclosure bridge (SEMB), skipped" : "unknown device, skipped");
        serial_write_string(" sig=");
        serial_write_hex64(sig);
        serial_write_string("\n");
        break;
    }
}

static void bring_up_port(struct ahci_hba *h, int n) {
    struct ahci_port *p = &ports[h->index][n];
    p->hba = h;
    p->num = n;
    p->regs = h->abar + 0x100 + 0x80 * (uint32_t)n;
    if (ahci_port_setup(p) != 0) { p->dead = 1; return; }
    int rc = ahci_port_comreset(p);
    if (rc != 1) { p->dead = 1; return; }
    if (ahci_port_start(p) != 0) {
        ahci_log_port(p);
        serial_write_string("would not start -- FAILED");
        ahci_log_regs(p);
        p->dead = 1;
        return;
    }
    classify_and_attach(p);
}

static void hba_init(const struct pci_device *pd) {
    if (nhbas == AHCI_MAX_HBAS) { return; }
    const struct pci_bar *bar = &pd->bar[5];
    if (bar->is_io || !bar->size) {
        serial_write_string("[ahci] controller without a memory BAR5, skipped\n");
        return;
    }
    struct ahci_hba *h = &hbas[nhbas];
    h->index = nhbas;
    h->abar = (volatile uint8_t *)mmio_map(bar->addr, bar->size);
    if (!h->abar) { log_hba(h, "cannot map ABAR -- FAILED\n"); return; }
    nhbas++;
    uint32_t cmd = pci_config_read32(pd, PCI_REG_COMMAND);
    pci_config_write32(pd, PCI_REG_COMMAND, (cmd & 0xFFFF) | PCI_CMD_MEM_SPACE);
    pci_enable_bus_master(pd);

    h->cap2 = hba_r32(h, AHCI_CAP2);
    bios_handoff(h);
    if (hba_reset(h) != 0) { return; }
    h->cap = hba_r32(h, AHCI_CAP);
    h->pi = hba_r32(h, AHCI_PI);
    h->nslots = (uint8_t)(AHCI_CAP_NCS(h->cap) + 1);
    h->s64a = (h->cap & AHCI_CAP_S64A) != 0;
    h->sncq = (h->cap & AHCI_CAP_SNCQ) != 0;
    h->spm  = (h->cap & AHCI_CAP_SPM) != 0;
    h->sclo = (h->cap & AHCI_CAP_SCLO) != 0;
    h->sss  = (h->cap & AHCI_CAP_SSS) != 0;

    log_hba(h, "version ");
    serial_write_hex64(hba_r32(h, AHCI_VS));
    serial_write_string(" slots=");
    ahci_log_dec(h->nslots);
    serial_write_string(" ports=");
    serial_write_hex64(h->pi);
    serial_write_string(h->sncq ? " ncq" : "");
    serial_write_string(h->s64a ? " 64bit" : "");
    serial_write_string(h->spm ? " pmp" : "");
    serial_write_string("\n");

    for (int n = 0; n < AHCI_MAX_PORTS; n++) {
        if (h->pi & (1u << n)) { bring_up_port(h, n); }
    }
}

void ahci_probe(void) {
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_device *pd = pci_get(i);
        if (pd->class == PCI_CLASS_STORAGE && pd->subclass == 0x06 && pd->prog_if == 0x01) {
            hba_init(pd);
        }
    }
}
