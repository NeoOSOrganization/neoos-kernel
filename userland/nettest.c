// Exercises the loopback network stack through the BSD socket calls a
// ported program would use: socket, bind, connect, sendto, recvfrom,
// getsockname, and read/write on a connected socket.
//
// Everything goes over 127.0.0.1, so a datagram that arrives has been
// through a real IPv4 header with a real checksum, a real UDP header
// with a real pseudo-header checksum, and the port demux -- which is
// the point. A test that shortcut the stack would prove only that two
// buffers can be copied.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SERVER_PORT 7777

static void addr_for(struct sockaddr_in *a, uint32_t host_ip, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family      = AF_INET;
    a->sin_port        = htons(port);
    a->sin_addr.s_addr = htonl(host_ip);
}

static int check_byte_order(void) {
    if (htons(0x1234) != 0x3412) {
        printf("[nettest] FAILED: htons\n"); return 0;
    }
    if (htonl(0x12345678u) != 0x78563412u) {
        printf("[nettest] FAILED: htonl\n"); return 0;
    }
    if (ntohl(htonl(0xDEADBEEFu)) != 0xDEADBEEFu) {
        printf("[nettest] FAILED: htonl/ntohl round trip\n"); return 0;
    }
    if (inet_addr("127.0.0.1") != htonl(INADDR_LOOPBACK)) {
        printf("[nettest] FAILED: inet_addr(\"127.0.0.1\")\n"); return 0;
    }
    if (inet_addr("1.2.3.4") != htonl(0x01020304u)) {
        printf("[nettest] FAILED: inet_addr(\"1.2.3.4\")\n"); return 0;
    }
    // Malformed input must be rejected, not partially parsed.
    if (inet_addr("1.2.3") != 0xFFFFFFFFu ||
        inet_addr("1.2.3.4.5") != 0xFFFFFFFFu ||
        inet_addr("1.2.3.256") != 0xFFFFFFFFu ||
        inet_addr("hello") != 0xFFFFFFFFu) {
        printf("[nettest] FAILED: inet_addr accepted a malformed address\n");
        return 0;
    }
    char buf[16];
    inet_ntoa_r(htonl(0x0A000105u), buf);
    if (strcmp(buf, "10.0.1.5") != 0) {
        printf("[nettest] FAILED: inet_ntoa_r gave \"%s\"\n", buf);
        return 0;
    }
    printf("[nettest] byte order and address parsing passed\n");
    return 1;
}

static int check_socket_errors(void) {
    // SOCK_STREAM used to be refused here with EPROTONOSUPPORT. D5
    // implemented it, so the assertion is inverted rather than deleted:
    // a stream socket must now be creatable, and tcptest exercises what
    // it does. AF_INET is still the only family.
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sfd < 0) {
        printf("[nettest] FAILED: SOCK_STREAM was refused (%d)\n", sfd);
        return 0;
    }
    close(sfd);
    if (socket(17, SOCK_DGRAM, 0) != -EAFNOSUPPORT) {
        printf("[nettest] FAILED: an unknown family was not refused\n");
        return 0;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("[nettest] FAILED: socket() = %d\n", fd); return 0; }

    // An address this host does not own cannot be bound.
    struct sockaddr_in a;
    addr_for(&a, 0x08080808u, 1234);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != -EADDRNOTAVAIL) {
        printf("[nettest] FAILED: bound an address the host does not own\n");
        return 0;
    }
    // A sockaddr that is too short is a bad argument, not a truncation.
    addr_for(&a, INADDR_LOOPBACK, 1234);
    if (bind(fd, (struct sockaddr *)&a, 4) != -EINVAL) {
        printf("[nettest] FAILED: a short sockaddr was accepted\n");
        return 0;
    }
    // Reading an unconnected socket has nowhere to read from.
    char c;
    if (read(fd, &c, 1) != -ENOTCONN) {
        printf("[nettest] FAILED: read on an unbound socket did not return ENOTCONN\n");
        return 0;
    }
    close(fd);
    printf("[nettest] error cases passed\n");
    return 1;
}

