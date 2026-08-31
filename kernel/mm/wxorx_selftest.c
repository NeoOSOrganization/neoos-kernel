// Boot-time assertion that the kernel address space is W^X after
// paging_protect_kernel() has run: no page in the kernel image is both
// writable and executable. A failure here means a section symbol is
// wrong or a huge page was not split -- decode the reported VA with
// `nm` / `addr2line`.

#include "mm/paging.h"
#include "dev/serial.h"

extern char __text_start[], __kernel_end[];

void wxorx_selftest(void) {
    uint64_t start = (uint64_t)(uintptr_t)__text_start;
    uint64_t end   = (uint64_t)(uintptr_t)__kernel_end;

    uint64_t checked = 0;
    for (uint64_t va = start; va < end; va += 4096) {
        uint64_t e = paging_leaf_entry(va);
        if (!(e & 1 /* PAGE_PRESENT */)) { continue; }
        checked++;
        int writable = (e & PAGE_WRITABLE) != 0;
        int exec     = (e & PAGE_NO_EXECUTE) == 0;
        if (writable && exec) {
            serial_write_string("[wxorx] kernel selftest FAILED: W+X page at ");
            serial_write_hex64(va);
            serial_write_string("\n");
            return;
        }
    }

    if (checked == 0) {
        serial_write_string("[wxorx] kernel selftest FAILED: walked no pages\n");
        return;
    }
    serial_write_string("[wxorx] kernel selftest passed\n");
}
