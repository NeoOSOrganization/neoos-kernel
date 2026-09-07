ENTRY(_start)

/* ===================================================================
   NeoOS userland linker script.

   POLICY (read before editing): this script is DERIVED FROM the stock
   GNU ld default script (`x86_64-neoos-linux-musl-ld --verbose`), not
   hand-written from a blank page. Every section the stock script names
   with KEEP(), and every boundary symbol it PROVIDE_HIDDEN()s, is
   reproduced here. The only intentional deviations are:

     1. load address 0x200000000000 + SIZEOF_HEADERS (see PML4 note
        below) instead of 0x400000;
     2. `_DYNAMIC` PROVIDEd inside the image (musl crt1 relocation);
     3. .init / .fini and dotnet NativeAOT's self-patched __unbox stubs
        routed into a *writable* output section, because the SDK's own
        objects mark them WAX and the stubs are patched at runtime with
        no mprotect() first -- a bounded W+X region kernel/elf.c
        explicitly permits;
     4. .tdata forced before .tbss, both named explicitly;
     5. no DATA_SEGMENT_* / RELRO markers -- NeoOS's ELF loader ignores
        PT_GNU_RELRO, so emitting the segment would buy nothing.

   WHY THE FULL KEEP SET MATTERS: dotnet NativeAOT's link line passes
   `-Wl,--gc-sections`. --gc-sections removes any section not reachable
   by a *relocation* from a GC root. A whole class of sections is
   reached only at RUN TIME through a mechanism the linker cannot see --
   a program header (PT_GNU_EH_FRAME), a crt loop over
   __init_array_start.._end, the dynamic loader's IRELATIVE pass over
   __rela_iplt_start.._end. Those are exactly the sections the stock
   script KEEP()s. A from-scratch minimal script that lists only
   .text/.rodata/.data/.bss silently drops all of them, and each such
   drop has cost this project a multi-day debugging session
   (.init/.fini closer -> ring-3 #GP; .init_array -> GC lock deadlock;
   .eh_frame -> GC.Collect() FailFast). Do not re-minimize this file.
   =================================================================== */

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

    /* --- Notes -------------------------------------------------------
       Named explicitly (as the stock script does, and for the same
       reason its comment gives) so they land in the read-only segment
       deterministically rather than depending on orphan-placement
       heuristics. .note.gnu.build-id must sit as close to the ELF
       header as possible so it survives in core dumps; .note.gnu.property
       carries x86-64 ISA / CET markers a loader may inspect;
       .note.ABI-tag is the SysV ABI/OS-version note. ILC passes
       -Wl,--build-id=sha1, so build-id is always present. */
    .note.gnu.build-id  : { *(.note.gnu.build-id) }
    .note.gnu.property   : { *(.note.gnu.property) }
    .note.ABI-tag        : { *(.note.ABI-tag) }

    /* --- Static relocations ----------------------------------------
       A statically linked binary has no dynamic loader, so its own crt
       start code applies R_X86_64_IRELATIVE relocations (ifunc
       resolvers -- e.g. a CPU-dispatched memcpy) by walking
       __rela_iplt_start..__rela_iplt_end itself. Omit this block and a
       binary that uses __attribute__((ifunc)) anywhere links fine but
       every ifunc call jumps through an unrelocated GOT slot to 0 --
       the same silent-until-it-faults failure mode as the .init_array
       bug. .rela.dyn is here too for completeness (IRELATIVE entries
       can land there under -z combreloc). PROVIDE_HIDDEN, exactly as
       the stock script does -- crt references these as weak, so
       defining them always is harmless when there are no ifuncs. */
    .rela.dyn :
    {
        *(.rela.init)
        *(.rela.text .rela.text.* .rela.gnu.linkonce.t.*)
        *(.rela.fini)
        *(.rela.rodata .rela.rodata.* .rela.gnu.linkonce.r.*)
        *(.rela.data .rela.data.* .rela.gnu.linkonce.d.*)
        *(.rela.tdata .rela.tdata.* .rela.gnu.linkonce.td.*)
        *(.rela.tbss .rela.tbss.* .rela.gnu.linkonce.tb.*)
        *(.rela.got)
        *(.rela.bss .rela.bss.* .rela.gnu.linkonce.b.*)
        *(.rela.ifunc)
    }
    .rela.plt :
    {
        *(.rela.plt)
        PROVIDE_HIDDEN (__rela_iplt_start = .);
        *(.rela.iplt)
        PROVIDE_HIDDEN (__rela_iplt_end = .);
    }

    .text ALIGN(4K) :
    {
        *(.text.unlikely .text.*_unlikely .text.unlikely.*)
        *(.text.exit .text.exit.*)
        *(.text.startup .text.startup.*)
        *(.text.hot .text.hot.*)
        *(.text .stub .text.*)
        *(.gnu.warning)
        *(.plt .plt.* .iplt)
    }
    .rodata ALIGN(4K) : { *(.rodata .rodata.* .gnu.linkonce.r.*) *(.rodata1) }

    /* --- Exception / unwind tables (read-only) ---------------------
       dotnet NativeAOT unwinds its *managed* code with DWARF CFI on
       x86_64: a GC stack walk goes UnixNativeCodeManager::FindMethodInfo
       -> UnwindHelpers::GetUnwindProcInfo -> libunwind's
       getInfoFromDwarfSection, which finds .eh_frame through the
       PT_GNU_EH_FRAME segment that indexes .eh_frame_hdr. ILC's own link
       line passes both -Wl,--eh-frame-hdr and -Wl,--gc-sections.

       Left unlisted, .eh_frame met the same fate as .init_array /
       .init / .fini / __unbox: nothing in the kept graph *relocates*
       to it -- it is reached only via a program header at run time --
       so --gc-sections dropped .eh_frame outright, --eh-frame-hdr then
       had nothing to index, and no .eh_frame_hdr / PT_GNU_EH_FRAME was
       emitted at all (confirmed with readelf: zero .eh_frame* sections,
       only LOAD/LOAD/TLS/GNU_STACK phdrs). Visible symptom: any
       GC.Collect() from a non-main thread FailFasts inside
       FindMethodInfo, because the GC's stack walk of the collecting
       thread starts at the return address baked into the RhCollect
       p/invoke transition frame -- a real managed PC -- and
       FindMethodInfo cannot resolve any PC with no unwind data present.

       KEEP() .eh_frame (--gc-sections would otherwise prune FDEs whose
       target it cannot associate across the __managedcode boundary);
       naming the .eh_frame_hdr OUTPUT section plus ILC's --eh-frame-hdr
       is what makes bfd ld emit PT_GNU_EH_FRAME. .gcc_except_table /
       .gnu_extab (LSDA data, reached from FDEs) and .sframe (the newer
       SFrame unwind format) round out the group. */
    .eh_frame_hdr ALIGN(8)    : { KEEP(*(.eh_frame_hdr)) *(.eh_frame_entry .eh_frame_entry.*) }
    .eh_frame ALIGN(8)        : { KEEP(*(.eh_frame)) *(.eh_frame.*) }
    .gcc_except_table ALIGN(4) : { KEEP(*(.gcc_except_table)) *(.gcc_except_table.*) }
    .gnu_extab               : { *(.gnu_extab*) }
    .sframe                  : { *(.sframe) *(.sframe.*) }

    /* --- Writable data + runtime-patched code --------------------
       .init/.fini: the ordinary musl crt convention (crti.o opens each,
       various objects contribute a middle fragment, crtn.o closes each
       with pop/ret). Left unlisted, GNU ld's orphan placement grouped
       them with .text (read+exec, no write) -- but this SDK's own
       object files mark .init/.fini WAX (write+alloc+exec), so .data
       (already writable) is where they belong.

       __unbox: dotnet NativeAOT's unboxing stubs -- genuinely both
       executable (SHF_EXECINSTR: merging them here makes ld warn "LOAD
       segment with RWX permissions") AND self-patched at run time with
       no mprotect() first (confirmed: instrumented sys_mprotect, zero
       calls before first use). A real W+X requirement. Merged into
       .data so the W+X region stays bounded to .data+__unbox (tens of
       KB) rather than the whole multi-hundred-KB .text. Needs the
       explicit __start___unbox/__stop___unbox below (ld only
       auto-synthesizes those for an output section of that exact bare
       name; libbootstrapper.o references them directly) and needs
       kernel/elf.c's bounded-W+X-PT_LOAD exception -- see
       docs/superpowers/plans/2026-09-07-hosted-neoos-gcc.md. */
    /* ALIGN(4K) on .got, not just .data: .got is the FIRST writable
       output section, so it is where the read-exec -> read-write
       segment boundary falls. Without a page boundary here bfd ld
       cannot start a fresh PT_LOAD for the writable data and instead
       widens the single LOAD segment to RWE -- putting the entire
       image, .text included, in a W+X mapping rather than just the
       bounded .data+__unbox region kernel/elf.c permits. */
    .got ALIGN(4K)    : { *(.got) *(.igot) }
    .got.plt          : { *(.got.plt) *(.igot.plt) }

    .data ALIGN(4K) :
    {
        /* .init/.fini are each assembled from MULTIPLE object files'
           contributions into ONE function per name, concatenated in
           link order. A combined `*(.init .fini)` does NOT do that:
           GNU ld walks input files in link order and for each file
           pulls sections in the NAME order given, interleaving one
           file's .init byte with its .fini byte, then the next file's
           -- neither byte stream is a valid function. Name them
           separately. And KEEP() each: --gc-sections (NativeAOT's own
           link line passes it) previously dropped crtn.o's anonymous
           one-byte closer (`pop %rax; ret`, nothing references it), so
           _init was a bare `push %rax` with no `ret` -- crt1 called it,
           fell through into following bytes, hit one decoding as `hlt`,
           ring-3 #GP. */
        KEEP(*(SORT_NONE(.init)))
        KEEP(*(SORT_NONE(.fini)))

        *(.data.rel.ro.local* .gnu.linkonce.d.rel.ro.local.*)
        *(.data.rel.ro .data.rel.ro.* .gnu.linkonce.d.rel.ro.*)
        *(.data .data.* .gnu.linkonce.d.*)
        *(.data1)
        SORT(CONSTRUCTORS)

        /* C++ global constructors/destructors. Left unlisted,
           --gc-sections dropped the ENTIRE .init_array/.fini_array
           input sections (nothing in the kept graph relocates to them),
           so __init_array_start/__init_array_end were never defined.
           musl's __libc_start_main declares both `weak hidden` -- an
           undefined weak symbol resolves to 0, not a link error -- so
           this failed SILENTLY: start=end=0, zero loop iterations, no
           diagnostic. dotnet NativeAOT's WorkstationGC then deadlocked
           on its first lock: the C++ ctor that sets
           global_region_allocator's GCDebugSpinLock to unlocked (-1)
           never ran, so the lock sat at raw zero -- which that lock
           reads as "held".

           .ctors/.dtors (legacy, pre-.init_array) and .jcr are KEEP()d
           for the same reason the stock script KEEP()s them: a crt or
           object built by an older/differently-configured compiler
           puts its constructors there instead, and --gc-sections would
           prune them. Empty in current builds, cheap insurance. */
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array))
        PROVIDE_HIDDEN(__preinit_array_end = .);

        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*) SORT_BY_INIT_PRIORITY(.ctors.*)))
        KEEP(*(.init_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .ctors))
        PROVIDE_HIDDEN(__init_array_end = .);

        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT_BY_INIT_PRIORITY(.fini_array.*) SORT_BY_INIT_PRIORITY(.dtors.*)))
        KEEP(*(.fini_array EXCLUDE_FILE (*crtbegin.o *crtbegin?.o *crtend.o *crtend?.o) .dtors))
        PROVIDE_HIDDEN(__fini_array_end = .);

        KEEP(*(.jcr))

        . = ALIGN(4K);
        PROVIDE(__start___unbox = .);
        KEEP(*(__unbox))
        PROVIDE(__stop___unbox = .);
    }

    /* .tdata and .tbss MUST be named here, and in this order. Left as
       orphan sections the linker placed both at the SAME address, so
       every thread-local in .tdata overlapped one in .tbss and the
       compiler-emitted offsets bore no relation to where the
       initialised bytes were. Symptom: a __thread int initialised to
       0x5a5a read as 0.

       .tdata first because PT_TLS's file-backed part is its leading
       bytes: p_offset points at .tdata, p_filesz covers only it, .tbss
       makes up the rest of p_memsz as zero. */
    .tdata ALIGN(4K) : { *(.tdata .tdata.* .gnu.linkonce.td.*) }
    .tbss : { *(.tbss .tbss.* .gnu.linkonce.tb.*) *(.tcommon) }

    .bss ALIGN(4K) :
    {
        *(.dynbss)
        *(.bss .bss.* .gnu.linkonce.b.*)
        *(COMMON)
    }

    /* Debug info, .comment, stabs, .gnu_debuglink etc. are non-alloc:
       ld keeps them out of every PT_LOAD on its own and orphan-places
       them at file offset only. Anything unexpected but allocatable is
       still orphan-placed (never silently dropped) -- but if you are
       adding a section here, copy how the stock script
       (`x86_64-neoos-linux-musl-ld --verbose`) has it rather than
       guessing. */
}
