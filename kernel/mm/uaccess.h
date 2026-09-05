#ifndef NEOOS_UACCESS_H
#define NEOOS_UACCESS_H

#include <stdint.h>

// The exception table: one entry per recoverable copy-loop
// instruction, pairing its address with where to resume instead on a
// genuinely unrecoverable fault. Populated by EX_TABLE_ENTRY inside
// copy_to_user/copy_from_user's inline asm (kernel/mm/uaccess.c);
// walked by kernel/arch/isr.c's page-fault handler.
//
// A ring-0 fault at an instruction NOT in this table still crashes
// the machine exactly as before -- this table is the ONLY thing that
// makes a kernel-mode fault survivable, and it is deliberately an
// explicit per-instruction opt-in, not a blanket "any fault in user
// address space is fine" rule. See the design spec section 2 for why
// the narrower rule is the safe one.
struct exception_entry {
    uint64_t fault_addr;
    uint64_t fixup_addr;
};

extern struct exception_entry __ex_table_start[], __ex_table_end[];

// Emits one .quad pair into the .ex_table section: the address of
// `fault_label` (the instruction that may fault) and `fixup_label`
// (where control resumes if it does and the fault turns out to be
// unrecoverable). Both labels are LOCAL numeric labels (e.g. "1" and
// "2") inside the SAME asm block that uses this macro -- GNU as scopes
// numeric local labels per nearest-occurrence, so reusing "1"/"2"
// across many copy_to_user/copy_from_user call sites in the same
// translation unit is safe by construction; no %=-uniquification
// needed.
#define EX_TABLE_ENTRY(fault_label, fixup_label) \
    ".pushsection .ex_table, \"a\"\n\t" \
    ".quad " #fault_label "b\n\t" \
    ".quad " #fixup_label "b\n\t" \
    ".popsection\n\t"

// Copies `n` bytes from kernel memory `src` to user memory `dst`.
// Returns 0 on full success, or the number of bytes NOT copied
// (Linux's copy_to_user convention) if `dst` turned out to be
// genuinely invalid partway through. A destination page that is valid
// but not yet backed is transparently demand-paged in and the copy
// continues -- the caller never sees that case as a failure.
uint64_t copy_to_user(void *dst, const void *src, uint64_t n);

// Same contract, opposite direction: `dst` is kernel memory, `src` is
// user memory.
uint64_t copy_from_user(void *dst, const void *src, uint64_t n);

#endif
