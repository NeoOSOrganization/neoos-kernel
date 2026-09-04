#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

// The kernel's own numbering; see kernel/syscall_nr.h.
#define SYS_SOCKET      45
#define SYS_BIND        46
#define SYS_CONNECT     47
#define SYS_SENDTO      48
#define SYS_RECVFROM    49
#define SYS_GETSOCKNAME 50
#define SYS_LISTEN      83
#define SYS_ACCEPT4     84
#define SYS_SHUTDOWN    85
#define SYS_GETPEERNAME 86
#define SYS_SETSOCKOPT  87
#define SYS_GETSOCKOPT  88

extern long __neoos_syscall3(long n, long a, long b, long c);
extern long __neoos_syscall6(long n, long a, long b, long c, long d, long e, long f);

int socket(int domain, int type, int protocol) {
    return (int)__neoos_syscall3(SYS_SOCKET, domain, type, protocol);
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)__neoos_syscall3(SYS_BIND, fd, (long)(uintptr_t)addr, len);
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    return (int)__neoos_syscall3(SYS_CONNECT, fd, (long)(uintptr_t)addr, len);
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)__neoos_syscall3(SYS_GETSOCKNAME, fd,
                                 (long)(uintptr_t)addr, (long)(uintptr_t)len);
}

int listen(int fd, int backlog) {
    return (int)__neoos_syscall3(SYS_LISTEN, fd, backlog, 0);
}

int accept4(int fd, struct sockaddr *addr, socklen_t *len, int flags) {
    return (int)__neoos_syscall6(SYS_ACCEPT4, fd, (long)(uintptr_t)addr,
                                 (long)(uintptr_t)len, flags, 0, 0);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    return accept4(fd, addr, len, 0);
}

int shutdown(int fd, int how) {
    return (int)__neoos_syscall3(SYS_SHUTDOWN, fd, how, 0);
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
    return (int)__neoos_syscall3(SYS_GETPEERNAME, fd,
                                 (long)(uintptr_t)addr, (long)(uintptr_t)len);
}

int setsockopt(int fd, int level, int opt, const void *val, socklen_t len) {
    return (int)__neoos_syscall6(SYS_SETSOCKOPT, fd, level, opt,
                                 (long)(uintptr_t)val, len, 0);
}

int getsockopt(int fd, int level, int opt, void *val, socklen_t *len) {
    return (int)__neoos_syscall6(SYS_GETSOCKOPT, fd, level, opt,
                                 (long)(uintptr_t)val, (long)(uintptr_t)len, 0);
}

int64_t sendto(int fd, const void *buf, uint64_t len, int flags,
               const struct sockaddr *dest, socklen_t dest_len) {
    return __neoos_syscall6(SYS_SENDTO, fd, (long)(uintptr_t)buf, (long)len,
                            flags, (long)(uintptr_t)dest, dest_len);
}

int64_t recvfrom(int fd, void *buf, uint64_t len, int flags,
                 struct sockaddr *src, socklen_t *src_len) {
    return __neoos_syscall6(SYS_RECVFROM, fd, (long)(uintptr_t)buf, (long)len,
                            flags, (long)(uintptr_t)src, (long)(uintptr_t)src_len);
}

int64_t send(int fd, const void *buf, uint64_t len, int flags) {
    return sendto(fd, buf, len, flags, 0, 0);
}

int64_t recv(int fd, void *buf, uint64_t len, int flags) {
    return recvfrom(fd, buf, len, flags, 0, 0);
}

// ------------------------------------------------------------ byte order

uint16_t htons(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }
uint16_t ntohs(uint16_t v) { return htons(v); }

uint32_t htonl(uint32_t v) {
    return ((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8)  | ((v & 0xFF000000u) >> 24);
}
uint32_t ntohl(uint32_t v) { return htonl(v); }

uint32_t inet_addr(const char *s) {
    uint32_t parts[4];
    int n = 0;

    while (n < 4) {
        if (*s < '0' || *s > '9') { return 0xFFFFFFFFu; }
        uint32_t v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s - '0');
            s++;
            if (++digits > 3 || v > 255) { return 0xFFFFFFFFu; }
        }
        parts[n++] = v;
        if (n < 4) {
            if (*s != '.') { return 0xFFFFFFFFu; }
            s++;
        }
    }
    if (*s != '\0') { return 0xFFFFFFFFu; }

    // The first octet is the most significant, and the result is in
    // NETWORK order -- so it is assembled as a host-order value and
    // then converted, rather than shifted into place by hand.
    return htonl((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
}

char *inet_ntoa_r(uint32_t addr_n, char *out) {
    uint32_t h = ntohl(addr_n);
    char *p = out;
    for (int i = 3; i >= 0; i--) {
        unsigned octet = (h >> (i * 8)) & 0xFF;
        if (octet >= 100) { *p++ = (char)('0' + octet / 100); }
        if (octet >= 10)  { *p++ = (char)('0' + (octet / 10) % 10); }
        *p++ = (char)('0' + octet % 10);
        if (i) { *p++ = '.'; }
    }
    *p = '\0';
    return out;
}
