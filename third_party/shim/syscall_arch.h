// NeoOS's replacement for musl's arch/x86_64/syscall_arch.h.
//
// Upstream issues `syscall` directly with Linux's number. NeoOS's
// numbers are its own, and some of its calls take differently shaped
// arguments, so every syscall is funnelled through one translator
// instead -- see third_party/shim/neoos_syscall.c.
//
// The funnel is deliberate: one place that knows both numbering
// schemes, so a call that has no NeoOS equivalent returns -ENOSYS
// loudly rather than landing on whatever NeoOS call happens to share
// Linux's number.

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __neoos_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

static __inline long __syscall0(long n)
{
	return __neoos_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __neoos_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __neoos_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __neoos_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __neoos_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __neoos_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __neoos_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define VDSO_USEFUL
#define VDSO_CGT_SYM "__vdso_clock_gettime"
#define VDSO_CGT_VER "LINUX_2.6"
#define VDSO_GETCPU_SYM "__vdso_getcpu"
#define VDSO_GETCPU_VER "LINUX_2.6"

#define IPC_64 0
