// D5's wire test: a TCP connection that leaves the machine.
//
// The guest connects OUT to a host-side echo server at 10.0.2.2.
// That direction is deliberate: QEMU's user-mode networking forwards
// guest-initiated TCP to the host with no privileges and no tap device,
// so this runs in CI as an ordinary user. The other direction would
// need hostfwd plus a listener inside NeoOS whose readiness the host
// has no way to observe.
//
// What it proves that tcptest cannot: the route table chose eth0, ARP
// resolved the gateway, the link layer framed the segments, the NIC
// transmitted them, the interrupt delivered the replies, and the netrx
// thread ran the receive path -- for a protocol with sequence numbers,
// against a peer this kernel does not control.

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define GATEWAY   0x0A000202u     /* 10.0.2.2, host order */
#define ECHO_PORT 7900
#define WIRE_BYTES (16 * 1024)

static unsigned char pat(uint32_t i) {
    return (unsigned char)((i * 31u + (i >> 8)) & 0xFF);
}

static void nap_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, 0);
}

int main(void) {
    printf("[tcpwire] start\n");

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { printf("[tcpwire] FAILED: socket = %d\n", fd); return 1; }

    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons(ECHO_PORT);
    a.sin_addr.s_addr = htonl(GATEWAY);

    int rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    if (rc != 0) {
        // NOT a failure. A developer running `make test` on a machine
        // with no host-side server must still get a green suite; the
        // gauntlet, which starts the server itself, asserts the marker.
        // ECONNREFUSED here is the honest, useful outcome -- it means
        // the stack got all the way to the host and was told no.
        printf("[tcpwire] SKIPPED: no echo server at 10.0.2.2:%d (connect = %d)\n",
               ECHO_PORT, rc);
        close(fd);
        return 0;
    }

    // getpeername must name the gateway, not something invented locally.
    struct sockaddr_in peer;
    socklen_t plen = sizeof peer;
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) != 0 ||
        peer.sin_addr.s_addr != htonl(GATEWAY)) {
        printf("[tcpwire] FAILED: getpeername did not report 10.0.2.2\n");
        close(fd);
        return 1;
    }

    static unsigned char out[2048];
    static unsigned char in[2048];
    uint32_t sent = 0, got = 0;
    int bad = 0, idle = 0;

    while (got < WIRE_BYTES && idle < 20000) {
        int progress = 0;

        if (sent < WIRE_BYTES) {
            uint32_t chunk = WIRE_BYTES - sent;
            if (chunk > sizeof out) { chunk = (uint32_t)sizeof out; }
            for (uint32_t i = 0; i < chunk; i++) { out[i] = pat(sent + i); }
            int64_t w = write(fd, out, chunk);
            if (w > 0) { sent += (uint32_t)w; progress = 1; }
            else if (w != -EAGAIN && w < 0) {
                printf("[tcpwire] FAILED: write returned %d\n", (int)w);
                close(fd);
                return 1;
            }
        }

        int64_t n = read(fd, in, sizeof in);
        if (n > 0) {
            for (int64_t i = 0; i < n; i++) {
                if (in[i] != pat(got + (uint32_t)i)) { bad = 1; }
            }
            got += (uint32_t)n;
            progress = 1;
        } else if (n == 0) {
            break;                       // the server closed
        } else if (n != -EAGAIN) {
            printf("[tcpwire] FAILED: read returned %d\n", (int)n);
            close(fd);
            return 1;
        }

        if (progress) { idle = 0; } else { idle++; nap_ms(1); }
    }

    close(fd);

    if (bad) {
        printf("[tcpwire] FAILED: the echo came back corrupted\n");
        return 1;
    }
    if (got != WIRE_BYTES) {
        printf("[tcpwire] FAILED: echoed %u of %u bytes\n",
               (unsigned)got, (unsigned)WIRE_BYTES);
        return 1;
    }
    printf("[tcpwire] %uKiB echoed by the host, byte for byte\n",
           (unsigned)(WIRE_BYTES / 1024));
    printf("[tcpwire] ALL PASSED\n");
    return 0;
}
