#include "fatfs.h"
#include "blkcache.h"
#include "../ata.h"
#include "../serial.h"
#include "../mm/heap.h"

// The legacy single-volume API. No longer declared in fatfs.h -- the
// VFS is the only way in from outside this file -- but the two
// selftests below still drive the driver's internals through it, so
// the prototypes live here instead.
uint32_t fat16_read_file(uint32_t first_cluster, uint32_t size, void *buffer);
void fat16_read_at(uint32_t first_cluster, uint32_t position, void *buf, uint32_t len);
int fat16_write_file(uint32_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint32_t *out_first_cluster, uint32_t *out_new_size);
void fat16_truncate(uint32_t first_cluster, uint32_t dir_lba, uint16_t dir_offset,
                    uint32_t *out_first_cluster);
void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint32_t first_cluster,
                             uint32_t size);
int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset);
int fat16_mkdir(const char *path);
int fat16_delete_entry(const char *path);
int fat16_find(const char *path, uint32_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset);
#include "../errno.h"

#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_LONG_NAME 0x0F
#define FAT16_EOC_MIN      0xFFF8

#define SECTOR_SIZE 512

struct fat16_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB, offsets 36-51. On a FAT16 volume these bytes
    // hold the drive number / boot signature / volume label instead and
    // are simply never read, because the variant is decided before
    // anything here is touched.
    uint32_t sectors_per_fat_32;   // offset 36
    uint16_t ext_flags;            // offset 40
    uint16_t fs_version;           // offset 42
    uint32_t root_cluster;         // offset 44
    uint16_t fs_info_sector;       // offset 48
    uint16_t backup_boot_sector;   // offset 50
} __attribute__((packed));

struct fat16_dirent {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_high; // unused in FAT16
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

// This is an ON-DISK layout, not a convenience struct: FAT fixes every
// field's width and offset. Widening one field silently reinterprets
// every directory on the volume -- which is exactly what a careless
// uint16->uint32 sweep of this file once did, turning /HELLO.TXT into
// garbage on the very first read.
_Static_assert(sizeof(struct fat16_dirent) == 32, "FAT directory entry must be 32 bytes");

#define DIRENTS_PER_SECTOR (SECTOR_SIZE / sizeof(struct fat16_dirent))

// Per-volume geometry. Was eight file-scope globals; becoming a struct
// is what lets a second volume exist at all. FAT32 fields are unused
// until the variant work lands, but live here from the start so that
// change touches only the code that reads them.
struct fat_volume {
    uint8_t  drive;
    enum { FAT_16, FAT_32 } variant;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint32_t fat_start_lba;
    uint32_t data_start_lba;
    uint32_t sectors_per_fat;
    uint32_t root_dir_start_lba;    // FAT16 only
    uint32_t root_dir_sector_count; // FAT16 only
    uint16_t root_entry_count;      // FAT16 only
    uint32_t root_cluster;          // FAT32 only

    // Where the last successful allocation stopped. Restarting the
    // free-cluster scan at cluster 2 every time makes filling a volume
    // quadratic; this makes the common case land on the first entry
    // looked at.
    uint32_t next_free_hint;

    // One-entry cursor for cluster_at_offset. Sequential I/O walks the
    // same chain forwards, so remembering where the last walk ended
    // turns an O(chain) re-walk per sector into a single step.
    uint32_t walk_first;            // chain start, or 0 when invalid
    uint32_t walk_index;            // cluster index within that chain
    uint32_t walk_cluster;          // the cluster at walk_index
};

// The one volume the legacy fat16_* API operates on. It disappears
// when that API does; until then it keeps this refactor invisible to
// callers.
static struct fat_volume legacy_volume;

static uint32_t cluster_to_lba(struct fat_volume *v, uint32_t cluster) {
    return v->data_start_lba + (uint32_t)(cluster - 2) * v->sectors_per_cluster;
}

// Reads and parses the boot sector into v, which must already have
// v->drive set. Returns 1 on success. Shared by the legacy
// fat16_mount() and the VFS driver's mount op.
static int fat_read_bpb(struct fat_volume *v) {
    // Every mount path lands here, so the allocation hint and the
    // chain cursor get their starting values in one place.
    v->next_free_hint = 2;
    v->walk_first = 0;

    uint8_t sector[SECTOR_SIZE];
    if (!blkcache_read(v->drive, 0, sector)) {
        return 0;
    }
    struct fat16_bpb *bpb = (struct fat16_bpb *)sector;

    v->bytes_per_sector = bpb->bytes_per_sector;
    v->sectors_per_cluster = bpb->sectors_per_cluster;
    v->root_entry_count = bpb->root_entry_count;
    v->fat_start_lba = bpb->reserved_sector_count;

    // FAT32 zeroes the 16-bit sectors_per_fat and uses the 32-bit
    // field at offset 36 instead.
    v->sectors_per_fat = bpb->sectors_per_fat ? bpb->sectors_per_fat
                                              : bpb->sectors_per_fat_32;

    if (v->bytes_per_sector == 0 || v->sectors_per_cluster == 0) {
        return 0;
    }

    v->root_dir_start_lba = v->fat_start_lba + (uint32_t)bpb->num_fats * v->sectors_per_fat;
    v->root_dir_sector_count = ((uint32_t)v->root_entry_count * sizeof(struct fat16_dirent)
                                 + v->bytes_per_sector - 1) / v->bytes_per_sector;
    v->data_start_lba = v->root_dir_start_lba + v->root_dir_sector_count;

    uint32_t total_sectors = bpb->total_sectors_16 ? bpb->total_sectors_16
                                                   : bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - v->data_start_lba;
    uint32_t cluster_count = data_sectors / v->sectors_per_cluster;

    // The official determination rule. FAT12 is detected only so it can
    // be rejected rather than silently misread as FAT16.
    if (cluster_count < 4085) {
        return 0;                       // FAT12 -- caller returns -ENODEV
    } else if (cluster_count < 65525) {
        v->variant = FAT_16;
        v->root_cluster = 0;
    } else {
        v->variant = FAT_32;
        v->root_cluster = bpb->root_cluster;
        // FAT32 has no fixed root region: data begins right after the
        // FATs, and the root is an ordinary cluster chain.
        v->root_dir_sector_count = 0;
        v->data_start_lba = v->root_dir_start_lba;
    }
    return 1;
}

int fat16_mount(void) {
    struct fat_volume *v = &legacy_volume;
    v->drive = 0;
    if (!fat_read_bpb(v)) {
        serial_write_string("[fat16] mount FAILED: could not read boot sector\n");
        return 0;
    }

    serial_write_string("[fat16] mounted: bytes_per_sector=");
    serial_write_hex64(v->bytes_per_sector);
    serial_write_string(" sectors_per_cluster=");
    serial_write_hex64(v->sectors_per_cluster);
    serial_write_string(" root_dir_lba=");
    serial_write_hex64(v->root_dir_start_lba);
    serial_write_string(" data_start_lba=");
    serial_write_hex64(v->data_start_lba);
    serial_write_string("\n");
    return 1;
}

static uint32_t fat16_next_cluster(struct fat_volume *v, uint32_t cluster) {
    uint32_t entry_size = (v->variant == FAT_32) ? 4 : 2;
    uint32_t fat_offset = cluster * entry_size;
    uint32_t fat_sector = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!blkcache_read(v->drive, fat_sector, sector)) {
        return 0x0FFFFFFF;
    }
    if (v->variant == FAT_32) {
        // Only the low 28 bits are the cluster number; the top 4 are
        // reserved and must be ignored on read.
        return (*(uint32_t *)(sector + offset_in_sector)) & 0x0FFFFFFF;
    }
    return *(uint16_t *)(sector + offset_in_sector);
}

