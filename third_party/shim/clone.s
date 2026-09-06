// NeoOS's clone.s. Was a deliberate -ENOSYS stub (see git history) --
// upstream's raw assembly is otherwise EXACTLY what NeoOS needs, since
// it already builds the raw Linux clone(2) register convention
// (rdi=flags, rsi=stack, rdx=ptid, r10=ctid, r8=tls) that NeoOS's own
// native syscall convention uses too (see kernel/sched/thread.c's
// clone_task and kernel/syscall/sys_proc.c's sys_clone). The ONLY two
// things that differ from upstream are the syscall NUMBERS, both of
// which collide with unrelated NeoOS syscalls under Linux's numbering:
//   - clone itself: Linux 56 is NeoOS's lstat. Use SYS_CLONE (92).
//   - the child's post-return exit call: Linux exit(2)=60 is NeoOS's
//     SYS_EXIT_GROUP (60) -- which would kill the WHOLE PROCESS, not
//     just this thread, the moment any pthread's start function
//     returned normally. Use SYS_THREAD_EXIT (18) instead, which is
//     what actually ends one thread on NeoOS. %edi already holds the
//     start function's return value at this point (mov %eax,%edi,
//     below), which is exactly SYS_THREAD_EXIT's one argument.
//
// See docs/superpowers/specs/2026-09-07-clone-pthread-design.md and
// docs/superpowers/plans/2026-09-07-clone-pthread.md.

.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	xor %eax,%eax
	mov $92,%al             /* NeoOS SYS_CLONE, not Linux's 56 */
	mov %rdi,%r11
	mov %rdx,%rdi
	mov %r8,%rdx
	mov %r9,%r8
	mov 8(%rsp),%r10
	mov %r11,%r9
	and $-16,%rsi
	sub $8,%rsi
	mov %rcx,(%rsi)
	syscall
	test %eax,%eax
	jnz 1f
	xor %ebp,%ebp
	pop %rdi
	call *%r9
	mov %eax,%edi
	xor %eax,%eax
	mov $18,%al             /* NeoOS SYS_THREAD_EXIT, not Linux exit's 60 */
	syscall
	hlt
1:	ret
