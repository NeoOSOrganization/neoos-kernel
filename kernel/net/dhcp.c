#include "net/dhcp.h"
#include "net/net.h"
#include "net/route.h"
#include "net/netrx.h"
#include "sync/waitq.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"
#include "drivers/char/timer.h"
#include "lib/rand.h"
#include "smp/smp.h"
#include "errno.h"

// The BOOTP fixed part, byte for byte. 236 bytes: a struct that comes
// out at 240 because the compiler padded chaddr is a bug whose only
// symptom is a server that silently ignores you, so it is asserted.
struct dhcp_msg {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs_n, flags_n;
    uint32_t ciaddr_n, yiaddr_n, siaddr_n, giaddr_n;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
} __attribute__((packed));
_Static_assert(sizeof(struct dhcp_msg) == 236, "BOOTP fixed part must be 236 bytes");

#define DHCP_OP_REQUEST 1
#define DHCP_OP_REPLY   2

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPNAK      6
#define DHCPACK      5

#define OPT_PAD           0
#define OPT_NETMASK       1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_PARAM_LIST    55
#define OPT_T1            58
#define OPT_T2            59
#define OPT_CLIENT_ID     61
#define OPT_END           255

#define DHCP_MAX_MSG   576
#define DHCP_MAGIC     0x63825363u

// Five seconds, then a static address. A network stack that hangs the
// boot because a server did not answer is worse than one that guesses
// QEMU user-networking's well-known first lease -- and the log line is
// DISTINCT, so a broken client is never mistaken for an absent server.
#define DHCP_FALLBACK_TICKS (5 * TIMER_HZ)
#define FALLBACK_ADDR 0x0F02000Au   // 10.0.2.15
#define FALLBACK_MASK 0x00FFFFFFu   // /24
#define FALLBACK_GW   0x0202000Au   // 10.0.2.2

static struct netdev     *ifdev;
static enum dhcp_state    state;
static struct dhcp_lease  lease;
static int                bound;
static uint32_t           cur_xid;
static struct waitq       dhcp_wait;
static uint64_t           started_tick;

// The offer being considered, filled by the receive hook and consumed
// by the thread. Volatile because the two run on different CPUs and the
// thread polls it.
static volatile int       offer_ready, ack_ready, nak_ready;
static struct dhcp_lease  pending;

enum dhcp_state dhcp_state(void) { return state; }
const struct dhcp_lease *dhcp_current(void) { return bound ? &lease : 0; }

// ------------------------------------------------------------ building

static uint32_t rd32(const void *p) {
    uint32_t v;
    for (int i = 0; i < 4; i++) { ((uint8_t *)&v)[i] = ((const uint8_t *)p)[i]; }
    return v;                        // network order, unconverted
}

static int build(uint8_t *buf, uint8_t type, uint32_t ciaddr_n,
                 uint32_t requested_n, uint32_t server_n) {
    for (int i = 0; i < DHCP_MAX_MSG; i++) { buf[i] = 0; }
    struct dhcp_msg *m = (struct dhcp_msg *)buf;
    m->op    = DHCP_OP_REQUEST;
    m->htype = 1;                     // Ethernet
    m->hlen  = 6;
    m->xid   = cur_xid;
    // Broadcast flag SET. We may not have an address yet, and a server
    // that unicasts a reply to an address the client does not hold has
    // to ARP for it -- which the client cannot answer.
    m->flags_n = htons_k(0x8000);
    m->ciaddr_n = ciaddr_n;
    for (int i = 0; i < 6; i++) { m->chaddr[i] = ifdev->hwaddr[i]; }

    uint8_t *o = buf + sizeof(struct dhcp_msg);
    uint32_t cookie = htonl_k(DHCP_MAGIC);
    for (int i = 0; i < 4; i++) { *o++ = ((const uint8_t *)&cookie)[i]; }

    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = type;

    *o++ = OPT_CLIENT_ID; *o++ = 7; *o++ = 1;
    for (int i = 0; i < 6; i++) { *o++ = ifdev->hwaddr[i]; }

    if (requested_n) {
        *o++ = OPT_REQUESTED_IP; *o++ = 4;
        for (int i = 0; i < 4; i++) { *o++ = ((const uint8_t *)&requested_n)[i]; }
    }
    if (server_n) {
        *o++ = OPT_SERVER_ID; *o++ = 4;
        for (int i = 0; i < 4; i++) { *o++ = ((const uint8_t *)&server_n)[i]; }
    }

    *o++ = OPT_PARAM_LIST; *o++ = 3;
    *o++ = OPT_NETMASK; *o++ = OPT_ROUTER; *o++ = OPT_DNS;

    *o++ = OPT_END;
    // Padded to 300 total, which is the smallest a BOOTP relay is
    // required to forward. Slirp does not care; a real relay does.
    uint32_t len = (uint32_t)(o - buf);
    return len < 300 ? 300 : (int)len;
}

