# D4 — UDP on the wire, and a DHCP client: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: use
> `superpowers:executing-plans`. Steps use `- [ ]` checkboxes.

**Goal:** NeoOS gets its address from a DHCP server rather than being
told one, and a UDP datagram survives a real round trip over the NIC.

**Architecture:** UDP barely changes — the point of having built it on
loopback first. What is new is `dhcp.c`: a lease state machine on its
own kernel thread, using the internal `net_udp_output` / deliver pair
rather than a socket, installing address, netmask and gateway into D2's
route table.

**Tech Stack:** C11 freestanding; QEMU slirp provides both the DHCP
server (10.0.2.2) and the DNS server (10.0.2.3) the tests use, with no
host cooperation and no privileges.

**Spec:** `docs/superpowers/specs/2026-09-04-d2-d5-network-stack-design.md` §5

**Status:** DONE. Results, including everything the design did not
predict, are in the spec's section 10.

## Global Constraints

- **Depends on D2 and D3.**
- **DHCP in the kernel is a deliberate divergence from Linux** and MUST
  be recorded in `docs/stdlib.md` in the commit that adds it. The seam
  is drawn so it can be undone later by adding raw sockets.
- **The 5-second static fallback is not optional.** A stack that hangs
  the boot because a server did not answer is worse than one that
  guesses QEMU's well-known lease — and it must log a *distinct* line,
  so a broken client never looks like an absent server.
- `[dhcp] ALL PASSED` and `[dns] ALL PASSED` join `REQUIRED_MARKERS`.

---

## File structure

| File | Responsibility |
|---|---|
| `kernel/net/dhcp.h/.c` | the lease state machine, option parsing, the fallback, `dhcp_selftest` |
| `kernel/net/net.c` | `udp_input` verifies a non-zero checksum; an internal UDP port hook for dhcp.c |
| `kernel/net/dnstest.c` | the UDP-over-the-wire proof: one hardcoded query to 10.0.2.3 |
| `Makefile` | both markers |

---

### Task 1: UDP checksums stop being optional

**Files:** Modify `kernel/net/net.c`

- [ ] **Step 1:** Add to `nettest`/`net_selftest` a case that injects a
      UDP datagram with a deliberately wrong checksum at a bound port
      and asserts it is **not** delivered, and a second with a zero
      checksum that **is** delivered (a zero checksum is legal on
      receive per RFC 768 and is never generated on transmit).
- [ ] **Step 2:** Run it; watch the wrong-checksum case be delivered.
- [ ] **Step 3:** Implement the check in `udp_input`: fold the
      pseudo-header sum with the datagram, require `0xFFFF`, drop and
      count `rx_csum_bad` otherwise. Skip the check when the received
      checksum field is zero.
- [ ] **Step 4:** `make test`; `[nettest] ALL PASSED`.
- [ ] **Step 5: Commit.**

```bash
git add kernel/net/net.c
git commit -m "udp: verify the checksum, now that there is a wire to corrupt it"
```

---

### Task 2: An internal UDP hook

**Files:** Modify `kernel/net/net.c`, `kernel/net/net.h`

**Interfaces:**
- Produces:
```c
// A kernel-internal consumer of one UDP port, checked BEFORE the socket
// table. dhcp.c is the only user; it exists so the lease can be
// obtained without SO_BROADCAST, raw sockets, or an address to bind to.
typedef void (*udp_kernel_handler)(struct netdev *dev,
                                   uint32_t src_n, uint16_t sport_n,
                                   const uint8_t *data, uint32_t len);
int net_udp_hook(uint16_t port_n, udp_kernel_handler fn);   // 0, or -EBUSY
```

- [ ] **Step 1:** Implement it as a two-entry table checked at the top of
      `udp_input`, before the socket lookup and before the
      port-unreachable path — a hooked port is never "closed".
- [ ] **Step 2:** A hooked datagram must still be accepted when the
      destination address is the **broadcast** 255.255.255.255 and the
      interface has no address yet. Today `net_ipv4_input` drops packets
      not addressed to us; add broadcast as an accepted destination.
- [ ] **Step 3:** `make test`; nothing regresses.
- [ ] **Step 4: Commit.**

```bash
git add kernel/net/net.c kernel/net/net.h
git commit -m "udp: a kernel-internal port hook, and accept broadcast"
```

