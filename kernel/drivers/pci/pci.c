// pci.c -- configuration space access and bus enumeration.
//
// D0 of the network track. Nothing here is interesting on its own; it
// exists because the NIC, an AHCI controller and any audio device are
// all PCI functions, and NeoOS had no way to find out that any of them
// were present.
//
// Enumeration is brute force: all 256 buses x 32 devices x 8 functions.
// That is 65536 config reads, a few milliseconds once at boot, and it
// needs no bridge-walking logic. Recursive enumeration is what you do
// when devices can appear later; nothing here hotplugs.

#include "pci.h"
#include "../../arch/io.h"
#include "../char/serial.h"

static struct pci_device devices[PCI_MAX_DEVICES];
static int device_count;
static int devices_dropped;   // found more than the table holds

// A config address is (enable | bus | device | function | register),
// with the low two bits of the register always zero: the data port is
// dword-wide and a misaligned register number selects the wrong dword.
static uint32_t config_address(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    return (uint32_t)0x80000000u
         | ((uint32_t)bus << 16)
         | ((uint32_t)(dev & 0x1F) << 11)
         | ((uint32_t)(fn & 0x07) << 8)
         | ((uint32_t)reg & 0xFC);
}

static uint32_t raw_read32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, reg));
    return inl(PCI_CONFIG_DATA);
}

static void raw_write32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t reg, uint32_t v) {
    outl(PCI_CONFIG_ADDRESS, config_address(bus, dev, fn, reg));
    outl(PCI_CONFIG_DATA, v);
}

uint32_t pci_config_read32(const struct pci_device *d, uint8_t reg) {
    return raw_read32(d->bus, d->dev, d->fn, reg);
}

void pci_config_write32(const struct pci_device *d, uint8_t reg, uint32_t value) {
    raw_write32(d->bus, d->dev, d->fn, reg, value);
}

void pci_enable_bus_master(const struct pci_device *d) {
    uint32_t cmd = pci_config_read32(d, PCI_REG_COMMAND);
    pci_config_write32(d, PCI_REG_COMMAND, cmd | PCI_CMD_BUS_MASTER);
}

// Sizing a BAR means writing all ones to it and reading back which bits
// the device let stick: the lowest set bit is the size. That WRITES to
// the device, so the original value is put back immediately -- a BAR
// left holding 0xFFFFFFFF is a device that has moved somewhere it does
// not decode.
//
// Returns the number of BAR slots consumed: a 64-bit memory BAR eats
// the next slot as its high half.
static int decode_bar(struct pci_device *d, int index) {
    uint8_t reg = (uint8_t)(PCI_REG_BAR0 + index * 4);
    uint32_t original = pci_config_read32(d, reg);
    if (original == 0) {
        return 1;                            // unimplemented
    }

    pci_config_write32(d, reg, 0xFFFFFFFFu);
    uint32_t probe = pci_config_read32(d, reg);
    pci_config_write32(d, reg, original);

    struct pci_bar *bar = &d->bar[index];

    if (original & 1) {                      // I/O space BAR
        uint32_t mask = probe & 0xFFFFFFFCu;
        if (mask == 0) { return 1; }
        bar->is_io = 1;
        bar->addr  = original & 0xFFFFFFFCu;
        bar->size  = (~mask + 1) & 0xFFFF;   // I/O BARs decode 16 bits
        return 1;
    }

    // Memory BAR. Bits 2:1 give the type; 0x2 means the base is 64 bits
    // wide and continues in the next BAR slot.
    int is_64 = ((original >> 1) & 0x3) == 0x2;
    int slots = (is_64 && index + 1 < PCI_NUM_BARS) ? 2 : 1;

    uint64_t addr = original & 0xFFFFFFF0u;
    uint64_t mask = probe & 0xFFFFFFF0u;

    if (slots == 2) {
        uint8_t hi_reg = (uint8_t)(reg + 4);
        uint32_t hi_original = pci_config_read32(d, hi_reg);
        pci_config_write32(d, hi_reg, 0xFFFFFFFFu);
        uint32_t hi_probe = pci_config_read32(d, hi_reg);
        pci_config_write32(d, hi_reg, hi_original);

        addr |= (uint64_t)hi_original << 32;
        mask |= (uint64_t)hi_probe << 32;
        if (mask == 0) { return slots; }     // decodes nothing
    } else {
        if (mask == 0) { return slots; }
        // A 32-bit BAR's size comes from the low half alone. Extending
        // the mask with ones first is what makes ~mask+1 stop at 4 GiB
        // instead of running off the top of the word.
        mask |= 0xFFFFFFFF00000000ull;
    }

    bar->is_io = 0;
    bar->addr  = addr;
    bar->size  = ~mask + 1;
    return slots;
}

