#include "tss.h"

#define IST1_STACK_SIZE 4096

struct tss_entry tss[MAX_TSS];

// One IST1 stack PER CPU, not one shared. IST1 is the double-fault and
// NMI stack: the CPU switches to it unconditionally on those vectors, so
// two CPUs faulting at the same time would land on the same stack and
// overwrite each other's frame -- turning two diagnosable faults into
// one unreadable corruption. 128 * 4KiB = 512KiB of BSS, which is cheap
// next to losing the ability to read a crash.
static unsigned char ist1_stacks[MAX_TSS][IST1_STACK_SIZE]
    __attribute__((aligned(16)));

void tss_init(void) {
    for (int c = 0; c < MAX_TSS; c++) {
        unsigned char *raw = (unsigned char *)&tss[c];
        for (unsigned int i = 0; i < sizeof(struct tss_entry); i++) {
            raw[i] = 0;
        }
        tss[c].ist1 = (uint64_t)(ist1_stacks[c] + IST1_STACK_SIZE);
        tss[c].iomap_base = sizeof(struct tss_entry);
    }
}
