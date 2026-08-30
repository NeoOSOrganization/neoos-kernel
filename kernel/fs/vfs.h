#ifndef NEOOS_VFS_H
#define NEOOS_VFS_H

#include <stdint.h>

#define MAX_MOUNTS    8
#define MAX_VNODES    64
#define VFS_MAX_PATH  128
#define VFS_NAME_MAX  13   // 8.3 name, dot, NUL

// Directory entry type codes. This struct crosses the syscall boundary
// via getdents(), so its layout is shared contract with
// lib/include/dirent.h -- the kernel and library trees do not share
// headers, so the definition is DUPLICATED there and the two must stay
// in lockstep, exactly like the syscall numbers in kernel/syscall.c and
// lib/syscall.c. Fixed-size fields only, so there is no padding
// ambiguity between the two builds.
#define DT_REG 1
#define DT_DIR 2
#define DT_CHR 3

struct dirent {
    char    name[VFS_NAME_MAX];
    uint8_t type;
};

enum vnode_type { VNODE_FILE, VNODE_DIR, VNODE_DEVICE };

struct vfs_mount;
struct process; // opened into by vfs_open_into; process.h includes this header

struct vnode {
    struct vfs_mount *mount;
    uint64_t          inode_id;   // driver-defined, unique within the mount
    enum vnode_type   type;
    uint32_t          size;
    uint32_t          refcount;   // live fds plus transient walk holds
    void             *fs_private; // driver state
    struct vnode     *next;       // hash-bucket chain
};

// No op pointer is ever NULL. A driver that cannot perform an
// operation supplies a stub returning -EPERM (or -ENOTDIR/-EISDIR
// where that is the truthful code), so callers never branch on which
// driver they are talking to.
struct vfs_ops {
    int     (*mount)(struct vfs_mount *m, const char *source);
    void    (*umount)(struct vfs_mount *m);
    int     (*read_inode)(struct vfs_mount *m, uint64_t inode_id, struct vnode *out);
    int     (*sync_inode)(struct vnode *vn);
    int     (*lookup)(struct vnode *dir, const char *name, uint64_t *out_inode_id);
    int64_t (*read)(struct vnode *vn, uint32_t pos, void *buf, uint32_t len);
    int64_t (*write)(struct vnode *vn, uint32_t pos, const void *buf, uint32_t len);
    int     (*create)(struct vnode *dir, const char *name, uint64_t *out_inode_id);
    int     (*mkdir)(struct vnode *dir, const char *name);
    int     (*unlink)(struct vnode *dir, const char *name);
    int     (*truncate)(struct vnode *vn);
    int     (*readdir)(struct vnode *dir, uint32_t index, struct dirent *out);
};

struct vfs_mount {
    int   in_use;
    char  path[VFS_MAX_PATH];
    const struct vfs_ops *ops;
    void *fs_private;
    struct vnode *root;
};

void vfs_init(void);

// Serialises filesystem OPERATIONS (as opposed to the mount table,
// which mount_lock guards). A sleeping mutex: everything it protects
// performs disk I/O. Held by the vnode file operations in
// kernel/file.c and by the path-taking syscalls; deliberately NOT held
// by anything that can block indefinitely, such as a pipe read.
void vfs_lock(void);
void vfs_unlock(void);
void vfs_selftest(void);

// Number of vnode pool slots currently claimed. A quiesced system
// reads exactly one per mount (each mount holds its own root).
uint32_t vfs_vnode_in_use_count(void);

// fstype is "fat", "ramfs", or "devfs". source is "hd0"/"hd1" for
// "fat" and ignored otherwise.
int vfs_mount_fs(const char *source, const char *target, const char *fstype);
int vfs_umount(const char *target);

// Returns the cached vnode with refcount already incremented, or 0 if
// the pool is exhausted or the driver's read_inode failed.
struct vnode *vnode_get(struct vfs_mount *m, uint64_t inode_id);
void vnode_put(struct vnode *vn);

// Resolves an absolute path to a vnode whose refcount is already
// taken -- caller must vnode_put it. On failure returns 0 and sets
// *out_err to a negative errno.
struct vnode *vfs_resolve(const char *path, int *out_err);

// Resolves the parent directory of `path` and copies the final path
// component into out_name (VFS_NAME_MAX bytes). Used by create, mkdir,
// and unlink. Same refcount and error contract as vfs_resolve.
struct vnode *vfs_resolve_parent(const char *path, char *out_name, int *out_err);

// Opens `path` into task t's fd slot `fd`, taking the vnode reference
// the slot will own. Used to give every new process its standard
// streams; ordinary opens go through SYS_OPEN instead.
int vfs_open_into(const char *path, struct process *p, int fd, int writable);

#endif
