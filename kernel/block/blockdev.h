#ifndef NEOOS_BLOCKDEV_H
#define NEOOS_BLOCKDEV_H

#include <stdint.h>

// Block devices: whole disks registered by controller drivers (ATA
// today; AHCI and NVMe later) and the partitions found on them. Names,
// majors and minors are Linux's -- they are visible to userland through
// /dev, mount(2) sources and /proc/partitions. See
// docs/superpowers/specs/2026-09-23-storage-01-block-layer-design.md.

#define BLOCKDEV_MAX       32
#define BLOCKDEV_NAME_MAX  16
// Selftest RAM disks: no /dev node, not in /proc/partitions.
#define BLOCKDEV_HIDDEN    0x1u
// Read-only medium (an optical drive): writes are -EROFS, and so is
// opening the node for writing.
#define BLOCKDEV_RO        0x2u
// Never scanned for partitions -- Linux's sr devices have one minor.
#define BLOCKDEV_NOPART    0x4u

struct blockdev;

struct blockdev_ops {
    // Return 0 or a negative errno. `count` sectors of `sector_size`.
    int (*read)(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
    int (*write)(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
    int (*flush)(struct blockdev *d);
};

struct blockdev {
    char     name[BLOCKDEV_NAME_MAX];   // "sda", "sda1", "nvme0n1p1"
    uint32_t sector_size;               // logical: 512 or 4096
    uint64_t sector_count;
    const struct blockdev_ops *ops;
    void    *priv;                      // driver state
    struct blockdev *parent;            // whole disk, for a partition; NULL otherwise
    uint64_t start_lba;                 // offset inside parent (0 for a whole disk)
    uint32_t major, minor;
    uint8_t  part_type[16];             // GPT type GUID; MBR type in [0]; else zero
    uint8_t  part_uuid[16];             // GPT partition GUID; else zero
    uint32_t flags;                     // BLOCKDEV_HIDDEN
    uint32_t partno;                    // 0 for a whole disk
    uint32_t claims;                    // mounted filesystems using it
    const char *driver;                 // "ahci", "ata", "ram"; partitions inherit the disk's
};

void blockdev_init(void);

// A controller driver registers each whole disk it finds. Assigns the
// major/minor, creates the /dev node, then scans for partitions.
// Returns 0, -EEXIST (name taken) or -ENOSPC (table full).
int  blockdev_register_disk(struct blockdev *d);
// Removes a disk and every partition on it (and their /dev nodes).
void blockdev_unregister_disk(struct blockdev *d);
// Used by the partition scanner. -EINVAL if the range is empty or
// runs past the disk.
int  blockdev_register_part(struct blockdev *disk, uint32_t partno,
                            uint64_t start_lba, uint64_t sector_count,
                            const uint8_t type[16], const uint8_t uuid[16]);
// Next free name for a prefix: "sd" gives sda..sdz, "sr" gives sr0..sr9.
// Whoever registers first gets the first name -- libata's probe-order
// rule. NVMe names come from the controller/namespace numbers.
int  blockdev_alloc_name(const char *prefix, char out[BLOCKDEV_NAME_MAX]);
// Linux's numbering for a whole-disk name: sdX is 8/(16*index), srN is
// 11/N (SCSI_CDROM_MAJOR), anything else 259 with the next free minor.
void blockdev_major_minor_for(const char *name, uint32_t *major, uint32_t *minor);

struct blockdev *blockdev_find(const char *name);
struct blockdev *blockdev_find_path(const char *path);   // "/dev/sda1" or "sda1"
void blockdev_foreach(void (*cb)(struct blockdev *d, void *arg), void *arg);

struct blockdev *blockdev_whole(struct blockdev *d);
uint64_t blockdev_disk_lba(struct blockdev *d, uint64_t lba);

// Bounds-checked I/O: -EIO for any range outside the device.
int  blockdev_read(struct blockdev *d, uint64_t lba, uint32_t count, void *buf);
int  blockdev_write(struct blockdev *d, uint64_t lba, uint32_t count, const void *buf);
int  blockdev_flush(struct blockdev *d);

// A mounted filesystem claims its device for as long as it is mounted.
// busy() is true if `d`, its whole disk, or any partition on that disk
// is claimed -- what Linux's open(O_EXCL) on a block device refuses with
// EBUSY, so mkfs/fdisk cannot rewrite a live filesystem underneath it.
void blockdev_claim(struct blockdev *d);
void blockdev_release(struct blockdev *d);
int  blockdev_busy(struct blockdev *d);

// Linux's dev_t encoding (sys/sysmacros.h makedev).
uint64_t blockdev_makedev(uint32_t major, uint32_t minor);

void blockdev_selftest(void);

#endif
