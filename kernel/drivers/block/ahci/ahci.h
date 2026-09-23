#ifndef NEOOS_AHCI_H
#define NEOOS_AHCI_H

#include <stdint.h>
#include "sync/lock.h"

struct blockdev;

// AHCI 1.3.1 host bus adapters: SATA disks (sdX), ATAPI drives (srN) and
// port multipliers. Polled completion, several slots in flight, native
// command queuing. See
// docs/superpowers/specs/2026-09-23-storage-02-ahci-design.md.

// ---- registers --------------------------------------------------------

#define AHCI_CAP        0x00
#define AHCI_GHC        0x04
#define AHCI_IS         0x08
#define AHCI_PI         0x0C
#define AHCI_VS         0x10
#define AHCI_CAP2       0x24
#define AHCI_BOHC       0x28

#define AHCI_CAP_NP(c)    ((c) & 0x1F)
#define AHCI_CAP_NCS(c)   (((c) >> 8) & 0x1F)
#define AHCI_CAP_SPM      (1u << 17)
#define AHCI_CAP_SCLO     (1u << 24)
#define AHCI_CAP_SSS      (1u << 27)
#define AHCI_CAP_SNCQ     (1u << 30)
#define AHCI_CAP_S64A     (1u << 31)
#define AHCI_CAP2_BOH     (1u << 0)

#define AHCI_GHC_HR       (1u << 0)
#define AHCI_GHC_IE       (1u << 1)
#define AHCI_GHC_AE       (1u << 31)

#define AHCI_BOHC_BOS     (1u << 0)
#define AHCI_BOHC_OOS     (1u << 1)
#define AHCI_BOHC_BB      (1u << 4)

// Port registers, at 0x100 + 0x80 * port.
#define PX_CLB    0x00
#define PX_CLBU   0x04
#define PX_FB     0x08
#define PX_FBU    0x0C
#define PX_IS     0x10
#define PX_IE     0x14
#define PX_CMD    0x18
#define PX_TFD    0x20
#define PX_SIG    0x24
#define PX_SSTS   0x28
#define PX_SCTL   0x2C
#define PX_SERR   0x30
#define PX_SACT   0x34
#define PX_CI     0x38

#define PX_CMD_ST    (1u << 0)
#define PX_CMD_SUD   (1u << 1)
#define PX_CMD_CLO   (1u << 3)
#define PX_CMD_FRE   (1u << 4)
#define PX_CMD_CCS(c) (((c) >> 8) & 0x1F)
#define PX_CMD_FR    (1u << 14)
#define PX_CMD_CR    (1u << 15)
#define PX_CMD_PMA   (1u << 17)

// PxIS bits that mean "the port stopped on an error".
#define PX_IS_TFES   (1u << 30)
#define PX_IS_HBFS   (1u << 29)
#define PX_IS_HBDS   (1u << 28)
#define PX_IS_IFS    (1u << 27)
#define PX_IS_ERRORS (PX_IS_TFES | PX_IS_HBFS | PX_IS_HBDS | PX_IS_IFS)

#define TFD_ERR  0x01
#define TFD_DRQ  0x08
#define TFD_BSY  0x80

#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u
#define SIG_PMP   0x96690101u
#define SIG_SEMB  0xC33C0101u

// ATA commands used here.
#define ATA_READ_DMA           0xC8
#define ATA_WRITE_DMA          0xCA
#define ATA_READ_DMA_EXT       0x25
#define ATA_WRITE_DMA_EXT      0x35
#define ATA_WRITE_DMA_FUA_EXT  0x3D
#define ATA_READ_FPDMA_QUEUED  0x60
#define ATA_WRITE_FPDMA_QUEUED 0x61
#define ATA_READ_LOG_EXT       0x2F
#define ATA_FLUSH_CACHE        0xE7
#define ATA_FLUSH_CACHE_EXT    0xEA
#define ATA_IDENTIFY           0xEC
#define ATA_IDENTIFY_PACKET    0xA1
#define ATA_PACKET             0xA0
#define ATA_READ_PM            0xE4
#define ATA_WRITE_PM           0xE8

// ---- sizes ------------------------------------------------------------

#define AHCI_MAX_HBAS     4
#define AHCI_MAX_PORTS    32
#define AHCI_PRDT_MAX     64
#define AHCI_CT_SIZE      (0x80 + AHCI_PRDT_MAX * 16)   // 1152: 128-aligned
#define AHCI_PRD_MAX_BYTES (4u << 20)
// One command's worth of data, split so the PRDT can never overflow:
// 60 pages need at most 61 entries whatever the buffer's alignment.
#define AHCI_CHUNK_BYTES  (60u * 4096u)
// Linux's default ATA/SCSI command timeout.
#define AHCI_CMD_TIMEOUT_NS 30000000000ULL
// The most requests one blockdev transfer keeps in flight at once.
#define AHCI_BATCH        8

// ---- structures -------------------------------------------------------

struct ahci_hba {
    volatile uint8_t *abar;
    uint32_t cap, cap2, pi;
    uint8_t  nslots, s64a, sncq, spm, sclo, sss;
    int      index;
};

struct ahci_req;

