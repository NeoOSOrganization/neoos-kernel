// kernel/ipc/memfd.c -- anonymous shared memory objects.
//
// The object owns its frames; a mapping only borrows them. That is the
// whole design: two processes can map the same memfd, and either can
// exit, without the pages going out from under the other. The frames go
// back to the pmm when the last DESCRIPTOR closes, not when the last
// mapping does.
//
// Pages are allocated by ftruncate rather than on fault, because
// vma_map_frames maps the whole list at mmap time -- there is no
// demand-paging path for a shared object. Sizing a memfd is therefore
// the moment its memory is committed.

#include "ipc/memfd.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "sched/fd_table.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/vma.h"
#include "sync/lock.h"
#include "drivers/char/serial.h"
#include "errno.h"

#define MEMFD_NAME_MAX 64
#define MEMFD_MAX_FRAMES 8192          // 32 MiB: a 1280x800x4 surface is 1000

struct memfd {
    struct spinlock lock;
    char     name[MEMFD_NAME_MAX];
    uint64_t *frames;                  // physical frames, one per page
    uint64_t  nframes;                 // frames currently allocated
    uint64_t  size;                    // logical length, <= nframes * PAGE
    unsigned  flags;
    int       refs;                    // descriptors, plus in-flight SCM_RIGHTS
};

static struct spinlock memfd_count_lock;
static uint64_t memfd_live;
static int memfd_count_ready;

static void memfd_count_init(void) {
    if (!memfd_count_ready) {
        spin_init(&memfd_count_lock, LOCK_RANK_PIPE, "memfd-count");
        memfd_count_ready = 1;
    }
}

static void memfd_count_add(int delta) {
    memfd_count_init();
    uint64_t f = spin_lock_irqsave(&memfd_count_lock);
    memfd_live = (uint64_t)((int64_t)memfd_live + delta);
    spin_unlock_irqrestore(&memfd_count_lock, f);
}

uint64_t memfd_live_count(void) {
    memfd_count_init();
    uint64_t f = spin_lock_irqsave(&memfd_count_lock);
    uint64_t n = memfd_live;
    spin_unlock_irqrestore(&memfd_count_lock, f);
    return n;
}

static void memfd_free(struct memfd *m) {
    for (uint64_t i = 0; i < m->nframes; i++) {
        if (m->frames[i]) { pmm_free(m->frames[i], 0); }
    }
    kfree(m->frames);
    kfree(m);
    memfd_count_add(-1);
}

// ---- file operations --------------------------------------------------

static int64_t mfd_read(struct file_descriptor *f, void *buf, uint64_t len) {
    struct memfd *m = f->priv;
    uint64_t f_ = spin_lock_irqsave(&m->lock);
    uint64_t pos = f->position;
    if (pos >= m->size) { spin_unlock_irqrestore(&m->lock, f_); return 0; }
    if (pos + len > m->size) { len = m->size - pos; }
    uint8_t *dst = buf;
    for (uint64_t done = 0; done < len; ) {
        uint64_t off = pos + done, page = off / PMM_FRAME_SIZE;
        uint64_t in = off % PMM_FRAME_SIZE, chunk = PMM_FRAME_SIZE - in;
        if (chunk > len - done) { chunk = len - done; }
        uint8_t *src = (uint8_t *)phys_to_virt(m->frames[page]) + in;
        for (uint64_t k = 0; k < chunk; k++) { dst[done + k] = src[k]; }
        done += chunk;
    }
    f->position += (uint32_t)len;
    spin_unlock_irqrestore(&m->lock, f_);
    return (int64_t)len;
}

