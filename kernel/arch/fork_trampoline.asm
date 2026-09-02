; kernel/fork_trampoline.asm — bootstraps a fork()'d child's very
; first entry into ring 3, resuming mid-program instead of at a fresh
; entry point (contrast kernel_thread_trampoline in context_switch.asm,
; used by spawn() for brand-new processes). Reached via a bare `ret`
; out of context_switch (see fork_task()'s initial stack setup in
; process.c) -- never called directly. The values it pops were planted
; on the child's kernel stack by fork_task(), copied from the parent's
; saved syscall frame at the moment of the fork() call.

section .text
[bits 64]
global fork_trampoline
; See the note in context_switch.asm: a brand-new thread never returns
; through context_switch, so the trampoline has to release the thread
; the CPU switched away from. Called BEFORE the pops below, since it
; clobbers the caller-saved registers they land in. `sub rsp, 8` is
; SysV alignment -- this label is entered with RSP ≡ 8 (mod 16).
extern sched_post_switch

fork_trampoline:
    sub rsp, 8
    call sched_post_switch
    add rsp, 8

    ; rbx/rbp/r12-r15 are NOT popped here -- context_switch's own
    ; epilogue (the `pop r15/r14/r13/r12/rbx/rbp` sequence right before
    ; its `ret`) already consumed those stack slots and restored the
    ; real registers before landing here, exactly as for
    ; kernel_thread_entry_trampoline. Only the slots fork_task() placed
    ; ABOVE this trampoline's own return-address slot remain on the
    ; stack at this point.

    ; fs_base first: fork_task plants it directly above the trampoline's
    ; own return-address slot. The other three are popped AFTER the
    ; WRMSR below, not before -- WRMSR takes the MSR number in ECX, and
    ; RCX is where the user RIP lives. Popping first and writing the MSR
    ; second destroyed it, and the child jumped to 0x00000000C0000100:
    ; the MSR NUMBER, executed as an address.
    pop r10             ; the child's thread pointer (IA32_FS_BASE)

    cli
    mov dx, 0x33        ; user data selector (RPL3)
    mov ds, dx
    mov es, dx
    mov fs, dx

    ; `mov fs, dx` ZEROED IA32_FS_BASE, exactly as the note below says
    ; `mov gs, ax` does to GS_BASE: writing a segment selector reloads
    ; the base from the descriptor, and the flat user data segment's
    ; base is 0. The scheduler had loaded this thread's FS base a few
    ; instructions earlier, and this threw it away.
    ;
    ; So a fork()'d child reached userland with NO thread pointer, in an
    ; address space where its TLS block was present and correct. Any C
    ; library reaching for thread-local storage died on its first
    ; instruction: musl's fork() calls __pthread_self() as its very
    ; first act in the child -- `mov %fs:0x0,%r10` -- which faulted on
    ; address 0. That is why a BusyBox PIPELINE segfaulted while
    ; `busybox echo external` did not: a pipeline forks WITHOUT exec,
    ; and exec is what would otherwise have built a fresh thread
    ; pointer.
    ;
    ; Done inline rather than by calling a C helper. Calling C from here
    ; is more delicate than it looks: the user-state registers have to
    ; be saved around it, and the three pushes that did so left RSP 8
    ; out of SysV alignment -- which the compiler's aligned SSE stores
    ; turned into a #GP storm inside spin_lock_irqsave. WRMSR needs no
    ; stack at all.
    mov rax, r10
    mov rdx, r10
    shr rdx, 32
    mov ecx, 0xC0000100 ; IA32_FS_BASE
    wrmsr

    pop rcx             ; user RIP (parent's, at the point it called fork())
    pop r11             ; user RFLAGS
    pop rsi             ; user RSP

    xor eax, eax        ; fork() returns 0 in the child

    mov dx, 0x33        ; RDX was clobbered by the WRMSR above
    ; Hand GS over to userland. Ordering is load-bearing twice over:
    ;   - `mov gs, ax` ZEROES GS_BASE as a side effect, so it must come
    ;     AFTER the swapgs, or the per-CPU block pointer is destroyed
    ;     before it can be parked in IA32_KERNEL_GS_BASE.
    ;   - interrupts must be off across the swapgs..iretq window: GS
    ;     already holds userland's value there, but the CPU is still at
    ;     CPL0, so isr_common_stub's conditional swapgs would (rightly)
    ;     decline to swap it back. iretq restores IF from the pushed
    ;     RFLAGS below.
    swapgs              ; GS_BASE <- userland's 0, KERNEL_GS_BASE <- per-CPU
    mov gs, dx

    push 0x33           ; SS
    push rsi            ; RSP (user stack)
    push r11            ; RFLAGS
    push 0x3B           ; CS (user code64, RPL3)
    push rcx            ; RIP
    iretq