static void fat16_set_next_cluster(struct fat_volume *v, uint32_t cluster, uint32_t value) {
    uint32_t entry_size = (v->variant == FAT_32) ? 4 : 2;
    uint32_t fat_offset = cluster * entry_size;
    uint32_t fat_sector = v->fat_start_lba + fat_offset / v->bytes_per_sector;
    uint32_t offset_in_sector = fat_offset % v->bytes_per_sector;

    uint8_t sector[SECTOR_SIZE];
    if (!blkcache_read(v->drive, fat_sector, sector)) {
        return;
    }
    if (v->variant == FAT_32) {
        uint32_t *slot = (uint32_t *)(sector + offset_in_sector);
        // Preserve the top 4 reserved bits rather than clobbering them.
        *slot = (*slot & 0xF0000000) | (value & 0x0FFFFFFF);
    } else {
        *(uint16_t *)(sector + offset_in_sector) = (uint16_t)value;
    }
    blkcache_write(v->drive, fat_sector, sector);

    // Any chain this entry belongs to may have just been extended,
    // truncated or freed, and the cursor caches a position inside one.
    // Rebuilding it costs one walk; trusting a stale one corrupts a
    // file.
    v->walk_first = 0;
}

// FAT32 splits a directory entry's cluster number across two fields:
// the low half at offset 26 and the high half at offset 20 (which FAT16
// leaves zero). Every read or write of a dirent's cluster must go
// through these, or FAT32 files past cluster 65535 silently alias onto
// the wrong chain.
static uint32_t dirent_cluster(struct fat_volume *v, const struct fat16_dirent *de) {
    if (v->variant == FAT_32) {
        return ((uint32_t)de->first_cluster_high << 16) | de->first_cluster_low;
    }
    return de->first_cluster_low;
}

static void dirent_set_cluster(struct fat_volume *v, struct fat16_dirent *de, uint32_t cluster) {
    de->first_cluster_low = (uint16_t)(cluster & 0xFFFF);
    de->first_cluster_high = (v->variant == FAT_32) ? (uint16_t)(cluster >> 16) : 0;
}

// End-of-chain markers differ by width. Every chain walk in this file
// must use this rather than comparing against a hardcoded FAT16 value.
static int fat_is_eoc(struct fat_volume *v, uint32_t cluster) {
    return (v->variant == FAT_32) ? (cluster >= 0x0FFFFFF8) : (cluster >= FAT16_EOC_MIN);
}

// The marker to write when terminating a chain.
static uint32_t fat_eoc_marker(struct fat_volume *v) {
    return (v->variant == FAT_32) ? 0x0FFFFFFF : 0xFFFF;
}

// Reads FAT entry `index` out of an already-loaded FAT sector.
static uint32_t fat_entry_in_sector(struct fat_volume *v, const uint8_t *sector, uint32_t index) {
    if (v->variant == FAT_32) {
        return (*(const uint32_t *)(sector + index * 4)) & 0x0FFFFFFF;
    }
    return *(const uint16_t *)(sector + index * 2);
}

// Finds a free (0x0000) FAT entry, marks it as a fresh chain's end,
// and returns it. Returns 0 if the volume is full.
//
// Scans a SECTOR at a time rather than an entry at a time. A FAT16
// sector holds 256 entries, so the old per-entry fat16_next_cluster
// call re-read the same sector 256 times in a row -- on the 32MiB test
// volume, up to 16,384 reads of 64 distinct sectors to answer one
// allocation. Combined with next_free_hint, filling a volume is now
// linear in the FAT rather than quadratic.
static uint32_t fat16_alloc_cluster(struct fat_volume *v) {
    uint32_t entry_size = (v->variant == FAT_32) ? 4 : 2;
    uint32_t total_entries = ((uint32_t)v->sectors_per_fat * v->bytes_per_sector) / entry_size;
    uint32_t per_sector = v->bytes_per_sector / entry_size;
    if (total_entries <= 2 || per_sector == 0) { return 0; }

    uint32_t hint = (v->next_free_hint < 2) ? 2 : v->next_free_hint;
    if (hint >= total_entries) { hint = 2; }

    uint8_t sector[SECTOR_SIZE];

    // Two passes: hint..end, then 2..hint. A full volume therefore
    // still reports full, and a mostly-full one still finds the hole
    // behind the hint.
    for (int pass = 0; pass < 2; pass++) {
        uint32_t cluster = (pass == 0) ? hint : 2;
        uint32_t limit   = (pass == 0) ? total_entries : hint;

        while (cluster < limit) {
            uint32_t fat_offset = cluster * entry_size;
            uint32_t fat_sector = v->fat_start_lba + fat_offset / v->bytes_per_sector;
            if (!blkcache_read(v->drive, fat_sector, sector)) { return 0; }

            for (uint32_t index = cluster % per_sector;
                 index < per_sector && cluster < limit;
                 index++, cluster++) {
                if (fat_entry_in_sector(v, sector, index) != 0) { continue; }
                fat16_set_next_cluster(v, cluster, fat_eoc_marker(v));
                v->next_free_hint = cluster + 1;
                return cluster;
            }
        }
    }
    return 0;
}

// Walks a cluster chain from first_cluster, zeroing every FAT entry.
static void fat16_free_chain(struct fat_volume *v, uint32_t first_cluster) {
    // 32-bit throughout: a uint16_t here truncated every FAT32 cluster
    // above 65535, so fat_is_eoc never matched and the walk ran off
    // into whatever the truncated number pointed at.
    uint32_t cluster = first_cluster;
    while (cluster >= 2 && !fat_is_eoc(v, cluster)) {
        uint32_t next = fat16_next_cluster(v, cluster);
        fat16_set_next_cluster(v, cluster, 0x0000);
        cluster = next;
    }
    // Freed space sits behind the hint; let the next allocation find it.
    if (first_cluster >= 2 && first_cluster < v->next_free_hint) {
        v->next_free_hint = first_cluster;
    }
}

