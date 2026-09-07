ENTRY(_start)

/* PML4 index 64 (0x0000200000000000) -- deliberately NOT under PML4
   index 0. Every process's PML4[0] is a direct COPY of the kernel's own
   p4_table[0], which boot.asm created with flags PRESENT|WRITABLE only
   (no PAGE_USER) since it was only ever meant for kernel-internal
   low-identity-map use. PML4-level permissions gate everything beneath
   them, so ANY address under index 0 -- the first 512GiB of the address
   space -- is permanently inaccessible to user mode no matter what the
   PDPT/PD/PT entries beneath it say (confirmed via a direct page-table
   walk: the final PTE was correctly PRESENT|WRITABLE|USER with no NX,
   yet execution still faulted, because the copied PML4[0] entry itself
   lacks PAGE_USER). Index 64 is untouched by any shared entry, so
   paging_map_into's table_entry creates it fresh with proper
   PAGE_USER flags via its own default_flags. */
SECTIONS
{
    /* + SIZEOF_HEADERS so the ELF header and the program header table
       fall INSIDE the first PT_LOAD segment and therefore have a
       virtual address at run time. Without it the headers sit at file
       offset 64 while the segment starts at 0x1000, no loaded segment
       covers them, and the kernel has no address to put in AT_PHDR --
       which is the only way a static executable can find its own PT_TLS
       template. Costs one page of address space and nothing else. */
    . = 0x200000000000 + SIZEOF_HEADERS;

    /* musl's crt1 entry does `lea _DYNAMIC(%rip),%rsi` against a WEAK
       undefined symbol. Undefined means 0, and a PC-relative
       displacement from 0x200000000000 down to 0 does not fit in the
       32 bits the relocation has -- so the link fails with "relocation
       truncated to fit" before anything runs.
       Defining it HERE, inside the image, makes the displacement
       small. Nothing reads the value: crt1.c's _start_c takes only the
       stack pointer and ignores the second argument, which is where
       this lands. PROVIDE so a program that defines its own wins. */
    PROVIDE(_DYNAMIC = .);

    .text ALIGN(4K) : { *(.text .text.*) }
    .rodata ALIGN(4K) : { *(.rodata .rodata.*) }

    /* dotnet NativeAOT's .init/.fini are 1-byte placeholders (it does
       not use the traditional ELF DT_INIT/DT_FINI mechanism at all),
       and libbootstrapper.o's InitializeRuntime() writes into that
       placeholder byte at runtime with no mprotect() call first
       (confirmed: instrumented sys_mprotect, zero calls before the
       crash; confirmed further by reading the actual section headers
       -- .text ends at EXACTLY the faulting address, with .init
       starting there). Left unlisted, GNU ld's orphan placement
       grouped them with .text (read+exec only, no write) instead of
       with .data -- which this SDK's own object files mark WAX
       (write+alloc+exec) by design, the real signal that its actual
       reference linker script groups .init/.fini with the writable
       data region, not with .text. */


    /* dotnet NativeAOT's unboxing stubs: genuinely both executable
       (SHF_EXECINSTR -- confirmed: merging *(__unbox) here makes ld
       warn "LOAD segment with RWX permissions") AND self-patched at
       runtime with no mprotect() call first (confirmed: instrumented
       sys_mprotect, zero calls before the crash reached here) -- a
       real W+X requirement, not a placement mistake on either side.
       Merged into .data (already writable) rather than kept as its
       own output section, so the resulting W+X region stays bounded
       to .data+__unbox's combined size -- tens of KB for this test
       binary -- rather than granting W+X to the whole multi-hundred-KB
       .text region. Requires manually defining __start___unbox/
       __stop___unbox below: GNU ld only auto-synthesizes those for an
       output section whose OWN name is the bare section name, which
       merging into .data forgoes -- but libbootstrapper.o's
       InitializeRuntime() references them directly, so something must
       supply them. Requires kernel/elf.c to accept a bounded W+X
       PT_LOAD segment for this to actually run -- see
       docs/superpowers/plans/2026-09-07-hosted-neoos-gcc.md's
       post-implementation notes for that deliberate policy decision. */
    .data ALIGN(4K) : {
        *(.init .fini)
        *(.data .data.*)
        . = ALIGN(4K);
        PROVIDE(__start___unbox = .);
        *(__unbox)
        PROVIDE(__stop___unbox = .);
    }

    /* .tdata and .tbss MUST be named here, and in this order. Left as
       orphan sections the linker placed both at the SAME address --
       .tbss at 0x...3e60 and .tdata at 0x...3e60 -- so every
       thread-local in .tdata overlapped one in .tbss and the offsets
       the compiler emitted bore no relation to where the initialised
       bytes actually were. The symptom was a __thread int initialised
       to 0x5a5a reading as 0.

       .tdata comes first because PT_TLS's file-backed part is its
       leading bytes: p_offset points at .tdata and p_filesz covers only
       it, with .tbss making up the rest of p_memsz as zero. */
    .tdata ALIGN(4K) : { *(.tdata .tdata.*) }
    .tbss : { *(.tbss .tbss.*) *(.tcommon) }

    .bss ALIGN(4K) : { *(COMMON) *(.bss .bss.*) }
}
