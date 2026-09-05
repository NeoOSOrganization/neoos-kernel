#!/bin/sh
# Installs the NeoOS shim into the musl submodule.
#
# The submodule is meant to stay pristine so `git diff` against upstream
# stays meaningful (see third_party/musl-README.md). This script is the
# ONLY thing that modifies it, it is idempotent, and third_party/shim/
# holds the real sources -- musl gets copies, with the originals kept
# alongside as *.orig so the shim can be backed out.
#
# Two kinds of file are replaced:
#
#   syscall_arch.h        every C-level syscall, funnelled through
#                         __neoos_syscall for number translation and
#                         argument reshaping
#   the five .s files     musl's hand-written assembly, each of which
#                         issues `syscall` ITSELF and would otherwise
#                         walk straight past the funnel with a Linux
#                         number that means something else entirely to
#                         NeoOS
set -e
here=$(cd "$(dirname "$0")" && pwd)
# Defaults to the monorepo layout (shim and musl as siblings under
# third_party/); the neoos-musl repo, where musl lives in its own repo
# instead, overrides this to point at its checkout instead.
musl="${MUSL_DIR:-$here/../musl}"

[ -d "$musl/arch/x86_64" ] || {
    echo "musl submodule missing: run 'git submodule update --init --recursive'" >&2
    exit 1
}

install() {
    src="$1"; dst="$musl/$2"
    [ -f "$dst.orig" ] || cp "$dst" "$dst.orig"
    cp "$src" "$dst"
}

install "$here/syscall_arch.h"      arch/x86_64/syscall_arch.h
install "$here/syscall_cp.s"        src/thread/x86_64/syscall_cp.s
install "$here/__set_thread_area.s" src/thread/x86_64/__set_thread_area.s
install "$here/__unmapself.s"       src/thread/x86_64/__unmapself.s
install "$here/clone.s"             src/thread/x86_64/clone.s
install "$here/restore.s"           src/signal/x86_64/restore.s
install "$here/vfork.s"             src/process/x86_64/vfork.s

# Not a replacement: a new file, so no .orig to keep.
cp "$here/neoos_syscall.c" "$musl/src/internal/neoos_syscall.c"

echo "shim installed into $musl"
