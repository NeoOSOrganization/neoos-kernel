# D2–D5: Ethernet, ARP, ICMP, DHCP and TCP — design

**Status:** design spec. Supersedes the D2–D5 outline in
`2026-09-03-pci-and-network-design.md`, which was written to the depth
needed to judge D0's shape and not to the depth needed to build.

**Goal:** a program on NeoOS opens a TCP connection to a host on the
other side of the NIC, and a program on the host opens one to NeoOS.
Everything between here and there — framing, address resolution,
routing, address configuration, and the state machine — is this spec.

**Scope decision (explicit, because it was asked and answered):** all
four milestones in one track, with full-featured TCP (sliding window,
Nagle, delayed ACK, Reno congestion control) and a DHCP client rather
than a hardcoded address. The D1 spec's advice that D5 should not share
a sitting with anything else stands as recorded advice; it was
overridden deliberately. Each of D2–D5 is separately verifiable and
separately committed, so the track degrades into "finished through
D<n>" rather than into rubble.

---

## 1. Where D1 left the seam, and why it must move

D1 gave the kernel a working virtio-net device and, importantly, a
context in which a received frame can be handled without being in an
interrupt: `netrx` copies frames in the ISR and a kernel thread drains
them. That part is right and does not change.

What is wrong is the interface above it. `struct netdev.transmit` is
documented to take *one complete IPv4 packet*, which is honest for
loopback and impossible for Ethernet, so `virtio_net` answers it with
`-ENETUNREACH` and exposes `transmit_frame` beside it as the only real
way onto the wire. D1 said so at the time: "D2 builds the layer between
them." This is that layer.

After D2:

- `transmit` means the same thing for every device: *here is an IPv4
  packet, get it to the other end.* An Ethernet device's implementation
  resolves the next hop, prepends a header, and calls its own
  `transmit_frame`.
- `transmit_frame` remains on the driver, but only the link layer calls
  it. Nothing above `eth.c` knows what a frame is.
- `netrx`'s handler is `eth_input`, not a counter.

That is the whole structural change. Everything else in D2–D5 is new
code in new files under an interface that already exists.

## 2. Files

```
kernel/net/eth.c/.h      framing, ethertype demux                 (D2)
kernel/net/arp.c/.h      the cache, the request/reply machine     (D2)
kernel/net/route.c/.h    the route table, source address selection(D2)
kernel/net/icmp.c/.h     echo reply, echo request, unreachable    (D3)
kernel/net/dhcp.c/.h     the lease state machine                  (D4)
kernel/net/tcp.c/.h      the TCB, the state machine, the timers   (D5)
kernel/net/tcp_sock.c    the socket-layer half of TCP             (D5)
```

`net.c` keeps IPv4 and UDP and loses its private routing; `socket.c`
keeps the datagram paths and gains the stream ones by dispatch rather
than by growing. `tcp.c` and `tcp_sock.c` are split because the state
machine and the syscall surface fail differently and are read at
different times: one is "what does the wire mean", the other is "what
does `accept` return".

## 3. D2 — Ethernet and ARP

### 3.1 Framing

```c
struct eth_header {
    uint8_t  dst[6];
    uint8_t  src[6];
    uint16_t ethertype_n;
} __attribute__((packed));   // 14 bytes, static_asserted
```

`eth_input` is `netrx`'s handler. It runs on the netrx thread, where
sleeping is allowed. It drops frames shorter than 14 bytes, dispatches
`0x0800` to `net_ipv4_input` and `0x0806` to `arp_input`, and counts
everything else as `rx_unknown` rather than logging it, because a real
segment carries broadcast traffic NeoOS has no opinion about and a log
line per frame is a denial of service against the serial port.

Frames shorter than 60 bytes are padded on transmit. The device does
not do it, and a 42-byte ARP request that arrives as 42 bytes on a real
switch is a bug that only appears off QEMU.

### 3.2 The ARP cache

Sixteen entries, each `{ ip_n, mac[6], state, expires_tick, pending }`.