// ------------------------------------------------------------- parsing

// Returns the message type, or 0 when the message is not a DHCP reply
// this client should act on. Every bound is explicit: an options walk
// that trusts a length field is a buffer overrun waiting for a hostile
// or merely broken server.
static int parse(const uint8_t *buf, uint32_t len, uint32_t expect_xid,
                 struct dhcp_lease *out) {
    if (len < sizeof(struct dhcp_msg) + 4) { return 0; }
    const struct dhcp_msg *m = (const struct dhcp_msg *)buf;
    if (m->op != DHCP_OP_REPLY) { return 0; }
    if (m->xid != expect_xid)   { return 0; }
    if (rd32(buf + sizeof(struct dhcp_msg)) != htonl_k(DHCP_MAGIC)) { return 0; }

    for (int i = 0; i < 4; i++) { ((uint8_t *)&out->addr_n)[i] = ((const uint8_t *)&m->yiaddr_n)[i]; }
    out->mask_n = 0; out->gw_n = 0; out->server_n = 0; out->dns_n = 0;
    out->lease_secs = 0; out->t1_secs = 0; out->t2_secs = 0;
    out->from_server = 1;

    int type = 0;
    uint32_t i = (uint32_t)sizeof(struct dhcp_msg) + 4;
    while (i < len) {
        uint8_t code = buf[i++];
        if (code == OPT_END) { break; }
        if (code == OPT_PAD) { continue; }
        if (i >= len) { return 0; }             // a code with no length
        uint8_t olen = buf[i++];
        if (i + olen > len) { return 0; }       // a length past the end
        const uint8_t *v = buf + i;
        switch (code) {
        case OPT_MSG_TYPE:   if (olen == 1) { type = v[0]; } break;
        case OPT_NETMASK:    if (olen == 4) { out->mask_n    = rd32(v); } break;
        case OPT_ROUTER:     if (olen >= 4) { out->gw_n      = rd32(v); } break;
        case OPT_DNS:        if (olen >= 4) { out->dns_n     = rd32(v); } break;
        case OPT_SERVER_ID:  if (olen == 4) { out->server_n  = rd32(v); } break;
        case OPT_LEASE_TIME: if (olen == 4) { out->lease_secs = ntohl_k(rd32(v)); } break;
        case OPT_T1:         if (olen == 4) { out->t1_secs    = ntohl_k(rd32(v)); } break;
        case OPT_T2:         if (olen == 4) { out->t2_secs    = ntohl_k(rd32(v)); } break;
        default: break;
        }
        i += olen;
    }
    if (!type) { return 0; }
    // The RFC's defaults, applied here rather than at every use.
    if (!out->t1_secs) { out->t1_secs = out->lease_secs / 2; }
    if (!out->t2_secs) { out->t2_secs = out->lease_secs / 8 * 7; }
    return type;
}

// ------------------------------------------------------------ the wire

