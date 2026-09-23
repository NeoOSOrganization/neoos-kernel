// The AHCI command engine: per-port DMA memory, port start/stop/reset,
// scatter-gather, and a polled multi-slot submit/wait with native
// command queuing and error recovery.
//
// Concurrency model. A port's lock covers its slot table, the CI/SACT
// writes, completion scanning and recovery -- never a wait. Submitters
// and waiters spin OUTSIDE the lock, calling port_poll() each time
// round, so whichever CPU happens to be polling completes everyone's
// requests and runs recovery for all of them.
#include "drivers/block/ahci/ahci.h"
#include "block/blockdev.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "time/ktime.h"
#include "drivers/char/serial.h"
#include "errno.h"

static void zero(void *p, uint64_t n) { uint8_t *d = (uint8_t *)p; for (uint64_t i = 0; i < n; i++) { d[i] = 0; } }
static void cpy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst; const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) { d[i] = s[i]; }
}
static inline void cpu_pause(void) { __asm__ volatile("pause" ::: "memory"); }
// Command tables and headers are ordinary write-back memory; CI is an
// uncached register. x86 keeps stores in order, but the compiler must
// not sink the table writes past the doorbell.
static inline void dma_wmb(void) { __asm__ volatile("sfence" ::: "memory"); }

static uint32_t max_inflight[AHCI_MAX_HBAS][AHCI_MAX_PORTS];

// ---- logging ----------------------------------------------------------

void ahci_log_dec(uint64_t v) {
    char b[21];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) { serial_putc(b[--n]); }
}

void ahci_log_port(struct ahci_port *p) {
    serial_write_string("[ahci] ");
    if (p->hba->index) { serial_write_string("hba"); ahci_log_dec((uint64_t)p->hba->index); serial_write_string(" "); }
    serial_write_string("port ");
    ahci_log_dec((uint64_t)p->num);
    serial_write_string(": ");
}

static void reg(const char *name, uint32_t v) {
    serial_write_string(name);
    serial_write_hex64(v);
}

void ahci_log_regs(struct ahci_port *p) {
    reg(" cmd=", px_r32(p, PX_CMD));
    reg(" tfd=", px_r32(p, PX_TFD));
    reg(" ssts=", px_r32(p, PX_SSTS));
    reg(" serr=", px_r32(p, PX_SERR));
    reg(" is=", px_r32(p, PX_IS));
    reg(" ci=", px_r32(p, PX_CI));
    reg(" sact=", px_r32(p, PX_SACT));
    serial_write_string("\n");
}

uint32_t ahci_port_max_inflight(struct ahci_port *p) { return max_inflight[p->hba->index][p->num]; }

// ---- lifecycle --------------------------------------------------------

int ahci_wait_reg(struct ahci_port *p, uint32_t off, uint32_t mask, uint32_t value, uint64_t ns) {
    uint64_t deadline = ktime_after_ns(ns);
    for (;;) {
        if ((px_r32(p, off) & mask) == value) { return 1; }
        if (ktime_get_ns() >= deadline) { return (px_r32(p, off) & mask) == value; }
        cpu_pause();
    }
}

static void delay_ns(uint64_t ns) {
    uint64_t deadline = ktime_after_ns(ns);
    while (ktime_get_ns() < deadline) { cpu_pause(); }
}

void ahci_port_stop(struct ahci_port *p) {
    px_w32(p, PX_CMD, px_r32(p, PX_CMD) & ~PX_CMD_ST);
    if (!ahci_wait_reg(p, PX_CMD, PX_CMD_CR, 0, 500000000ULL)) {
        ahci_log_port(p);
        serial_write_string("command list never stopped");
        ahci_log_regs(p);
    }
}

// Stops the port entirely (ST and FRE), per AHCI 1.3.1 section 10.1.2,
// before its memory pointers may be changed.
static int port_idle(struct ahci_port *p) {
    uint32_t cmd = px_r32(p, PX_CMD);
    if (cmd & (PX_CMD_ST | PX_CMD_CR)) {
        px_w32(p, PX_CMD, cmd & ~PX_CMD_ST);
        if (!ahci_wait_reg(p, PX_CMD, PX_CMD_CR, 0, 500000000ULL)) { return 0; }
    }
    cmd = px_r32(p, PX_CMD);
    if (cmd & (PX_CMD_FRE | PX_CMD_FR)) {
        px_w32(p, PX_CMD, cmd & ~PX_CMD_FRE);
        if (!ahci_wait_reg(p, PX_CMD, PX_CMD_FR, 0, 500000000ULL)) { return 0; }
    }
    return 1;
}