- **VALID** for 60 seconds from the last confirmation. Linux uses
  minutes; 60 s makes expiry observable inside a boot, and this is
  recorded as a divergence.
- **PENDING** while a request is outstanding. The entry holds **one**
  queued packet — not a queue. A second packet to the same unresolved
  address replaces the first. Head-of-line dropping is what every small
  stack does here and the alternative is a per-entry allocation policy
  that buys nothing until there is a workload to measure.
- Three requests, one second apart. Then the entry is freed, the queued
  packet dropped, and the caller's next attempt starts over. A send
  into a PENDING entry returns 0 (the packet is queued), not an error;
  a send that cannot allocate an entry returns `-EHOSTUNREACH`.

**Learning.** Any ARP packet — request or reply — updates the cache
from its sender fields if an entry for that address already exists, and
creates one only from a request addressed to us. This is the standard
rule and it is what makes the reply to our own request land in the
right place.

**Answering.** A request whose target protocol address is one of our
interface addresses gets a reply. Nothing else does; NeoOS is not a
proxy ARP router.

**Locking.** `LOCK_RANK_ARP`, a spinlock, placed immediately below
`LOCK_RANK_NETRX`. It is taken from the netrx thread (input) and from
any thread that transmits (output), it never sleeps, and it is never
held across a call into the driver — the queued packet is copied out
under the lock and transmitted after it is dropped. Holding a spinlock
across `virtio_net_transmit`, which spins waiting for the device, is
the deadlock this rule exists to prevent.

### 3.3 Routing

`net_route` currently answers "loopback" for everything, which was
correct when there was one device. It becomes a table:

```c
struct route { uint32_t dst_n, mask_n, gw_n; struct netdev *dev; };
```

Four entries is the whole table:

| dst | mask | gw | dev |
|---|---|---|---|
| 127.0.0.0 | /8 | — | lo |
| our own address | /32 | — | lo |
| the NIC's subnet | from the lease | — | eth0 |
| 0.0.0.0 | /0 | the lease's gateway | eth0 |

Longest prefix wins; ties are impossible in a table this shape. The
own-address route is what makes a program's `sendto` to its own
interface address work without going out and coming back, which is what
Linux does and what a ported program assumes.

`net_route` returns the device. A second function, `route_lookup`,
returns device *and next hop*, because the link layer needs to know
whether to resolve the destination or the gateway, and that distinction
does not exist above it.

**Source address selection**: `net_ipv4_output` takes `src_n == 0` to
mean "choose", and chooses the outgoing device's address. Today every
caller passes an address explicitly and gets away with it because there
is one device.

### 3.4 D2's test — `[arp] ALL PASSED`

- resolve the gateway **through the ARP layer** (D1 proved the wire by
  hand-building a request in the driver's selftest; this proves the
  cache, the pending queue, and the completion path)
- an injected request for our own address produces a correctly formed
  reply, checked field by field
- an injected request for someone else's address produces nothing
- an entry expires: force `expires_tick` into the past, confirm the next
  send re-requests rather than using the stale MAC
- the route table answers the four cases above correctly

## 4. D3 — ICMP

Three things, and no more:

- **Echo reply.** The first externally visible result: the host pings
  NeoOS and NeoOS answers. Identifier and sequence are echoed, payload
  is copied verbatim, checksum recomputed over the whole message.
- **Echo request**, with a small demux for replies. This exists because
  the *automated* test cannot rely on a human running `ping`: the
  selftest pings 10.0.2.2, which slirp answers itself, and asserts on
  the reply. It also gives userland a `ping` later without new kernel
  work.
- **Destination unreachable / port unreachable**, generated when a UDP
  datagram arrives for a port no socket holds. Linux does this; without
  it a wrong port is silent, which is the single most common thing to
  get wrong when bringing up a network stack and the single least
  diagnosable.

Rate limiting: at most one generated unreachable per 100 ms, globally.
Not for security — NeoOS has no adversary here — but because a
misconfigured peer blasting a closed port would otherwise fill the wire
with our replies.

**No** timestamp, address mask, redirect, or router discovery. **No**
ICMP socket type; the echo path is kernel-internal and reached by
userland later through a raw socket that this spec does not add.

