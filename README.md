# NeoOS Kernel

x86_64 operating system kernel built from scratch in C and assembly.

## Quick Start

You need:
- x86_64-elf cross-compiler toolchain
- nasm, grub-mkrescue, mtools
- qemu-system-x86_64

Build:
```sh
make            # Build kernel (needs LIBNEOOS_DIR, below)
make iso        # Create ISO
make test       # Run regression tests in QEMU
make run        # Boot in QEMU with display
```

The four boot-critical userland programs (init/login/term/nsh) are
linked directly into the kernel image (`embedfs`, a read-only
filesystem serving blobs baked in at build time — see
`docs/superpowers/specs/2026-09-05-embedded-test-and-app-architecture.md`),
so a build needs `neoos-libneoos` (native ABI) and `neoos-musl`
(login's musl link) nearby:

```sh
git clone https://github.com/NeoOSOrganization/neoos-libneoos ../neoos-libneoos
(cd ../neoos-libneoos && make)
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
(cd ../neoos-musl && make KERNEL_SHIM_DIR=$(pwd)/third_party/shim)
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
```

`make test` alone (no `EMBED_DIRS`) runs a reduced regression set —
just the kernel-internal selftests, no port or test-suite dependency.
For full coverage, point `EMBED_DIRS` at one or more directories of
`<name>.nex` + `<name>.test.json` pairs (`neoos-kernel-tests-common`'s
`build/`, a port's `build/`):

```sh
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="../neoos-kernel-tests-common/build ../neoos-busybox/build" test
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
- **[neoos-libneoos](https://github.com/NeoOSOrganization/neoos-libneoos)** — NeoOS-native libc alternative to musl (this kernel depends on it)
- **[neoos-kernel-tests-common](https://github.com/NeoOSOrganization/neoos-kernel-tests-common)** — The regression suite, embedded via `EMBED_DIRS`
- **[neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder)** — Assemble custom OS images
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Architecture and development guides
- **[Port Template: neoos-busybox](https://github.com/NeoOSOrganization/neoos-busybox)** — Example of porting an application
