#ifndef NEOOS_SYS_UTSNAME_H
#define NEOOS_SYS_UTSNAME_H

// Linux's x86-64 layout exactly: six fields of 65 bytes, NUL-terminated,
// no padding. A program compiled against Linux's <sys/utsname.h> indexes
// into this directly, so the shape is not ours to choose.
//
// `sysname` is "NeoOS", not "Linux" -- see docs/stdlib.md for why that
// divergence is deliberate.
#define _UTSNAME_LENGTH 65

struct utsname {
    char sysname[_UTSNAME_LENGTH];
    char nodename[_UTSNAME_LENGTH];
    char release[_UTSNAME_LENGTH];
    char version[_UTSNAME_LENGTH];
    char machine[_UTSNAME_LENGTH];
    char domainname[_UTSNAME_LENGTH];
};

int uname(struct utsname *u);

#endif
