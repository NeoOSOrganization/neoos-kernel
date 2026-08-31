#ifndef NEOOS_EVDEV_H
#define NEOOS_EVDEV_H

#include <stdint.h>

// Forward declarations
struct file_descriptor;
struct file_ops;

// Linux ioctl encoding macros (from <asm-generic/ioctl.h>)
#define _IOC_NRBITS     8
#define _IOC_TYPEBITS   8
#define _IOC_SIZEBITS  14
#define _IOC_DIRBITS    2

#define _IOC_NRMASK     ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK   ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK   ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK    ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT    0
#define _IOC_TYPESHIFT  (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT  (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT   (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE   0U
#define _IOC_WRITE  1U
#define _IOC_READ   2U

#define _IOC(dir,type,nr,size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

#define _IOR(type,nr,size)   _IOC(_IOC_READ,  (type), (nr), sizeof(size))
#define _IOW(type,nr,size)   _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type,nr,size)  _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size))

// Extract from an encoded ioctl value
#define _IOC_DIR(x)  (((x) >> _IOC_DIRSHIFT) & _IOC_DIRMASK)
#define _IOC_TYPE(x) (((x) >> _IOC_TYPESHIFT) & _IOC_TYPEMASK)
#define _IOC_NR(x)   (((x) >> _IOC_NRSHIFT) & _IOC_NRMASK)
#define _IOC_SIZE(x) (((x) >> _IOC_SIZESHIFT) & _IOC_SIZEMASK)

// Linux input subsystem constants
#define EV_VERSION  0x010001

// Input device ID structure (Linux layout)
struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

#define BUS_I8042   0x0011

// evdev ioctl codes (Linux <linux/input.h> values)
#define EVIOCGVERSION       _IOR('E', 0x01, int)
#define EVIOCGID            _IOR('E', 0x02, struct input_id)
#define EVIOCGNAME(len)     _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPHYS(len)     _IOC(_IOC_READ, 'E', 0x07, len)
#define EVIOCGUNIQ(len)     _IOC(_IOC_READ, 'E', 0x08, len)
#define EVIOCGBIT(ev, len)  _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGKEY(len)      _IOC(_IOC_READ, 'E', 0x18, len)
#define EVIOCGRAB           _IOW('E', 0x90, int)
#define EVIOCSCLOCKID       _IOW('E', 0xa0, int)

// The evdev file operations for /dev/input/event0
extern const struct file_ops evdev_file_ops;

// Called when opening /dev/input/event0. Initializes f->priv with an evdev client.
int evdev_devfs_open(struct file_descriptor *f);

#endif