### 4.1 D3's test — `[icmp] ALL PASSED`

- an injected echo request produces a byte-exact reply
- a real echo request to 10.0.2.2 gets a real reply, through the real
  driver, with a plausible round-trip time
- a UDP datagram to a closed local port produces a port-unreachable
  with the offending header quoted correctly
- the rate limiter actually limits

## 5. D4 — UDP on the wire, and DHCP

### 5.1 What changes in UDP

Very little, which is the point of having built it on loopback first.
The checksum stops being optional: loopback tolerated a zero checksum
and the wire does not, so `udp_input` now verifies a non-zero checksum
and drops on mismatch (a zero checksum remains legal on receive, as in
the RFC, and is never generated on transmit).

### 5.2 DHCP, and why it is in the kernel

**Decision: the client is kernel code**, on its own thread, using an
internal UDP hook rather than a socket.

The reason is surface. A userspace client needs `AF_PACKET` or raw
sockets (to receive an offer addressed to an address we do not yet
have), `SO_BROADCAST`, and a way to install a route — three new pieces
of user-facing kernel surface, each with its own ABI obligations under
`CLAUDE.md`, all to obtain a lease that the kernel is going to install
into its own route table anyway. The kernel client needs none of them.

This is a **divergence from Linux**, where DHCP is unambiguously
userspace, and it is recorded in `docs/stdlib.md` as such. The seam is
drawn so it can be undone: `dhcp.c` talks to the same
`net_udp_output`/deliver pair a socket would, so replacing it with a
userspace client later means adding raw sockets, not unpicking the
stack.

**The state machine:** INIT → SELECTING (DISCOVER broadcast, retry at
1/2/4/8 s) → REQUESTING (REQUEST for the first offer) → BOUND
(install address, netmask, gateway, and the two eth0 routes) →
RENEWING at T1 (0.5× lease) → REBINDING at T2 (0.875×) → back to INIT
if the lease expires. NAK returns to INIT.

**The fallback that keeps the gauntlet honest:** if no lease is BOUND
within 5 seconds, the client installs **10.0.2.15/24 via 10.0.2.2**
statically, logs that it did, and keeps trying in the background. A
network stack that hangs the boot because a server did not answer is
worse than one that guesses QEMU's well-known lease, and the log line
means nobody is ever confused about which happened.

Options parsed: 1 (netmask), 3 (router), 51 (lease time), 53 (message
type), 54 (server id), 58/59 (T1/T2). Options sent: 53, 55 (parameter
request list), 61 (client id, from the MAC). Everything else ignored.
**No** DNS resolver — option 6 is parsed and stored for later, and
nothing in this track consumes it.

### 5.3 D4's tests

- `[dhcp] ALL PASSED` — a real lease from slirp's built-in server:
  address in 10.0.2.0/24, gateway 10.0.2.2, lease time non-zero, routes
  installed. Plus a unit-shaped check that a malformed offer (truncated
  options, wrong xid, wrong magic cookie) is rejected rather than
  half-parsed.
- `[dns] ALL PASSED` — a UDP query to slirp's DNS at 10.0.2.3 for a
  name, asserting on the response's transaction id and QR bit. This is
  a **real UDP round trip over the wire** obtained without writing a
  resolver: the query is 30 hardcoded bytes and the assertion is on the
  header, not the answer.

## 6. D5 — TCP

### 6.1 Shape

One `struct tcb` per connection, in a fixed array of 64, hashed by the
4-tuple. A listening socket is a TCB in LISTEN with a backlog queue of
completed connections; `accept` pops one. No dynamic allocation on the
receive path — a SYN flood on a hobby OS should exhaust a fixed table
and refuse, not exhaust the heap and panic.

**All eleven states**, transitioning as RFC 793 amended by 1122
describes them. The one deliberate departure is **MSL = 5 seconds**, so
TIME_WAIT is 10 s rather than 60 s; a 60 s TIME_WAIT cannot be observed
inside a 150 s boot that also has to run forty other suites. Recorded
as a divergence.

