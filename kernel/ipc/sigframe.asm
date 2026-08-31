; kernel/sigframe.asm -- returns to user mode with a fully arbitrary
; register state, which rt_sigreturn requires and the ordinary sysret
; epilogue cannot provide: sysretq takes RIP from RCX and RFLAGS from
; R11 and refuses a non-canonical RIP, so it cannot faithfully restore a
; context that was interrupted by preemption rather than by a syscall.

section .text
[bits 64]
global sigreturn_to_user

; void sigreturn_to_user(struct iret_ctx *ctx) -- never returns.
; struct iret_ctx is laid out exactly in pop order, ending with the
; five qwords iretq consumes. Its field order and this pop sequence must
; match EXACTLY; nothing checks that but the reader's eyes.
sigreturn_to_user:
    mov rsp, rdi
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
    ; [rsp] now holds RIP, CS, RFLAGS, RSP, SS -- iretq's own frame.
    ; GS still names the per-CPU block; hand it back to userland first.
    swapgs
    iretq