int ahci_port_setup(struct ahci_port *p) {
    spin_init(&p->lock, LOCK_RANK_DRIVER, "ahci-port");
    p->active_pmp = -1;
    if (!port_idle(p)) {
        ahci_log_port(p);
        serial_write_string("would not go idle -- FAILED");
        ahci_log_regs(p);
        return -EIO;
    }
    p->mem_phys = pmm_alloc(0);
    p->ct_phys = pmm_alloc(4);
    if (!p->mem_phys || !p->ct_phys) {
        if (p->mem_phys) { pmm_free(p->mem_phys, 0); }
        if (p->ct_phys) { pmm_free(p->ct_phys, 4); }
        ahci_log_port(p);
        serial_write_string("no memory for command structures -- FAILED\n");
        return -ENOMEM;
    }
    p->cl = (uint8_t *)phys_to_virt(p->mem_phys);
    p->rfis = p->cl + 0x800;
    p->ct = (uint8_t *)phys_to_virt(p->ct_phys);
    zero(p->cl, 4096);
    zero(p->ct, 32ULL * AHCI_CT_SIZE);

    // Every header points at its own table once, for good.
    for (int s = 0; s < 32; s++) {
        uint32_t *h = (uint32_t *)(p->cl + s * 32);
        uint64_t ctba = p->ct_phys + (uint64_t)s * AHCI_CT_SIZE;
        h[2] = (uint32_t)ctba;
        h[3] = (uint32_t)(ctba >> 32);
    }
    px_w32(p, PX_CLB, (uint32_t)p->mem_phys);
    px_w32(p, PX_CLBU, (uint32_t)(p->mem_phys >> 32));
    px_w32(p, PX_FB, (uint32_t)(p->mem_phys + 0x800));
    px_w32(p, PX_FBU, (uint32_t)((p->mem_phys + 0x800) >> 32));
    px_w32(p, PX_SERR, 0xFFFFFFFFu);
    px_w32(p, PX_IS, 0xFFFFFFFFu);
    px_w32(p, PX_IE, 0);
    px_w32(p, PX_CMD, px_r32(p, PX_CMD) | PX_CMD_FRE);
    if (p->hba->sss) {
        // Staggered spin-up: this port only, now.
        px_w32(p, PX_CMD, px_r32(p, PX_CMD) | PX_CMD_SUD);
    }
    return 0;
}

// COMRESET, then wait for the link and the device. Returns 1 when a
// device is present and ready, 0 when the port is empty (not a failure),
// -EIO when a device answered and then never became ready.
int ahci_port_comreset(struct ahci_port *p) {
    // DET=1 for at least 1 ms, with partial/slumber disabled (IPM=3),
    // as libata's sata_link_hardreset does.
    uint32_t sctl = px_r32(p, PX_SCTL) & ~0xFFFu;
    px_w32(p, PX_SCTL, sctl | 0x301);
    delay_ns(2000000ULL);
    px_w32(p, PX_SCTL, sctl | 0x300);
    // libata's debounce: done as soon as DET has held one value for
    // 100 ms (an empty port settles at 0 at once), or at 1 s. DET=3 is a
    // link with a device on it.
    uint64_t deadline = ktime_after_ns(1000000000ULL);
    uint32_t det = px_r32(p, PX_SSTS) & 0xF;
    uint64_t stable_since = ktime_get_ns();
    while (det != 3 && ktime_get_ns() < deadline && ktime_get_ns() - stable_since < 100000000ULL) {
        cpu_pause();
        uint32_t now = px_r32(p, PX_SSTS) & 0xF;
        if (now != det) { det = now; stable_since = ktime_get_ns(); }
    }
    if (det != 3) {
        ahci_log_port(p);
        serial_write_string(det == 0 ? "no device\n" : "link down\n");
        return 0;
    }
    px_w32(p, PX_SERR, 0xFFFFFFFFu);
    // Real disks spin up here; QEMU is ready at once.
    if (!ahci_wait_reg(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 10000000000ULL)) {
        // A port multiplier answers with BSY until it is addressed as
        // PMP 15 -- the caller's soft reset does that, so this is only a
        // failure when the HBA cannot do port multipliers at all.
        if (!p->hba->spm) {
            ahci_log_port(p);
            serial_write_string("device never ready -- FAILED");
            ahci_log_regs(p);
            return -EIO;
        }
    }
    return 1;
}

