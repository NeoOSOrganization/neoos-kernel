#ifndef NEOOS_TSS_H
#define NEOOS_TSS_H

#include <stdint.h>

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

#define MAX_TSS 128  // one per CPU, matching MAX_CPUS

// Do NOT include cpu_local.h here: cpu_local.h includes this header,
// and the reverse include would be circular.
extern struct tss_entry tss[MAX_TSS];

void tss_init(void);

#endif