---

### Task 3: The DHCP client

**Files:** Create `kernel/net/dhcp.h`, `kernel/net/dhcp.c`; modify
`kernel/kernel.c`, `docs/stdlib.md`, `Makefile`

**Interfaces:**
- Produces:
```c
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

enum dhcp_state { DHCP_INIT, DHCP_SELECTING, DHCP_REQUESTING,
                  DHCP_BOUND, DHCP_RENEWING, DHCP_REBINDING };

struct dhcp_lease {
    uint32_t addr_n, mask_n, gw_n, server_n, dns_n;
    uint32_t lease_secs, t1_secs, t2_secs;
    int      from_server;      // 0 when this is the static fallback
};

void dhcp_start(struct netdev *dev);       // starts the thread
enum dhcp_state dhcp_state(void);
const struct dhcp_lease *dhcp_current(void);   // 0 until bound
void dhcp_selftest(void);
```

- [ ] **Step 1: Write the failing selftest.**

```c
void dhcp_selftest(void) {
    serial_write_string("[dhcp] selftest\n");
    int failed = 0;
    if (!net_device()) { serial_write_string("[dhcp] SKIPPED: no NIC\n"); return; }
    if (smp_cpu_count() < 2) { serial_write_string("[dhcp] SKIPPED: one CPU\n"); return; }

    for (int i = 0; i < 800 && dhcp_state() != DHCP_BOUND; i++) {
        __asm__ volatile("hlt");
    }
    const struct dhcp_lease *l = dhcp_current();
    if (!l) {
        serial_write_string("[dhcp] FAILED: never bound\n"); failed = 1;
    } else {
        // A REAL lease, not the fallback. This is the assertion that
        // stops a broken client looking like an absent server.
        if (!l->from_server) {
            serial_write_string("[dhcp] FAILED: fell back to the static address\n");
            failed = 1;
        }
        if ((l->addr_n & 0x00FFFFFFu) != 0x0002000Au) {
            serial_write_string("[dhcp] FAILED: address is not in 10.0.2.0/24\n");
            failed = 1;
        }
        if (l->gw_n != 0x0202000Au) {
            serial_write_string("[dhcp] FAILED: gateway is not 10.0.2.2\n");
            failed = 1;
        }
        if (l->mask_n != 0x00FFFFFFu) {
            serial_write_string("[dhcp] FAILED: netmask is not /24\n");
            failed = 1;
        }
        if (l->lease_secs == 0) {
            serial_write_string("[dhcp] FAILED: zero lease time\n"); failed = 1;
        }
        // and the routes are actually installed
        uint32_t nh = 0;
        if (route_lookup(0x08080808u, &nh) != net_device() || nh != l->gw_n) {
            serial_write_string("[dhcp] FAILED: default route not installed\n");
            failed = 1;
        }
    }
    // Malformed offers are rejected rather than half-parsed.
    if (dhcp_parse_test_bad_cookie() == 0 ||
        dhcp_parse_test_truncated_options() == 0 ||
        dhcp_parse_test_wrong_xid() == 0) {
        serial_write_string("[dhcp] FAILED: a malformed offer was accepted\n");
        failed = 1;
    }
    serial_write_string(failed ? "[dhcp] FAILED\n" : "[dhcp] ALL PASSED\n");
}
```

- [ ] **Step 2:** Run; watch it fail to link.

- [ ] **Step 3: Implement the message.** The 236-byte BOOTP fixed part
      (`op htype hlen hops xid secs flags ciaddr yiaddr siaddr giaddr
      chaddr[16] sname[64] file[128]`), the magic cookie
      `63 82 53 63`, then options. `_Static_assert` the fixed part at
      236 bytes; a struct that is 240 because the compiler padded
      `chaddr` is a bug that presents as a server silently ignoring you.

- [ ] **Step 4: Implement parsing.** Walk options with explicit bounds:
      option 0 is pad (1 byte), 255 is end, everything else is
      `[code][len][data]`. Reject on a length that runs past the buffer,
      a missing cookie, or an xid that does not match ours. Parse 1, 3,
      6, 51, 53, 54, 58, 59. Ignore the rest. `t1`/`t2` default to
      0.5× and 0.875× of the lease when absent.

