// The deterministic core of D5: a TCP connection over 127.0.0.1,
// driven through the same socket calls a ported program would use.
//
// Loopback, so it is deterministic and runs in the gauntlet -- and so
// that every byte has been through a real IPv4 header with a real
// checksum, a real TCP header with a real pseudo-header checksum, the
// four-tuple demux, the send and receive rings, the window, and the
// state machine. A test that shortcut any of that would prove only
// that two buffers can be copied.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
#include <neoos_test.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PORT_ECHO   7801
#define PORT_CLOSED 7899
#define PORT_BULK   7802
#define PORT_HALF   7803
#define PORT_NB     7804
#define PORT_LOSS   7805

// Larger than TCP_SNDBUF (32 KiB), on purpose: a transfer that fits in
// the buffer never closes the window, and a window that never closes
// proves nothing about the window. 96 KiB rather than 256, because the
// whole suite shares one 150-second boot with forty others and this
// still closes and reopens the window twice over.
#define BULK_BYTES (96 * 1024)

static int failures;

static void fail(const char *msg) {
    printf("[tcptest] FAILED: %s\n", msg);
    failures++;
}

static void addr_for(struct sockaddr_in *a, uint32_t host_ip, uint16_t port) {
    memset(a, 0, sizeof(*a));
    a->sin_family      = AF_INET;
    a->sin_port        = htons(port);
    a->sin_addr.s_addr = htonl(host_ip);
}

static int listen_on(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fail("socket(SOCK_STREAM)"); return -1; }
    struct sockaddr_in a;
    addr_for(&a, INADDR_LOOPBACK, port);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) != 0) { fail("bind"); return -1; }
    if (listen(fd, 4) != 0) { fail("listen"); return -1; }
    return fd;
}

static int connect_to(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { return -1; }
    struct sockaddr_in a;
    addr_for(&a, INADDR_LOOPBACK, port);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc != 0) { close(fd); return rc; }
    return fd;
}

// A byte pattern with a long period, so a transfer that drops or
// duplicates a block is caught rather than papered over by a repeating
// byte that happens to match.
static unsigned char pat(uint32_t i) { return (unsigned char)((i * 31u + (i >> 8)) & 0xFF); }

// ---- 1. connect / accept / getpeername ------------------------------

struct echo_arg { int lfd; uint32_t bytes; };

static void *echo_server(void *v) {
    struct echo_arg *e = (struct echo_arg *)v;
    int cfd = accept(e->lfd, 0, 0);
    if (cfd < 0) { fail("accept"); return 0; }
    unsigned char buf[1024];
    uint32_t total = 0;
    while (total < e->bytes) {
        int64_t n = read(cfd, buf, sizeof buf);
        if (n <= 0) { break; }
        int64_t off = 0;
        while (off < n) {
            int64_t w = write(cfd, buf + off, (uint64_t)(n - off));
            if (w <= 0) { fail("server write"); close(cfd); return 0; }
            off += w;
        }
        total += (uint32_t)n;
    }
    // Half-close: the client must see EOF while our reads still work.
    shutdown(cfd, SHUT_WR);
    close(cfd);
    return 0;
}

static void test_handshake(void) {
    int lfd = listen_on(PORT_ECHO);
    if (lfd < 0) { return; }

    struct echo_arg arg = { lfd, 16 };
    pthread_t th;
    pthread_create(&th, 0, echo_server, &arg);

    int cfd = connect_to(PORT_ECHO);
    if (cfd < 0) { fail("connect to a listening port"); close(lfd); return; }

    // getpeername on the client must name the server's port.
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    if (getpeername(cfd, (struct sockaddr *)&peer, &plen) != 0) {
        fail("getpeername");
    } else if (ntohs(peer.sin_port) != PORT_ECHO) {
        fail("getpeername reported the wrong port");
    }

    const char *msg = "sixteen bytes!!";
    if (write(cfd, msg, 16) != 16) { fail("client write"); }
    unsigned char back[16];
    int64_t got = 0;
    while (got < 16) {
        int64_t n = read(cfd, back + got, (uint64_t)(16 - got));
        if (n <= 0) { break; }
        got += n;
    }
    if (got != 16 || memcmp(back, msg, 16) != 0) {
        fail("echo did not come back byte-exact");
    }
    // The server half-closed, so a further read must report EOF rather
    // than blocking forever.
    if (read(cfd, back, 1) != 0) { fail("no EOF after the peer's FIN"); }

    close(cfd);
    pthread_join(th, 0);
    close(lfd);
    printf("[tcptest] handshake, echo and EOF passed\n");
}

// ---- 2. a transfer larger than the send buffer ----------------------

