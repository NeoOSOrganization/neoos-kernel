#include "dev/evdev.h"
#include "dev/input.h"
#include "fs/file.h"
#include "sched/proc.h"
#include "errno.h"
#include <stdint.h>
#include <stddef.h>

// Forward declarations from input.h
struct evdev_client;

// Simple memcpy for freestanding environment
static void *memcpy_local(void *dest, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    for (uint64_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dest;
}

// Simple strlen for freestanding environment
static uint64_t strlen_local(const char *s) {
    uint64_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

// Helper: copy a string to userland with a size limit, return byte count
// Returns the number of bytes written (not including null terminator)
// Returns -EFAULT or -EINVAL if the copy fails
static int64_t copy_string_to_user(const char *src, void *user_buf, uint64_t size) {
    if (!user_buf || size == 0) {
        return -EINVAL;
    }

    uint64_t src_len = strlen_local(src);
    uint64_t copy_len = (src_len < size) ? src_len : (size - 1);

    memcpy_local(user_buf, src, copy_len);

    // Null-terminate if there's room
    if (size > copy_len) {
        ((char *)user_buf)[copy_len] = '\0';
    }

    return (int64_t)copy_len;
}

// Helper: get EV bitmap showing which event types are supported
// We support: EV_SYN (0), EV_KEY (1), EV_MSC (4)
static void get_ev_bitmap(uint8_t *out, uint64_t len) {
    if (!out || len == 0) {
        return;
    }

    // Clear the buffer
    for (uint64_t i = 0; i < len; i++) {
        out[i] = 0;
    }

    // Set the bits for EV_SYN (bit 0), EV_KEY (bit 1), EV_MSC (bit 4)
    if (len >= 1) {
        out[0] |= (1 << 0);  // EV_SYN
        out[0] |= (1 << 1);  // EV_KEY
        out[0] |= (1 << 4);  // EV_MSC
    }
}

// Helper: get MSC bitmap showing which MSC events are supported
// We support: MSC_SCAN (bit 4)
static void get_msc_bitmap(uint8_t *out, uint64_t len) {
    if (!out || len == 0) {
        return;
    }

    // Clear the buffer
    for (uint64_t i = 0; i < len; i++) {
        out[i] = 0;
    }

    // Set bit for MSC_SCAN (code 4, so bit 4)
    if (len >= 1) {
        out[0] |= (1 << 4);  // MSC_SCAN at bit 4
    }
}

// Helper: extract the size from an encoded ioctl request
// For requests like EVIOCGNAME(len), the len is encoded in the size field
static uint64_t get_ioctl_size(uint64_t request) {
    return _IOC_SIZE(request);
}

// File operations for /dev/input/event0

static int64_t evdev_fop_read(struct file_descriptor *f, void *buf, uint64_t len) {
    if (!f || !f->priv) {
        return -EBADF;
    }
    struct evdev_client *c = (struct evdev_client *)f->priv;
    return evdev_client_read(c, buf, len, f->nonblock);
}

static int64_t evdev_fop_write(struct file_descriptor *f, const void *buf, uint64_t len) {
    (void)f;
    (void)buf;
    (void)len;
    return -EINVAL;
}

static int64_t evdev_fop_lseek(struct file_descriptor *f, int64_t offset, int whence) {
    (void)f;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static int64_t evdev_fop_getdents(struct file_descriptor *f, void *buf, int bytes) {
    (void)f;
    (void)buf;
    (void)bytes;
    return -ENOTDIR;
}

static int64_t evdev_fop_ioctl(struct file_descriptor *f, uint64_t request, void *arg) {
    if (!f || !f->priv) {
        return -EBADF;
    }

    struct evdev_client *c = (struct evdev_client *)f->priv;
    uint64_t size = get_ioctl_size(request);
    uint64_t type = _IOC_TYPE(request);
    uint64_t nr = _IOC_NR(request);

    // Must be 'E' type requests
    if (type != 'E') {
        return -EINVAL;
    }

    switch (nr) {
    case 0x01: {  // EVIOCGVERSION
        if (!arg || size != sizeof(int)) {
            return -EINVAL;
        }
        int version = EV_VERSION;
        memcpy_local(arg, &version, sizeof(version));
        return 0;
    }

    case 0x02: {  // EVIOCGID
        if (!arg || size != sizeof(struct input_id)) {
            return -EINVAL;
        }
        struct input_id id;
        id.bustype = BUS_I8042;
        id.vendor = 0x0001;
        id.product = 0x0001;
        id.version = 0x0100;
        memcpy_local(arg, &id, sizeof(id));
        return 0;
    }

    case 0x06: {  // EVIOCGNAME(len)
        if (!arg || size == 0) {
            return -EINVAL;
        }
        const char *name = "NeoOS AT keyboard";
        return copy_string_to_user(name, arg, size);
    }

    case 0x07:    // EVIOCGPHYS(len)
    case 0x08:    // EVIOCGUNIQ(len)
        return -ENOENT;

    case 0x18: {  // EVIOCGKEY(len)
        if (!arg || size == 0) {
            return -EINVAL;
        }
        evdev_client_state_bitmap((uint8_t *)arg, size);
        return (int64_t)size;
    }

    case 0x20:    // EVIOCGBIT(0, len) - EV bitmap
    case 0x21:    // EVIOCGBIT(EV_KEY, len)
    case 0x24: {  // EVIOCGBIT(EV_MSC, len)
        if (!arg || size == 0) {
            return -EINVAL;
        }

        if (nr == 0x20) {  // EV bitmap
            get_ev_bitmap((uint8_t *)arg, size);
        } else if (nr == 0x21) {  // EV_KEY bitmap
            evdev_client_key_bitmap((uint8_t *)arg, size);
        } else if (nr == 0x24) {  // EV_MSC bitmap
            get_msc_bitmap((uint8_t *)arg, size);
        }

        return (int64_t)size;
    }

    case 0x90: {  // EVIOCGRAB
        int grab = (arg != NULL) ? 1 : 0;
        return evdev_client_grab(c, grab);
    }

    case 0xa0: {  // EVIOCSCLOCKID
        (void)arg;
        return -EINVAL;
    }

    default:
        return -EINVAL;
    }
}

static int evdev_fop_poll(struct file_descriptor *f, int events) {
    (void)events;
    if (!f || !f->priv) {
        return 0;
    }
    struct evdev_client *c = (struct evdev_client *)f->priv;
    return evdev_client_poll(c);
}

static void evdev_fop_dup(struct file_descriptor *f) {
    // Called when an fd is duplicated (e.g., via fork)
    // We maintain a refcount on the client
    if (f && f->priv) {
        struct evdev_client *c = (struct evdev_client *)f->priv;
        // Note: refcount management would require taking a lock here
        // For now, we rely on the client not being freed while any fd references it
        (void)c;
    }
}

static void evdev_fop_close(struct file_descriptor *f) {
    if (!f || !f->priv) {
        return;
    }

    struct evdev_client *c = (struct evdev_client *)f->priv;

    // Close the client (this will release the grab if held)
    evdev_client_close(c);

    f->priv = NULL;
}

const struct file_ops evdev_file_ops = {
    .name     = "evdev",
    .read     = evdev_fop_read,
    .write    = evdev_fop_write,
    .lseek    = evdev_fop_lseek,
    .getdents = evdev_fop_getdents,
    .ioctl    = evdev_fop_ioctl,
    .poll     = evdev_fop_poll,
    .dup      = evdev_fop_dup,
    .close    = evdev_fop_close,
};

// Called by devfs when /dev/input/event0 is opened
int evdev_devfs_open(struct file_descriptor *f) {
    if (!f) {
        return -EINVAL;
    }

    struct evdev_client *c = evdev_client_open();
    if (!c) {
        return -ENOMEM;
    }

    f->priv = c;
    f->ops = &evdev_file_ops;

    return 0;
}
