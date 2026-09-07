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

    /* .init/.fini here are the ordinary musl crt convention (crti.o
       opens each, various objects contribute a middle fragment,
       crtn.o closes each with pop/ret) -- NOT, as an earlier version
       of this comment claimed, a NativeAOT placeholder byte that
       InitializeRuntime() patches at runtime. That was checked
       directly by disassembling libbootstrapper.o's main() (which
       inlines InitializeRuntime()): it never references .init/.fini
       at all. Left unlisted, GNU ld's orphan placement grouped
       .init/.fini with .text (read+exec only, no write) instead of
       with .data -- which this SDK's own object files mark WAX
       (write+alloc+exec) by design, matching the fact that .init/
       .fini need to be writable-at-link-time output but are pure
       code otherwise; .data was the natural place to route them. */


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
        /* .init and .fini are each assembled from MULTIPLE object
           files' contributions into ONE callable function per name
           (crti.o's opening instruction + every object's middle
           fragment + crtn.o's closing pop/ret) -- concatenated in
           link order. A single combined wildcard `*(.init .fini)`
           does NOT concatenate all .init fragments followed by all
           .fini fragments: GNU ld walks input files in link order and
           for each file pulls sections in the NAME order given, so it
           interleaves crti.o's one-byte .init with crti.o's one-byte
           .fini, then the next object's .init with its .fini, and so
           on -- neither resulting byte stream is a valid function.
           Confirmed directly via -Wl,-Map: crti.o's one-byte opening
           (which defines the _init/_fini symbols) was placed, but
           crtn.o's one-byte closing (`pop %rax; ret`, anonymous --
           nothing else references it) was DROPPED by --gc-sections
           (NativeAOT's own link line passes it), listed in the map at
           address 0x0. So _init was really just a bare `push %rax`
           with no `ret`: crt1 called it, execution fell through into
           whatever bytes followed in .data, and eventually hit one
           that happened to decode as `hlt` -- a ring-3 #GP (hlt is
           privileged). KEEP() is the actual fix: it marks a matched
           input section as a GC root so --gc-sections cannot drop it
           even though nothing in the kept graph relocates to it. */
        KEEP(*(.init))
        KEEP(*(.fini))
        *(.data .data.*)

        /* C++ global constructors/destructors, found the same way as
           .init/.fini above and by the same mechanism: left unlisted,
           --gc-sections dropped the ENTIRE .init_array/.fini_array
           input sections (nothing in the kept graph relocates to
           them), so no __init_array_start/__init_array_end ever got
           defined. musl's __libc_start_main declares both `weak
           hidden` (src/env/__libc_start_main.c) -- an undefined weak
           symbol resolves to address 0 rather than a link error, so
           this failed SILENTLY: start=end=0, the constructor loop's
           `a < end` is `0 < 0`, zero iterations, no diagnostic of any
           kind. Confirmed directly: readelf -S on a linked binary
           showed no .init_array section at all, and dotnet
           NativeAOT's WorkstationGC deadlocked on its very first
           lock acquisition, because the C++ constructor that would
           have set global_region_allocator's GCDebugSpinLock to its
           unlocked value (-1) never ran -- the lock sat at its raw
           zero-BSS value instead, and 0 means "held" in that lock's
           own convention. KEEP() plus explicit PROVIDE()s (not GNU
           ld's automatic per-section-name synthesis, to match this
           file's existing style for __unbox below and because the
           exact symbol names musl's C code declares must be matched
           exactly regardless of what ld would infer). */
        PROVIDE(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE(__preinit_array_end = .);
        PROVIDE(__init_array_start = .);
        KEEP(*(.init_array))
        PROVIDE(__init_array_end = .);
        PROVIDE(__fini_array_start = .);
        KEEP(*(.fini_array))
        PROVIDE(__fini_array_end = .);

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
