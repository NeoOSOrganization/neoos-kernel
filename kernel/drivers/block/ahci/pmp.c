// SATA port multipliers behind AHCI (command-based switching).
//
// UNVERIFIED: no hardware or emulator exercises this file. QEMU emulates
// no port multiplier and its ICH9 AHCI never sets CAP.SPM, so no boot
// here reaches ahci_pmp_attach. It follows AHCI 1.3.1 section 9 and the
// SATA Port Multiplier 1.2 specification, in libata's order
// (sata_pmp_attach). Only the FIS encoders are tested (ahci_selftest).
// See docs/abi-compatibility.md.
#include "drivers/block/ahci/ahci.h"
#include "time/ktime.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define PMP_CTRL        15      // the multiplier's own control port
#define GSCR_PROD_ID    0
#define GSCR_REV        1
#define GSCR_PORT_INFO  2
#define GSCR_ERROR_EN   33
#define PSCR_SSTATUS    0
#define PSCR_SERROR     1
#define PSCR_SCONTROL   2
#define PMP_MAX_PORTS   15

static void zero(uint8_t *p, int n) { for (int i = 0; i < n; i++) { p[i] = 0; } }
static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }

void ahci_fis_pmp_read(uint8_t fis[20], uint8_t port, uint16_t reg) {
    zero(fis, 20);
    fis[0] = 0x27;
    fis[1] = 0x80 | PMP_CTRL;
    fis[2] = ATA_READ_PM;
    fis[3] = (uint8_t)reg;
    fis[11] = (uint8_t)(reg >> 8);
    fis[7] = port & 0xF;
}

void ahci_fis_pmp_write(uint8_t fis[20], uint8_t port, uint16_t reg, uint32_t value) {
    ahci_fis_pmp_read(fis, port, reg);
    fis[2] = ATA_WRITE_PM;
    fis[12] = (uint8_t)value;
    fis[4] = (uint8_t)(value >> 8);
    fis[5] = (uint8_t)(value >> 16);
    fis[6] = (uint8_t)(value >> 24);
}

// The D2H register FIS the device last sent, at +0x40 in the received
// FIS area: its COUNT and LBA bytes carry a signature after a reset and
// a register value after READ PORT MULTIPLIER.
static uint32_t d2h_value(struct ahci_port *p) {
    const volatile uint8_t *d = p->rfis + 0x40;
    return (uint32_t)d[12] | (uint32_t)d[4] << 8 | (uint32_t)d[5] << 16 | (uint32_t)d[6] << 24;
}

static int pmp_read(struct ahci_port *p, uint8_t port, uint16_t reg, uint32_t *out) {
    struct ahci_link l = { p, PMP_CTRL, 0 };
    struct ahci_req r;
    ahci_req_init(&r);
    ahci_fis_pmp_read(r.fis, port, reg);
    int rc = ahci_exec(&l, &r);
    if (rc == 0) { *out = d2h_value(p); }
    return rc;
}

static int pmp_write(struct ahci_port *p, uint8_t port, uint16_t reg, uint32_t value) {
    struct ahci_link l = { p, PMP_CTRL, 0 };
    struct ahci_req r;
    ahci_req_init(&r);
    ahci_fis_pmp_write(r.fis, port, reg, value);
    return ahci_exec(&l, &r);
}

// Software reset aimed at one PMP port: an H2D control FIS with SRST
// set (command header R = reset, C = clear busy on R_OK), then one with
// it clear. The device answers with a D2H FIS carrying its signature.
// Returns the signature, or 0.
static uint32_t soft_reset(struct ahci_port *p, uint8_t pmp) {
    struct ahci_link l = { p, pmp, 0 };
    struct ahci_req r;
    ahci_req_init(&r);
    r.fis[0] = 0x27;
    r.fis[1] = pmp & 0xF;                       // C clear: a control FIS
    r.fis[15] = 0x04;                           // SRST
    r.hdr_extra = (1u << 8) | (1u << 10);       // R, C
    if (ahci_exec(&l, &r) != 0) { return 0; }
    uint64_t t = ktime_after_ns(10000ULL);      // SRST held >= 5 us
    while (ktime_get_ns() < t) { cpu_pause(); }
    ahci_req_init(&r);
    r.fis[0] = 0x27;
    r.fis[1] = pmp & 0xF;
    r.fis[15] = 0x00;
    if (ahci_exec(&l, &r) != 0) { return 0; }
    if (!ahci_wait_reg(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 10000000000ULL)) { return 0; }
    return d2h_value(p);
}

