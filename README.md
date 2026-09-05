# NeoOS Kernel

x86_64 operating system kernel built from scratch in C and assembly.

## Quick Start

You need:
- x86_64-elf cross-compiler toolchain
- nasm, grub-mkrescue, mtools
- qemu-system-x86_64

Build:
```sh
make            # Build kernel
make iso        # Create ISO
make test       # Run regression tests in QEMU
make run        # Boot in QEMU with display
```

## Documentation

- **Build guide:** See `BUILD.md`
- **Architecture:** See `docs/` directory
- **Development workflow:** See the organization README at https://github.com/NeoOSOrganization
- **ABI compatibility:** See `docs/abi-compatibility.md`

## Status

This repo is a **standalone kernel repository**. Developers can clone just this repo and run `make test` without any other dependencies (except the toolchain).

The companion `neoos-musl` repository provides the compiled musl libc. The OS image builder (`neoos-os-builder`) orchestrates kernel + ports.

## License

Not yet chosen.

## In This Organization

- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — Compiled musl libc (this kernel depends on it)
- **[neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder)** — Assemble custom OS images
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Architecture and development guides
- **[Port Template: neoos-busybox](https://github.com/NeoOSOrganization/neoos-busybox)** — Example of porting an application
