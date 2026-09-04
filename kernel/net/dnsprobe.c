#include "net/dnsprobe.h"
#include "net/net.h"
#include "net/netrx.h"
#include "drivers/char/serial.h"
#include "drivers/char/timer.h"
#include "smp/smp.h"

#define DNS_SERVER_N 0x0302000Au    // 10.0.2.3, slirp's built-in resolver
#define DNS_PORT     53
#define PROBE_PORT   0xBEEF         // arbitrary, and never bound by a socket

// QNAME "example.com", QTYPE A, QCLASS IN, built by hand so this does
// not depend on a name encoder that does not exist.
static const uint8_t query[] = {
    0xC0, 0xDE,                     // transaction id
    0x01, 0x00,                     // standard query, recursion desired
    0x00, 0x01,                     // one question
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7, 'e','x','a','m','p','l','e',
    3, 'c','o','m',
    0,
    0x00, 0x01,                     // QTYPE  A
    0x00, 0x01,                     // QCLASS IN
};

static volatile int reply_seen;
static uint8_t  reply[64];
static uint32_t reply_len;

static void dns_rx(struct netdev *dev, uint32_t src_n, uint16_t sport_n,
                   uint32_t dst_n, uint16_t dport_n,
                   const uint8_t *data, uint32_t len) {
    (void)dev; (void)src_n; (void)sport_n; (void)dst_n; (void)dport_n;
    reply_len = len < sizeof reply ? len : (uint32_t)sizeof reply;
    for (uint32_t i = 0; i < reply_len; i++) { reply[i] = data[i]; }
    reply_seen = 1;
}

static int dns_fail(const char *msg) {
    serial_write_string("[dns] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

void dns_probe_selftest(void) {
    serial_write_string("[dns] selftest\n");
    if (!net_device()) { serial_write_string("[dns] SKIPPED: no NIC\n"); return; }
    if (smp_cpu_count() < 2) {
        serial_write_string("[dns] SKIPPED: one CPU, nothing drains netrx\n");
        return;
    }
    int failed = 0;

    if (net_udp_hook(htons_k(PROBE_PORT), dns_rx) != 0) {
        failed |= dns_fail("no free UDP hook slot");
        serial_write_string("[dns] FAILED\n");
        return;
    }

    uint64_t w = netrx_boot_window_open();
    int rc = net_udp_output(0, htons_k(PROBE_PORT),
                            DNS_SERVER_N, htons_k(DNS_PORT),
                            query, sizeof query);
    for (int i = 0; rc == 0 && i < 400 && !reply_seen; i++) { netrx_boot_park(); }
    netrx_boot_window_close(w);

    if (rc != 0) {
        failed |= dns_fail("could not send the query");
    } else if (!reply_seen) {
        failed |= dns_fail("no response from 10.0.2.3");
    } else {
        if (reply_len < 12) {
            failed |= dns_fail("response is shorter than a DNS header");
        } else {
            if (reply[0] != 0xC0 || reply[1] != 0xDE) {
                failed |= dns_fail("response id does not match the query");
            }
            if (!(reply[2] & 0x80)) {
                failed |= dns_fail("response does not have the QR bit set");
            }
            // RCODE 0 (answered) and 3 (NXDOMAIN) are BOTH a complete
            // round trip. Asserting on the answer would make this test
            // depend on what the host's resolver believes about
            // example.com, which is not what is being tested.
            uint8_t rcode = reply[3] & 0x0F;
            if (rcode != 0 && rcode != 3) {
                failed |= dns_fail("response carries an unexpected rcode");
            }
            serial_write_string("[dns] round trip, rcode=");
            serial_write_hex64(rcode);
            serial_write_string("\n");
        }
    }

    serial_write_string(failed ? "[dns] FAILED\n" : "[dns] ALL PASSED\n");
}