static void dhcp_rx(struct netdev *dev, uint32_t src_n, uint16_t sport_n,
                    uint32_t dst_n, uint16_t dport_n,
                    const uint8_t *data, uint32_t len) {
    (void)dev; (void)src_n; (void)sport_n; (void)dst_n; (void)dport_n;
    struct dhcp_lease l;
    int type = parse(data, len, cur_xid, &l);
    if (type == DHCPOFFER)     { pending = l; offer_ready = 1; }
    else if (type == DHCPACK)  { pending = l; ack_ready = 1; }
    else if (type == DHCPNAK)  { nak_ready = 1; }
    waitq_wake_all(&dhcp_wait);
}

static int send_msg(uint8_t type, uint32_t ciaddr_n, uint32_t requested_n,
                    uint32_t server_n, uint32_t dst_n) {
    static uint8_t buf[DHCP_MAX_MSG];
    int len = build(buf, type, ciaddr_n, requested_n, server_n);
    return net_udp_output(ifdev->ip_n, htons_k(DHCP_CLIENT_PORT),
                          dst_n, htons_k(DHCP_SERVER_PORT), buf, (uint32_t)len);
}

static void sleep_ticks(uint64_t n) {
    waitq_sleep_timeout(&dhcp_wait, 0, timer_ticks() + n);
}

static void install(const struct dhcp_lease *l) {
    lease = *l;
    if (!lease.mask_n) { lease.mask_n = FALLBACK_MASK; }
    net_ifconfig(ifdev, lease.addr_n, lease.mask_n, lease.gw_n);
    bound = 1;
    state = DHCP_BOUND;

    // ONE serial write, not two. The client runs on another CPU while
    // the BSP is printing its own selftests, and a line emitted as
    // prefix-then-address comes out with somebody else's line spliced
    // into the middle of it. That was found by proving the detector,
    // where the fallback line and the failure it caused arrived
    // interleaved and unreadable.
    static const char pre_bound[] = "[dhcp] bound ";
    static const char pre_fall[]  = "[dhcp] no lease in 5s -- "
                                    "falling back to static ";
    const char *pre = l->from_server ? pre_bound : pre_fall;
    char line[80];
    int n = 0;
    for (const char *p = pre; *p; p++) { line[n++] = *p; }
    const uint8_t *a = (const uint8_t *)&lease.addr_n;
    for (int i = 0; i < 4; i++) {
        uint8_t v = a[i];
        if (v >= 100) { line[n++] = (char)('0' + v / 100); }
        if (v >= 10)  { line[n++] = (char)('0' + (v / 10) % 10); }
        line[n++] = (char)('0' + v % 10);
        line[n++] = (i < 3) ? '.' : '/';
    }
    // The prefix length, so "10.0.2.15/24 via 10.0.2.2" reads as one
    // fact rather than three separate ones to correlate.
    uint32_t m = ntohl_k(lease.mask_n);
    int bits = 0;
    while (m & 0x80000000u) { bits++; m <<= 1; }
    if (bits >= 10) { line[n++] = (char)('0' + bits / 10); }
    line[n++] = (char)('0' + bits % 10);
    if (lease.gw_n) {
        for (const char *p = " via "; *p; p++) { line[n++] = *p; }
        const uint8_t *g = (const uint8_t *)&lease.gw_n;
        for (int i = 0; i < 4; i++) {
            uint8_t v = g[i];
            if (v >= 100) { line[n++] = (char)('0' + v / 100); }
            if (v >= 10)  { line[n++] = (char)('0' + (v / 10) % 10); }
            line[n++] = (char)('0' + v % 10);
            if (i < 3) { line[n++] = '.'; }
        }
    }
    line[n++] = '\n';
    serial_write_raw_n(line, (uint64_t)n);
}

static void fallback(void) {
    struct dhcp_lease l = { 0 };
    l.addr_n = FALLBACK_ADDR;
    l.mask_n = FALLBACK_MASK;
    l.gw_n   = FALLBACK_GW;
    l.from_server = 0;
    install(&l);
}

// --------------------------------------------------------- the machine

