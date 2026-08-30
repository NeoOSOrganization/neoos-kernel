; kernel/ap_trampoline.asm -- application-processor entry.
;
; Copied to physical 0x8000 by smp_start_aps() and entered in 16-bit
; real mode by SIPI. Everything here is position-dependent on 0x8000:
; the SIPI vector IS the page number, so this code cannot be relocated
; without changing AP_BASE and AP_TRAMPOLINE_PHYS together.
;
; pmm never hands out memory below 0x100000 (kernel/mm/pmm.c), so this
; page cannot be allocated out from under us.
;
; This is boot/boot.asm's long-mode path with the page-table
; CONSTRUCTION removed: the low 4GiB identity map built at boot is
; permanent (kernel/mm/paging.c keeps PML4[0] because pmm dereferences
; through it), so the AP simply adopts the live p4_table.
;
; EVERY intra-trampoline reference uses AP_BASE + (label -
; ap_trampoline_start) rather than a link-time address, because the
; linker places this section in the higher half while the CPU is
; executing it at 0x8000.

AP_BASE equ 0x8000

section .ap_trampoline
[bits 16]
global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_stack
global ap_trampoline_index
global ap_trampoline_cr3
extern ap_main

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [AP_BASE + (ap_gdt_pointer - ap_trampoline_start)]

    mov eax, cr0
    or  eax, 1                  ; PE: enter 32-bit protected mode
    mov cr0, eax
    jmp dword 0x08:(AP_BASE + (ap_protected - ap_trampoline_start))

[bits 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Adopt the BSP's page tables rather than building our own.
    mov eax, [AP_BASE + (ap_trampoline_cr3 - ap_trampoline_start)]
    mov cr3, eax

    mov eax, cr4
    or  eax, 1 << 5             ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or  eax, 1 << 8             ; LME
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31            ; PG
    mov cr0, eax

    jmp 0x18:(AP_BASE + (ap_long - ap_trampoline_start))

[bits 64]
ap_long:
    mov rsp, [abs AP_BASE + (ap_trampoline_stack - ap_trampoline_start)]
    xor rdi, rdi
    mov edi, [abs AP_BASE + (ap_trampoline_index - ap_trampoline_start)]

    ; ap_main is linked in the higher half; a rel32 call cannot reach it,
    ; exactly as boot.asm documents for kmain.
    mov rax, ap_main
    call rax

.hang:
    cli
    hlt
    jmp .hang

align 8
; Patched by the BSP before each SIPI (stack, index) and once before the
; first (cr3).
ap_trampoline_stack: dq 0
ap_trampoline_index: dd 0
ap_trampoline_cr3:   dd 0

align 8
ap_gdt:
    dq 0
    ; 0x08: 32-bit code, base 0, limit 4G
    dq 0x00CF9A000000FFFF
    ; 0x10: 32-bit data, base 0, limit 4G
    dq 0x00CF92000000FFFF
    ; 0x18: 64-bit code
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
ap_gdt_pointer:
    dw ap_gdt_pointer - ap_gdt - 1
    dq AP_BASE + (ap_gdt - ap_trampoline_start)

ap_trampoline_end:
