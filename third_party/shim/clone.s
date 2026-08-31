// NeoOS's replacement for musl's src/thread/x86_64/clone.s.
//
// NeoOS HAS NO clone. Upstream would issue `syscall` with Linux's
// number 56 -- which in NeoOS's own numbering is lstat. A pthread
// creation would have silently called lstat with a stack pointer as
// its path argument.
//
// Returning -ENOSYS is the honest answer, and it is what tells us the
// primitive belongs in the kernel rather than in a shim: musl's
// pthread_create will fail cleanly with EAGAIN/ENOSYS instead of
// corrupting something.

.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	mov $-38,%eax   /* -ENOSYS */
	ret
