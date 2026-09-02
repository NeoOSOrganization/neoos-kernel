#ifndef NEOOS_PCI_H
#define NEOOS_PCI_H

#include <stdint.h>

// PCI configuration space, the legacy way: an address register and a
// data register in I/O space. ECAM would be faster and reaches
// functions past 255 buses, but it needs the MCFG table out of ACPI,
// which NeoOS does not parse. Port I/O works on every PC and covers
// every device this kernel wants to talk to.
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

// Register offsets in the type 0 header, by name, because 0x3C is not
// self-explanatory at the call site.
#define PCI_REG_VENDOR      0x00   // vendor (15:0), device (31:16)
#define PCI_REG_COMMAND     0x04   // command (15:0), status (31:16)
#define PCI_REG_CLASS       0x08   // revision, prog-if, subclass, class
#define PCI_REG_HEADER      0x0C   // cache line, latency, header type, BIST
#define PCI_REG_BAR0        0x10
#define PCI_REG_INTERRUPT   0x3C   // irq line (7:0), irq pin (15:8)

#define PCI_CMD_IO_SPACE     0x0001
#define PCI_CMD_MEM_SPACE    0x0002
#define PCI_CMD_BUS_MASTER   0x0004

#define PCI_HEADER_MULTIFUNC 0x80

#define PCI_CLASS_STORAGE    0x01
#define PCI_CLASS_NETWORK    0x02
#define PCI_CLASS_BRIDGE     0x06
#define PCI_SUBCLASS_HOST    0x00

#define PCI_MAX_DEVICES 64
#define PCI_NUM_BARS    6

struct pci_bar {
    uint64_t addr;    // base address, with the type bits masked off
    uint64_t size;    // 0 when the BAR is unimplemented
    int      is_io;   // I/O space rather than memory space
};

struct pci_device {
    uint8_t  bus, dev, fn;
    uint16_t vendor, device;
    uint8_t  class, subclass, prog_if, header_type;
    uint8_t  irq_line, irq_pin;
    struct pci_bar bar[PCI_NUM_BARS];
};

// Enumerate the whole bus space once. Called on the BSP, before the APs
// start; the table is read-only afterwards, which is why nothing here
// takes a lock.
void pci_init(void);
void pci_selftest(void);

int pci_count(void);
const struct pci_device *pci_get(int index);
const struct pci_device *pci_find_id(uint16_t vendor, uint16_t device);
const struct pci_device *pci_find_class(uint8_t class, uint8_t subclass);

uint32_t pci_config_read32(const struct pci_device *d, uint8_t reg);
void     pci_config_write32(const struct pci_device *d, uint8_t reg, uint32_t value);

// Separate and explicit: a device cannot DMA until this bit is set, and
// a driver that forgets it looks correct and moves no data. Naming it
// in the interface is cheaper than rediscovering it in a driver.
void pci_enable_bus_master(const struct pci_device *d);

#endif