// Returns the cluster containing byte offset `byte_offset` within the
// chain starting at `first_cluster`. Caller must ensure the chain is
// long enough.
//
// write_range and fat16_read_at_v call this once per SECTOR, always
// moving forwards through one chain, so walking from the head every
// time made a whole-file read O(clusters^2). The per-volume cursor
// makes the forward case a single step; anything else (a new chain, a
// backwards seek) falls back to the full walk. fat16_set_next_cluster
// invalidates the cursor whenever a chain changes shape.
static uint32_t cluster_at_offset(struct fat_volume *v, uint32_t first_cluster, uint32_t byte_offset) {
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t cluster_index = byte_offset / cluster_size_bytes;

    uint32_t cluster = first_cluster;
    uint32_t i = 0;
    if (v->walk_first == first_cluster && first_cluster >= 2 &&
        v->walk_index <= cluster_index) {
        cluster = v->walk_cluster;
        i = v->walk_index;
    }

    for (; i < cluster_index; i++) {
        cluster = fat16_next_cluster(v, cluster);
        if (cluster < 2 || fat_is_eoc(v, cluster)) {
            v->walk_first = 0;
            return cluster;
        }
    }

    v->walk_first   = first_cluster;
    v->walk_index   = cluster_index;
    v->walk_cluster = cluster;
    return cluster;
}

// Writes `len` bytes into the cluster chain starting at chain_start,
// beginning at byte offset write_position. If zero_fill is set, zero
// bytes are written instead of reading from src (used for gap-filling
// past old EOF). Does read-modify-write for any partial sector.
// Assumes the chain already has enough clusters to cover
// write_position+len -- callers must extend it first.
static void write_range(struct fat_volume *v, uint32_t chain_start, uint32_t write_position, const void *src, uint32_t len, int zero_fill) {
    const uint8_t *in = (const uint8_t *)src;
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t written = 0;

    while (written < len) {
        uint32_t abs_offset = write_position + written;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / v->bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % v->bytes_per_sector;

        uint32_t cluster = cluster_at_offset(v, chain_start, abs_offset);
        uint32_t lba = cluster_to_lba(v, cluster) + sector_index;

        uint32_t to_write = v->bytes_per_sector - offset_in_sector;
        if (to_write > len - written) {
            to_write = len - written;
        }

        uint8_t sector[SECTOR_SIZE];
        if (offset_in_sector != 0 || to_write != v->bytes_per_sector) {
            blkcache_read(v->drive, lba, sector); // partial sector: preserve untouched bytes
        }
        for (uint32_t i = 0; i < to_write; i++) {
            sector[offset_in_sector + i] = zero_fill ? 0 : in[written + i];
        }
        blkcache_write(v->drive, lba, sector);

        written += to_write;
    }
}

uint32_t fat16_read_file(uint32_t first_cluster, uint32_t size, void *buffer) {
    struct fat_volume *v = &legacy_volume;
    uint8_t *out = (uint8_t *)buffer;
    uint32_t bytes_read = 0;
    uint32_t cluster = first_cluster;
    uint8_t sector_buf[SECTOR_SIZE];

    while (!fat_is_eoc(v, cluster) && bytes_read < size) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster && bytes_read < size; s++) {
            blkcache_read(v->drive, lba + s, sector_buf);
            uint32_t to_copy = size - bytes_read;
            if (to_copy > v->bytes_per_sector) {
                to_copy = v->bytes_per_sector;
            }
            for (uint32_t i = 0; i < to_copy; i++) {
                out[bytes_read + i] = sector_buf[i];
            }
            bytes_read += to_copy;
        }
        cluster = fat16_next_cluster(v, cluster);
    }
    return bytes_read;
}

void fat16_read_at_v(struct fat_volume *v, uint32_t first_cluster, uint32_t position, void *buf, uint32_t len) {
    uint8_t *out = (uint8_t *)buf;
    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t read_so_far = 0;

    while (read_so_far < len) {
        uint32_t abs_offset = position + read_so_far;
        uint32_t offset_in_cluster = abs_offset % cluster_size_bytes;
        uint32_t sector_index = offset_in_cluster / v->bytes_per_sector;
        uint32_t offset_in_sector = offset_in_cluster % v->bytes_per_sector;

        uint32_t cluster = cluster_at_offset(v, first_cluster, abs_offset);
        uint32_t lba = cluster_to_lba(v, cluster) + sector_index;

        uint32_t to_read = v->bytes_per_sector - offset_in_sector;
        if (to_read > len - read_so_far) {
            to_read = len - read_so_far;
        }

        uint8_t sector[SECTOR_SIZE];
        blkcache_read(v->drive, lba, sector);
        for (uint32_t i = 0; i < to_read; i++) {
            out[read_so_far + i] = sector[offset_in_sector + i];
        }

        read_so_far += to_read;
    }
}

int fat16_write_file_v(struct fat_volume *v, uint32_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint32_t *out_first_cluster, uint32_t *out_new_size) {
    if (len == 0) {
        *out_first_cluster = first_cluster;
        *out_new_size = current_size;
        return 0;
    }

    uint32_t cluster_size_bytes = (uint32_t)v->sectors_per_cluster * v->bytes_per_sector;
    uint32_t end_position = position + len;
    uint32_t clusters_needed = (end_position + cluster_size_bytes - 1) / cluster_size_bytes;

    uint32_t chain_start = first_cluster;
    uint32_t last_cluster = 0;
    uint32_t existing_clusters = 0;

    if (chain_start != 0) {
        uint32_t c = chain_start;
        existing_clusters = 1;
        while (!fat_is_eoc(v, fat16_next_cluster(v, c))) {
            c = fat16_next_cluster(v, c);
            existing_clusters++;
        }
        last_cluster = c;
    }

    while (existing_clusters < clusters_needed) {
        uint32_t new_cluster = fat16_alloc_cluster(v);
        if (new_cluster == 0) {
            return -ENOSPC;
        }
        if (chain_start == 0) {
            chain_start = new_cluster;
        } else {
            fat16_set_next_cluster(v, last_cluster, new_cluster);
        }
        last_cluster = new_cluster;
        existing_clusters++;
    }

    if (position > current_size) {
        write_range(v, chain_start, current_size, NULL, position - current_size, 1);
    }
    write_range(v, chain_start, position, buf, len, 0);

    *out_first_cluster = chain_start;
    *out_new_size = end_position > current_size ? end_position : current_size;
    return (int)len;
}

void fat16_truncate_v(struct fat_volume *v, uint32_t first_cluster, uint32_t dir_lba, uint16_t dir_offset, uint32_t *out_first_cluster) {
    if (first_cluster != 0) {
        fat16_free_chain(v, first_cluster);
    }
    *out_first_cluster = 0;
    fat16_update_entry_size(dir_lba, dir_offset, 0, 0);
}

