#ifndef NEOOS_SYS_WAIT_H
#define NEOOS_SYS_WAIT_H

#define WNOHANG    1
#define WUNTRACED  2
#define WCONTINUED 8

// Linux-compatible status encoding, so musl's own macros agree with it.
#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) >> 8) & 0xff)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0xff) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WCOREDUMP(s)    ((s) & 0x80)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     (((s) >> 8) & 0xff)
#define WIFCONTINUED(s) ((s) == 0xffff)

// POSIX-shaped. NeoOS's own wait() (one pid, bare exit code) lives in
// <unistd.h> and is NOT replaced by this -- see docs/stdlib.md.
int wait4(int pid, int *status, int options, void *rusage);
int waitpid(int pid, int *status, int options);

int setpgid(int pid, int pgid);
int getpgid(int pid);
int setsid(void);
int getsid(int pid);

#endif