int ahci_port_start(struct ahci_port *p) {
    if (!ahci_wait_reg(p, PX_CMD, PX_CMD_CR, 0, 500000000ULL)) { return -EIO; }
    px_w32(p, PX_IS, 0xFFFFFFFFu);
    px_w32(p, PX_CMD, px_r32(p, PX_CMD) | PX_CMD_FRE | PX_CMD_ST);
    return 0;
}

// ---- FIS and PRDT -----------------------------------------------------

static int is_lba28_cmd(uint8_t cmd) {
    return cmd == ATA_READ_DMA || cmd == ATA_WRITE_DMA || cmd == ATA_FLUSH_CACHE;
}

void ahci_fis_rw(uint8_t fis[20], uint8_t cmd, uint64_t lba, uint32_t count, uint8_t pmp) {
    zero(fis, 20);
    fis[0] = 0x27;                              // H2D register
    fis[1] = (uint8_t)(0x80 | (pmp & 0xF));     // C: this is a command
    fis[2] = cmd;
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = 0x40;                              // LBA mode
    if (is_lba28_cmd(cmd)) {
        fis[7] |= (uint8_t)((lba >> 24) & 0x0F);
    } else {
        fis[8] = (uint8_t)(lba >> 24);
        fis[9] = (uint8_t)(lba >> 32);
        fis[10] = (uint8_t)(lba >> 40);
    }
    fis[12] = (uint8_t)count;
    fis[13] = (uint8_t)(count >> 8);
}

void ahci_fis_ncq(uint8_t fis[20], int write, uint64_t lba, uint32_t count, int tag, int fua, uint8_t pmp) {
    zero(fis, 20);
    fis[0] = 0x27;
    fis[1] = (uint8_t)(0x80 | (pmp & 0xF));
    fis[2] = write ? ATA_WRITE_FPDMA_QUEUED : ATA_READ_FPDMA_QUEUED;
    fis[3] = (uint8_t)count;                    // sector count lives in FEATURES
    fis[11] = (uint8_t)(count >> 8);
    fis[4] = (uint8_t)lba;
    fis[5] = (uint8_t)(lba >> 8);
    fis[6] = (uint8_t)(lba >> 16);
    fis[7] = (uint8_t)(0x40 | (fua ? 0x80 : 0));
    fis[8] = (uint8_t)(lba >> 24);
    fis[9] = (uint8_t)(lba >> 32);
    fis[10] = (uint8_t)(lba >> 40);
    fis[12] = (uint8_t)(tag << 3);              // the tag lives in COUNT[7:3]
}

static void put_prd(uint8_t *table, int i, uint64_t pa, uint32_t n) {
    uint32_t *e = (uint32_t *)(table + 0x80 + i * 16);
    e[0] = (uint32_t)pa;
    e[1] = (uint32_t)(pa >> 32);
    e[2] = 0;
    e[3] = (n - 1) & 0x3FFFFF;                  // byte count - 1; I (bit 31) clear
}

int ahci_build_prdt(uint8_t *table, const void *buf, uint32_t len, int s64a) {
    uint64_t va = (uint64_t)(uintptr_t)buf;
    if ((va & 1) || (len & 1)) { return -EINVAL; }
    int n = 0;
    uint64_t run_pa = 0, run_len = 0;
    while (len) {
        uint64_t pa = paging_translate_current(va);
        if (!pa) { return -EIO; }
        uint32_t chunk = 4096 - (uint32_t)(va & 0xFFF);
        if (chunk > len) { chunk = len; }
        if (run_len && run_pa + run_len == pa && run_len + chunk <= AHCI_PRD_MAX_BYTES) {
            run_len += chunk;
        } else {
            if (run_len) {
                if (n == AHCI_PRDT_MAX) { return -E2BIG; }
                if (!s64a && run_pa + run_len > (1ULL << 32)) { return -EIO; }
                put_prd(table, n++, run_pa, (uint32_t)run_len);
            }
            run_pa = pa;
            run_len = chunk;
        }
        va += chunk;
        len -= chunk;
    }
    if (run_len) {
        if (n == AHCI_PRDT_MAX) { return -E2BIG; }
        if (!s64a && run_pa + run_len > (1ULL << 32)) { return -EIO; }
        put_prd(table, n++, run_pa, (uint32_t)run_len);
    }
    return n;
}

// ---- submit / poll / wait ---------------------------------------------

