# NeoOS Kernel

An x86_64 operating system kernel written from scratch in C and
assembly. Multiboot2 boot to SMP, virtual memory, networking, a
graphical stack, and a Linux-shaped syscall ABI — roughly 44,000 lines,
no third-party kernel code.

It runs BusyBox, Doom, curl over TLS, .NET applications including
ASP.NET Core, and a compositing window system.

## Highlights

**Linux-shaped ABI.** Struct layouts, flag values, errno numbers and
edge-case semantics match Linux x86-64 wherever a program can observe
them. NeoOS keeps its own syscall numbers; a thin shim in musl's arch
directory maps Linux's onto them. Every deliberate divergence is
recorded in [`docs/stdlib.md`](docs/stdlib.md).

**Dynamic linking.** `PT_INTERP`, `ET_DYN`, and `dlopen` — the kernel
loads the interpreter and describes both objects in the auxiliary
vector; musl's `ld.so` does all relocation. C++ shared objects work,
including exceptions thrown across library boundaries, RTTI, TLS and
static constructors. Executables link at the customary `0x400000`.

**SMP and an EEVDF scheduler.** Per-CPU run queues on an augmented
red-black tree, weighted fairness with Linux's nice-to-weight table,
work stealing, IPI-based rescheduling and TLB shootdown.

**Virtual memory.** Demand paging, copy-on-write `fork`, `mmap` of
anonymous memory, files and devices, and enforced W^X — no mapping is
ever both writable and executable.

**Networking.** A TCP/IP stack over virtio-net: ARP, ICMP, DHCP, DNS,
UDP and TCP with retransmission and reassembly, reached through BSD
sockets with `poll`, `select` and `epoll` (including edge-triggered).

**Filesystems.** A VFS over FAT16, ramfs, devfs, procfs and embedfs, on
a block cache with a polled PIO ATA driver.

**IPC.** Pipes, AF_UNIX sockets with `SCM_RIGHTS` descriptor passing,
`memfd`, POSIX signals, futexes and eventfd.

**Graphics and input.** A linear framebuffer exposing the Linux fbdev
ABI, six virtual terminals, a PS/2 keyboard and mouse behind evdev, and
AC97 audio.

**138 syscalls**, verified by a regression suite that boots the kernel
under QEMU and asserts against its serial output.

## Building

Requires an `x86_64-elf` cross toolchain, `nasm`, `grub-mkrescue`,
`mtools`, and `qemu-system-x86_64`.

The four boot-critical userland programs (`init`, `login`, `term`,
`nsh`) are linked into the kernel image, so a build needs
`neoos-libneoos` and `neoos-musl` alongside this repository:

```sh
git clone https://github.com/NeoOSOrganization/neoos-libneoos ../neoos-libneoos
(cd ../neoos-libneoos && make)

git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
(cd ../neoos-musl && make KERNEL_SHIM_DIR=$(pwd)/third_party/shim)

make LIBNEOOS_DIR=../neoos-libneoos/build-output \
     MUSL_DIR=../neoos-musl/build-output test
```

| target | what it does |
|---|---|
| `make iso` | build the bootable ISO |
| `make run` | boot in QEMU with a display |
| `make test` | run the regression suite headless |
| `make shell` | boot to an interactive shell |

`make test` on its own runs the kernel's internal selftests. For full
coverage, point `EMBED_DIRS` at directories of `<name>.nex` +
`<name>.test.json` pairs:

```sh
make LIBNEOOS_DIR=../neoos-libneoos/build-output \
     MUSL_DIR=../neoos-musl/build-output \
     EMBED_DIRS="../neoos-kernel-tests-common/build ../neoos-busybox/build" test
```

## Documentation

| | |
|---|---|
| [`BUILD.md`](BUILD.md) | build guide |
| [`docs/stdlib.md`](docs/stdlib.md) | the C library surface and every deliberate divergence from POSIX and Linux |
| [`docs/abi-compatibility.md`](docs/abi-compatibility.md) | what of the Linux ABI is implemented, stubbed, or missing |
| [`docs/superpowers/specs/`](docs/superpowers/specs/) | design specifications, one per milestone |

## Repositories

| | |
|---|---|
| [neoos-musl](https://github.com/NeoOSOrganization/neoos-musl) | musl libc with the NeoOS syscall shim |
| [neoos-libneoos](https://github.com/NeoOSOrganization/neoos-libneoos) | native libc alternative to musl |
| [neoos-kernel-tests-common](https://github.com/NeoOSOrganization/neoos-kernel-tests-common) | the regression suite |
| [neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder) | assembles bootable images from the kernel and a chosen set of ports |
| [neoos-hosted-gcc](https://github.com/NeoOSOrganization/neoos-hosted-gcc) | hosted `x86_64-neoos-linux-musl` GCC toolchain |
| [neoos-lvgl](https://github.com/NeoOSOrganization/neoos-lvgl) | LVGL with C# bindings |
| [neoos-tinygl](https://github.com/NeoOSOrganization/neoos-tinygl) | software OpenGL to the framebuffer |
| [neoos-busybox](https://github.com/NeoOSOrganization/neoos-busybox) · [neoos-doom](https://github.com/NeoOSOrganization/neoos-doom) · [neoos-curl](https://github.com/NeoOSOrganization/neoos-curl) · [neoos-openssl](https://github.com/NeoOSOrganization/neoos-openssl) · [neoos-libssh2](https://github.com/NeoOSOrganization/neoos-libssh2) | ports |

## License

Not yet chosen.