static int64_t mfd_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    struct memfd *m = f->priv;
    uint64_t f_ = spin_lock_irqsave(&m->lock);
    uint64_t pos = f->position;
    if (pos >= m->size) { spin_unlock_irqrestore(&m->lock, f_); return -ENOSPC; }
    if (pos + len > m->size) { len = m->size - pos; }
    const uint8_t *src = buf;
    for (uint64_t done = 0; done < len; ) {
        uint64_t off = pos + done, page = off / PMM_FRAME_SIZE;
        uint64_t in = off % PMM_FRAME_SIZE, chunk = PMM_FRAME_SIZE - in;
        if (chunk > len - done) { chunk = len - done; }
        uint8_t *dst = (uint8_t *)phys_to_virt(m->frames[page]) + in;
        for (uint64_t k = 0; k < chunk; k++) { dst[k] = src[done + k]; }
        done += chunk;
    }
    f->position += (uint32_t)len;
    spin_unlock_irqrestore(&m->lock, f_);
    return (int64_t)len;
}

static int64_t mfd_lseek(struct file_descriptor *f, int64_t off, int whence) {
    struct memfd *m = f->priv;
    int64_t base = whence == 1 ? (int64_t)f->position
                 : whence == 2 ? (int64_t)m->size : 0;
    int64_t pos = base + off;
    if (pos < 0) { return -EINVAL; }
    f->position = (uint32_t)pos;
    return pos;
}

