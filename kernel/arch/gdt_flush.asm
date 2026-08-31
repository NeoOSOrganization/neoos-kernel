; kernel/gdt_flush.asm — loads a new GDTR, reloads the data segment
; registers, reloads CS via a far return (required in 64-bit mode —
; CS cannot be loaded with a plain mov), then loads the TSS selector.

section .text
[bits 64]
global gdt_flush

; void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
;                uint16_t code_selector, uint16_t tss_selector)
; System V AMD64: rdi=gdtr_ptr, rsi=data_selector, rdx=code_selector, rcx=tss_selector
gdt_flush:
    lgdt [rdi]

    mov ax, si
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push rdx
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ltr cx
    ret
