; kernel/syscall_entry.asm — SYSCALL entry point (target of LSTAR).
;
; Entered with: RCX = user RIP (return address), R11 = user RFLAGS,
; RAX = syscall number, RDI/RSI/RDX/R10 = args 1-4. CS/SS are already
; switched to kernel selectors (via STAR); RSP is UNCHANGED (still the
; user stack) -- SYSCALL never switches stacks automatically.

extern syscall_dispatch

; Per-CPU block offsets -- must match kernel/cpu_local.h's CPU_*
; defines, which are _Static_assert'd against struct cpu's layout.
; Nothing checks THESE against that header, so keep them in sync by eye.
CPU_USER_RSP equ 32
CPU_KSTACK   equ 40

section .text
[bits 64]
global syscall_entry

syscall_entry:
    ; SYSCALL leaves RSP on the USER stack. swapgs brings this CPU's
    ; per-CPU block into GS (userland's GS value goes to
    ; IA32_KERNEL_GS_BASE), giving us a scratch slot and the kernel
    ; stack pointer without clobbering any register the caller owns.
    ; kernel_stack is kept up to date by the scheduler on every context
    ; switch (see process.c's schedule()), so it always names the right
    ; stack regardless of which task is running.
    swapgs
    mov [gs:CPU_USER_RSP], rsp
    mov rsp, [gs:CPU_KSTACK]

    push qword [gs:CPU_USER_RSP] ; user RSP
    push rcx                           ; user RIP
    push r11                           ; user RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    ; The register shuffle below and syscall_dispatch itself (an
    ; ordinary C function, free to clobber any SysV caller-saved
    ; register) will destroy the original argument registers. The only
    ; register a syscall is supposed to change from the caller's
    ; perspective is RAX (the return value) -- every userland syscall
    ; wrapper's clobber list (rcx, r11 only) relies on that convention,
    ; so save and restore the rest here rather than changing every
    ; wrapper's clobber list to document a leakier contract.
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; Safe now that we're on the kernel stack with everything saved --
    ; keeps the system preemptible during (potentially long) syscall
    ; processing, mirroring SFMASK's guarantee that only the brief
    ; stack-swap above ran with interrupts off.
    sti

    ; Reorder incoming syscall args (rax=num, rdi=a1, rsi=a2, rdx=a3,
    ; r10=a4) into SysV call registers for syscall_dispatch (rdi=num,
    ; rsi=a1, rdx=a2, rcx=a3, r8=a4). Safe to clobber rdi/rsi/rdx/r10
    ; here despite just having pushed their original values above --
    ; those pushes preserved copies on the stack; the registers
    ; themselves are free to reuse until the pops below.
    mov r9, rax
    mov rax, r10
    mov r10, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, r9
    mov rcx, r10
    mov r8, rax
    mov r9, rsp   ; base of the saved-register block -- syscall_dispatch's 6th argument

    call syscall_dispatch

    cli   ; mask again before restoring user state, mirroring SFMASK's entry guarantee
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11
    pop rcx
    pop qword [gs:CPU_USER_RSP]
    mov rsp, [gs:CPU_USER_RSP]
    swapgs

    o64 sysret