void ahci_req_init(struct ahci_req *r) { zero(r, sizeof *r); r->slot = -1; }

// Caller holds the lock. Finishes one slot's request.
static void complete_slot(struct ahci_port *p, int s, int status) {
    struct ahci_req *r = p->slot_req[s];
    uint32_t bit = 1u << s;
    p->slot_req[s] = 0;
    p->busy &= ~bit;
    p->ncq_busy &= ~bit;
    if (r && !r->ncq) { p->exclusive = 0; }
    if (!p->busy) { p->active_pmp = -1; }
    if (r) {
        uint32_t tfd = px_r32(p, PX_TFD);
        r->tfd_status = (uint8_t)tfd;
        r->tfd_error = (uint8_t)(tfd >> 8);
        r->status = status;
        r->slot = -1;
        __asm__ volatile("" ::: "memory");
        r->done = 1;
    }
}

// Caller holds the lock; the request has been checked issuable. Builds
// the table and header for slot `s` and rings the doorbell.
static int issue_locked(struct ahci_port *p, struct ahci_req *r, int s) {
    uint8_t *t = p->ct + (uint64_t)s * AHCI_CT_SIZE;
    uint8_t pmp = r->link ? r->link->pmp : 0;
    int prdtl = ahci_build_prdt(t, r->buf, r->len, p->hba->s64a);
    if (prdtl < 0) { return prdtl; }
    if (r->ncq) { r->fis[12] = (uint8_t)(s << 3); }   // tag == slot
    cpy(t, r->fis, 20);
    zero(t + 0x40, 16);
    if (r->atapi) { cpy(t + 0x40, r->cdb, 16); }

    uint32_t *h = (uint32_t *)(p->cl + s * 32);
    h[0] = 5u                                   // CFL: a 20-byte FIS
         | (r->atapi ? 1u << 5 : 0)
         | (r->write ? 1u << 6 : 0)
         | ((uint32_t)(pmp & 0xF) << 12)
         | r->hdr_extra
         | ((uint32_t)prdtl << 16);
    h[1] = 0;                                   // PRDBC: bytes transferred
    uint32_t bit = 1u << s;
    p->slot_req[s] = r;
    p->busy |= bit;
    if (r->ncq) { p->ncq_busy |= bit; } else { p->exclusive = 1; }
    p->active_pmp = pmp;
    r->slot = s;
    dma_wmb();
    if (r->ncq) { px_w32(p, PX_SACT, bit); }
    px_w32(p, PX_CI, bit);

    uint32_t n = 0;
    for (uint32_t b = p->busy; b; b &= b - 1) { n++; }
    uint32_t *mx = &max_inflight[p->hba->index][p->num];
    if (n > *mx) { *mx = n; }
    return 0;
}

static void ahci_recover_locked(struct ahci_port *p);

// Caller holds the lock. Completes finished slots; runs recovery first
// when the port has stopped on an error.
static void port_poll_locked(struct ahci_port *p) {
    if (!p->busy) { return; }
    if (px_r32(p, PX_IS) & PX_IS_ERRORS) {
        ahci_recover_locked(p);
        return;
    }
    uint32_t pending = px_r32(p, PX_CI) | px_r32(p, PX_SACT);
    for (uint32_t b = p->busy & ~pending; b; b &= b - 1) {
        complete_slot(p, __builtin_ctz(b), 0);
    }
}

static int ncq_count(uint32_t m) { int n = 0; for (; m; m &= m - 1) { n++; } return n; }

// Caller holds the lock. The lowest free slot, or -1.
static int free_slot(struct ahci_port *p) {
    for (int s = 0; s < p->hba->nslots; s++) {
        if (!(p->busy & (1u << s))) { return s; }
    }
    return -1;
}

// Caller holds the lock. Whether `r` may go out now, per the queueing
// rules: a non-queued command runs alone; NCQ runs beside other NCQ on
// the same link, up to the device's depth; a port multiplier's links
// (command-based switching) take turns.
static int can_issue(struct ahci_port *p, struct ahci_req *r) {
    uint8_t pmp = r->link ? r->link->pmp : 0;
    if (p->exclusive) { return 0; }
    if (p->busy && p->active_pmp >= 0 && p->active_pmp != pmp) { return 0; }
    if (!r->ncq) { return p->busy == 0; }
    if (p->exclusive_waiting) { return 0; }
    if (ncq_count(p->ncq_busy) >= (r->link ? r->link->ncq_depth : 1)) { return 0; }
    return free_slot(p) >= 0;
}