- [ ] **Step 5: Implement the state machine** on its own kernel thread:
      INIT → SELECTING (DISCOVER to 255.255.255.255:67 from 0.0.0.0:68,
      retrying at 1/2/4/8 s) → REQUESTING (REQUEST for the first offer,
      3 tries) → BOUND → RENEWING at T1 (unicast to the server) →
      REBINDING at T2 (broadcast) → INIT on expiry. A NAK returns to
      INIT. Binding calls `route_flush_dev(dev)` then installs the
      subnet route and the default route, and sets `dev->ip_n`.

- [ ] **Step 6: Implement the fallback.** If not BOUND 5 seconds after
      `dhcp_start`, install 10.0.2.15/24 via 10.0.2.2 with
      `from_server = 0`, log
      `[dhcp] no lease in 5s -- falling back to static 10.0.2.15/24`,
      and keep trying in the background.

- [ ] **Step 7: Wire it up.** `dhcp_start(net_device())` in `kernel.c`
      after `netrx_start()` and before the selftests;
      `dhcp_selftest()` after `arp_selftest()`.

- [ ] **Step 8: `make test`.** Expected `[dhcp] ALL PASSED` and a log
      line naming the leased address.

- [ ] **Step 9: Prove the detectors.**

| Break | Must fire |
|---|---|
| never send DISCOVER | `never bound` — and then the fallback line, proving both |
| accept an offer whose xid differs | `a malformed offer was accepted` |
| skip the route installation on bind | `default route not installed` |
| force the fallback path | `fell back to the static address` |

- [ ] **Step 10: Record the divergence in `docs/stdlib.md`:** DHCP runs
      in the kernel, not in a userspace client; the reason (raw sockets
      and `SO_BROADCAST` are a larger user-facing surface than the lease
      itself); and the fact that `dhcp.c` uses the same
      `net_udp_output`/deliver pair a socket would, so the decision is
      reversible.

- [ ] **Step 11: Add `"[dhcp] ALL PASSED"` to `REQUIRED_MARKERS`. Commit.**

```bash
git add kernel/net/dhcp.c kernel/net/dhcp.h kernel/kernel.c docs/stdlib.md Makefile
git commit -m "D4: NeoOS asks for its address instead of being told one"
```

---

### Task 4: A real UDP round trip — the DNS probe

**Files:** Create `kernel/net/dnstest.c`; modify `Makefile`

This is not a resolver. It is thirty hardcoded bytes and an assertion on
the response header, and it exists because it is the cheapest honest
proof that a UDP datagram left the machine and an answer came back.

- [ ] **Step 1: Write the test.**

```c
// QNAME "example.com", QTYPE A, QCLASS IN. Built by hand so the test
// does not depend on a name encoder that does not exist.
static const uint8_t query[] = {
    0xC0, 0xDE,             // id
    0x01, 0x00,             // recursion desired
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    7,'e','x','a','m','p','l','e', 3,'c','o','m', 0,
    0x00, 0x01, 0x00, 0x01,
};
```

Bind a socket to an ephemeral port through the *internal* UDP hook (not
a user socket — this runs at boot), send to 10.0.2.3:53, and wait up to
3 s. Assert: a response arrived; its id is `0xC0DE`; its QR bit is set;
its RCODE is 0 or 3 (`NXDOMAIN` is a perfectly good round trip and the
test must not depend on what the host's resolver believes about
`example.com`).

- [ ] **Step 2:** Run; watch it fail before the send is implemented.
- [ ] **Step 3:** Implement; `make test`; expect `[dns] ALL PASSED`.
- [ ] **Step 4: Prove the detector.** Send to 10.0.2.9 instead of
      10.0.2.3; confirm the timeout fires; revert.
- [ ] **Step 5:** Add `"[dns] ALL PASSED"` to `REQUIRED_MARKERS`. Commit.

```bash
git add kernel/net/dnstest.c Makefile
git commit -m "D4: a UDP datagram leaves the machine and an answer comes back"
```

---

## Definition of done

- `[dhcp] ALL PASSED` and `[dns] ALL PASSED` in the log and in
  `REQUIRED_MARKERS`.
- The boot log names the leased address, and says plainly whether it
  came from a server or from the fallback.
- `docs/stdlib.md` records the kernel-DHCP divergence.
- The gauntlet is green.
