#!/bin/bash
# Stamp a NeoOS executable: copy an ELF and rewrite its magic to NOX.
#
#   tools/nexify.sh <in.elf> <out.nex>
#
# The magic becomes 0x7F 'N' 'O' 'X' -- ELF's shape, one non-printable
# guard byte then three characters. Everything from e_ident[4] on is
# untouched ELF64, so a .nex file IS an ELF file with four bytes changed.
#
# COPIES rather than stamping in place, deliberately. objdump, readelf,
# nm and gdb all refuse a file whose magic they do not recognise, and
# disassembling a userland binary is how the fork/TLS bug in the BusyBox
# track was actually found. The build keeps a valid ELF in build/ and
# stamps only what goes onto the disk image.
set -e

in="$1"
out="$2"
[ -n "$in" ] && [ -n "$out" ] || { echo "usage: nexify.sh <in> <out>" >&2; exit 1; }
[ -f "$in" ] || { echo "nexify: no such file: $in" >&2; exit 1; }

cp "$in" "$out"
printf 'NOX' | dd of="$out" bs=1 seek=1 conv=notrunc status=none