static void dhcp_thread(void) {
    for (;;) {
        switch (state) {

        case DHCP_INIT:
            cur_xid = (uint32_t)rand_u64();
            offer_ready = ack_ready = nak_ready = 0;
            state = DHCP_SELECTING;
            break;

        case DHCP_SELECTING: {
            // 1, 2, 4, 8 seconds. The RFC asks for randomised backoff;
            // this is the doubling without the jitter, because the
            // jitter exists to desynchronise a room full of machines
            // booting at once and there is one machine here.
            int sent = 0;
            for (uint64_t wait = 1; wait <= 8 && !offer_ready; wait *= 2) {
                send_msg(DHCPDISCOVER, 0, 0, 0, IP_BROADCAST_N);
                sent = 1;
                for (uint64_t t = 0; t < wait * TIMER_HZ && !offer_ready; t += 5) {
                    sleep_ticks(5);
                }
                if (!bound && !offer_ready &&
                    timer_ticks() - started_tick > DHCP_FALLBACK_TICKS) {
                    fallback();
                    // NOT a return: keep asking. The fallback is a way
                    // to make progress, not a decision to stop trying.
                    break;
                }
            }
            (void)sent;
            if (offer_ready) {
                state = DHCP_REQUESTING;
            } else if (bound) {
                sleep_ticks(30 * TIMER_HZ);
                state = DHCP_INIT;
            }
            break;
        }

        case DHCP_REQUESTING: {
            struct dhcp_lease want = pending;
            ack_ready = nak_ready = 0;
            for (int try = 0; try < 3 && !ack_ready && !nak_ready; try++) {
                send_msg(DHCPREQUEST, 0, want.addr_n, want.server_n, IP_BROADCAST_N);
                for (uint64_t t = 0; t < 2 * TIMER_HZ && !ack_ready && !nak_ready; t += 5) {
                    sleep_ticks(5);
                }
            }
            if (ack_ready)      { install(&pending); }
            else                { state = DHCP_INIT; }
            break;
        }

        case DHCP_BOUND:
            sleep_ticks((lease.t1_secs ? lease.t1_secs : 600) * TIMER_HZ);
            state = DHCP_RENEWING;
            break;

        case DHCP_RENEWING:
            ack_ready = nak_ready = 0;
            cur_xid = (uint32_t)rand_u64();
            // Unicast to the server that granted it, from our own
            // address: renewal is a conversation with one server, not an
            // auction.
            send_msg(DHCPREQUEST, lease.addr_n, 0, 0, lease.server_n);
            for (uint64_t t = 0; t < 10 * TIMER_HZ && !ack_ready && !nak_ready; t += 5) {
                sleep_ticks(5);
            }
            if (ack_ready)      { install(&pending); }
            else if (nak_ready) { state = DHCP_INIT; }
            else                { state = DHCP_REBINDING; }
            break;

        case DHCP_REBINDING:
            ack_ready = nak_ready = 0;
            cur_xid = (uint32_t)rand_u64();
            send_msg(DHCPREQUEST, lease.addr_n, 0, 0, IP_BROADCAST_N);
            for (uint64_t t = 0; t < 10 * TIMER_HZ && !ack_ready && !nak_ready; t += 5) {
                sleep_ticks(5);
            }
            state = ack_ready ? (install(&pending), DHCP_BOUND) : DHCP_INIT;
            break;
        }
    }
}

void dhcp_start(struct netdev *dev) {
    if (!dev) { return; }
    ifdev = dev;
    state = DHCP_INIT;
    bound = 0;
    started_tick = timer_ticks();
    waitq_init(&dhcp_wait);
    net_udp_hook(htons_k(DHCP_CLIENT_PORT), dhcp_rx);
    // The broadcast route, so a DISCOVER has somewhere to go before
    // there is an address. net_ifconfig re-installs it on every lease.
    net_ifconfig(dev, 0, 0, 0);
    thread_alloc_kernel(dhcp_thread);
}

// ------------------------------------------------------- parser tests

