ENTRY(_start)

KERNEL_VIRT_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    /* Low half: Multiboot2 header and boot.asm's 32/64-bit bootstrap.
       VMA == LMA here -- this code runs before any higher-half mapping
       exists, so it cannot reference high addresses. */
    . = 1M;

    /* kernel_phys_start covers the WHOLE image, boot block included --
       the frame allocator (kernel/mm/pmm.c) uses this to exclude the
       kernel's physical footprint from its free lists, and that must
       include boot.asm's live p4_table/p3_table/p2_tables and boot
       stack (all in .boot.bss below), not just the high-half C kernel.
       The low and high blocks are physically contiguous (the high
       block's AT() picks up right where the low block ends), so a
       single range here correctly covers both. */
    kernel_phys_start = .;

    .multiboot ALIGN(8) :
    {
        *(.multiboot_header)
    }

    .boot.text ALIGN(4K) :
    {
        *(.boot.text)
    }

    .boot.rodata ALIGN(4K) :
    {
        *(.boot.rodata)
    }

    .boot.bss ALIGN(4K) :
    {
        *(.boot.bss)
    }

    /* The AP trampoline. Lives in the LOW half so its symbols carry
       physical addresses the BSP can memcpy from -- smp_start_aps copies
       it to 0x8000, where the APs actually execute it. Its own internal
       references are all computed from AP_BASE, never from these
       link-time addresses. */
    .ap_trampoline ALIGN(4K) :
    {
        *(.ap_trampoline)
    }

    /* High half: the C kernel (gdt_flush.asm and isr.asm's default
       .text included) and everything it calls. Linked at
       KERNEL_VIRT_BASE, loaded contiguously in physical memory right
       after the boot block via AT(). boot.asm's static page tables
       already map this physical range at this VMA (see
       set_up_page_tables), so `call kmain` lands correctly the moment
       paging is enabled -- no separate trampoline needed. */
    . = ALIGN(4K);
    . += KERNEL_VIRT_BASE;

    /* Section-boundary symbols, all 4 KiB-aligned, for the NX / W^X pass
       in kernel/mm/paging.c: .text is mapped read-only + executable,
       everything from .rodata onward is NX (and .rodata is read-only). */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VIRT_BASE)
    {
        __text_start = .;
        *(.text .text.*)
        . = ALIGN(4K);
        __text_end = .;
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VIRT_BASE)
    {
        __rodata_start = .;
        *(.rodata .rodata.*)
        . = ALIGN(4K);
        __rodata_end = .;
    }

    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VIRT_BASE)
    {
        __data_start = .;
        *(.data .data.*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VIRT_BASE)
    {
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(4K);
        __kernel_end = .;   /* higher-half VA end, for paging_protect_kernel */
    }

    kernel_phys_end = . - KERNEL_VIRT_BASE;
}