### 6.2 Send side

- 64 KB send ring per connection. `snd_una`, `snd_nxt`, `snd_wnd`,
  `snd_wl1/wl2` for window updates.
- **MSS** from the SYN option, defaulting to 536, capped by the
  outgoing device's MTU minus 40.
- **Nagle**: no small segment goes out while unacknowledged data is
  outstanding. `TCP_NODELAY` disables it, and is implemented rather than
  accepted-and-ignored, because a program that sets it and does not get
  it behaves mysteriously.
- **Retransmission**: one timer per connection. RTO from Jacobson/Karels
  (`SRTT`, `RTTVAR`, `RTO = SRTT + 4·RTTVAR`), clamped to [200 ms, 60 s],
  doubling on each timeout, 12 attempts before the connection fails with
  `ETIMEDOUT`. **Karn's algorithm**: a retransmitted segment never
  contributes an RTT sample.
- **Zero-window probing**: persist timer, so a peer that advertises zero
  and then loses the window update does not deadlock the connection.

### 6.3 Receive side

- 64 KB receive ring. `rcv_nxt`, `rcv_wnd`.
- **Out-of-order reassembly**: 8 segment descriptors per connection.
  A ninth out-of-order segment is dropped, which costs a retransmission
  and cannot corrupt the stream.