int ahci_submit(struct ahci_link *l, struct ahci_req *r) {
    struct ahci_port *p = l->port;
    r->link = l;
    r->done = 0;
    r->status = 0;
    r->slot = -1;
    r->next = 0;
    r->deadline = ktime_after_ns(AHCI_CMD_TIMEOUT_NS);
    int waiting = 0;
    for (;;) {
        uint64_t f = spin_lock_irqsave(&p->lock);
        if (p->dead) {
            if (waiting) { p->exclusive_waiting--; }
            spin_unlock_irqrestore(&p->lock, f);
            return -EIO;
        }
        port_poll_locked(p);
        if (can_issue(p, r)) {
            if (waiting) { p->exclusive_waiting--; }
            int rc = issue_locked(p, r, free_slot(p));
            spin_unlock_irqrestore(&p->lock, f);
            return rc;
        }
        if (!r->ncq && !waiting) { p->exclusive_waiting++; waiting = 1; }
        spin_unlock_irqrestore(&p->lock, f);
        if (ktime_get_ns() >= r->deadline) {
            f = spin_lock_irqsave(&p->lock);
            if (waiting) { p->exclusive_waiting--; }
            spin_unlock_irqrestore(&p->lock, f);
            ahci_log_port(p);
            serial_write_string("no slot within the command timeout -- FAILED\n");
            return -ETIMEDOUT;
        }
        cpu_pause();
    }
}

// Caller holds the lock. The port hung: reset it and fail everything.
static void timeout_locked(struct ahci_port *p) {
    ahci_log_port(p);
    serial_write_string("command timeout -- FAILED");
    ahci_log_regs(p);
    ahci_port_stop(p);
    for (int s = 0; s < 32; s++) {
        if (p->busy & (1u << s)) { complete_slot(p, s, -ETIMEDOUT); }
    }
    for (struct ahci_req *q = p->requeue; q; ) {
        struct ahci_req *n = q->next;
        q->status = -ETIMEDOUT;
        q->done = 1;
        q = n;
    }
    p->requeue = 0;
    p->exclusive = 0;
    if (ahci_port_comreset(p) != 1 || ahci_port_start(p) != 0) {
        ahci_log_port(p);
        serial_write_string("did not come back after reset -- FAILED\n");
        p->dead = 1;
    }
}

int ahci_wait(struct ahci_link *l, struct ahci_req *r) {
    struct ahci_port *p = l->port;
    for (;;) {
        if (r->done) { __asm__ volatile("" ::: "memory"); return r->status; }
        uint64_t f = spin_lock_irqsave(&p->lock);
        port_poll_locked(p);
        if (!r->done && ktime_get_ns() >= r->deadline) { timeout_locked(p); }
        spin_unlock_irqrestore(&p->lock, f);
        if (!r->done) { cpu_pause(); }
    }
}

int ahci_exec(struct ahci_link *l, struct ahci_req *r) {
    int rc = ahci_submit(l, r);
    return rc ? rc : ahci_wait(l, r);
}

// ---- error recovery ---------------------------------------------------

// AHCI 1.3.1 section 6.2.2. Caller holds the lock. For now every request
// in flight fails; NCQ tag analysis and reissue come with the error
// recovery task.
static void ahci_recover_locked(struct ahci_port *p) {
    uint32_t is = px_r32(p, PX_IS);
    ahci_log_port(p);
    serial_write_string("error, recovering:");
    ahci_log_regs(p);
    ahci_port_stop(p);
    px_w32(p, PX_SERR, 0xFFFFFFFFu);
    px_w32(p, PX_IS, is);
    int ok = 1;
    if (px_r32(p, PX_TFD) & (TFD_BSY | TFD_DRQ)) {
        if (p->hba->sclo) {
            px_w32(p, PX_CMD, px_r32(p, PX_CMD) | PX_CMD_CLO);
            ok = ahci_wait_reg(p, PX_CMD, PX_CMD_CLO, 0, 500000000ULL);
        } else {
            ok = 0;
        }
        if (!ok) { ok = ahci_port_comreset(p) == 1; }
    }
    for (int s = 0; s < 32; s++) {
        if (p->busy & (1u << s)) { complete_slot(p, s, -EIO); }
    }
    p->exclusive = 0;
    if (!ok || ahci_port_start(p) != 0) {
        ahci_log_port(p);
        serial_write_string("recovery did not restart the port -- FAILED\n");
        p->dead = 1;
    }
}