static void to_fat_name(const char *name, uint8_t *out11) {
    for (int i = 0; i < 11; i++) {
        out11[i] = ' ';
    }
    int out_i = 0;
    int i = 0;
    while (name[i] != '\0' && name[i] != '.' && out_i < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 'a' + 'A');
        }
        out11[out_i] = (uint8_t)c;
        out_i++;
        i++;
    }
    while (name[i] != '\0' && name[i] != '.') {
        i++; // skip name characters beyond 8 -- 8.3 only, per this milestone's scope
    }
    if (name[i] == '.') {
        i++;
        int ext_i = 8;
        while (name[i] != '\0' && ext_i < 11) {
            char c = name[i];
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            out11[ext_i] = (uint8_t)c;
            ext_i++;
            i++;
        }
    }
}

static int fat_name_matches(const uint8_t *entry_name, const uint8_t *target_name) {
    for (int i = 0; i < 11; i++) {
        if (entry_name[i] != target_name[i]) {
            return 0;
        }
    }
    return 1;
}

// Scans one sector's worth of directory entries for target_name.
// Returns 1 (found, *out filled, *out_lba/*out_offset set to the
// entry's on-disk location, both nullable), 0 (not found in this
// sector, keep scanning), or -1 (hit the end-of-directory marker).
static int scan_sector_for_name(const uint8_t *sector, uint32_t sector_lba, const uint8_t *target_name,
                                  struct fat16_dirent *out, uint32_t *out_lba, uint16_t *out_offset) {
    const struct fat16_dirent *entries = (const struct fat16_dirent *)sector;
    for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
        if (entries[e].name[0] == 0x00) {
            return -1;
        }
        if (entries[e].name[0] == 0xE5) {
            continue; // deleted entry
        }
        if ((entries[e].attr & FAT_ATTR_LONG_NAME) == FAT_ATTR_LONG_NAME) {
            continue; // long-filename entry -- not supported, 8.3 only
        }
        if (entries[e].attr & FAT_ATTR_VOLUME_ID) {
            continue;
        }
        if (fat_name_matches(entries[e].name, target_name)) {
            *out = entries[e];
            if (out_lba) {
                *out_lba = sector_lba;
            }
            if (out_offset) {
                *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
            }
            return 1;
        }
    }
    return 0;
}

static int find_in_root(struct fat_volume *v, const uint8_t *target_name, struct fat16_dirent *out,
                          uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    for (uint32_t s = 0; s < v->root_dir_sector_count; s++) {
        uint32_t lba = v->root_dir_start_lba + s;
        blkcache_read(v->drive, lba, sector);
        int result = scan_sector_for_name(sector, lba, target_name, out, out_lba, out_offset);
        if (result != 0) {
            return result > 0;
        }
    }
    return 0;
}

static int find_in_directory_cluster(struct fat_volume *v, uint32_t dir_cluster, const uint8_t *target_name, struct fat16_dirent *out,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];
    uint32_t cluster = dir_cluster;
    while (!fat_is_eoc(v, cluster)) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
            blkcache_read(v->drive, lba + s, sector);
            int result = scan_sector_for_name(sector, lba + s, target_name, out, out_lba, out_offset);
            if (result != 0) {
                return result > 0;
            }
        }
        cluster = fat16_next_cluster(v, cluster);
    }
    return 0;
}

static void write_dirent(struct fat_volume *v, struct fat16_dirent *entry, const uint8_t *fat_name, uint8_t attr,
                           uint32_t first_cluster, uint32_t size) {
    for (int i = 0; i < 11; i++) {
        entry->name[i] = fat_name[i];
    }
    entry->attr = attr;
    entry->nt_reserved = 0;
    entry->create_time_tenth = 0;
    entry->create_time = 0;
    entry->create_date = 0;
    entry->access_date = 0;
    entry->write_time = 0;
    entry->write_date = 0;
    dirent_set_cluster(v, entry, first_cluster);
    entry->file_size = size;
}

// Finds a free slot (0x00 never-used or 0xE5 deleted) in the given
// directory (in_root selects the fixed-size root directory over
// dir_cluster) and writes a new entry there. For a non-root directory
// that's completely full, allocates and links one more cluster before
// retrying. Returns 1 on success (fills *out_lba/*out_offset with the
// new entry's location), or -ENOSPC (root full, or disk full when a
// non-root directory needs to grow).
static int create_entry_in_directory(struct fat_volume *v, uint32_t dir_cluster, int in_root, const uint8_t *fat_name,
                                       uint8_t attr, uint32_t first_cluster, uint32_t size,
                                       uint32_t *out_lba, uint16_t *out_offset) {
    uint8_t sector[SECTOR_SIZE];

    if (in_root) {
        for (uint32_t s = 0; s < v->root_dir_sector_count; s++) {
            uint32_t lba = v->root_dir_start_lba + s;
            blkcache_read(v->drive, lba, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(v, &entries[e], fat_name, attr, first_cluster, size);
                    blkcache_write(v->drive, lba, sector);
                    if (out_lba) {
                        *out_lba = lba;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        return -ENOSPC;
    }

    uint32_t cluster = dir_cluster;
    uint32_t last_cluster = dir_cluster;
    while (!fat_is_eoc(v, cluster)) {
        uint32_t lba = cluster_to_lba(v, cluster);
        for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
            blkcache_read(v->drive, lba + s, sector);
            struct fat16_dirent *entries = (struct fat16_dirent *)sector;
            for (uint32_t e = 0; e < DIRENTS_PER_SECTOR; e++) {
                if (entries[e].name[0] == 0x00 || entries[e].name[0] == 0xE5) {
                    write_dirent(v, &entries[e], fat_name, attr, first_cluster, size);
                    blkcache_write(v->drive, lba + s, sector);
                    if (out_lba) {
                        *out_lba = lba + s;
                    }
                    if (out_offset) {
                        *out_offset = (uint16_t)(e * sizeof(struct fat16_dirent));
                    }
                    return 1;
                }
            }
        }
        last_cluster = cluster;
        cluster = fat16_next_cluster(v, cluster);
    }

    uint32_t new_cluster = fat16_alloc_cluster(v);
    if (new_cluster == 0) {
        return -ENOSPC;
    }
    fat16_set_next_cluster(v, last_cluster, new_cluster);

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t new_lba = cluster_to_lba(v, new_cluster);
    for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
        blkcache_write(v->drive, new_lba + s, zero_sector);
    }

    struct fat16_dirent *entries = (struct fat16_dirent *)zero_sector;
    write_dirent(v, &entries[0], fat_name, attr, first_cluster, size);
    blkcache_write(v->drive, new_lba, zero_sector);
    if (out_lba) {
        *out_lba = new_lba;
    }
    if (out_offset) {
        *out_offset = 0;
    }
    return 1;
}

// Resolves the parent directory of `path` (its last '/'-separated
// component is the new name being created; everything before it must
// already exist and be a directory). On success (1), fills
// *out_in_root/*out_dir_cluster/*out_fat_name (11 bytes). On failure,
// returns -ENOENT (a parent component doesn't exist) or -ENOTDIR (a
// parent component exists but isn't a directory).
static int resolve_parent(struct fat_volume *v, const char *path, int *out_in_root, uint32_t *out_dir_cluster, uint8_t *out_fat_name) {
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return -ENOENT;
    }

    int in_root = 1;
    uint32_t current_dir_cluster = 0;

    for (;;) {
        char component[13];
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        int is_last = (path[i] != '/');
        if (is_last) {
            to_fat_name(component, out_fat_name);
            *out_in_root = in_root;
            *out_dir_cluster = current_dir_cluster;
            return 1;
        }

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);
        struct fat16_dirent entry;
        int found = in_root ? find_in_root(v, fat_name, &entry, NULL, NULL)
                             : find_in_directory_cluster(v, current_dir_cluster, fat_name, &entry, NULL, NULL);
        if (!found) {
            return -ENOENT;
        }
        if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
            return -ENOTDIR;
        }
        current_dir_cluster = dirent_cluster(v, &entry);
        in_root = 0;

        path += i + 1; // skip the '/'
    }
}