static void set_pma(struct ahci_port *p, int on) {
    ahci_port_stop(p);
    uint32_t cmd = px_r32(p, PX_CMD);
    px_w32(p, PX_CMD, on ? (cmd | PX_CMD_PMA) : (cmd & ~PX_CMD_PMA));
    ahci_port_start(p);
}

// Hard reset of one fan-out port through its SControl, then wait for its
// link. Returns 1 when a device is there.
static int fanout_link_up(struct ahci_port *p, uint8_t n) {
    uint32_t v;
    if (pmp_write(p, n, PSCR_SCONTROL, 0x301) != 0) { return 0; }
    uint64_t t = ktime_after_ns(2000000ULL);
    while (ktime_get_ns() < t) { cpu_pause(); }
    if (pmp_write(p, n, PSCR_SCONTROL, 0x300) != 0) { return 0; }
    uint64_t deadline = ktime_after_ns(1000000000ULL);
    for (;;) {
        if (pmp_read(p, n, PSCR_SSTATUS, &v) != 0) { return 0; }
        if ((v & 0xF) == 3) { break; }
        if (ktime_get_ns() >= deadline) { return 0; }
    }
    pmp_write(p, n, PSCR_SERROR, 0xFFFFFFFFu);
    return 1;
}

int ahci_pmp_attach(struct ahci_port *p) {
    if (!p->hba->spm) { return -ENODEV; }
    set_pma(p, 1);
    uint32_t sig = soft_reset(p, PMP_CTRL);
    if (sig != SIG_PMP) {
        // A plain device: back to direct addressing, and a reset at PMP 0
        // so PxSIG describes it for the caller.
        set_pma(p, 0);
        soft_reset(p, 0);
        return -ENODEV;
    }

    uint32_t id = 0, rev = 0, info = 0;
    if (pmp_read(p, PMP_CTRL, GSCR_PROD_ID, &id) || pmp_read(p, PMP_CTRL, GSCR_REV, &rev) ||
        pmp_read(p, PMP_CTRL, GSCR_PORT_INFO, &info)) {
        ahci_log_port(p);
        serial_write_string("port multiplier GSCR read FAILED\n");
        return -EIO;
    }
    // Asynchronous error notification off: nothing here listens for it.
    pmp_write(p, PMP_CTRL, GSCR_ERROR_EN, 0);
    uint32_t nports = info & 0xF;
    if (nports > PMP_MAX_PORTS) { nports = PMP_MAX_PORTS; }
    p->pmp_attached = 1;

    ahci_log_port(p);
    serial_write_string("port multiplier, ");
    ahci_log_dec(nports);
    serial_write_string(" ports, id=");
    serial_write_hex64(id);
    serial_write_string(" (unverified driver path)\n");

    for (uint8_t n = 0; n < nports; n++) {
        if (!fanout_link_up(p, n)) { continue; }
        uint32_t s = soft_reset(p, n);
        struct ahci_link *l = ahci_link_alloc(p, n);
        if (!l) { break; }
        ahci_log_port(p);
        serial_write_string("pmp port ");
        ahci_log_dec(n);
        serial_write_string(": ");
        if (s == SIG_ATA) {
            serial_write_string("disk\n");
            ahci_disk_attach(l);
        } else if (s == SIG_ATAPI) {
            serial_write_string("ATAPI\n");
            ahci_atapi_attach(l);
        } else {
            serial_write_string("unknown sig ");
            serial_write_hex64(s);
            serial_write_string(", skipped\n");
        }
    }
    return 0;
}
