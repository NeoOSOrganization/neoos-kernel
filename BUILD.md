# Building NeoOS Kernel

## Prerequisites

Install cross-compiler and tools:
```sh
# Debian/Ubuntu
sudo apt-get install nasm grub-common grub-mkrescue mtools qemu-system-x86_64 xorriso

# macOS
brew install nasm grub mtools qemu
```

Build the toolchain:
```sh
./toolchain/build.sh  # Creates x86_64-elf-gcc, x86_64-elf-ld, etc.
```

## Build Targets

```makefile
make              # Build kernel to build/neoos.bin
make iso          # Create ISO at build/neoos.iso
make disk-image   # Create disk images at build/disk1.img, build/disk2.img
make test         # Run regression suite headless in QEMU
make run          # Boot in QEMU with display (for development)
make clean        # Remove build/
```

## Musl Dependency

The kernel depends on compiled musl from `neoos-musl`. Clone and build that first:

```bash
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
cd ../neoos-musl && make
cd ../neoos-kernel
make MUSL_DIR=../neoos-musl/build-output
```

The kernel's own `Makefile` uses `MUSL_DIR` as a plain assignment
(`MUSL_DIR := third_party/musl` by default), but a command-line
override always wins in GNU Make regardless of `:=` vs `?=`, so the
above just works without editing the Makefile.

## Troubleshooting

- **"x86_64-elf-gcc: command not found"** → Run `./toolchain/build.sh` and add to PATH
- **"nasm: not found"** → Install nasm via package manager
- **QEMU hangs** → Add `timeout` before the qemu command
