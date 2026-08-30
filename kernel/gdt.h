#ifndef NEOOS_GDT_H
#define NEOOS_GDT_H

#include <stdint.h>

#define GDT_KERNEL_CODE_SELECTOR 0x08
#define GDT_KERNEL_DATA_SELECTOR 0x10
#define GDT_TSS_SELECTOR         0x18 // legacy single-CPU TSS slot; see gdt.c
#define GDT_USER_CODE32_SELECTOR 0x28 // never loaded -- exists only for STAR's SYSRET offset arithmetic
#define GDT_USER_DATA_SELECTOR   (0x30 | 3)
#define GDT_USER_CODE_SELECTOR   (0x38 | 3)

void gdt_init(void);

// Loads the SHARED GDT on the calling CPU and installs that CPU's own
// TSS. Every CPU loads the same table and differs only in the selector
// it feeds to ltr.
void gdt_load(int cpu_index);

// Selector for `cpu_index`'s TSS descriptor. Distinct per CPU: two CPUs
// sharing one TSS share rsp0, and the second ring-3 entry would land on
// the first CPU's kernel stack.
uint16_t gdt_tss_selector(int cpu_index);

#endif
