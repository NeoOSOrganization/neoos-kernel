#ifndef NEOOS_POLL_H
#define NEOOS_POLL_H

struct pollfd {
    int   fd;
    short events;
    short revents;
};

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

// timeout_ms: <0 blocks, 0 polls, else milliseconds (10 ms resolution).
// nfds is capped at 16 -- see docs/stdlib.md.
int poll(struct pollfd *fds, unsigned long nfds, int timeout_ms);

// select: fd_set is 1024 bits. No cap below FD_SETSIZE -- the kernel
// sizes its descriptor array to the caller's request (CS2).
typedef struct { unsigned long __bits[16]; } fd_set;
#define FD_ZERO(s)   do { for (int __i = 0; __i < 16; __i++) (s)->__bits[__i] = 0; } while (0)
#define FD_SET(fd,s)   ((s)->__bits[(fd)/64] |=  (1UL << ((fd)%64)))
#define FD_CLR(fd,s)   ((s)->__bits[(fd)/64] &= ~(1UL << ((fd)%64)))
#define FD_ISSET(fd,s) (((s)->__bits[(fd)/64] >> ((fd)%64)) & 1UL)

struct timeval { long tv_sec; long tv_usec; };
int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv);

#endif
