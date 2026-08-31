; kernel/isr.asm — one stub per IDT vector (0-255) plus a shared
; trampoline that builds a uniform register frame and calls into C.
;
; Vectors 8, 10, 11, 12, 13, 14, 17 have a CPU-pushed error code;
; every other vector (including all IRQs, which never have one) gets
; a fake zero pushed so every vector produces the same stack layout.
;
; In long mode the CPU always pushes SS and RSP, at every privilege
; level, so the frame layout is uniform. Entries from ring 3 need
; swapgs; entries from ring 0 must NOT have it, or GS would end up
; holding userland's value inside the kernel -- see isr_common_stub.

extern isr_handler

section .text
[bits 64]

%macro ISR_NOERR 1
isr%1:
    push 0
    push %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
isr%1:
    push %1
    jmp isr_common_stub
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

%assign i 32
%rep 224
ISR_NOERR i
%assign i i+1
%endrep

isr_common_stub:
    ; [rsp]=vector [rsp+8]=error_code [rsp+16]=RIP [rsp+24]=CS.
    ; CPL is CS[1:0]; 3 means the interrupt came from user mode and GS
    ; must be swapped. Swapping unconditionally would point GS at
    ; userland's value for interrupts taken in the kernel.
    test byte [rsp+24], 3
    jz .no_swapgs_in
    swapgs
.no_swapgs_in:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp
    call isr_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; drop vector_number + error_code
    ; [rsp]=RIP [rsp+8]=CS now that the vector/error pair is gone.
    test byte [rsp+8], 3
    jz .no_swapgs_out
    swapgs
.no_swapgs_out:
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 256
    dq isr%+i
%assign i i+1
%endrep