// Single-threaded, and deliberately so. A second thread was the
// obvious shape and cost an afternoon: a user thread's stack is four
// pages, printf and an 8 KiB buffer do not both fit, and the failure
// arrived as a bare SIGSEGV with the transfer stalled behind it.
// Driving both ends from one thread with non-blocking sockets removes
// the thread, removes the stack, and tests the window HARDER -- because
// interleaving the writes and reads is what makes the window close and
// reopen repeatedly rather than once.
// There is no usleep in this libc; nanosleep is what exists.
static void nap_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

static void test_bulk(void) {
    int lfd = listen_on(PORT_BULK);
    if (lfd < 0) { return; }
    set_nonblock(lfd);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) { fail("bulk socket"); close(lfd); return; }
    set_nonblock(cfd);
    struct sockaddr_in a;
    addr_for(&a, INADDR_LOOPBACK, PORT_BULK);
    int rc = connect(cfd, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && rc != -EINPROGRESS) { fail("bulk connect"); close(cfd); close(lfd); return; }

    int sfd = -1;
    for (int spin = 0; spin < 2000 && sfd < 0; spin++) {
        sfd = accept(lfd, 0, 0);
        if (sfd < 0) { nap_ms(1); }
    }
    if (sfd < 0) { fail("bulk accept never completed"); close(cfd); close(lfd); return; }
    set_nonblock(sfd);

    static unsigned char out[4096];
    static unsigned char in[4096];
    uint32_t sent = 0, got = 0;
    int bad = 0, idle = 0;

    while (got < BULK_BYTES && idle < 5000) {
        int progress = 0;

        if (sent < BULK_BYTES) {
            uint32_t chunk = BULK_BYTES - sent;
            if (chunk > sizeof out) { chunk = (uint32_t)sizeof out; }
            for (uint32_t i = 0; i < chunk; i++) { out[i] = pat(sent + i); }
            int64_t w = write(cfd, out, chunk);
            if (w > 0) { sent += (uint32_t)w; progress = 1; }
            else if (w != -EAGAIN) {
                printf("[tcptest] FAILED: bulk write returned %d\n", (int)w);
                failures++;
                break;
            }
        }

        int64_t n = read(sfd, in, sizeof in);
        if (n > 0) {
            if (n > (int64_t)sizeof in) { fail("read overran the buffer"); break; }
            for (int64_t i = 0; i < n; i++) {
                if (in[i] != pat(got + (uint32_t)i)) { bad = 1; }
            }
            got += (uint32_t)n;
            progress = 1;
        } else if (n != -EAGAIN && n != 0) {
            printf("[tcptest] FAILED: bulk read returned %d\n", (int)n);
            failures++;
            break;
        }

        if (progress) { idle = 0; } else { idle++; nap_ms(1); }
    }

    if (bad)                { fail("bulk transfer corrupted a byte"); }
    if (got != BULK_BYTES)  {
        printf("[tcptest] FAILED: bulk moved %u of %u bytes\n",
               (unsigned)got, (unsigned)BULK_BYTES);
        failures++;
    }
    close(cfd);
    close(sfd);
    close(lfd);
    if (!bad && got == BULK_BYTES) {
        printf("[tcptest] %uKiB through a 32KiB window passed\n",
               (unsigned)(BULK_BYTES / 1024));
    }
}

// ---- 3. refusal, options, and non-blocking connect ------------------

static void test_refused(void) {
    int rc = connect_to(PORT_CLOSED);
    if (rc != -ECONNREFUSED) {
        printf("[tcptest] FAILED: connect to a closed port gave %d, "
               "wanted -ECONNREFUSED\n", rc);
        failures++;
    } else {
        printf("[tcptest] connect to a closed port refused\n");
    }
}

static void test_options(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fail("socket for options"); return; }

    int on = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on) != 0) {
        fail("setsockopt(TCP_NODELAY)");
    }
    int back = 0; socklen_t bl = sizeof back;
    if (getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &back, &bl) != 0 || back != 1) {
        fail("TCP_NODELAY did not read back");
    }
    // An unknown option must be REFUSED, not silently accepted: a
    // program that sets an option and does not get it behaves
    // mysteriously forever after.
    if (setsockopt(fd, SOL_SOCKET, 9999, &on, sizeof on) != -ENOPROTOOPT) {
        fail("an unknown sockopt was accepted");
    }
    int type = 0; socklen_t tl = sizeof type;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &tl) != 0 || type != SOCK_STREAM) {
        fail("SO_TYPE");
    }
    close(fd);
    printf("[tcptest] socket options passed\n");
}