// Each builds a plausible OFFER, damages exactly one thing, and asserts
// the parser refuses it. They return nonzero on success (refused).
static uint32_t build_offer(uint8_t *buf, uint32_t xid, int good_cookie) {
    for (int i = 0; i < 400; i++) { buf[i] = 0; }
    struct dhcp_msg *m = (struct dhcp_msg *)buf;
    m->op = DHCP_OP_REPLY; m->htype = 1; m->hlen = 6;
    m->xid = xid;
    m->yiaddr_n = 0x0F02000Au;
    uint32_t cookie = htonl_k(good_cookie ? DHCP_MAGIC : 0x12345678u);
    uint8_t *o = buf + sizeof(struct dhcp_msg);
    for (int i = 0; i < 4; i++) { *o++ = ((const uint8_t *)&cookie)[i]; }
    *o++ = OPT_MSG_TYPE; *o++ = 1; *o++ = DHCPOFFER;
    *o++ = OPT_NETMASK;  *o++ = 4; *o++ = 255; *o++ = 255; *o++ = 255; *o++ = 0;
    *o++ = OPT_END;
    return (uint32_t)(o - buf);
}

int dhcp_parse_test_bad_cookie(void) {
    uint8_t buf[400]; struct dhcp_lease l;
    uint32_t len = build_offer(buf, cur_xid, 0);
    return parse(buf, len, cur_xid, &l) == 0;
}

int dhcp_parse_test_truncated_options(void) {
    uint8_t buf[400]; struct dhcp_lease l;
    uint32_t len = build_offer(buf, cur_xid, 1);
    // Claim a netmask 40 bytes long, in a buffer that ends before it.
    buf[sizeof(struct dhcp_msg) + 4 + 3 + 1] = 40;
    return parse(buf, len, cur_xid, &l) == 0;
}

int dhcp_parse_test_wrong_xid(void) {
    uint8_t buf[400]; struct dhcp_lease l;
    uint32_t len = build_offer(buf, cur_xid ^ 0xA5A5A5A5u, 1);
    return parse(buf, len, cur_xid, &l) == 0;
}

// ------------------------------------------------------------ selftest

static int dhcp_fail(const char *msg) {
    serial_write_string("[dhcp] FAILED: ");
    serial_write_string(msg);
    serial_write_string("\n");
    return 1;
}

void dhcp_selftest(void) {
    serial_write_string("[dhcp] selftest\n");
    if (!net_device()) { serial_write_string("[dhcp] SKIPPED: no NIC\n"); return; }
    if (smp_cpu_count() < 2) {
        serial_write_string("[dhcp] SKIPPED: one CPU, nothing runs the client\n");
        return;
    }
    int failed = 0;

    uint64_t w = netrx_boot_window_open();
    for (int i = 0; i < 1200 && state != DHCP_BOUND; i++) { netrx_boot_park(); }
    netrx_boot_window_close(w);

    const struct dhcp_lease *l = dhcp_current();
    if (!l) {
        failed |= dhcp_fail("never bound");
    } else {
        // A REAL lease, not the fallback. Without this assertion a
        // broken client and an absent server look identical.
        if (!l->from_server) {
            failed |= dhcp_fail("fell back to the static address");
        }
        if ((l->addr_n & 0x00FFFFFFu) != 0x0002000Au) {
            failed |= dhcp_fail("address is not in 10.0.2.0/24");
        }
        if (l->gw_n != 0x0202000Au) {
            failed |= dhcp_fail("gateway is not 10.0.2.2");
        }
        if (l->mask_n != 0x00FFFFFFu) {
            failed |= dhcp_fail("netmask is not /24");
        }
        if (l->lease_secs == 0) {
            failed |= dhcp_fail("zero lease time");
        }
        uint32_t nh = 0;
        if (route_lookup(0x08080808u, &nh) != net_device() || nh != l->gw_n) {
            failed |= dhcp_fail("default route not installed");
        }
    }

    if (!dhcp_parse_test_bad_cookie() ||
        !dhcp_parse_test_truncated_options() ||
        !dhcp_parse_test_wrong_xid()) {
        failed |= dhcp_fail("a malformed offer was accepted");
    }

    serial_write_string(failed ? "[dhcp] FAILED\n" : "[dhcp] ALL PASSED\n");
}
