// AHCI selftests.
#include "drivers/block/ahci/ahci.h"
#include "block/blockdev.h"

static struct ahci_link *scratch_link;
static struct blockdev *scratch_dev;

void ahci_note_scratch(struct ahci_link *l, struct blockdev *d) { scratch_link = l; scratch_dev = d; }

void ahci_selftest(void) {}
