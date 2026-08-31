# x2APIC — design

**Date:** 2026-08-31
**Status:** design (solo brainstorm — see roadmap). Isolated: touches
`kernel/dev/lapic.c` and the SMP bring-up only.
**Context:** `kernel/dev/lapic.c` (MMIO LAPIC), `kernel/smp/smp.c`
(dense-index ↔ apic-id map, already `uint32_t`), `kernel/dev/acpi.c`
(MADT walk).

## Problem

The LAPIC is driven through the legacy xAPIC MMIO window
(`lapic_base[reg/4]` at `0xFEE00000`). That works but:

- The ICR destination field is 8 bits, capping the machine at 255
  addressable APIC IDs; the SIPI/INIT and IPI paths all write
  `dest << 24` into `ICR_HIGH`.
- Every IPI does a read-poll loop on `ICR_DELIVERY_PENDING`
  (`lapic_wait_idle`) — an extra MMIO round trip per IPI.
- MMIO LAPIC access is serializing and slower than the MSR path.

x2APIC replaces the MMIO window with a bank of MSRs, widens APIC IDs to
32 bits, and makes the ICR a single atomic 64-bit MSR write with no
delivery-pending poll.

## Goals

- Detect x2APIC (`CPUID.01H:ECX[21]`). If present **and** not disabled
  by firmware/hypervisor, enable it on every CPU (BSP + APs) and route
  all LAPIC access through the MSR bank.
- Keep a working **xAPIC fallback** for a machine (or QEMU CPU model)
  without x2APIC — the code compiles and runs both ways, chosen at
  boot.
- IPI / INIT / SIPI / EOI / timer LVT / spurious-vector setup all work
  identically from the callers' point of view — `lapic.h` API is
  unchanged.
- APIC IDs are treated as 32-bit end to end (they already are in
  `smp.c`; audit for stray `uint8_t` / `<< 24`).

## Non-goals

- MADT type-9 (Local x2APIC) parsing / support for >255 CPUs. NeoOS
  runs 4 CPUs; type-0 entries are enough. The 32-bit-ID plumbing is
  done so that adding type-9 later is small, but it is not done here.
- Cluster / logical destination mode. NeoOS uses physical destination
  for all directed IPIs and does not need broadcast-except-self beyond
  what the shorthand fields already give.
- Interrupt remapping (VT-d / AMD-Vi). Separate, large, and unrelated.
- x2APIC for the IOAPIC RTEs — the IOAPIC is a separate chip and its
  programming does not change (destination stays within 8 bits for
  physical mode with ≤255 CPUs).

## Design

### 1. Mode selection (`lapic.c`)

```c
static int x2apic_mode;   // set once, before any lapic_read/write
```

`lapic_init(address)`:
1. `CPUID.01:ECX[21]` → x2apic capable?
2. Read `IA32_APIC_BASE` (MSR `0x1B`). If capable: set bit 11 (global
   enable, usually already set) and bit 10 (EXTD / x2APIC enable),
   write it back, set `x2apic_mode = 1`.
   - Once EXTD is set it cannot be cleared without a reset; that is
     fine, NeoOS never wants to.
3. If not capable: `x2apic_mode = 0`, map the MMIO window as today.
4. Program SVR (software-enable + spurious vector `0xFF`) through the
   mode-appropriate accessor.

APs call the same `lapic_init` path in their bring-up (they must set
EXTD in their own `IA32_APIC_BASE` — it is per-CPU).

### 2. Register accessors

```c
#define X2APIC_MSR_BASE 0x800        // reg 0x020 (ID) -> MSR 0x802, i.e. 0x800 + reg/16

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic_mode) return (uint32_t)rdmsr(X2APIC_MSR_BASE + (reg >> 4));
    return lapic_base[reg / 4];
}
static void lapic_write(uint32_t reg, uint32_t value) {
    if (x2apic_mode) { wrmsr(X2APIC_MSR_BASE + (reg >> 4), value); return; }
    lapic_base[reg / 4] = value;
}
```

Registers that do not exist in x2APIC (e.g. the old `ICR_HIGH` at
`0x310`, DFR at `0x0E0`) are simply never used in x2APIC paths — see
the ICR handling below. EOI in x2APIC is a write of 0 to MSR `0x80B`,
which the formula above yields from `reg 0x0B0`. `wrmsr`/`rdmsr`
helpers already exist in `kernel/arch/msr.h`.

### 3. ICR / IPI path

```c
static void lapic_send_icr(uint32_t apic_id, uint32_t low) {
    if (x2apic_mode) {
        // Single 64-bit write: dest in the high 32 bits, no self-clear
        // status bit, no delivery-pending poll.
        wrmsr(0x830, ((uint64_t)apic_id << 32) | low);
        return;
    }
    lapic_write(LAPIC_REG_ICR_HIGH, apic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
    lapic_wait_idle();
}
```

