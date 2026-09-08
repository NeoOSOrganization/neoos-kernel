#ifndef NEOOS_UNIX_SOCK_H
#define NEOOS_UNIX_SOCK_H

#include <stdint.h>

// AF_UNIX stream sockets: socket/bind/listen/accept4/connect.
//
// socketpair(2) already existed, but it can only ever connect processes
// that share an ancestor. A compositor needs the other shape -- an
// unrelated process connecting to a name -- which is what this adds.
//
// The byte stream is two of kernel/ipc/pipe.c's rings, cross-wired,
// exactly as socketpair.c does it. That is not just reuse: it means
// poll, epoll and the edge-triggered readiness counter all work here
// with no new machinery.

struct file_descriptor;
struct k_sockaddr;

// Linux's layout, exactly.
#define UNIX_PATH_MAX 108
struct k_sockaddr_un {
    uint16_t sun_family;
    char     sun_path[UNIX_PATH_MAX];
};

int64_t unix_socket_create(int type, int protocol);
int     unix_socket_is(struct file_descriptor *f);

int64_t unix_bind(struct file_descriptor *f, const struct k_sockaddr *addr, uint32_t len);
int64_t unix_listen(struct file_descriptor *f, int backlog);
int64_t unix_accept4(struct file_descriptor *f, struct k_sockaddr *addr,
                     uint32_t *len, int flags);
int64_t unix_connect(struct file_descriptor *f, const struct k_sockaddr *addr, uint32_t len);

void unix_sock_init(void);
void unix_sock_selftest(void);

#endif