int fat16_create_file(const char *path, uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    struct fat_volume *v = &legacy_volume;
    int in_root;
    uint32_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(v, path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(v, dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    int created = create_entry_in_directory(v, dir_cluster, in_root, fat_name, 0, 0, 0, out_dir_lba, out_dir_offset);
    return created > 0 ? 0 : created;
}

int fat16_mkdir(const char *path) {
    struct fat_volume *v = &legacy_volume;
    int in_root;
    uint32_t dir_cluster;
    uint8_t fat_name[11];
    int result = resolve_parent(v, path, &in_root, &dir_cluster, fat_name);
    if (result < 0) {
        return result;
    }

    struct fat16_dirent existing;
    int already_exists = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                                   : find_in_directory_cluster(v, dir_cluster, fat_name, &existing, NULL, NULL);
    if (already_exists) {
        return -EEXIST;
    }

    uint32_t new_cluster = fat16_alloc_cluster(v);
    if (new_cluster == 0) {
        return -ENOSPC;
    }

    uint8_t zero_sector[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        zero_sector[i] = 0;
    }
    uint32_t lba = cluster_to_lba(v, new_cluster);
    for (uint8_t s = 0; s < v->sectors_per_cluster; s++) {
        blkcache_write(v->drive, lba + s, zero_sector);
    }

    uint32_t dir_lba;
    uint16_t dir_offset;
    int created = create_entry_in_directory(v, dir_cluster, in_root, fat_name, FAT_ATTR_DIRECTORY, new_cluster, 0, &dir_lba, &dir_offset);
    if (created <= 0) {
        fat16_free_chain(v, new_cluster);
        return created;
    }
    return 0;
}

int fat16_delete_entry(const char *path) {
    struct fat_volume *v = &legacy_volume;
    uint32_t cluster;
    uint32_t size;
    uint32_t dir_lba;
    uint16_t dir_offset;
    if (!fat16_find(path, &cluster, &size, &dir_lba, &dir_offset)) {
        return -ENOENT;
    }

    uint8_t sector[SECTOR_SIZE];
    blkcache_read(v->drive, dir_lba, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    if (entry->attr & FAT_ATTR_DIRECTORY) {
        return -EISDIR;
    }

    if (cluster != 0) {
        fat16_free_chain(v, cluster);
    }
    entry->name[0] = 0xE5;
    blkcache_write(v->drive, dir_lba, sector);
    return 0;
}

void fat16_update_entry_size_v(struct fat_volume *v, uint32_t dir_lba, uint16_t dir_offset, uint32_t first_cluster, uint32_t size) {
    uint8_t sector[SECTOR_SIZE];
    blkcache_read(v->drive, dir_lba, sector);
    struct fat16_dirent *entry = (struct fat16_dirent *)(sector + dir_offset);
    dirent_set_cluster(v, entry, first_cluster);
    entry->file_size = size;
    blkcache_write(v->drive, dir_lba, sector);
}

int fat16_find(const char *path, uint32_t *out_cluster, uint32_t *out_size,
               uint32_t *out_dir_lba, uint16_t *out_dir_offset) {
    struct fat_volume *v = &legacy_volume;
    if (path[0] == '/') {
        path++;
    }
    if (*path == '\0') {
        return 0; // empty path (or just "/") is not a valid file lookup
    }

    struct fat16_dirent entry;
    int in_root = 1;
    uint32_t current_dir_cluster = 0;
    uint32_t dir_lba = 0;
    uint16_t dir_offset = 0;

    while (*path != '\0') {
        char component[13]; // 8 + '.' + 3 + NUL -- 8.3 only
        int i = 0;
        while (path[i] != '\0' && path[i] != '/' && i < 12) {
            component[i] = path[i];
            i++;
        }
        component[i] = '\0';

        uint8_t fat_name[11];
        to_fat_name(component, fat_name);

        int found = in_root ? find_in_root(v, fat_name, &entry, &dir_lba, &dir_offset)
                             : find_in_directory_cluster(v, current_dir_cluster, fat_name, &entry, &dir_lba, &dir_offset);
        if (!found) {
            return 0;
        }

        path += i;
        if (*path == '/') {
            path++;
            if (!(entry.attr & FAT_ATTR_DIRECTORY)) {
                return 0; // tried to descend into a non-directory
            }
            current_dir_cluster = dirent_cluster(v, &entry);
            in_root = 0;
        }
    }

    *out_cluster = entry.first_cluster_low;
    *out_size = entry.file_size;
    if (out_dir_lba) {
        *out_dir_lba = dir_lba;
    }
    if (out_dir_offset) {
        *out_dir_offset = dir_offset;
    }
    return 1;
}

static int buffer_equals_string(const uint8_t *buffer, uint32_t len, const char *expected) {
    for (uint32_t i = 0; i < len; i++) {
        if ((char)buffer[i] != expected[i]) {
            return 0;
        }
    }
    return expected[len] == '\0';
}

void fat16_selftest(void) {
    uint32_t cluster;
    uint32_t size;
    uint8_t *buffer = (uint8_t *)kmalloc(8192);
    if (!buffer) {
        serial_write_string("[fat16] selftest FAILED: kmalloc returned NULL\n");
        return;
    }

    if (!fat16_find("/HELLO.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "Hello from NeoOS FAT16!\n")) {
        serial_write_string("[fat16] selftest FAILED: /HELLO.TXT contents mismatch\n");
        return;
    }

    if (!fat16_find("/BIGFILE.TXT", &cluster, &size, NULL, NULL) || size != 8192) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT not found or wrong size\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size) {
        serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT short read\n");
        return;
    }
    for (uint32_t i = 0; i < size; i++) {
        if (buffer[i] != 'N') {
            serial_write_string("[fat16] selftest FAILED: /BIGFILE.TXT byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (!fat16_find("/DIR/NESTED.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT not found\n");
        return;
    }
    if (fat16_read_file(cluster, size, buffer) != size ||
        !buffer_equals_string(buffer, size, "nested file contents\n")) {
        serial_write_string("[fat16] selftest FAILED: /DIR/NESTED.TXT contents mismatch\n");
        return;
    }

    if (fat16_find("/DIR/MISSING.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] selftest FAILED: /DIR/MISSING.TXT should not be found\n");
        return;
    }

    kfree(buffer);
    serial_write_string("[fat16] selftest passed\n");
}

void fat16_write_selftest(void) {
    struct fat_volume *v = &legacy_volume;
    uint32_t cluster = fat16_alloc_cluster(v);
    if (cluster == 0) {
        serial_write_string("[fat16] write selftest FAILED: alloc_cluster returned 0\n");
        return;
    }

    uint8_t write_buf[SECTOR_SIZE];
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        write_buf[i] = (uint8_t)(i & 0xFF);
    }
    uint32_t lba = cluster_to_lba(v, cluster);
    if (!blkcache_write(v->drive, lba, write_buf)) {
        serial_write_string("[fat16] write selftest FAILED: sector write failed\n");
        return;
    }

    uint8_t read_buf[SECTOR_SIZE];
    if (!blkcache_read(v->drive, lba, read_buf)) {
        serial_write_string("[fat16] write selftest FAILED: sector read failed\n");
        return;
    }
    for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
        if (read_buf[i] != write_buf[i]) {
            serial_write_string("[fat16] write selftest FAILED: byte mismatch at offset ");
            serial_write_hex64(i);
            serial_write_string("\n");
            return;
        }
    }

    if (!fat_is_eoc(v, fat16_next_cluster(v, cluster))) {
        serial_write_string("[fat16] write selftest FAILED: newly allocated cluster not marked EOC\n");
        return;
    }

    fat16_free_chain(v, cluster);
    if (fat16_next_cluster(v, cluster) != 0x0000) {
        serial_write_string("[fat16] write selftest FAILED: freed cluster not zeroed in FAT\n");
        return;
    }

    uint32_t size;
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT already exists before creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWFILE.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL) || cluster != 0 || size != 0) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT not found or not empty after creation\n");
        return;
    }
    if (fat16_create_file("/NEWFILE.TXT", NULL, NULL) != -EEXIST) {
        serial_write_string("[fat16] write selftest FAILED: creating /NEWFILE.TXT again did not return -EEXIST\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_delete_entry(/NEWFILE.TXT) failed\n");
        return;
    }
    if (fat16_find("/NEWFILE.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWFILE.TXT still found after deletion\n");
        return;
    }
    if (fat16_delete_entry("/NEWFILE.TXT") != -ENOENT) {
        serial_write_string("[fat16] write selftest FAILED: deleting /NEWFILE.TXT again did not return -ENOENT\n");
        return;
    }

    if (fat16_mkdir("/NEWDIR") != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_mkdir(/NEWDIR) failed\n");
        return;
    }
    if (fat16_create_file("/NEWDIR/INNER.TXT", NULL, NULL) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/NEWDIR/INNER.TXT) failed\n");
        return;
    }
    if (!fat16_find("/NEWDIR/INNER.TXT", &cluster, &size, NULL, NULL)) {
        serial_write_string("[fat16] write selftest FAILED: /NEWDIR/INNER.TXT not found after creation\n");
        return;
    }

    uint32_t dir_lba;
    uint16_t dir_offset;
    if (fat16_create_file("/WDATA.TXT", &dir_lba, &dir_offset) != 0) {
        serial_write_string("[fat16] write selftest FAILED: fat16_create_file(/WDATA.TXT) failed\n");
        return;
    }
    const char *phrase = "Hello, written file!"; // NOTE: 20 bytes, see the next line
    uint32_t phrase_len = 20;
    uint32_t new_cluster;
    uint32_t new_size;
    int written = fat16_write_file(0, 0, 0, phrase, phrase_len, &new_cluster, &new_size);
    if (written != (int)phrase_len || new_size != phrase_len) {
        serial_write_string("[fat16] write selftest FAILED: fat16_write_file(/WDATA.TXT) wrote wrong length\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);

    uint8_t readback[64];
    fat16_read_at(new_cluster, 0, readback, phrase_len);
    for (uint32_t i = 0; i < phrase_len; i++) {
        if (readback[i] != (uint8_t)phrase[i]) {
            serial_write_string("[fat16] write selftest FAILED: /WDATA.TXT readback mismatch\n");
            return;
        }
    }

    // Random-access overwrite mid-file: "written" (position 7) -> "WRITTEN".
    const char *patch = "WRITTEN";
    written = fat16_write_file(new_cluster, new_size, 7, patch, 7, &new_cluster, &new_size);
    if (written != 7) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    if (!buffer_equals_string(readback, new_size, "Hello, WRITTEN file!")) {
        serial_write_string("[fat16] write selftest FAILED: mid-file overwrite readback mismatch\n");
        return;
    }

    // Write past current EOF (position 25, current size 20): the
    // 5-byte gap [20,25) must be zero-filled.
    written = fat16_write_file(new_cluster, new_size, new_size + 5, "END", 3, &new_cluster, &new_size);
    if (written != 3) {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write failed\n");
        return;
    }
    fat16_update_entry_size(dir_lba, dir_offset, new_cluster, new_size);
    fat16_read_at(new_cluster, 0, readback, new_size);
    for (uint32_t i = 20; i < 25; i++) {
        if (readback[i] != 0) {
            serial_write_string("[fat16] write selftest FAILED: gap not zero-filled\n");
            return;
        }
    }
    if (readback[25] != 'E' || readback[26] != 'N' || readback[27] != 'D') {
        serial_write_string("[fat16] write selftest FAILED: past-EOF write data mismatch\n");
        return;
    }

    fat16_delete_entry("/WDATA.TXT");

    serial_write_string("[fat16] write selftest passed\n");
}

// Legacy single-volume wrappers. Each is the only thing still binding
// &legacy_volume for these four operations; they disappear with the
// rest of the legacy API once syscall.c moves to the VFS.
void fat16_read_at(uint32_t first_cluster, uint32_t position, void *buf, uint32_t len) {
    fat16_read_at_v(&legacy_volume, first_cluster, position, buf, len);
}

int fat16_write_file(uint32_t first_cluster, uint32_t current_size, uint32_t position,
                      const void *buf, uint32_t len,
                      uint32_t *out_first_cluster, uint32_t *out_new_size) {
    return fat16_write_file_v(&legacy_volume, first_cluster, current_size, position,
                               buf, len, out_first_cluster, out_new_size);
}

void fat16_truncate(uint32_t first_cluster, uint32_t dir_lba, uint16_t dir_offset,
                    uint32_t *out_first_cluster) {
    uint32_t wide = first_cluster;
    fat16_truncate_v(&legacy_volume, first_cluster, dir_lba, dir_offset, &wide);
    if (out_first_cluster) { *out_first_cluster = wide; }
}

void fat16_update_entry_size(uint32_t dir_lba, uint16_t dir_offset, uint32_t first_cluster,
                             uint32_t size) {
    fat16_update_entry_size_v(&legacy_volume, dir_lba, dir_offset, first_cluster, size);
}

// ---------------------------------------------------------------------
// VFS driver
// ---------------------------------------------------------------------

// Per-open-file driver state, hung off vnode->fs_private. These are
// exactly the three fields that used to live in struct
// file_descriptor -- moving them here is what decouples syscall.c
// from FAT.
struct fatfs_inode {
    int      in_use;
    uint32_t first_cluster;
    uint32_t dir_entry_lba;
    uint16_t dir_entry_offset;
    uint32_t size;
    int      is_dir;
    int      dirty_cluster; // first_cluster changed since read_inode
};

#define FATFS_MAX_INODES MAX_VNODES
static struct fatfs_inode inode_pool[FATFS_MAX_INODES];

#define FATFS_MAX_VOLUMES 4
static struct fat_volume volumes[FATFS_MAX_VOLUMES];
static int volume_used[FATFS_MAX_VOLUMES];

static struct fatfs_inode *inode_alloc(void) {
    for (int i = 0; i < FATFS_MAX_INODES; i++) {
        if (!inode_pool[i].in_use) {
            inode_pool[i].in_use = 1;
            inode_pool[i].dirty_cluster = 0;
            return &inode_pool[i];
        }
    }
    return 0;
}

static uint64_t inode_id_of(uint32_t lba, uint16_t offset) {
    return ((uint64_t)lba << 16) | offset;
}

// Inverse of to_fat_name: "FILE    TXT" -> "FILE.TXT".
static void from_fat_name(const uint8_t *raw, char *out) {
    int o = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) { out[o++] = (char)raw[i]; }
    if (raw[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) { out[o++] = (char)raw[i]; }
    }
    out[o] = '\0';
}