`lapic_wait_idle` becomes a no-op / `#if` guard in x2APIC mode (there
is no delivery-status bit to poll — the `wrmsr` is architecturally
complete when it retires). `lapic_send_init` / `_sipi` / `_ipi` /
`_nmi` are unchanged — they all funnel through `lapic_send_icr`.

The INIT-deassert IPI (`ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL` with no
assert bit) is legal in x2APIC too; QEMU and real hardware accept it.

### 4. ID plumbing audit

- `smp.c` `lapic_ids[]` is already `uint32_t`; `smp_lapic_for_index`
  returns `uint32_t`. Good.
- `acpi.c` MADT type-0 `apic_id` is `uint8_t` — that is the on-disk
  format and stays; it is widened to `uint32_t` when stored in
  `info->cpus[i].lapic_id` (check the struct field width).
- `lapic_id()` (self-id): `lapic_read(LAPIC_REG_ID)` — in xAPIC the ID
  is in bits 31:24, in x2APIC it is the full 32-bit value. The
  accessor must shift:
  ```c
  uint32_t lapic_id(void) {
      uint32_t v = lapic_read(LAPIC_REG_ID);
      return x2apic_mode ? v : (v >> 24);
  }
  ```
- Grep the tree for `<< 24` and `>> 24` near APIC code and for
  `(uint8_t)` casts of an apic id.

### 5. QEMU

`-cpu Nehalem` does not expose x2APIC by default; add `+x2apic`:
`QEMU_COMMON := -cpu Nehalem,+x2apic ...` in the `Makefile`. This is a
shared prerequisite with any future RDRAND work (which would want a
newer model or `+rdrand`); note it in the roadmap. With `+x2apic` the
code takes the x2APIC path; to exercise the fallback, a
`make test X2APIC=0` that drops the flag and (temporarily) a boot
option to force `x2apic_mode = 0`.

## Testing

- Boot log already prints `[smp] steal selftest passed` etc., which
  exercise IPIs (reschedule IPI, TLB shootdown IPI) across all 4 CPUs.
  If x2APIC IPI delivery is wrong, AP bring-up hangs (no
  `[smp] cpu N online`) or the shootdown selftest fails — both caught
  by the gauntlet.
- Add `[lapic] x2apic mode` / `[lapic] xapic (mmio) mode` to the boot
  log so the run's mode is visible; assert one of them is present
  (`REQUIRED_MARKERS` accepts either via a small `||` — or just log
  `[lapic] mode selected`).
- A selftest that sends a self-IPI on a spare vector and confirms the
  handler ran, in both modes if a fallback build is run.
- Gauntlet ×3 (IPI paths are the concurrency-sensitive part).

## ABI / stdlib impact

None — LAPIC access is entirely kernel-internal. `docs/optimization-
summary.md` gets a note; `docs/abi-compatibility.md` is untouched.

## Risks

1. **AP bring-up regression.** The SIPI path now goes through the
   64-bit ICR MSR. If the encoding is off, APs never start and the
   boot hangs before the scheduler. Mitigation: land the accessor +
   mode-detect first with IPIs still on the MMIO path (`x2apic_mode`
   forced 0), verify green, then flip the ICR path.
2. **EXTD must be set per-CPU.** Forgetting it on an AP means that AP
   does MMIO LAPIC access while the BSP does MSR — inconsistent EOIs,
   lost interrupts. The AP init path must call the same enable
   sequence.
3. **QEMU vs real hardware:** QEMU is lenient about the INIT-deassert
   and about reserved ICR bits; keep the encoding strict to the SDM so
   it also works on metal.
4. **`lapic_wait_idle` callers** outside `lapic_send_icr` — grep to be
   sure there are none that would spin forever polling a bit that is
   always 0 in x2APIC.

## Plan sketch (for `writing-plans`)

1. `Makefile`: `-cpu Nehalem,+x2apic`. `rdmsr`/`wrmsr` audit in
   `msr.h`. Gauntlet (no code change — just the flag; verify still
   green on the new CPU model).
2. `lapic.c`: mode detect + EXTD enable + MSR accessors, IPIs still
   forced to MMIO (`x2apic_mode` used only by `lapic_read/write` and
   `lapic_id`). Boot-log the mode. Gauntlet.
3. Flip the ICR path to the 64-bit MSR write in x2APIC mode; drop the
   idle poll there. Self-IPI selftest. Gauntlet ×3.
4. ID-plumbing audit + fallback build (`X2APIC=0`) verified once.
5. Docs.