// serial_write_hex64 pads to sixteen digits, which turns a bus:dev.fn
// line into forty characters of leading zeros. These lines are read by
// eye at every boot, so they get their own narrow printer.
static void hexn(uint64_t v, int digits) {
    static const char *hexdigits = "0123456789abcdef";
    for (int i = digits - 1; i >= 0; i--) {
        serial_putc(hexdigits[(v >> (i * 4)) & 0xF]);
    }
}

static void log_device(const struct pci_device *d) {
    serial_write_string("[pci] ");
    hexn(d->bus, 2);  serial_putc(':');
    hexn(d->dev, 2);  serial_putc('.');
    hexn(d->fn, 1);
    serial_write_string(" ");
    hexn(d->vendor, 4); serial_putc(':');
    hexn(d->device, 4);
    serial_write_string(" class=");
    hexn(d->class, 2); serial_putc(':');
    hexn(d->subclass, 2); serial_putc(':');
    hexn(d->prog_if, 2);
    serial_write_string(" irq=");
    hexn(d->irq_line, 2);
    for (int b = 0; b < PCI_NUM_BARS; b++) {
        if (!d->bar[b].size) { continue; }
        serial_write_string(" bar");
        hexn((uint64_t)b, 1);
        serial_write_string(d->bar[b].is_io ? "=io:" : "=mem:");
        hexn(d->bar[b].addr, 8);
        serial_write_string("/");
        hexn(d->bar[b].size, 8);
    }
    serial_write_string("\n");
}

static void record(uint8_t bus, uint8_t dev, uint8_t fn, uint32_t id) {
    if (device_count >= PCI_MAX_DEVICES) {
        devices_dropped++;
        return;
    }
    struct pci_device *d = &devices[device_count];

    d->bus = bus; d->dev = dev; d->fn = fn;
    d->vendor = (uint16_t)(id & 0xFFFF);
    d->device = (uint16_t)(id >> 16);

    uint32_t cls = raw_read32(bus, dev, fn, PCI_REG_CLASS);
    d->prog_if  = (uint8_t)((cls >> 8)  & 0xFF);
    d->subclass = (uint8_t)((cls >> 16) & 0xFF);
    d->class    = (uint8_t)((cls >> 24) & 0xFF);

    uint32_t hdr = raw_read32(bus, dev, fn, PCI_REG_HEADER);
    d->header_type = (uint8_t)((hdr >> 16) & 0xFF);

    uint32_t irq = raw_read32(bus, dev, fn, PCI_REG_INTERRUPT);
    d->irq_line = (uint8_t)(irq & 0xFF);
    d->irq_pin  = (uint8_t)((irq >> 8) & 0xFF);

    // BARs only exist in the type 0 header. A bridge (type 1) has two,
    // laid out differently, and none of them are wanted here; a
    // cardbus bridge (type 2) has none at all. Sizing them anyway would
    // write ones into a bridge's window registers, which is exactly the
    // kind of "harmless probe" that unmaps the disk.
    if ((d->header_type & 0x7F) == 0) {
        for (int i = 0; i < PCI_NUM_BARS; ) {
            i += decode_bar(d, i);
        }
    }

    device_count++;
    log_device(d);
}

