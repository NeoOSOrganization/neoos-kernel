// SATA port multipliers behind AHCI.
#include "drivers/block/ahci/ahci.h"
#include "errno.h"

int ahci_pmp_attach(struct ahci_port *p) { (void)p; return -ENODEV; }