struct ahci_port {
    struct ahci_hba *hba;
    int      num;
    volatile uint8_t *regs;
    uint64_t mem_phys;          // page: command list at 0, received FIS at 0x800
    uint8_t *cl, *rfis;
    uint64_t ct_phys;           // command tables, one per slot
    uint8_t *ct;
    struct spinlock lock;
    struct ahci_req *slot_req[32];
    uint32_t busy;              // slots in flight
    uint32_t ncq_busy;          // the subset issued as NCQ
    int      exclusive;         // a non-queued command owns the port
    int      exclusive_waiting; // non-queued submitters holding NCQ off
    int      active_pmp;        // link with commands outstanding; -1 none
    int      pmp_attached;
    int      dead;              // abandoned: every submit fails -EIO
    struct ahci_req *requeue;   // innocent requests to reissue after recovery
};

// A device: a port, and the port-multiplier port it sits behind (0 when
// attached directly).
struct ahci_link {
    struct ahci_port *port;
    uint8_t  pmp;
    uint8_t  ncq_depth;         // 0: this device takes no NCQ
};

struct ahci_req {
    uint8_t  fis[20];           // H2D register FIS
    uint8_t  cdb[16];           // ATAPI packet, when atapi
    uint8_t  atapi, write, ncq;
    uint16_t hdr_extra;         // extra command-header bits (soft reset: R|C)
    void    *buf;               // kernel virtual, word-aligned
    uint32_t len;               // bytes
    // Filled in by the engine.
    int      slot;
    int      status;            // 0, -EIO, -ENOMEDIUM, -ETIMEDOUT
    volatile uint8_t done;
    uint8_t  tfd_status, tfd_error;   // PxTFD at completion (ATAPI sense key lives in error)
    uint64_t deadline;
    struct ahci_link *link;
    struct ahci_req *next;      // requeue list
};

// ---- register access --------------------------------------------------

static inline uint32_t hba_r32(struct ahci_hba *h, uint32_t off) {
    return *(volatile uint32_t *)(h->abar + off);
}
static inline void hba_w32(struct ahci_hba *h, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(h->abar + off) = v;
}
static inline uint32_t px_r32(struct ahci_port *p, uint32_t off) {
    return *(volatile uint32_t *)(p->regs + off);
}
static inline void px_w32(struct ahci_port *p, uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(p->regs + off) = v;
}

// ---- port.c: memory, lifecycle, command engine -------------------------

int  ahci_port_setup(struct ahci_port *p);        // allocate + program CLB/FB, FRE on
void ahci_port_stop(struct ahci_port *p);         // ST off; FRE stays
int  ahci_port_start(struct ahci_port *p);
int  ahci_port_comreset(struct ahci_port *p);     // 1: link up and device ready
int  ahci_wait_reg(struct ahci_port *p, uint32_t off, uint32_t mask, uint32_t value, uint64_t ns);

// Builds the PRDT for `buf` into a command table. Returns the entry
// count, -EINVAL for an odd address or length, -E2BIG past
// AHCI_PRDT_MAX entries, -EIO for an address the HBA cannot reach.
int  ahci_build_prdt(uint8_t *table, const void *buf, uint32_t len, int s64a);

// H2D register FIS for a non-queued command. LBA28 commands (0xC8,
// 0xCA, 0xE7) carry LBA bits 27:24 in the device register.
void ahci_fis_rw(uint8_t fis[20], uint8_t cmd, uint64_t lba, uint32_t count, uint8_t pmp);
// READ/WRITE FPDMA QUEUED: count in FEATURES, tag in COUNT[7:3], FUA in
// the device register's bit 7.
void ahci_fis_ncq(uint8_t fis[20], int write, uint64_t lba, uint32_t count, int tag, int fua, uint8_t pmp);

int  ahci_submit(struct ahci_link *l, struct ahci_req *r);
int  ahci_wait(struct ahci_link *l, struct ahci_req *r);
int  ahci_exec(struct ahci_link *l, struct ahci_req *r);
// A request with no data and a FIS the caller filled in.
void ahci_req_init(struct ahci_req *r);

// Most commands in flight on this port at once since boot.
uint32_t ahci_port_max_inflight(struct ahci_port *p);

void ahci_log_port(struct ahci_port *p);          // "[ahci] port N: " (+ "hbaH " if H > 0)
void ahci_log_dec(uint64_t v);
void ahci_log_regs(struct ahci_port *p);          // CMD TFD SSTS SERR IS CI SACT

// ---- device classes ---------------------------------------------------

int  ahci_disk_attach(struct ahci_link *l);
// For the selftests: the link behind an AHCI disk's blockdev (0 if `d`
// is not one), and a disk read/write request built exactly as the block
// ops build theirs.
struct ahci_link *ahci_disk_link(struct blockdev *d);
void ahci_disk_build(struct blockdev *d, struct ahci_req *r, uint64_t lba, uint32_t n, void *buf, int wr);
int  ahci_atapi_attach(struct ahci_link *l);
int  ahci_pmp_attach(struct ahci_port *p);

// ---- hba.c ------------------------------------------------------------

void ahci_probe(void);
struct ahci_link *ahci_link_alloc(struct ahci_port *p, uint8_t pmp);

// ---- selftest.c -------------------------------------------------------

void ahci_selftest(void);
// disk.c records the scratch disk (serial NEOOSSCRATCH) here; the
// selftest runs the error-injection test on it.
void ahci_note_scratch(struct ahci_link *l, struct blockdev *d);

#endif