// Marks a directory entry deleted in place by writing 0xE5 over the
// first name byte -- the same thing fat16_delete_entry does inline;
// this exposes it for the VFS path.
static int mark_dirent_deleted(struct fat_volume *v, uint32_t lba, uint16_t offset) {
    uint8_t sector[SECTOR_SIZE];
    if (!blkcache_read(v->drive, lba, sector)) { return -ENOSPC; }
    sector[offset] = 0xE5;
    if (!blkcache_write(v->drive, lba, sector)) { return -ENOSPC; }
    return 0;
}

// Returns the index'th real 8.3 entry of a directory, skipping free
// (0x00), deleted (0xE5), volume-label, and VFAT long-name entries.
// Contract matches ramfs: 0 with *out filled for a valid index,
// -ENOENT once past the last entry.
static int fat_dir_nth(struct fat_volume *v, uint32_t dir_cluster, int in_root,
                       uint32_t index, struct dirent *out) {
    uint32_t seen = 0;
    uint32_t sectors = in_root ? v->root_dir_sector_count : v->sectors_per_cluster;
    uint32_t cluster = dir_cluster;

    for (;;) {
        uint32_t base = in_root ? v->root_dir_start_lba : cluster_to_lba(v, cluster);
        for (uint32_t s = 0; s < sectors; s++) {
            uint8_t sector[SECTOR_SIZE];
            if (!blkcache_read(v->drive, base + s, sector)) { return -ENOENT; }
            for (uint32_t o = 0; o < SECTOR_SIZE; o += sizeof(struct fat16_dirent)) {
                struct fat16_dirent *de = (struct fat16_dirent *)(sector + o);
                if (de->name[0] == 0x00) { return -ENOENT; } // end of directory
                if (de->name[0] == 0xE5) { continue; }       // deleted
                if ((de->attr & FAT_ATTR_LONG_NAME) == FAT_ATTR_LONG_NAME) { continue; }
                if (de->attr & FAT_ATTR_VOLUME_ID) { continue; }
                if (seen == index) {
                    from_fat_name(de->name, out->name);
                    out->type = (de->attr & FAT_ATTR_DIRECTORY) ? DT_DIR : DT_REG;
                    return 0;
                }
                seen++;
            }
        }
        if (in_root) { return -ENOENT; }
        cluster = fat16_next_cluster(v, cluster);
        if (fat_is_eoc(v, cluster) || cluster < 2) { return -ENOENT; }
    }
}