// Growing commits memory; shrinking releases it. A grown region is
// zeroed here rather than on first touch, because there is no fault
// path for these pages -- vma_map_frames maps the list as it stands.
static int64_t mfd_truncate(struct file_descriptor *f, uint64_t len) {
    struct memfd *m = f->priv;
    uint64_t want = (len + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (want > MEMFD_MAX_FRAMES) { return -EFBIG; }

    uint64_t fl = spin_lock_irqsave(&m->lock);
    if (want > m->nframes) {
        uint64_t *nf = kmalloc(want * sizeof(uint64_t));
        if (!nf) { spin_unlock_irqrestore(&m->lock, fl); return -ENOMEM; }
        for (uint64_t i = 0; i < m->nframes; i++) { nf[i] = m->frames[i]; }
        for (uint64_t i = m->nframes; i < want; i++) {
            nf[i] = pmm_alloc(0);
            if (!nf[i]) {
                for (uint64_t b = m->nframes; b < i; b++) { pmm_free(nf[b], 0); }
                kfree(nf);
                spin_unlock_irqrestore(&m->lock, fl);
                return -ENOMEM;
            }
            uint8_t *z = (uint8_t *)phys_to_virt(nf[i]);
            for (uint64_t k = 0; k < PMM_FRAME_SIZE; k++) { z[k] = 0; }
        }
        kfree(m->frames);
        m->frames = nf;
        m->nframes = want;
    } else if (want < m->nframes) {
        // Shrinking frees pages past the end. An existing MAPPING still
        // points at them, so this is only safe because shrinking a memfd
        // that is mapped is a thing no caller does -- and the mapping
        // holds PAGE_NOFREE pages, so the pmm would hand them out again.
        // Refuse rather than corrupt.
        if (m->refs > 1) { spin_unlock_irqrestore(&m->lock, fl); return -EBUSY; }
        for (uint64_t i = want; i < m->nframes; i++) {
            if (m->frames[i]) { pmm_free(m->frames[i], 0); m->frames[i] = 0; }
        }
        m->nframes = want;
    }
    m->size = len;
    spin_unlock_irqrestore(&m->lock, fl);
    return 0;
}

static int64_t mfd_mmap(struct file_descriptor *f, struct mmap_req *r) {
    struct memfd *m = f->priv;
    if (r->len == 0) { return -EINVAL; }
    if (r->off & (PMM_FRAME_SIZE - 1)) { return -EINVAL; }
    if (r->prot & PROT_EXEC) { return -EINVAL; }        // W^X

    // MAP_SHARED only. MAP_PRIVATE would need copy-on-write, and there
    // is no fault path for these pages; a compositor never wants a
    // private view of a surface. Recorded in docs/stdlib.md.
    if (!(r->flags & MAP_SHARED)) { return -EINVAL; }

    uint64_t fl = spin_lock_irqsave(&m->lock);
    uint64_t first = r->off / PMM_FRAME_SIZE;
    uint64_t n = (r->len + PMM_FRAME_SIZE - 1) / PMM_FRAME_SIZE;
    if (first + n > m->nframes) { spin_unlock_irqrestore(&m->lock, fl); return -EINVAL; }
    // Copy the slice: vma_map_frames must not read the list while
    // another thread's ftruncate is replacing it.
    uint64_t *slice = kmalloc(n * sizeof(uint64_t));
    if (!slice) { spin_unlock_irqrestore(&m->lock, fl); return -ENOMEM; }
    for (uint64_t i = 0; i < n; i++) { slice[i] = m->frames[first + i]; }
    spin_unlock_irqrestore(&m->lock, fl);

    int64_t rc = vma_map_frames(current_proc(), slice, n, (uint32_t)r->prot);
    kfree(slice);
    if (rc < 0) { return rc; }
    r->out_addr = (uint64_t)rc;
    return 0;
}

static int mfd_poll(struct file_descriptor *f, int events) {
    (void)f;
    return events & (POLLIN | POLLOUT);      // memory is always ready
}

static int64_t mfd_ioctl(struct file_descriptor *f, uint64_t req, void *arg) {
    (void)f; (void)req; (void)arg; return -ENOTTY;
}

static int64_t mfd_getdents(struct file_descriptor *f, void *b, int n) {
    (void)f; (void)b; (void)n; return -ENOTDIR;
}

void memfd_get(struct memfd *m) {
    __atomic_add_fetch(&m->refs, 1, __ATOMIC_SEQ_CST);
}

void memfd_put(struct memfd *m) {
    if (__atomic_sub_fetch(&m->refs, 1, __ATOMIC_SEQ_CST) == 0) { memfd_free(m); }
}

static void mfd_dup(struct file_descriptor *f)   { memfd_get(f->priv); }
static void mfd_close(struct file_descriptor *f) { memfd_put(f->priv); f->priv = 0; }

const struct file_ops memfd_ops = {
    .name     = "memfd",
    .read     = mfd_read,
    .write    = mfd_write,
    .lseek    = mfd_lseek,
    .getdents = mfd_getdents,
    .ioctl    = mfd_ioctl,
    .poll     = mfd_poll,
    .truncate = mfd_truncate,
    .mmap     = mfd_mmap,
    .dup      = mfd_dup,
    .close    = mfd_close,
};

// ---- creation ---------------------------------------------------------

int64_t memfd_create_fd(const char *name, uint64_t name_len, unsigned flags) {
    struct process *proc = current_proc();
    if (!proc) { return -ESRCH; }
    if (flags & ~(unsigned)(MFD_CLOEXEC | MFD_ALLOW_SEALING)) { return -EINVAL; }

    struct memfd *m = kmalloc(sizeof(struct memfd));
    if (!m) { return -ENOMEM; }
    spin_init(&m->lock, LOCK_RANK_PIPE, "memfd");
    m->frames = 0;
    m->nframes = 0;
    m->size = 0;
    m->flags = flags;
    m->refs = 1;
    uint64_t i = 0;
    for (; i < name_len && i < MEMFD_NAME_MAX - 1 && name[i]; i++) { m->name[i] = name[i]; }
    m->name[i] = 0;

    int fd = fd_table_alloc(proc->fd_table);
    if (fd < 0) { kfree(m); return fd; }
    struct file_descriptor *f = fd_table_get(proc->fd_table, fd);
    if (!f) { fd_table_close(proc->fd_table, fd); kfree(m); return -EBADF; }

    f->ops      = &memfd_ops;
    f->priv     = m;
    f->readable = 1;
    f->writable = 1;
    f->position = 0;
    memfd_count_add(+1);
    // MFD_CLOEXEC is accepted and inert: NeoOS has no close-on-exec
    // machinery, the same as pipe2's O_CLOEXEC. MFD_ALLOW_SEALING is
    // stored but F_ADD_SEALS is not implemented. Both in docs/stdlib.md.
    return fd;
}
