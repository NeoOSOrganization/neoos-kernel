; kernel/context_switch.asm — minimal callee-saved context switch.
;
; void context_switch(uint64_t *old_rsp, uint64_t *new_rsp)
; System V: rdi = old_rsp, rsi = new_rsp
;
; Saves the outgoing task's callee-saved registers and RSP onto its
; own kernel stack, then loads the incoming task's saved RSP and pops
; its callee-saved registers. The final `ret` resumes execution
; wherever the incoming task last called context_switch from -- for a
; brand-new task, that's a fake frame set up by
; task_create_kernel_thread (or spawn(), from Task 5 on) rather than a
; real prior call.

section .text
[bits 64]
global context_switch

; sched_post_switch (sched.c) releases the thread the CPU switched away
; from and puts it back on a run queue. schedule() calls it itself on
; the far side of context_switch -- but a BRAND-NEW thread lands on a
; trampoline below instead and never reaches that code, so each
; trampoline calls it too. Without this the outgoing thread sits in
; cpus[].prev_pending, runnable nowhere, until that CPU next schedules
; -- a full time slice later at best, and never at all if the new thread
; goes to userland and stays there.
;
; The `sub rsp, 8` before each call is alignment, not a slot: every
; trampoline is entered with RSP ≡ 8 (mod 16), and the SysV ABI wants
; RSP ≡ 8 (mod 16) at the callee's first instruction, i.e. ≡ 0 at the
; `call`.
extern sched_post_switch

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; Bootstraps a brand-new kernel-mode task's very first run. Reached
; via a bare `ret` out of context_switch (see task_create_kernel_thread's
; initial stack setup in process.c), never called directly.
;
; A brand-new task's fake initial frame makes context_switch's `ret`
; jump straight into C code, with no iretq in between -- but iretq is
; the ONLY thing that normally restores RFLAGS (including IF) when
; resuming a task. If this task's first scheduling-in happened from
; inside an interrupt handler (timer preemption of some other task),
; the CPU's IF flag is 0 at that point (cleared by the interrupt-gate
; entry) and would stay 0 forever once this task starts running
; normally, permanently masking the timer. Explicitly re-enabling
; interrupts here, before running the real entry point, fixes that.
global kernel_thread_entry_trampoline

kernel_thread_entry_trampoline:
    sub rsp, 8
    call sched_post_switch
    add rsp, 8

    pop rax   ; entry function pointer, planted by task_create_kernel_thread
    sti
    call rax
.hang:        ; entry should never return, but halt safely if it does
    hlt
    jmp .hang

; Bootstraps a brand-new task's very first entry into ring 3. Reached
; via a bare `ret` out of context_switch (see spawn()'s initial stack
; setup in process.c) -- never called directly. The two values it
; pops were planted on the stack by spawn(), right below the
; trampoline's own "return address" slot.
global kernel_thread_trampoline

kernel_thread_trampoline:
    sub rsp, 8
    call sched_post_switch
    add rsp, 8

    pop rdi   ; entry_rip, planted by spawn()/thread_create()
    pop rsi   ; user_rsp,  planted by spawn()/thread_create()
    pop rdx   ; arg -- becomes the user entry point's SysV first
              ; argument. spawn() plants 0 here so both callers share
              ; one stack layout.

    cli
    mov ax, 0x33        ; user data selector (RPL3)
    mov ds, ax
    mov es, ax
    mov fs, ax
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
    mov gs, ax

    push 0x33           ; SS
    push rsi            ; RSP (user stack)
    push 0x202          ; RFLAGS: reserved bit 1 set, IF (bit 9) set
    push 0x3B           ; CS (user code64, RPL3)
    push rdi            ; RIP (entry point)
    mov rdi, rdx        ; SysV arg 1 for the entry point
    iretq
