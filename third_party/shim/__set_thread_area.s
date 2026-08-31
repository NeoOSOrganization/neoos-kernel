// NeoOS's replacement for musl's src/thread/x86_64/__set_thread_area.s.
//
// Upstream issues `syscall` with Linux's arch_prctl number (158)
// directly. NeoOS's highest syscall number is far below that, so the
// call returned -ENOSYS, __init_tp returned -1, and __init_tls hit its
// one a_crash() -- a `hlt` in ring 3, which NeoOS reports as SIGSEGV.
//
// That was the entire reason a musl binary died before main: the
// thread pointer could never be installed.
//
// This one goes through the funnel, because unlike the restorer and
// __unmapself below it is an ordinary call with an ordinary stack.

.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	push %rbp
	mov %rsp,%rbp
	// __neoos_syscall(158 /*arch_prctl*/, 0x1002 /*ARCH_SET_FS*/, p, 0,0,0,0)
	mov %rdi,%rdx          // a2 = p
	mov $0x1002,%esi       // a1 = ARCH_SET_FS
	mov $158,%edi          // n  = SYS_arch_prctl
	xor %ecx,%ecx          // a3
	xor %r8d,%r8d          // a4
	xor %r9d,%r9d          // a5
	sub $16,%rsp           // a6 on the stack, and keep %rsp 16-aligned
	movq $0,(%rsp)
	call __neoos_syscall
	leave
	ret
