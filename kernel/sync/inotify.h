#ifndef NEOOS_INOTIFY_H
#define NEOOS_INOTIFY_H

#include <stdint.h>

struct file_descriptor;

// inotify(7) -- a minimal, honest stub, not a lie: it hands back a
// real fd that behaves exactly like an inotify fd watching filesystems
// where nothing ever changes (poll/select/epoll never report it
// readable; a blocking read() never returns). NeoOS's filesystems
// have no change-notification machinery of their own to hook a real
// implementation into. Found missing (as inotify_init1, fatal --
// unlike most syscalls in docs/stdlib.md's "found missing" section,
// dotnet's config-file-watcher treats -ENOSYS as fatal rather than
// falling back) getting a real ASP.NET Core app (Kestrel) running:
// the generic host's configuration system creates one unconditionally
// to watch appsettings.json for changes, whether or not the app ever
// reloads config at runtime.
//
// DIVERGENCE: hot-reload of watched files never fires. Nothing on
// NeoOS depends on it yet; if something comes to, this is the file to
// extend -- most likely by having vfs.c's write path notify a
// registered watch list, the same shape as poll_head's own
// notify-on-change design.
extern const struct file_ops inotify_file_ops;

int inotify_create(int flags);
int inotify_add_watch_do(struct file_descriptor *f, uint32_t mask);
int inotify_rm_watch_do(struct file_descriptor *f, int wd);

#endif
