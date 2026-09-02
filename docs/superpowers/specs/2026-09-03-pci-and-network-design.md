# PCI, and the road to TCP — design

**Status:** design spec. D0 (PCI) is specified in full and built first;
D1–D5 are specified to the depth needed to know D0 is the right shape,
and each gets its own plan when reached.

**Goal:** NeoOS can be reached from the outside. The end state is a TCP
connection between the host and a program running on NeoOS.

---

## 1. Why PCI first

Everything wanted next sits behind bus enumeration. The NIC (e1000 or
virtio-net), audio (AC'97, HDA) and a DMA disk controller (AHCI) are all
PCI devices, and NeoOS has **no PCI support at all** today: no config
space access, no enumeration, no BAR decoding, no interrupt routing.

So D0 is not a detour on the way to networking. It is the shared
foundation, and it pays off three times:

- **the NIC**, which is what this track is for;
- **audio**, whenever that milestone comes;
- **AHCI** — and that one is not speculative. The gauntlet's recurring
  `[ata] read FAILED: BSY never cleared` is the emulated **PIO** path
  timing out under load. A DMA controller is the actual fix, and it
  needs PCI.

## 2. What exists to build on

- `struct netdev` with a `transmit` hook already exists
  (`kernel/net/net.c`), currently with one implementation: loopback.
  A real driver slots in beside it rather than replacing anything.
- IPv4 and UDP are implemented, 344 lines, and are exercised by
  `nettest` over loopback.
- The whole receive path runs **in the sender's context** today, because
  loopback delivers synchronously. A real NIC receives in an
  **interrupt**, which is the single biggest change D1 forces and the
  reason it is its own milestone.
- Interrupts, IRQ routing through the IOAPIC, and per-CPU state are all
  in place.

## 3. Decisions

- **Legacy config access (0xCF8/0xCFC), not ECAM.** ECAM needs the MCFG
  ACPI table, and NeoOS does not parse ACPI tables yet. Port I/O works on
  every PC, needs nothing new, and covers every device this track wants.
  ECAM becomes worthwhile with PCIe-only hardware, which QEMU's `pc`
  machine is not.
- **Enumerate by brute force**, all 256 buses × 32 devices × 8
  functions, once at boot. 65,536 config reads is a few milliseconds and
  needs no bridge-walking logic. Recursive enumeration is what you do
  when hotplug matters; nothing here hotplugs.
- **No PCI driver model, no bus/driver binding framework.** A registry
  of discovered devices and a `pci_find(class, subclass)` /
  `pci_find_id(vendor, device)` lookup is all D1 needs. A binding
  framework with probe callbacks is infrastructure for drivers that do
  not exist yet, and NeoOS already has `struct con_driver` and
  `struct netdev` as the places drivers actually plug in.
- **Interrupts: the legacy INTx line from config space**, not MSI. The
  IOAPIC path already works and e1000 supports INTx. MSI removes a
  shared-interrupt problem NeoOS does not yet have.

---

## D0 — PCI enumeration

### The mechanism

Config space is reached through two I/O ports:

```
0xCF8  CONFIG_ADDRESS   31: enable | 23:16 bus | 15:11 dev | 10:8 fn | 7:2 reg
0xCFC  CONFIG_DATA      the 32-bit word at that address
```

A function exists if its vendor ID is not `0xFFFF`. Multi-function
devices are indicated by bit 7 of the header type; a device whose
function 0 lacks that bit has no other functions, which is worth
honouring only because probing all eight otherwise wastes nothing but
time.

### What is recorded

Per discovered function: bus/device/function, vendor and device id,
class/subclass/prog-if, header type, interrupt line and pin, and the six
BARs — decoded into (address, size, is_io) by the standard
write-all-ones-and-read-back dance, restoring the original value after.

Sizing a BAR **writes to it**, so it happens once at enumeration with
interrupts already configured but before any driver claims the device.
Getting that order wrong is how a BAR ends up pointing at nothing.

### The interface

```c
struct pci_device {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if, header_type;
    uint8_t  irq_line, irq_pin;
    struct pci_bar { uint64_t addr; uint64_t size; int is_io; } bar[6];
};

void pci_init(void);                       // enumerate once, at boot
int  pci_count(void);
const struct pci_device *pci_get(int i);
const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device);
const struct pci_device *pci_find_class(uint8_t cls, uint8_t sub);

uint32_t pci_config_read32(const struct pci_device *d, uint8_t reg);
void     pci_config_write32(const struct pci_device *d, uint8_t reg, uint32_t v);
void     pci_enable_bus_master(const struct pci_device *d);
```

`pci_enable_bus_master` is separate and explicit because a device that
does DMA cannot do it until that bit is set, and forgetting it produces
a driver that looks correct and moves no data — a failure mode worth
naming in the interface rather than burying in a driver.

### Locking

None. Enumeration runs once on the BSP before APs start, and the table
is read-only afterwards. Config-space *writes* from a driver are the
driver's business; if two drivers ever touch the same device's config
space concurrently, that is the point at which this needs a lock and not
before.

### Test

`pci_selftest`, run at boot like the others:

- **at least one device is found.** A machine with zero PCI functions
  means enumeration is broken, not that the machine is empty.
- **the host bridge is present** — class 0x06, subclass 0x00. Every PC
  has one at 00:00.0, so its absence is a specific, diagnosable failure
  rather than a vague one.
- **every reported BAR is sane**: a non-zero size is a power of two, and
  an I/O BAR fits in 16 bits.
- **config space round-trips**: read a device's command register, write
  it back unchanged, read again, compare. This proves the address/data
  port pair is actually wired, which a read-only test cannot.
- **the known QEMU devices are found** by id — the `pc` machine always
  has an Intel 82441FX host bridge and a PIIX3 ISA bridge. Asserting on
  ids that are guaranteed by the emulator makes the test specific;
  asserting on the NIC does not, since `-net` is optional.

The serial log gets one line per device (`[pci] 00:01.1 8086:7010 class
01:01`), which is what makes the next milestone's "is the NIC even
there?" a two-second question.

---

## D1–D5 — the road to TCP, in outline

Each becomes its own plan. Listed here so D0's shape can be judged
against where it leads.

**D1 — the NIC.** `virtio-net` over `e1000`: its rings are simpler, it
needs no PHY handling, and QEMU implements it well. The cost is that it
is a paravirtual device, so it teaches less about real hardware and
cannot be pointed at a physical card later. **Open question for D1, not
D0** — either fits behind `struct netdev`.

The hard part is not the register programming; it is that packets now
arrive in an **interrupt**, where today's receive path runs in the
sender's context and may sleep. D1 must introduce a receive queue and a
kernel thread to drain it, or make the input path interrupt-safe.

**D2 — Ethernet and ARP.** Framing, a small ARP cache with timeouts, and
the send path learning to resolve a next-hop MAC before it can transmit.

**D3 — ICMP.** Echo reply, so the host can `ping` NeoOS. The first
externally visible result, and the natural end of a first sitting.

**D4 — UDP over the wire.** Mostly done: `net_route` learns a second
interface and a real subnet, and checksums stop being optional as they
are on loopback.

**D5 — TCP.** The state machine, retransmission, windowing, and the
socket API surface (`listen`, `accept`, `connect`, streams). This is
the largest single milestone in the track and will not be attempted in
the same sitting as anything else.

## What this does not do

- **No ACPI parsing**, hence no ECAM and no interrupt routing tables.
  The IOAPIC path already in use is enough for INTx.
- **No MSI/MSI-X.**
- **No PCI bridges walked recursively**, no hotplug, no power
  management, no capability list parsing beyond what a driver needs.
- **No IPv6, no DNS, no DHCP.** A static address is configured; DHCP
  needs raw sockets and BPF, which is a separate argument.

## Risks

- **QEMU-shaped.** Everything here is verified against QEMU's `pc`
  machine. Real hardware differs in ways this spec cannot anticipate,
  and the selftest's device-id assertions are explicitly QEMU's.
- **The receive-in-interrupt change (D1) is the real difficulty** of
  this track, not PCI. D0 is mechanical; D1 changes an invariant the
  existing stack was written around.
- **BAR sizing writes to config space.** Done wrong or at the wrong
  moment it can leave a device unreachable. It happens once, before any
  driver touches the device, and the original value is restored.