static int fatfs_mount_op(struct vfs_mount *m, const char *source) {
    uint8_t drive;
    if (source && source[0] == 'h' && source[1] == 'd' && source[2] == '1') {
        drive = 1;
    } else if (source && source[0] == 'h' && source[1] == 'd' && source[2] == '0') {
        drive = 0;
    } else {
        return -ENODEV;
    }

    int slot = -1;
    for (int i = 0; i < FATFS_MAX_VOLUMES; i++) {
        if (!volume_used[i]) { slot = i; break; }
    }
    if (slot < 0) { return -ENOSPC; }

    struct fat_volume *v = &volumes[slot];
    v->drive = drive;
    if (!fat_read_bpb(v)) {
        return -ENODEV;
    }

    volume_used[slot] = 1;
    m->fs_private = v;

    serial_write_string("[fatfs] mounted drive=");
    serial_write_hex64(v->drive);
    serial_write_string(" variant=");
    serial_write_string(v->variant == FAT_32 ? "FAT32" : "FAT16");
    serial_write_string(" sectors_per_cluster=");
    serial_write_hex64(v->sectors_per_cluster);
    serial_write_string("\n");
    return 0;
}

static void fatfs_umount_op(struct vfs_mount *m) {
    struct fat_volume *v = (struct fat_volume *)m->fs_private;
    for (int i = 0; i < FATFS_MAX_VOLUMES; i++) {
        if (&volumes[i] == v) { volume_used[i] = 0; }
    }
    // Nothing else on this drive is mounted once the volume is
    // released, and a remount must not be handed sectors cached from
    // the image that was there before.
    if (v) { blkcache_invalidate_drive(v->drive); }
    m->fs_private = 0;
}

static int fatfs_read_inode(struct vfs_mount *m, uint64_t inode_id, struct vnode *out) {
    struct fat_volume *v = (struct fat_volume *)m->fs_private;

    struct fatfs_inode *n = inode_alloc();
    if (!n) { return -ENFILE; }

    if (inode_id == FATFS_ROOT_INODE) {
        // FAT16's root is a fixed region (cluster 0 is a sentinel for
        // it); FAT32's is an ordinary cluster chain like any other
        // directory, so the in_root special case collapses away there.
        n->first_cluster = (v->variant == FAT_32) ? v->root_cluster : 0;
        n->dir_entry_lba = 0;
        n->dir_entry_offset = 0;
        n->size = 0;
        n->is_dir = 1;
        out->type = VNODE_DIR;
        out->size = 0;
        out->fs_private = n;
        return 0;
    }

    uint32_t lba = (uint32_t)(inode_id >> 16);
    uint16_t off = (uint16_t)(inode_id & 0xFFFF);

    uint8_t sector[SECTOR_SIZE];
    if (!blkcache_read(v->drive, lba, sector)) {
        n->in_use = 0;
        return -ENOENT;
    }
    struct fat16_dirent *de = (struct fat16_dirent *)(sector + off);

    n->first_cluster = dirent_cluster(v, de);
    n->dir_entry_lba = lba;
    n->dir_entry_offset = off;
    n->size = de->file_size;
    n->is_dir = (de->attr & FAT_ATTR_DIRECTORY) != 0;

    out->type = n->is_dir ? VNODE_DIR : VNODE_FILE;
    out->size = n->size;
    out->fs_private = n;
    return 0;
}