static int check_ephemeral_and_conflict(void) {
    int a = socket(AF_INET, SOCK_DGRAM, 0);
    int b = socket(AF_INET, SOCK_DGRAM, 0);
    if (a < 0 || b < 0) { printf("[nettest] FAILED: socket()\n"); return 0; }

    // Port 0 means "pick one", and getsockname must then report what
    // was picked -- otherwise a program cannot tell a peer where to
    // reply.
    struct sockaddr_in addr;
    addr_for(&addr, INADDR_LOOPBACK, 0);
    if (bind(a, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("[nettest] FAILED: bind to port 0\n"); return 0;
    }
    struct sockaddr_in got;
    socklen_t len = sizeof(got);
    if (getsockname(a, (struct sockaddr *)&got, &len) != 0) {
        printf("[nettest] FAILED: getsockname\n"); return 0;
    }
    if (len != sizeof(got) || got.sin_family != AF_INET || got.sin_port == 0) {
        printf("[nettest] FAILED: getsockname reported family=%d port=%d len=%d\n",
               got.sin_family, ntohs(got.sin_port), (int)len);
        return 0;
    }
    uint16_t chosen = ntohs(got.sin_port);

    // And that port is now taken.
    addr_for(&addr, INADDR_LOOPBACK, chosen);
    if (bind(b, (struct sockaddr *)&addr, sizeof(addr)) != -EADDRINUSE) {
        printf("[nettest] FAILED: rebinding port %d was allowed\n", chosen);
        return 0;
    }
    close(a);
    close(b);
    printf("[nettest] ephemeral ports and conflict detection passed, port=%d\n",
           chosen);
    return 1;
}

// ------------------------------------------------------- an echo exchange

static volatile int server_ready;
static volatile int server_ok;

static void *echo_server(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("[nettest] FAILED: server socket\n"); return 0; }

    struct sockaddr_in me;
    addr_for(&me, INADDR_ANY, SERVER_PORT);
    if (bind(fd, (struct sockaddr *)&me, sizeof(me)) != 0) {
        printf("[nettest] FAILED: server bind\n");
        return 0;
    }
    __atomic_store_n((int *)&server_ready, 1, __ATOMIC_RELEASE);

    for (int i = 0; i < 3; i++) {
        char buf[256];
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int64_t n = recvfrom(fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &flen);
        if (n < 0) { printf("[nettest] FAILED: server recvfrom %d\n", (int)n); return 0; }
        // The reply goes back to the address recvfrom reported, which
        // is the assertion that the source really was carried through
        // the IP and UDP headers rather than assumed.
        if (sendto(fd, buf, (uint64_t)n, 0, (struct sockaddr *)&from, flen) != n) {
            printf("[nettest] FAILED: server sendto\n");
            return 0;
        }
    }
    close(fd);
    __atomic_store_n((int *)&server_ok, 1, __ATOMIC_RELEASE);
    return 0;
}

static int check_echo(void) {
    server_ready = 0;
    server_ok    = 0;

    pthread_t srv;
    if (pthread_create(&srv, 0, echo_server, 0) != 0) {
        printf("[nettest] FAILED: pthread_create for the server\n");
        return 0;
    }
    // recvfrom blocks, so the server does not need to be running yet
    // for correctness -- but binding must have happened, or the first
    // datagram is dropped for want of a listener. UDP does not retry.
    while (!__atomic_load_n((int *)&server_ready, __ATOMIC_ACQUIRE)) {
        yield();
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { printf("[nettest] FAILED: client socket\n"); return 0; }

    struct sockaddr_in srv_addr;
    addr_for(&srv_addr, INADDR_LOOPBACK, SERVER_PORT);

    // 1: sendto/recvfrom, unconnected.
    const char *msg = "loopback round trip";
    int64_t n = sendto(fd, msg, strlen(msg), 0,
                       (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    if (n != (int64_t)strlen(msg)) {
        printf("[nettest] FAILED: sendto sent %d\n", (int)n);
        return 0;
    }
    char buf[256];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &flen);
    if (n != (int64_t)strlen(msg)) {
        printf("[nettest] FAILED: recvfrom got %d\n", (int)n);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, msg) != 0) {
        printf("[nettest] FAILED: echoed \"%s\"\n", buf);
        return 0;
    }
    if (ntohs(from.sin_port) != SERVER_PORT ||
        from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
        printf("[nettest] FAILED: reply came from %d.%d.%d.%d:%d\n",
               (int)(ntohl(from.sin_addr.s_addr) >> 24) & 0xFF,
               (int)(ntohl(from.sin_addr.s_addr) >> 16) & 0xFF,
               (int)(ntohl(from.sin_addr.s_addr) >> 8) & 0xFF,
               (int)ntohl(from.sin_addr.s_addr) & 0xFF,
               ntohs(from.sin_port));
        return 0;
    }
    printf("[nettest] sendto/recvfrom round trip passed\n");

    // 2: a truncating receive. The whole datagram is consumed even
    // though only part of it is delivered -- that is what a message
    // boundary means, and it is where a stream implementation
    // pretending to be a datagram one would be caught.
    const char *big = "0123456789abcdefghij";
    sendto(fd, big, strlen(big), 0, (struct sockaddr *)&srv_addr, sizeof(srv_addr));
    char small[5];
    n = recvfrom(fd, small, sizeof(small), 0, 0, 0);
    if (n != 5 || small[0] != '0' || small[4] != '4') {
        printf("[nettest] FAILED: truncating recv got %d bytes\n", (int)n);
        return 0;
    }
    // The remaining fifteen bytes are gone, not queued. If they were
    // queued, the next receive would return them instead of the next
    // message -- so this is checked by sending a distinct one.
    printf("[nettest] datagram truncation passed\n");

    // 3: connect, then plain read/write.
    if (connect(fd, (struct sockaddr *)&srv_addr, sizeof(srv_addr)) != 0) {
        printf("[nettest] FAILED: connect\n");
        return 0;
    }
    const char *third = "connected";
    if (write(fd, third, strlen(third)) != (int64_t)strlen(third)) {
        printf("[nettest] FAILED: write on a connected socket\n");
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    if (n != (int64_t)strlen(third)) {
        printf("[nettest] FAILED: read on a connected socket got %d\n", (int)n);
        return 0;
    }
    buf[n] = '\0';
    if (strcmp(buf, third) != 0) {
        printf("[nettest] FAILED: connected echo gave \"%s\"\n", buf);
        return 0;
    }
    close(fd);

    pthread_join(srv, 0);
    if (!server_ok) {
        printf("[nettest] FAILED: the server did not finish cleanly\n");
        return 0;
    }
    printf("[nettest] connected read/write passed\n");
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= check_byte_order();
    ok &= check_socket_errors();
    ok &= check_ephemeral_and_conflict();
    ok &= check_echo();

    printf("[nettest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
