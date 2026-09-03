# D1 — virtio-net, and packets that arrive in an interrupt

**Spec.** Implements D1 of
`docs/superpowers/specs/2026-09-03-pci-and-network-design.md`, which
left the choice of NIC open. It is virtio-net.

**Goal:** a real packet leaves NeoOS and a real reply comes back, through
a device that is not loopback.

---

## 1. Why virtio-net, given e1000 was already on the bus

e1000 is the better *network* answer: it is attached already, it uses
descriptor rings like every modern NIC, and it runs on real hardware.
virtio is the better *whole-system* answer, and that is the trade being
taken deliberately.

Almost all the work in virtio-net is the **virtio transport** — feature
negotiation, virtqueue setup, notification — and none of that is
specific to networking. Once it exists, **virtio-blk is nearly free**,
and virtio-blk is a faster and better-understood fix for the
`[ata] read FAILED: BSY never cleared` timeouts than writing AHCI would
be. One transport layer, two of this kernel's problems.

The cost is honest and worth stating: virtio is paravirtual. This driver
will never run on metal, and it teaches less about how hardware actually
behaves than e1000 would.

## 2. Legacy virtio, not modern

QEMU's `virtio-net-pci` is *transitional* by default: it speaks both the
legacy (0.9.5) and modern (1.0) interfaces.

- **Legacy** puts a small register block in **BAR0, in I/O space**.
  Device configuration is `inl`/`outl` at fixed offsets. No PCI
  capability list, no MMIO mapping, no page-table work at all.
- **Modern** requires walking the PCI capability list to locate four
  separate structures (common config, notify, ISR, device config), each
  in some BAR at some offset, and mapping them.

D0 deliberately did not parse capability lists, and legacy does not need
it. This is roughly 80 lines of difference in the transport for
identical behaviour on QEMU. Legacy it is, with the caveat recorded:
a PCIe-only virtio device (`disable-legacy=on`) would not work, and
moving to modern is a transport-layer change that no driver above it
would notice.

## 3. The architecture

```
  kernel/drivers/virtio/virtio.{c,h}     transport: handshake, virtqueues
  kernel/drivers/net/virtio_net.{c,h}    the NIC: RX/TX, MAC, the IRQ
  kernel/net/netrx.{c,h}                 the RX queue and its kernel thread
```

The split matters. `virtio.c` knows nothing about packets; `virtio_net.c`
knows nothing about how a virtqueue is laid out. virtio-blk, when it
comes, adds one file beside `virtio_net.c` and changes nothing else.

### 3.1 The transport

Device initialisation is a fixed handshake, and the order is not
advisory — a device that sees DRIVER_OK before its queues are configured
is entitled to do anything:

1. reset (write 0 to the status register)
2. status |= ACKNOWLEDGE — "I see you"
3. status |= DRIVER — "I know how to drive you"
4. read device features, write back the subset we accept
5. configure each virtqueue
6. status |= DRIVER_OK — the device may now use the queues

**Features accepted: `VIRTIO_NET_F_MAC` only.** Everything else is
refused. Checksum offload, TSO, and the mergeable-receive-buffer layout
all change the shape of the packets the driver sees, and each is a
separate thing to get wrong. A driver that accepts nothing gets plain
whole frames, which is what a first driver wants. `F_MAC` is accepted
because the alternative is inventing a MAC address.

### 3.2 Virtqueues

A split virtqueue is three arrays in physically contiguous memory:

| Part | What it is |
|---|---|
| descriptor table | 16 bytes each: physical address, length, flags, next |
| available ring | driver → device: "these descriptor chains are yours" |
| used ring | device → driver: "these are done, and this much was written" |

`pmm_alloc(order)` already returns 2^order **physically contiguous**
frames, and `phys_to_virt` reaches them, so no new allocator is needed.
The legacy layout requires the used ring to start on a page boundary
after the available ring, which is the one piece of arithmetic here that
is easy to get subtly wrong and produces a device that silently
completes nothing.