static int fatfs_sync_inode(struct vnode *vn) {
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    if (!n) { return 0; }
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    // Push any size/cluster change back into the directory entry. The
    // root has no entry to patch.
    if (vn->inode_id != FATFS_ROOT_INODE && (n->size != vn->size || n->dirty_cluster)) {
        fat16_update_entry_size_v(v, n->dir_entry_lba, n->dir_entry_offset,
                                  n->first_cluster, vn->size);
    }
    n->in_use = 0;
    return 0;
}

static int fatfs_lookup(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent found;
    uint32_t lba;
    uint16_t off;
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
    int ok = in_root ? find_in_root(v, fat_name, &found, &lba, &off)
                     : find_in_directory_cluster(v, d->first_cluster, fat_name, &found, &lba, &off);
    if (!ok) {
        return -ENOENT;
    }
    *out_inode_id = inode_id_of(lba, off);
    return 0;
}

static int64_t fatfs_read(struct vnode *vn, uint32_t pos, void *buf, uint32_t len) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    if (pos >= vn->size) { return 0; }
    if (pos + len > vn->size) { len = vn->size - pos; }
    fat16_read_at_v(v, n->first_cluster, pos, buf, len);
    return (int64_t)len;
}

static int64_t fatfs_write(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;

    uint32_t new_cluster;
    uint32_t new_size;
    int rc = fat16_write_file_v(v, n->first_cluster, vn->size, pos, buf, len,
                                &new_cluster, &new_size);
    if (rc < 0) { return rc; }

    if (new_cluster != n->first_cluster) {
        n->first_cluster = new_cluster;
        n->dirty_cluster = 1;
    }
    vn->size = new_size;
    // Patch the directory entry now rather than at sync_inode time:
    // the entry must be correct on disk even if the machine stops
    // before the last fd closes.
    fat16_update_entry_size_v(v, n->dir_entry_lba, n->dir_entry_offset, new_cluster, new_size);
    n->size = new_size;
    return (int64_t)len;
}

static int fatfs_create(struct vnode *dir, const char *name, uint64_t *out_inode_id) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent existing;
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
    int already = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                          : find_in_directory_cluster(v, d->first_cluster, fat_name,
                                                      &existing, NULL, NULL);
    if (already) { return -EEXIST; }

    uint32_t lba;
    uint16_t off;
    // create_entry_in_directory reports success as 1, not 0 -- it is
    // boolean-with-negative-errno, the same convention find_in_root
    // uses. Translate here rather than leaking it to the VFS, whose
    // ops are all 0-on-success.
    int rc = create_entry_in_directory(v, d->first_cluster, in_root,
                                       fat_name, 0, 0, 0, &lba, &off);
    if (rc <= 0) { return rc == 0 ? -ENOSPC : rc; }
    *out_inode_id = inode_id_of(lba, off);
    return 0;
}

static int fatfs_mkdir_op(struct vnode *dir, const char *name) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent existing;
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
    int already = in_root ? find_in_root(v, fat_name, &existing, NULL, NULL)
                          : find_in_directory_cluster(v, d->first_cluster, fat_name,
                                                      &existing, NULL, NULL);
    if (already) { return -EEXIST; }

    uint32_t cluster = fat16_alloc_cluster(v);
    if (!cluster) { return -ENOSPC; }
    fat16_set_next_cluster(v, cluster, fat_eoc_marker(v));

    // Zero the new directory's cluster so its first entry reads as
    // end-of-directory rather than whatever was there before.
    uint8_t zero[SECTOR_SIZE];
    for (int i = 0; i < SECTOR_SIZE; i++) { zero[i] = 0; }
    uint32_t base = cluster_to_lba(v, cluster);
    for (uint32_t s = 0; s < v->sectors_per_cluster; s++) {
        blkcache_write(v->drive, base + s, zero);
    }

    uint32_t lba;
    uint16_t off;
    // Same boolean-success convention as fatfs_create above.
    int rc = create_entry_in_directory(v, d->first_cluster, in_root,
                                       fat_name, FAT_ATTR_DIRECTORY, cluster, 0, &lba, &off);
    if (rc <= 0) {
        fat16_free_chain(v, cluster);
        return rc == 0 ? -ENOSPC : rc;
    }
    return 0;
}

static int fatfs_unlink_op(struct vnode *dir, const char *name) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;

    uint8_t fat_name[11];
    to_fat_name(name, fat_name);

    struct fat16_dirent found;
    uint32_t lba;
    uint16_t off;
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
    int ok = in_root ? find_in_root(v, fat_name, &found, &lba, &off)
                     : find_in_directory_cluster(v, d->first_cluster, fat_name, &found, &lba, &off);
    if (!ok) { return -ENOENT; }
    if (found.attr & FAT_ATTR_DIRECTORY) { return -EISDIR; }

    uint32_t victim = dirent_cluster(v, &found);
    if (victim != 0) {
        fat16_free_chain(v, victim);
    }
    return mark_dirent_deleted(v, lba, off);
}

static int fatfs_truncate_op(struct vnode *vn) {
    struct fat_volume *v = (struct fat_volume *)vn->mount->fs_private;
    struct fatfs_inode *n = (struct fatfs_inode *)vn->fs_private;
    fat16_truncate_v(v, n->first_cluster, n->dir_entry_lba, n->dir_entry_offset,
                     &n->first_cluster);
    vn->size = 0;
    n->size = 0;
    return 0;
}

static int fatfs_readdir_op(struct vnode *dir, uint32_t index, struct dirent *out) {
    struct fat_volume *v = (struct fat_volume *)dir->mount->fs_private;
    struct fatfs_inode *d = (struct fatfs_inode *)dir->fs_private;
    int in_root = (dir->inode_id == FATFS_ROOT_INODE) && (v->variant == FAT_16);
    return fat_dir_nth(v, d->first_cluster, in_root, index, out);
}

const struct vfs_ops fatfs_ops = {
    .mount      = fatfs_mount_op,
    .umount     = fatfs_umount_op,
    .read_inode = fatfs_read_inode,
    .sync_inode = fatfs_sync_inode,
    .lookup     = fatfs_lookup,
    .read       = fatfs_read,
    .write      = fatfs_write,
    .create     = fatfs_create,
    .mkdir      = fatfs_mkdir_op,
    .unlink     = fatfs_unlink_op,
    .truncate   = fatfs_truncate_op,
    .readdir    = fatfs_readdir_op,
};
