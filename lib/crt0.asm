; lib/crt0.asm — C runtime startup. The ELF entry point
; (userland/user.ld's ENTRY(_start)); kernel_thread_trampoline's
; `iretq` lands here.
;
; Written in assembly, not C, because at this point there is no stack
; FRAME, only a stack: RSP points at argc, with argv, envp and the
; auxiliary vector laid out above it by the kernel's
; build_initial_stack(). A C function would emit a prologue that
; assumes a normal call chain and would lose the alignment the SysV ABI
; requires before the first `call`.
;
; The three arguments handed to __libc_start are the three regions of
; that block:
;
;     [rsp]            argc
;     [rsp+8]          argv[0] .. argv[argc-1], NULL
;     after that       envp[0] .. NULL
;     after that       auxv, pairs terminated by AT_NULL
;
; Finding envp and auxv means walking the NULL terminators, which is
; done here rather than in C only because the pointers are trivially
; derived from RSP and passing three registers is simpler than passing
; one and re-deriving them.

extern __libc_start

section .text
[bits 64]
global _start

_start:
    ; RSP is 16-byte aligned on entry and points at argc.
    mov rdi, [rsp]              ; argc
    lea rsi, [rsp + 8]          ; argv
    ; envp starts one past argv's NULL terminator: argv + (argc+1)*8.
    lea rdx, [rdi + 1]
    shl rdx, 3
    add rdx, rsi                ; envp

    ; auxv starts one past envp's NULL terminator. Walk it.
    mov rcx, rdx
.scan_env:
    cmp qword [rcx], 0
    je .found_auxv
    add rcx, 8
    jmp .scan_env
.found_auxv:
    add rcx, 8                  ; step over the NULL
                                ; rcx = auxv, the 4th argument
    ; SysV: RSP must be 16-byte aligned at the point of the `call`, so
    ; that the callee sees RSP+8 aligned after the return address is
    ; pushed. It already is -- _start has pushed nothing.
    call __libc_start
.hang:                          ; __libc_start never returns
    hlt
    jmp .hang
