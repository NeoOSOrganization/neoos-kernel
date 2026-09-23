#ifndef NEOOS_FLOCK_H
#define NEOOS_FLOCK_H
#include <stdint.h>

// POSIX advisory record locks: fcntl(F_GETLK / F_SETLK / F_SETLKW).
// Linux semantics throughout, including the awkward ones SQLite is
// written around: locks belong to the PROCESS (not the fd), and closing
// ANY descriptor for a file releases every lock the process holds on it.

#define F_GETLK   5
#define F_SETLK   6
#define F_SETLKW  7
#define F_RDLCK   0
#define F_WRLCK   1
#define F_UNLCK   2

// Linux x86_64 struct flock -- 32 bytes.
struct k_flock {
    int16_t l_type;
    int16_t l_whence;
    int32_t _pad0;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
    int32_t _pad1;
};
_Static_assert(sizeof(struct k_flock) == 32, "struct flock is 32 bytes on x86_64");

struct file_descriptor;
struct vnode;

void flock_init(void);
// cmd is F_GETLK/F_SETLK/F_SETLKW; fl is the kernel copy of the user's
// struct (F_GETLK writes its answer back into it).
int  flock_fcntl(struct file_descriptor *f, int cmd, struct k_flock *fl);
// close(): every lock `pid` holds on `vn`.
void flock_release_vnode(struct vnode *vn, int pid);
// exit: every lock `pid` holds anywhere.
void flock_release_pid(int pid);
#endif
