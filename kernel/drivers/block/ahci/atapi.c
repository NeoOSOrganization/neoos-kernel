// ATAPI (packet) devices behind AHCI: optical drives, registered as srN.
#include "drivers/block/ahci/ahci.h"
#include "drivers/char/serial.h"
#include "errno.h"

int ahci_atapi_attach(struct ahci_link *l) {
    ahci_log_port(l->port);
    serial_write_string("ATAPI device, not a disk\n");
    return -ENODEV;
}