void pci_init(void) {
    device_count = 0;
    devices_dropped = 0;

    for (int bus = 0; bus < 256; bus++) {
        for (int dev = 0; dev < 32; dev++) {
            uint32_t id = raw_read32((uint8_t)bus, (uint8_t)dev, 0, PCI_REG_VENDOR);
            if ((id & 0xFFFF) == 0xFFFF) {
                continue;                    // nothing at function 0
            }
            record((uint8_t)bus, (uint8_t)dev, 0, id);

            // A device that does not claim to be multi-function has no
            // functions past 0, and probing them is how you get
            // phantom devices on hardware that aliases the decode.
            uint32_t hdr = raw_read32((uint8_t)bus, (uint8_t)dev, 0, PCI_REG_HEADER);
            if (!(((hdr >> 16) & 0xFF) & PCI_HEADER_MULTIFUNC)) {
                continue;
            }
            for (int fn = 1; fn < 8; fn++) {
                uint32_t fid = raw_read32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, PCI_REG_VENDOR);
                if ((fid & 0xFFFF) == 0xFFFF) { continue; }
                record((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, fid);
            }
        }
    }

    serial_write_string("[pci] enumerated ");
    hexn((uint64_t)device_count, 2);
    serial_write_string(" functions\n");
    if (devices_dropped) {
        // Say it rather than silently truncating: a missing NIC three
        // milestones later is a much worse way to learn the table is
        // too small.
        serial_write_string("[pci] WARNING: table full, dropped ");
        hexn((uint64_t)devices_dropped, 2);
        serial_write_string(" functions\n");
    }
}

int pci_count(void) { return device_count; }

const struct pci_device *pci_get(int index) {
    if (index < 0 || index >= device_count) { return 0; }
    return &devices[index];
}

const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].vendor == vendor && devices[i].device == device) {
            return &devices[i];
        }
    }
    return 0;
}

const struct pci_device *pci_find_class(uint8_t class, uint8_t subclass) {
    for (int i = 0; i < device_count; i++) {
        if (devices[i].class == class && devices[i].subclass == subclass) {
            return &devices[i];
        }
    }
    return 0;
}

// ---------------------------------------------------------------------
// Selftest.

static int failures;

static void fail(const char *what) {
    serial_write_string("[pci] FAILED: ");
    serial_write_string(what);
    serial_write_string("\n");
    failures++;
}

void pci_selftest(void) {
    failures = 0;

    // 1. Something was found. Zero functions on a PC means enumeration
    //    is broken, not that the machine is empty.
    if (device_count == 0) {
        fail("no PCI functions found at all");
        return;
    }

    // 2. The host bridge. Every PC has one at 00:00.0, so its absence
    //    is a specific failure rather than a vague one.
    const struct pci_device *host = pci_find_class(PCI_CLASS_BRIDGE, PCI_SUBCLASS_HOST);
    if (!host) {
        fail("no host bridge (class 06:00)");
    } else if (host->bus != 0 || host->dev != 0 || host->fn != 0) {
        fail("host bridge is not at 00:00.0");
    }

    // 3. Every reported BAR is sane. A size that is not a power of two
    //    means the read-back mask was misinterpreted, and an I/O BAR
    //    wider than 16 bits means the I/O and memory cases got crossed.
    for (int i = 0; i < device_count; i++) {
        const struct pci_device *d = &devices[i];
        for (int b = 0; b < PCI_NUM_BARS; b++) {
            uint64_t size = d->bar[b].size;
            if (size == 0) { continue; }
            if (size & (size - 1)) {
                fail("BAR size is not a power of two");
            }
            if (d->bar[b].is_io && (d->bar[b].addr > 0xFFFF || size > 0x10000)) {
                fail("I/O BAR does not fit in 16 bits");
            }
        }
    }

    // 4. Config space round-trips. Reading proves nothing about the
    //    address/data pairing -- a wedged CONFIG_ADDRESS would still
    //    return whatever the last programmed address held. Writing the
    //    command register back UNCHANGED and reading it again proves
    //    both ports are actually wired, and changes no state.
    if (host) {
        uint32_t before = pci_config_read32(host, PCI_REG_COMMAND);
        pci_config_write32(host, PCI_REG_COMMAND, before);
        uint32_t after = pci_config_read32(host, PCI_REG_COMMAND);
        // Only the command half is written back; the status half above
        // it has write-1-to-clear bits and may legitimately differ.
        if ((before & 0xFFFF) != (after & 0xFFFF)) {
            fail("config space did not round-trip");
        }
    }

    // 5. The two devices QEMU's `pc` machine always has. Asserting on
    //    ids the emulator guarantees makes this specific; asserting on
    //    a NIC would not, since -net is optional and this must pass on
    //    a machine with no network at all.
    if (!pci_find_id(0x8086, 0x1237)) {
        fail("no 82441FX host bridge (8086:1237)");
    }
    if (!pci_find_id(0x8086, 0x7000)) {
        fail("no PIIX3 ISA bridge (8086:7000)");
    }

    if (failures == 0) {
        serial_write_string("[pci] ALL PASSED\n");
    }
}