Queue size comes from the device (256 in QEMU). Two queues: RX (0) and
TX (1). The control queue is not used, because no feature that needs it
is accepted.

### 3.3 Receive, and the invariant this milestone breaks

**This is the actual difficulty of D1, not the register programming.**

Today every packet arrives via loopback, synchronously, in the context
of whoever sent it. `net_ipv4_input` may therefore take locks and sleep,
and the socket layer below it does exactly that.

A NIC delivers in an **interrupt**, where sleeping is a panic and taking
a sleepable lock is a lock-order inversion waiting to be found by the
gauntlet at three in the morning. So the interrupt handler does the
least possible work:

1. read the ISR status register (this also acknowledges the interrupt)
2. walk the used ring, copying each completed frame into an RX queue
3. hand the descriptor back to the device
4. wake the RX thread

and a **kernel thread (`netrx`) drains that queue** and calls the
existing, sleepable, receive path. The queue is a fixed ring of frame
buffers with a rank-checked spinlock taken with interrupts disabled;
overflow drops the oldest and counts it, because a network stack that
blocks its own interrupt handler is worse than one that drops.

The alternative — making the whole input path interrupt-safe — was
rejected: it would push `spin_lock_irqsave` down through the socket
layer and into anything a future protocol wants to do on receive, and
receive is precisely where TCP will later want to do a great deal.

### 3.4 Transmit

Synchronous and simple: fill a descriptor, put it in the available ring,
notify the device, and — for now — wait for it to appear in the used
ring before returning. Not because that is fast, but because it makes
buffer ownership obvious in the first version of a driver where getting
ownership wrong means the device DMAs into memory the kernel has reused.
Asynchronous completion is a later change with a real test behind it.

Every virtio-net buffer carries a `virtio_net_hdr` in front of the
frame. Its size depends on what was negotiated, which is a trap worth
naming: **10 bytes on legacy without `MRG_RXBUF`**, which is what this
driver gets, but **12 bytes under modern virtio or with mergeable
receive buffers**, where a `num_buffers` field is appended. With no
offload features accepted it is all zeroes on transmit and ignored on
receive, but it is *always* there, and getting its size wrong shifts
every frame by two bytes — which looks like a broken device rather than
a miscounted header.

## 4. What `struct netdev` becomes

Today it carries **IP packets, not Ethernet frames**, and its own header
says that is "the seam a real driver will have to change."

D1 changes it minimally:

- add `uint8_t hwaddr[6]` and a `type` (loopback or ethernet)
- add `transmit_frame`, which takes a complete Ethernet frame
- leave `transmit` alone for loopback

Full framing, ARP, and a route that resolves a next-hop MAC are **D2**.
D1 stops at the point where a frame can be sent and a frame can be
received, which is enough to prove every part of the driver.

## 5. Proving it works

The temptation is to defer the test to D2, when ARP exists. That would
leave the whole driver unproven for a milestone. Instead:

`virtio_net_selftest` hand-builds **one ARP request** for the gateway
(10.0.2.2 under QEMU user networking) — 42 bytes of known constants, not
an ARP layer — sends it, and waits for the reply. Passing requires:

- the PCI device was found and the transport handshake completed
- the MAC was read out of device configuration
- the TX ring, the notification, and the used-ring completion all work
- the device raised an interrupt
- the RX ring delivered a frame, and the RX thread got it out of the
  queue and into the stack

That is every part of D1 in one round trip, against a peer NeoOS does
not control. A driver that passes this is working; a driver that fails
it tells you which of the six steps stopped, because each logs.

Failure to get a reply is a **FAILED**, not a skip: QEMU's user
networking always answers ARP for the gateway.

Detectors will be proven the usual way — break each, watch it fire,
revert. Specifically: refuse `F_MAC` and check the MAC check fails; skip
DRIVER_OK and check nothing completes; drop the header and check
the reply is not recognised.

## 6. QEMU, and reaching the host

Two ways in, and the default must need no privileges:

- **`-netdev user`** (SLIRP) becomes the default for `make test` and the
  gauntlet. It answers ARP, routes outbound TCP, and needs no root. It
  cannot be pinged *from* the host.
- **`make shell-tap`** adds a tap device for manual testing, where the
  host can ping NeoOS and act as a real TCP peer. It needs one-time host
  setup with root, so it is never on the automated path.

The e1000 that QEMU attaches by default is replaced by
`-device virtio-net-pci`, so `pci_selftest`'s device-id assertions —
which deliberately assert only on the host bridge and PIIX3 — stay true.

## 7. Risks

- **Ownership of DMA buffers.** The device writes into physical memory
  the kernel handed it. A descriptor returned to the device while the
  kernel still reads it, or a buffer freed while queued, is silent
  corruption somewhere else entirely. Synchronous transmit and a fixed,
  never-freed pool of RX buffers are chosen to make this boring.
- **The used-ring page alignment** in the legacy layout. Wrong, and the
  device completes nothing, with no error anywhere.
- **The RX thread is a new place to deadlock.** It runs the receive path
  with a lock rank that must sit below everything the socket layer takes.
- **QEMU-specific gateway address.** The selftest hardcodes 10.0.2.2,
  which is SLIRP's. Under tap it would be whatever the host is, so the
  selftest runs only on the user-networking path and says so.

## 8. Not in D1

No ARP layer, no Ethernet framing in the stack, no IP over the NIC, no
multiple queues, no offload of any kind, no virtio-blk, no modern virtio,
and no MSI-X. Each is either a later milestone or a deliberate refusal
recorded above.

---

## 9. Results (2026-09-03)

Implemented as designed. `[virtio-net] ALL PASSED` is a required marker
in the Makefile and in the pgauntlet, which boots with
`-netdev user -device virtio-net-pci`.

```
[virtio-net] mac=52:54:00:12:34:56
[virtio-net] ready
[ioapic] virtio-net routed: gsi=0x0b vector=0x22
[virtio-net] gateway 10.0.2.2 is at 52:55:0a:00:02:02
[virtio-net] ALL PASSED
```

Detectors were proven the usual way -- broken, watched to fire,
reverted:

| Break | What fired |
|---|---|
| refuse `F_MAC` | `FAILED: device does not offer F_MAC` |
| skip `DRIVER_OK` | `transmit timed out waiting for completion` |
| 12-byte header | `FAILED: no ARP reply from the gateway` |

### Two things the design did not predict

**The ISR register is at 0x13, not 0x14.** 0x14 is where device
configuration begins, and the two are adjacent. Reading the ISR from
0x14 returned the first byte of the MAC (0x52), whose bit 0 is clear, so
every interrupt looked spurious and was dismissed. The ISR read IS the
acknowledgement, so the level-triggered line stayed asserted: 110,000
interrupts taken and not one frame delivered, with the frame visible in
the used ring the whole time. The interrupt count is now a statistic
(`virtio_net_stats`) precisely because that failure is otherwise silent.

**kmain is not a thread, so the selftest cannot yield.** The BSP's
`c->current` is still 0 during boot -- which is why `timer_handler`
refuses to preempt it -- so calling `schedule()` from a selftest
switches away from a bootstrap stack there is nothing to save into, and
the machine runs on having quietly abandoned its own boot. The wait is
`hlt` only, and it is ANOTHER CPU that runs the netrx thread and
delivers the frame; `netrx_start()` therefore runs before the APs come
up. On a single-CPU machine the frame arrives and nothing drains it,
which the selftest reports as SKIPPED rather than FAILED, because it is
a missing CPU and not a broken driver.

### One design point corrected

`LOCK_RANK_NETRX` was first placed up with the leaf ranks, on the
reasoning that it is taken from an interrupt. That is the wrong axis:
the netrx thread hands the lock to `waitq_sleep()` as `release`, so it
must rank strictly BELOW `LOCK_RANK_WAITQ`, exactly like the IPC guards.
It now sits at 13, and the band above it shifted up by one. The rank
checker caught this on the first sleep, which is what it is for.
