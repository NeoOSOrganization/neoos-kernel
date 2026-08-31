/*
 * User-facing input device structures and ioctl definitions.
 * Linux x86_64 ABI compatible.
 */
#ifndef _LINUX_INPUT_H
#define _LINUX_INPUT_H

#include <stdint.h>
#include <linux/input-event-codes.h>

/* Input event structure — exactly 24 bytes on x86_64 (Linux ABI) */
struct input_event {
    int64_t  tv_sec;
    int64_t  tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t  value;
};

/* Device identification structure */
struct input_id {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

/* ioctl() command encoding — from Linux asm-generic/ioctl.h, x86_64 variant */
#define _IOC_NRBITS   8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS  2

#define _IOC_NRMASK   ((1 << _IOC_NRBITS) - 1)
#define _IOC_TYPEMASK ((1 << _IOC_TYPEBITS) - 1)
#define _IOC_SIZEMASK ((1 << _IOC_SIZEBITS) - 1)
#define _IOC_DIRMASK  ((1 << _IOC_DIRBITS) - 1)

#define _IOC_NRSHIFT   0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT  (_IOC_SIZESHIFT + _IOC_SIZEBITS)

#define _IOC_NONE  0U
#define _IOC_WRITE 1U
#define _IOC_READ  2U

#define _IOC(dir, type, nr, size) \
    (((dir)  << _IOC_DIRSHIFT) | \
     ((type) << _IOC_TYPESHIFT) | \
     ((nr)   << _IOC_NRSHIFT) | \
     ((size) << _IOC_SIZESHIFT))

#define _IOR(type, nr, size)   _IOC(_IOC_READ, (type), (nr), sizeof(size))
#define _IOW(type, nr, size)   _IOC(_IOC_WRITE, (type), (nr), sizeof(size))
#define _IOWR(type, nr, size)  _IOC(_IOC_READ|_IOC_WRITE, (type), (nr), sizeof(size))

/* evdev ioctl commands for /dev/input/eventX devices */
#define EVIOCGVERSION        _IOR('E', 0x01, int)
#define EVIOCGID             _IOR('E', 0x02, struct input_id)
#define EVIOCGREP            _IOR('E', 0x03, unsigned int[2])
#define EVIOCSREP            _IOW('E', 0x03, unsigned int[2])
#define EVIOCGKEYCODE        _IOR('E', 0x04, unsigned int[2])
#define EVIOCSKEYCODE        _IOW('E', 0x04, unsigned int[2])
#define EVIOCGKEYCODE_V2     _IOR('E', 0x04, struct input_keymap_entry)
#define EVIOCSKEYCODE_V2     _IOW('E', 0x04, struct input_keymap_entry)
#define EVIOCGNAME(len)      _IOC(_IOC_READ, 'E', 0x06, len)
#define EVIOCGPHYS(len)      _IOC(_IOC_READ, 'E', 0x07, len)
#define EVIOCGUNIQ(len)      _IOC(_IOC_READ, 'E', 0x08, len)
#define EVIOCGPROP(len)      _IOC(_IOC_READ, 'E', 0x09, len)
#define EVIOCGMTSLOTS(len)   _IOC(_IOC_READ, 'E', 0x0a, len)
#define EVIOCGKEY(len)       _IOC(_IOC_READ, 'E', 0x18, len)
#define EVIOCGALL(len)       _IOC(_IOC_READ, 'E', 0x19, len)
#define EVIOCGBIT(ev, len)   _IOC(_IOC_READ, 'E', 0x20 + (ev), len)
#define EVIOCGSND(len)       _IOC(_IOC_READ, 'E', 0x30, len)
#define EVIOCGSW(len)        _IOC(_IOC_READ, 'E', 0x1b, len)
#define EVIOCGABS(abs)       _IOR('E', 0x40 + (abs), struct input_absinfo)
#define EVIOCSABS(abs)       _IOW('E', 0xc0 + (abs), struct input_absinfo)
#define EVIOCSFF             _IOW('E', 0x80, struct ff_effect)
#define EVIOCRMFF            _IOW('E', 0x81, int)
#define EVIOCGEFFECTS        _IOR('E', 0x84, int)
#define EVIOCGRAB            _IOW('E', 0x90, int)
#define EVIOCREVOKE          _IOW('E', 0x91, int)
#define EVIOCGMASK           _IOR('E', 0x92, struct input_mask)
#define EVIOCSMASK           _IOW('E', 0x93, struct input_mask)
#define EVIOCSCLOCKID        _IOW('E', 0xa0, int)

/* For compatibility: EV_VERSION must be bumped whenever the event ABI changes */
#define EV_VERSION 0x010001

#endif /* _LINUX_INPUT_H */