- **Delayed ACK**: 200 ms, or immediately on every second full-sized
  segment, or immediately when the segment was out of order (so the
  peer's fast retransmit triggers).
- **Silly window avoidance** on the advertised window: do not advertise
  an increase smaller than one MSS or half the buffer.

### 6.4 Congestion control — Reno

`cwnd`, `ssthresh`, `dupacks`.

- **Slow start** from `cwnd = 2·MSS` while `cwnd < ssthresh`.
- **Congestion avoidance**: `cwnd += MSS·MSS/cwnd` per ACK above
  `ssthresh`.
- **Fast retransmit** on the third duplicate ACK; **fast recovery**
  inflates `cwnd` by one MSS per further duplicate and deflates on the
  recovery ACK.
- **Timeout** sets `ssthresh = max(flight/2, 2·MSS)` and `cwnd = MSS`.

### 6.5 Timers

A dedicated `tcp_timer` kernel thread, woken every tick (100 Hz),
walking the active TCBs and firing retransmission, delayed-ACK, persist,
keepalive-free TIME_WAIT and connection-establishment timers. A thread
rather than a tick callback because it needs to transmit, and
transmitting takes the ARP lock and may spin on the device — neither of
which belongs in a timer interrupt.

**10 ms is coarse for an RTO**, and it is what the existing 100 Hz
timer gives. The RTO floor is 200 ms, twenty ticks, so the granularity
is not the binding constraint in practice. Recorded, not fixed.

### 6.6 ISN, checksums, and the things that get forgotten

- **ISN** from the existing random source, not a counter. A predictable
  ISN is the kind of thing that is embarrassing to have written down.
- **Checksum** over the same pseudo-header UDP already uses;
  `udp_pseudo_sum` generalises to `ip_pseudo_sum(proto)`.
- **RST** generation for segments that match no TCB, with the correct
  sequence/ack derivation for the ACK and non-ACK cases. Without it, a
  connection to a closed port hangs instead of being refused, and
  `ECONNREFUSED` never happens.
- **Simultaneous open and simultaneous close** are implemented because
  they are three lines each in a state machine that already exists, and
  are a well-known source of hangs when omitted.

### 6.7 The socket surface

New syscalls, each with a musl shim entry and a `docs/stdlib.md` line,
per `CLAUDE.md`:

```
listen(fd, backlog)
accept(fd, addr, addrlen) / accept4(fd, addr, addrlen, flags)
shutdown(fd, how)
getpeername(fd, addr, addrlen)
setsockopt / getsockopt
```

Options implemented: `SO_REUSEADDR`, `SO_ERROR`, `SO_RCVBUF`/`SO_SNDBUF`
(reported, not honoured — the buffers are fixed; the divergence is
recorded), `TCP_NODELAY`. An unknown option returns `-ENOPROTOOPT`
rather than succeeding silently.

`socket(AF_INET, SOCK_STREAM, 0)` stops returning `-EPROTONOSUPPORT`.
`connect` blocks, or returns `-EINPROGRESS` under `O_NONBLOCK` with the
result readable through `SO_ERROR` and `poll(POLLOUT)`. `close` on an
established connection sends FIN and leaves the TCB to finish closing
without the file descriptor — which means the TCB's lifetime is *not*
the socket's, and that is the single most important lifetime fact in
this milestone. `poll` integrates through the existing poll head.

### 6.8 Locking

`LOCK_RANK_TCP`, between `LOCK_RANK_SOCKET` and `LOCK_RANK_NETRX`. Taken
by the netrx thread (input), the tcp_timer thread (timeouts), and
syscall threads (send/recv). The rank checker settles the ordering
question the way it settled D1's — by failing loudly the first time it
is got wrong.

### 6.9 D5's tests

- `[tcptest] ALL PASSED` — **the deterministic core, over loopback.**
  Connect/accept, a full-duplex byte-exact transfer larger than the send
  buffer (so the window actually closes and reopens), an ordered close
  with both sides observing EOF, a connection to a closed port returning
  `ECONNREFUSED`, a `shutdown(SHUT_WR)` half-close, and a TIME_WAIT
  observed to expire.
- `[tcpwire] ALL PASSED` — an outbound connection **through slirp to a
  host-side echo server** started by the test script, proving the wire
  path, ARP resolution of the gateway, and a real RTT. Slirp forwards
  guest-initiated TCP without privileges, so this runs in CI.
- Loss and reordering are exercised by a **kernel-side fault injector**
  compiled in under a flag: drop 1 in N transmitted segments, so the
  retransmission timer, fast retransmit and reassembly queue are proven
  by observation rather than by inspection. A stack whose recovery paths
  have never run is a stack whose recovery paths do not work.

## 7. Sequencing, and what "done" degrades to

Four commits, in order, each leaving the tree green:

1. **D2** — the seam moves, ARP and routing land. `[arp] ALL PASSED`.
2. **D3** — ICMP. The host can ping NeoOS. `[icmp] ALL PASSED`.
3. **D4** — UDP on the wire and DHCP. `[dhcp]`, `[dns]`.
4. **D5** — TCP. `[tcptest]`, then `[tcpwire]`.

Every marker joins `REQUIRED_MARKERS` in the Makefile as it lands, so a
later stage cannot silently break an earlier one. Detectors are proven
the way D1 proved them: deliberately broken, watched to fire, reverted,
with the table recorded in the results section.

If the track stops early it stops at a stage boundary, with a working
committed kernel and this spec saying plainly which stages exist.

## 8. What this does not do

No IPv6. No SACK, no timestamps, no window scaling (a 64 KB buffer does
not need it), no PMTU discovery, no ECN. No `AF_PACKET`, no raw sockets,
no `AF_UNIX` over this path. No DNS resolver. No firewall, no
forwarding — NeoOS is a host, not a router. No MSI-X, no multiqueue, no
checksum or segmentation offload. No TCP fast open, no keepalives.

## 9. Risks

- **Volume.** This is four milestones. The mitigation is the staging in
  §7 and nothing else; there is no clever way to make it smaller.
- **TCP's failure mode is a hang, not a fault.** A wrong state
  transition does not panic — it waits, and the gauntlet reports a
  missing marker 150 seconds later with no clue in the log. Mitigation:
  every TCB carries a state-transition log ring dumped on the
  connection-establishment timeout, so a hang produces a trace rather
  than a silence.
- **Slirp is not the internet.** It never drops, never reorders, and
  answers in microseconds. That is why §6.9's fault injector exists; the
  wire test proves interoperability, and only the injector proves
  recovery.
- **The DHCP fallback can mask a real failure** — a broken client looks
  identical to an absent server from the route table's point of view.
  Mitigation: the fallback logs a distinct line, and `[dhcp] ALL PASSED`
  asserts a *real lease*, not merely a working address.

---

## 10. Results (2026-09-05)

Built as specified, in four commits. A program on NeoOS opens a TCP
connection to a process on the host and gets its bytes back:

```
[route] ALL PASSED
[dhcp] bound 10.0.2.15/24 via 10.0.2.2
[arp]  10.0.2.2 is at 52:55:0a:00:02:02
[icmp] gateway rtt ticks=0
[dns]  round trip, rcode=0
[tcp]  ALL PASSED
[tcptest] 96KiB through a 32KiB window passed
[tcptest] 32KiB under 1-in-8 loss, retrans=2 reasm=3
[tcpwire] 16KiB echoed by the host, byte for byte
```

### The one thing that cost more than everything else

**TCP over synchronous loopback recurses without bound.** Loopback
delivers in the sender's context, so send and receive are one call
chain: A sends, B acknowledges *inside that call*, A receives the ACK
and sends more. Each turn costs several kilobytes of segment buffers. A
256 KiB transfer nested about a hundred and seventy deep, ran off the
kernel stack, and faulted in `resolve_walk` — a VFS function with no
connection to any of it.

UDP never noticed because a UDP round trip nests twice. The socket
layer had *already recorded* the hazard ("on loopback the send path
runs straight into the receive path") and worked around it by holding
no lock; TCP is the first code that had to obey it with sequence
numbers in hand, and holding no lock was not enough.

Two mechanisms came out of it, and both are load-bearing:

- **Segments are built under the connection's lock and transmitted
  after it.** Over 127.0.0.1 both ends of a connection live in the same
  table, so a send under A's lock takes B's — and the other end doing
  the same thing at the same moment is a deadlock, not merely the rank
  complaint that surfaced it.
- **Loopback limits nesting to four and defers the rest**, which the
  outermost frame drains as it unwinds. Everything is still delivered
  before the original send returns, so the synchronous contract the
  boot selftests depend on survives; only the stack depth changes.

**The first version of the second fix was the same bug again.** The
depth counter was per-CPU, and a thread preempted mid-delivery
migrates: one CPU is left permanently deep and the other underflows
into never limiting anything. A recursion depth describes a *call
chain*, and a call chain belongs to a thread.

### Things the design did not predict

| | |
|---|---|
| **The NIC had no address.** | D1 never assigned one and D4 was to bring DHCP, but ARP cannot be tested by a host with nothing to put in the sender field — slirp does not answer a request from 0.0.0.0. `net_ifconfig()` landed in D2 with a provisional address, and D4 deleted the provisional one, because a hardcoded address in the boot path is indistinguishable from a working client. |
| **`virtio_net_transmit` had no lock.** | One bounce buffer, one queue, and a spin for the completion. D1 transmitted from exactly one place; D2 made a second caller permanent, since the netrx thread answers ARP while any other thread may be sending. |
| **`virtio_net_selftest` never restored the netrx handler.** | It installed its own and left it, so after D1's suite every frame went to a counter. Invisible until the next milestone's suite times out for no stated reason. |
| **`bind()` asked the wrong question.** | It tested `net_route()`, which means "can I reach it" — the same question as "do I own it" only while loopback is the sole interface. The moment a default route existed, `bind(8.8.8.8)` succeeded. |
| **A DISCOVER for 255.255.255.255 followed the default route** | and was unicast to the gateway. A broadcast destination now takes the broadcast MAC whatever the route says, and `net_ifconfig` always installs a `255.255.255.255/32` on-link route so a client has somewhere to send before it has an address. |
| **A RST is not always `ECONNRESET`.** | Answering our SYN it means the port is closed, which Linux reports as `ECONNREFUSED`. |

### Design points corrected

- **`LOCK_RANK_ARP` went in below `LOCK_RANK_NETRX`** on the reasoning
  that it is taken from a receive. That is the wrong axis — a rank is
  decided by what is *already held*. The netrx thread asks
  `arp_pending()` under its own queue lock, so ARP is acquired *under*
  netrx and ranks above it. The checker caught it on the first boot,
  which is exactly what D1's results section said it was for.
- **Sixteen connections at 32 KiB each way, not sixty-four at 64 KiB.**
  The table is static — nothing allocates on the receive path, so a SYN
  flood exhausts a fixed table and refuses rather than exhausting the
  heap — which makes its size a permanent charge against a 128 MiB
  machine. 9 MiB of `.bss` became 1.2 MiB, and a 32 KiB window still
  needs no window scaling, which is the property the size was chosen
  for.
- **`tcp_sock.c` was not split out.** `struct socket`, `sock_ref_of`,
  `addr_in` and `addr_out` are all file-private to `socket.c`, and
  splitting would have meant exporting four statics out of a working
  file to move four hundred lines out of it. It is a marked section
  instead.
- **The gauntlet now reads `BOOT_TIMEOUT` from the Makefile** rather
  than keeping its own. The two had already drifted: the script used 60
  while its own header claimed 150. `BOOT_TIMEOUT` itself was raised to
  240 and then put back to 150 — see §11.

### Proving the detectors

Every detector was broken deliberately, watched to fire, and reverted.

| Break | What fired |
|---|---|
| return the first route, not the longest | `/0 beat /24` |
| never send the ARP request | `gateway never resolved` |
| reply to any ARP request regardless of target | `replied on behalf of another address` |
| skip ARP's learn step | `did not learn from a request addressed to us` |
| drop the ARP length check | `parsed a truncated ARP packet` |
| build the ICMP reply instead of echoing it | `reply did not echo the payload` |
| quote the IP header but not the 8 payload bytes | `unreachable did not quote the UDP ports` |
| disable the ICMP rate limiter | `rate limiter did not limit` |
| ping an address nothing answers | `no echo reply from the gateway` |
| never send DHCPDISCOVER | `fell back to the static address` |
| ignore the DHCP transaction id | `a malformed offer was accepted` |
| bind a lease without installing routes | `default route not installed` |
| query a DNS address nothing answers | `no response from 10.0.2.3` |
| make the fault injector drop nothing | `1-in-8 loss produced no retransmissions` |

**Three of those found bugs in the TESTS, not the code**, and each had
been passing for the wrong reason:

- The route selftest added the `/24` before the `/0`, so a first-match
  table still answered `10.0.2.1` correctly by tripping over the `/24`
  first.
- The ARP truncation test used a target that was not ours, so a parser
  ignoring the length still declined to reply.
- `FIN_WAIT_1 + FIN -> CLOSING` was asserted while the injected FIN
  *also* acknowledged our FIN — which is a simultaneous close
  *completing*, and TIME_WAIT is then the correct answer. Whether the
  ACK covers our FIN is a separate axis from the flags.

The TCP state-machine selftest was wrong twice more before it was
right: `SYN_SENT` was staged with `snd_nxt == iss` when the SYN is
already out and `snd_nxt` is one past it, and the transition arm ran
over loopback, where every reply came back, matched no connection (its
ports are the connection's mirrored), and RST'd the machine's own
connection. The traces showed each state being reached and then CLOSED
microseconds later — which is what the per-TCB state ring was added
for, and it earned itself on its first use.

### Not done

- **No window scaling, SACK, timestamps, ECN, Fast Open or
  keepalives.** Absent rather than stubbed.
- **No resolver.** DHCP option 6 is parsed and stored and nothing reads
  it, so every address a program uses must be numeric. This is the most
  likely single reason a real network application will not run
  unmodified.
- **No raw sockets or `AF_PACKET`**, so ICMP exists and `ping(8)`
  cannot.
- **No IPv6, no `AF_UNIX`, no `sendmsg`/`recvmsg`.**

### One pre-existing failure, not this track's

The gauntlet's known `schedule() with a spinlock held ... socktable`
panic still appears intermittently. It is whitelisted there as a Phase
13.6 residual and predates this work. What this track added is the
means to find it: the held-lock stack now records **where** each lock
was acquired. The name alone lies, because
`spin_unlock_irqrestore` assumes strict LIFO and simply decrements —
so a caller that releases an outer lock while an inner one is still
held (`net_udp_deliver` does exactly that, deliberately) leaves the
stack labelled with the wrong lock. That is why this panic has always
pointed at socktable, and the return address is the thing that does not
lie.

---

## 11. What the gauntlet found afterwards, and what it cost to find

The suite was green and the wire test worked, and the gauntlet still
failed 14/15. Chasing that was worth more than the fix it produced,
because most of what it turned up was method rather than code.

### The first two gauntlets were measuring nothing

Two runs were live against the same `build/` tree at once — an earlier
one that had not actually died. They starved each other, and their
timeouts were read as *the suite has grown*, which is how `BOOT_TIMEOUT`
went from 150 to 240. Measured properly afterwards, from the tick log:
**a healthy boot is forty-eight seconds** and `tcptest` costs about one
of them. The timeout went back to 150, which is three times a healthy
boot — what a hang detector should be. Raising it had only made a real
hang slower to report.

The lesson generalises: `tools/gauntlet.sh` shares `build/` with `make`,
and any concurrent build kills it **silently**, leaving output that
stops after the header and looks exactly like a run still in progress.

### The bisect, and why its conclusion was weaker than it looked

| Point | Result |
|---|---|
| D1 (before this track) | 8/8, no retries |
| D4 (everything but TCP) | 8/8, no retries |
| TCP state machine | 8/8, one small retry |
| main | the long-tail hang |

That reads as "D5 introduced it", and it is not that solid: the failure
rate is roughly one run in seven, so **eight clean runs is about a 27%
outcome even if the bug predates the track**. The bisect narrowed
suspicion; it did not establish blame, and it is recorded here as the
former rather than the latter.

### What the hang actually was

`bbspike` is a `wait` entry, so when BusyBox stalled partway through a
shell pipeline (`ps | grep init`) it blocked init and every entry after
it — which is why the symptom was eighteen suites that never printed a
line. **The stalling pipeline touches no sockets at all.**

Three defects were fixed on the way, none of which had failed a test:

- **Loopback dropped datagrams it had no right to drop.** A packet too
  large for the 2 KiB defer slot was discarded, on the reasoning that
  "loopback is a network and UDP may lose a datagram". True of a
  network, false of this one: MPI sends 8 KiB datagrams over loopback
  and has no retransmission, so one loss means `mpitest` waits forever.
  Oversized packets are now delivered INLINE — safe precisely where the
  depth limit matters, since the limit exists for TCP's ping-pong and a
  TCP segment always fits in a slot.
- **Pollers were registered on the TCB.** A `poll_reg` lives on the
  poller's stack and is threaded into the object's list; the TCB
  outlives its socket and is then RECYCLED, carrying that list into the
  next connection. Pollers now use the socket's head, whose lifetime is
  the descriptor's, and stream readiness reaches them through the
  global broadcast.
- **The TCP timer thread woke 100 times a second forever**, including
  with an empty connection table — scheduler churn charged to every
  other test in the suite. It now sleeps 200 ms while there is nothing
  to time.

After those, the gauntlet is **15/15**. Two runs still needed a solo
retry for a two-marker contention artifact, so it is not yet the
zero-retry bar the previous milestone set, and that gap is real and
unclosed.

### Still open

- **`bbspike`'s shell pipeline stalls intermittently.** It uses no
  sockets, and it belongs to the same family as the pre-existing
  musl/BusyBox flake already recorded in this project. Whether this
  track made an old race easier to hit — by changing timing — or
  introduced one, is not established.
- **`tcptest`'s loss phase has segfaulted once** in userland, at a point
  where the code has no large stack frame. Seen once, not reproduced.
- **The known `socktable` panic** remains, whitelisted in the gauntlet
  as a Phase 13.6 residual. This track added the diagnostic that should
  locate it: the held-lock stack now records where each lock was
  ACQUIRED, because the name alone lies — `spin_unlock_irqrestore`
  assumes strict LIFO and simply decrements, and `net_udp_deliver`
  deliberately releases an outer lock while holding an inner one.
