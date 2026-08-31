// NeoOS's replacement for musl's src/thread/x86_64/__unmapself.s.
//
// Like the signal restorer, this cannot touch the stack -- it is in
// the middle of unmapping the very stack it runs on -- so the numbers
// are translated inline instead of going through the funnel.
//
// NeoOS: munmap is 38, exit is 0. Linux: 11 and 60.

.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	movl $38,%eax   /* NeoOS munmap(arg2, arg3) */
	syscall
	xor %rdi,%rdi   /* always report success */
	movl $0,%eax    /* NeoOS exit */
	syscall