static void test_nonblocking_connect(void) {
    int lfd = listen_on(PORT_NB);
    if (lfd < 0) { return; }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { fail("socket for nonblocking connect"); close(lfd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    struct sockaddr_in a;
    addr_for(&a, INADDR_LOOPBACK, PORT_NB);
    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc != -EINPROGRESS && rc != 0) {
        printf("[tcptest] FAILED: nonblocking connect gave %d\n", rc);
        failures++;
    }

    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (poll(&pfd, 1, 3000) <= 0 || !(pfd.revents & POLLOUT)) {
        fail("poll(POLLOUT) never fired for a nonblocking connect");
    } else {
        int err = 0; socklen_t el = sizeof err;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
            printf("[tcptest] FAILED: SO_ERROR after connect = %d\n", err);
            failures++;
        }
    }
    close(fd);
    close(lfd);
    printf("[tcptest] nonblocking connect passed\n");
}

// ---- 4. the recovery paths, which otherwise never run ---------------
//
// Loopback delivers perfectly and so does slirp. Without deliberate
// loss the retransmission timer, fast retransmit and the reassembly
// queue are dead code that happens to compile. This turns on 1-in-8
// drop and 1-in-5 reordering, moves 32 KiB through it, and asserts BOTH
// that the bytes are exact AND that the recovery counters moved -- a
// clean transfer under 1-in-8 loss means the injector is not wired up,
// which is a failure of the test rather than a success of the stack.
#define LOSS_BYTES (32 * 1024)

static void test_loss(void) {
    if (neoos_test_tcp_fault(0, 0) == -ENOSYS) {
        printf("[tcptest] SKIPPED: no test hooks in this build\n");
        return;
    }

    int lfd = listen_on(PORT_LOSS);
    if (lfd < 0) { return; }
    set_nonblock(lfd);

    int cfd = socket(AF_INET, SOCK_STREAM, 0);
    if (cfd < 0) { fail("loss socket"); close(lfd); return; }
    set_nonblock(cfd);
    struct sockaddr_in a;
    addr_for(&a, INADDR_LOOPBACK, PORT_LOSS);
    int rc = connect(cfd, (struct sockaddr *)&a, sizeof a);
    if (rc != 0 && rc != -EINPROGRESS) { fail("loss connect"); close(cfd); close(lfd); return; }

    int sfd = -1;
    for (int spin = 0; spin < 3000 && sfd < 0; spin++) {
        sfd = accept(lfd, 0, 0);
        if (sfd < 0) { nap_ms(1); }
    }
    if (sfd < 0) { fail("loss accept never completed"); close(cfd); close(lfd); return; }
    set_nonblock(sfd);

    long rt0 = neoos_test_tcp_retrans();
    long re0 = neoos_test_tcp_reasm();
    neoos_test_tcp_fault(8, 5);

    static unsigned char out[2048];
    static unsigned char in[2048];
    uint32_t sent = 0, got = 0;
    int bad = 0, idle = 0;

    while (got < LOSS_BYTES && idle < 30000) {
        int progress = 0;
        if (sent < LOSS_BYTES) {
            uint32_t chunk = LOSS_BYTES - sent;
            if (chunk > sizeof out) { chunk = (uint32_t)sizeof out; }
            for (uint32_t i = 0; i < chunk; i++) { out[i] = pat(sent + i); }
            int64_t w = write(cfd, out, chunk);
            if (w > 0) { sent += (uint32_t)w; progress = 1; }
        }
        int64_t n = read(sfd, in, sizeof in);
        if (n > 0) {
            for (int64_t i = 0; i < n; i++) {
                if (in[i] != pat(got + (uint32_t)i)) { bad = 1; }
            }
            got += (uint32_t)n;
            progress = 1;
        }
        if (progress) { idle = 0; } else { idle++; nap_ms(1); }
    }

    neoos_test_tcp_fault(0, 0);
    long rt1 = neoos_test_tcp_retrans();
    long re1 = neoos_test_tcp_reasm();

    if (bad)               { fail("lossy transfer corrupted a byte"); }
    if (got != LOSS_BYTES) {
        printf("[tcptest] FAILED: lossy transfer moved %u of %u bytes\n",
               (unsigned)got, (unsigned)LOSS_BYTES);
        failures++;
    }
    // The assertion that catches an injector that is not actually
    // injecting -- which would make everything above pass for the wrong
    // reason.
    if (rt1 == rt0) { fail("1-in-8 loss produced no retransmissions"); }
    if (re1 == re0) { fail("1-in-5 reordering queued nothing out of order"); }

    close(cfd);
    close(sfd);
    close(lfd);
    if (!failures) {
        // %d, not %ld: this libc's printf has no length modifiers.
        printf("[tcptest] 32KiB under 1-in-8 loss, retrans=%d reasm=%d\n",
               (int)(rt1 - rt0), (int)(re1 - re0));
    }
}

int main(void) {
    printf("[tcptest] start\n");
    test_handshake();
    test_bulk();
    test_refused();
    test_options();
    test_nonblocking_connect();
    test_loss();
    if (failures) {
        printf("[tcptest] SOME CHECKS FAILED\n");
        return 1;
    }
    printf("[tcptest] ALL PASSED\n");
    return 0;
}
