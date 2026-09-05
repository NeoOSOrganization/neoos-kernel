# Building NeoOS Kernel

## Prerequisites

Install tools:
```sh
# Debian/Ubuntu
sudo apt-get install nasm grub-common grub-mkrescue mtools qemu-system-x86_64 xorriso

# macOS
brew install nasm grub mtools qemu
```

Build the cross-toolchain (binutils + gcc, no libc — ~20 minutes,
one-time):
```sh
./toolchain/build.sh   # installs to ~/opt/cross-x86_64-elf by default
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
```

## Build Targets

```makefile
make              # Build kernel to build/kernel.elf
make iso          # Create ISO at build/neoos.iso
make disk-image   # Create disk images at build/disk.img, build/disk2.img
make test         # Run regression suite headless in QEMU
make run          # Boot in QEMU with display (for development)
make clean-kernel # Remove build/*.o (keeps build/embedfs-obj/ — see below)
```

## Dependencies: libneoos, musl, and the regression suite

Boot-critical apps (init/login/term/nsh) are linked directly into the
kernel image (`embedfs`) at build time, so a build needs `neoos-libneoos`
(native ABI) and `neoos-musl` (login's musl link) built first:

```bash
git clone https://github.com/NeoOSOrganization/neoos-libneoos ../neoos-libneoos
(cd ../neoos-libneoos && make)

git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
(cd ../neoos-musl && make KERNEL_SHIM_DIR=$(pwd)/third_party/shim)

make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
```

`make test` alone (as above) runs a **reduced** regression set — no
test-suite or port dependency. For full coverage (what BusyBox and the
regression suite exercise at boot), point `EMBED_DIRS` at one or more
directories of `<name>.nex` + `<name>.test.json` pairs:

```bash
git clone https://github.com/NeoOSOrganization/neoos-kernel-tests-common ../neoos-kernel-tests-common
(cd ../neoos-kernel-tests-common && make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output)

make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS=../neoos-kernel-tests-common/build test
```

Both `LIBNEOOS_DIR` and `MUSL_DIR` are `?=` defaults
(`../neoos-libneoos/build-output` / `../neoos-musl/build-output`), so
a command-line override always wins.

## `clean-kernel` and `embedfs-obj/`

`clean-kernel` deletes `build/*.o` but deliberately excludes
`build/embedfs-obj/` — that directory is a cache `tools/gen-embedfs.py`
manages itself (the link step reaches it via a shell command
substitution invisible to Make's dependency graph), and sweeping it
without something forcing a regeneration is a straight link failure,
not a "gets rebuilt anyway." `make clean` removes everything, including
`embedfs-obj/`, for a genuinely fresh tree.

## Troubleshooting

- **"x86_64-elf-gcc: command not found"** → Run `./toolchain/build.sh` and add `~/opt/cross-x86_64-elf/bin` to `PATH`
- **"nasm: not found"** → Install nasm via package manager
- **"error: .../lib/crt0.o not found"** → Build `neoos-libneoos` first (or `neoos-musl`, for the login error)
- **QEMU hangs** → Add `timeout` before the qemu command; check for a lingering `qemu-system-x86_64` holding the disk image's write lock
