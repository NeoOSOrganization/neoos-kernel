#ifndef NEOOS_PIPE_H
#define NEOOS_PIPE_H

#include <stdint.h>

// POSIX pipes: a bounded byte stream with a read end and a write end,
// each an ordinary file descriptor.
//
// Linux's capacity is 64KiB by default; NeoOS uses one 4KiB page, which
// is POSIX's guaranteed minimum (PIPE_BUF, 512 bytes) many times over
// and is what a single pmm_alloc hands back. The size is observable
// only as the point at which a writer blocks, and is recorded in
// docs/stdlib.md.
#define PIPE_CAPACITY 4096

struct file_descriptor;
struct file_ops;
struct poll_head;

// Creates a pipe and installs its two ends in the current process's fd
// table. Writes the read end to fds[0] and the write end to fds[1], as
// POSIX requires. Returns 0, or a negative errno.
//
// `flags` accepts O_NONBLOCK and O_CLOEXEC's Linux values; see
// pipe.c for which are honoured.
int pipe_create(int fds[2], int flags);

const struct file_ops *pipe_file_ops(void);

void pipe_selftest(void);

// ---- exposed for kernel/ipc/socketpair.c -----------------------------
//
// A socketpair end reads one pipe and writes a DIFFERENT one (its
// sibling end's outgoing pipe), unlike a plain pipe fd where both
// directions of the pair share a single struct pipe. socketpair.c
// therefore drives these directly, parameterized on which pipe and
// which direction(s), rather than going through a file_descriptor
// whose own `priv`/`readable`/`writable`/`nonblock` name only one
// pipe. `struct pipe` itself stays fully opaque; nothing outside
// pipe.c reaches into its fields.
struct pipe;

struct pipe *pipe_alloc(void);
void         pipe_free(struct pipe *p);
void         pipe_init_ends(struct pipe *p);
int64_t      pipe_read_ep(struct pipe *p, int nonblock, void *buf, uint64_t len);
int64_t      pipe_write_ep(struct pipe *p, int nonblock, const void *buf, uint64_t len);
void         pipe_dup_ep(struct pipe *p, int as_reader, int as_writer);
void         pipe_close_ep(struct pipe *p, int as_reader, int as_writer);
int          pipe_poll_ep(struct pipe *p, int as_reader, int as_writer, int events);

#endif
