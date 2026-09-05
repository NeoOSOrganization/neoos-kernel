#include "mm/uaccess.h"
#include "drivers/char/serial.h"
#include <stdint.h>

// Runs at early boot, like every other selftest here -- before any
// process/scheduler exists (current_proc() returns 0 at this point,
// so the ONLY address space in play is the kernel's own page tables,
// including its own low-identity map at PML4[0]).
//
// That genuinely limits this selftest to baseline correctness only:
// both of the mechanism's actually interesting properties --
// "a valid-but-untouched page is demand-paged in and the copy
// transparently succeeds" and "a genuinely invalid address fails
// cleanly instead of halting the machine" -- are properties of
// vma_fault() walking a PROCESS's VMA list. Without a process there
// is no VMA list to have an opinion about any address at all: a
// address that would be "genuinely invalid" for a real process may
// perfectly well land inside the kernel's OWN present, writable
// identity map (confirmed empirically while writing this file: a
// write to address 0 from ring 0 succeeds silently here, because
// PML4[0]'s identity map has PRESENT|WRITABLE set and ring 0 does not
// care about the missing PAGE_USER bit -- there is no "invalid
// address" available to test against in this context that means what
// the test would want it to mean).
//
// Both properties ARE verified for real: the neoos-doom WAD-load
// regression re-run (this plan's Task 5) is a real process, with a
// real fresh mmap'd buffer for the success case, going through the
// real syscall path -- a more convincing proof than a synthetic
// in-kernel one in a context with no process could ever be anyway.
void uaccess_selftest(void) {
    uint8_t src[16], dst[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(i + 1); }

    uint64_t missed = copy_to_user(dst, src, sizeof src);
    if (missed != 0) {
        serial_write_string("[uaccess] selftest FAILED: baseline copy_to_user reported a miss\n");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (dst[i] != src[i]) {
            serial_write_string("[uaccess] selftest FAILED: baseline copy_to_user corrupted data\n");
            return;
        }
    }

    for (int i = 0; i < 16; i++) { dst[i] = 0; }
    missed = copy_from_user(dst, src, sizeof src);
    if (missed != 0) {
        serial_write_string("[uaccess] selftest FAILED: baseline copy_from_user reported a miss\n");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (dst[i] != src[i]) {
            serial_write_string("[uaccess] selftest FAILED: baseline copy_from_user corrupted data\n");
            return;
        }
    }

    serial_write_string("[uaccess] selftest passed\n");
}
