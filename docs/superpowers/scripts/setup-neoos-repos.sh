#!/bin/bash
# Setup script: Initialize all NeoOSOrganization repos with documentation
# Run this locally after repos are created on GitHub

set -e

ORG="NeoOSOrganization"
REPOS_DIR="${1:-.}"  # Optional: specify directory to clone repos into

echo "Setting up NeoOSOrganization repositories..."
echo "Cloning into: $REPOS_DIR"

# Function to set up a repo
setup_repo() {
    local repo_name=$1
    local repo_url="git@github.com:${ORG}/${repo_name}.git"

    echo ""
    echo "=== Setting up $repo_name ==="

    if [ -d "$REPOS_DIR/$repo_name" ]; then
        echo "Repository already exists, skipping clone"
        cd "$REPOS_DIR/$repo_name"
    else
        cd "$REPOS_DIR"
        git clone "$repo_url"
        cd "$repo_name"
    fi

    # Reset to clean state
    git reset --hard HEAD 2>/dev/null || true
    git clean -fd

    return
}

# === KERNEL REPO ===
setup_repo "neoos-kernel"
cat > .gitignore << 'EOF'
# Build artifacts
build/
*.o
*.a
*.so
*.elf
*.bin
*.iso

# IDE
.idea/
.vscode/
*.swp
*.swo
*~

# OS-specific
.DS_Store
Thumbs.db

# CMake
cmake-build-debug/
cmake-build-release/
EOF

cat > README.md << 'EOF'
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

Not yet chosen. Vendored musl under `third_party/musl` is MIT.

## In This Organization

- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — Compiled musl libc (this kernel depends on it)
- **[neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder)** — Assemble custom OS images
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Architecture and development guides
- **[Port Template: neoos-busybox](https://github.com/NeoOSOrganization/neoos-busybox)** — Example of porting an application
EOF

cat > BUILD.md << 'EOF'
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

## Build Options

Override with environment variables:

```makefile
make CPU_FEATURES="avx2 sse4_2" OPTIMIZATION_LEVEL=O2
```

- `CPU_FEATURES`: Space-separated or "auto" (default)
- `OPTIMIZATION_LEVEL`: O0, O1, O2, O3 (default: O2)

## Headless Testing (CI)

```sh
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw \
  -display none -no-reboot -serial file:/tmp/neoos.log

# Check for failures
grep FAILED /tmp/neoos.log && exit 1
```

## Musl Dependency

The kernel depends on compiled musl from `neoos-musl`. Clone and build that first:

```bash
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
cd ../neoos-musl && make
cd ../neoos-kernel
make MUSL_DIR=../neoos-musl/build-output
```

Or set `MUSL_DIR` to point to a pre-built musl directory.

## Troubleshooting

- **"x86_64-elf-gcc: command not found"** → Run `./toolchain/build.sh` and add to PATH
- **"nasm: not found"** → Install nasm via package manager
- **QEMU hangs** → Add `timeout` before the qemu command

---

**See also:** `docs/superpowers/specs/2026-09-04-github-organization-design.md` for full design
EOF

git add .gitignore README.md BUILD.md
git commit -m "docs: initial kernel repo scaffolding"
git push -u origin main
echo "✅ neoos-kernel initialized"

# === MUSL REPO ===
setup_repo "neoos-musl"
cat > .gitignore << 'EOF'
build-output/
*.o
*.a
*.so

.idea/
.vscode/
*.swp

.DS_Store
Thumbs.db
EOF

cat > README.md << 'EOF'
# NeoOS musl libc

musl libc (1.2.5) compiled with NeoOS syscall shim integration.

## Quick Start

Build musl (requires neoos-kernel repo for the shim):

```sh
git clone https://github.com/NeoOSOrganization/neoos-kernel ../neoos-kernel
make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim
# Produces: build-output/include/ and build-output/lib/libc.a
```

## How It Works

The NeoOS shim (in neoos-kernel) translates Linux syscall numbers to NeoOS syscall numbers. This repo:

1. Clones the kernel repo to get the shim
2. Integrates the shim into musl's arch directory during build
3. Compiles musl with the integrated shim
4. Produces headers and static library for kernel and ports to link against

## Integration

Kernel and ports link against the compiled musl:

```makefile
MUSL_DIR ?= ../neoos-musl/build-output

CFLAGS += -I$(MUSL_DIR)/include
LDFLAGS += -L$(MUSL_DIR)/lib -lc
```

## Documentation

- **Build options:** See `BUILD.md`
- **Architecture:** See the spec at https://github.com/NeoOSOrganization/neoos-docs

## License

musl is MIT licensed. NeoOS shim integration is under the same license as NeoOS kernel.

## In This Organization

- **[neoos-kernel](https://github.com/NeoOSOrganization/neoos-kernel)** — Kernel that uses this libc
- **[neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder)** — OS image builder (uses kernel + musl + ports)
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Full documentation
EOF

cat > BUILD.md << 'EOF'
# Building musl with NeoOS Shim

## Prerequisites

- neoos-kernel repository (for the shim)
- x86_64-elf cross-compiler
- Standard build tools (make, autoconf, etc.)

## Build

```bash
make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim
```

### Environment Variables

- `KERNEL_SHIM_DIR`: Path to the NeoOS shim (default: `../neoos-kernel/third_party/shim`)
- `PREFIX`: Installation prefix (default: `build-output`)

## Output

After build:
- `build-output/include/` — musl headers with integrated NeoOS shim
- `build-output/lib/libc.a` — static library ready to link

## Verify

Check that the shim is integrated:

```bash
grep -l "NeoOS" build-output/include/sys/syscall.h
# Should find NeoOS references in syscall.h
```

## Clean

```bash
make clean  # Removes build-output/
```
EOF

cat > Makefile << 'EOF'
# NeoOS musl build
# Full implementation in Phase 4 (musl migration)

KERNEL_SHIM_DIR ?= ../neoos-kernel/third_party/shim
PREFIX ?= build-output
UPSTREAM_DIR ?= upstream

.PHONY: all clean

all:
	@echo "musl build: placeholder (implementation in Phase 4)"
	@echo "When complete, will:"
	@echo "  1. Integrate shim from $(KERNEL_SHIM_DIR)"
	@echo "  2. Build musl at $(UPSTREAM_DIR)"
	@echo "  3. Install to $(PREFIX)"

clean:
	rm -rf $(PREFIX)
EOF

git add .gitignore README.md BUILD.md Makefile
git commit -m "docs: initial musl repo scaffolding"
git push -u origin main
echo "✅ neoos-musl initialized"

# === OS BUILDER REPO ===
setup_repo "neoos-os-builder"
cat > .gitignore << 'EOF'
build/
*.iso
*.img
*.log

__pycache__/
*.pyc
.env

.idea/
.vscode/
*.swp

.DS_Store
Thumbs.db
EOF

cat > README.md << 'EOF'
# NeoOS OS Builder

Interactive and config-driven OS image builder for NeoOS.

## Features

- **Interactive TUI** — Interactively select kernel options, CPU features, and ports
- **Config-driven** — Reproducible builds from YAML/JSON configuration files
- **Single output** — Produces bootable ISO + disk images + metadata + ready-to-run QEMU script

## Quick Start

```sh
# Interactive mode
neoos-builder

# Config-driven mode
neoos-builder build config.yaml

# Preview what would be built
neoos-builder preview config.yaml
```

## Output

```
build/
├── neoos-custom.iso          # Bootable ISO
├── disk1.img                 # Data disk
├── disk2.img                 # Additional storage
├── metadata.json             # Build details
├── config.yaml               # Configuration used
└── qemu-run.sh               # Ready-to-run QEMU launcher
```

Run immediately:
```sh
./build/qemu-run.sh
```

## Configuration Example

```yaml
kernel:
  version: "latest"           # or specific git tag
  cpu_features: "auto"        # or "minimal", "standard", "optimized"
  optimization_level: "O2"

ports:
  - busybox
  - 3d-ascii-viewer

iso:
  name: "neoos-custom"
  disk_size: 2G
```

## Documentation

- **Detailed usage:** See `USAGE.md`
- **Build contracts:** See https://github.com/NeoOSOrganization/neoos-kernel/blob/main/BUILD.md
- **Port template:** See https://github.com/NeoOSOrganization/neoos-busybox

## License

Same as NeoOS kernel (license TBD).

## In This Organization

- **[neoos-kernel](https://github.com/NeoOSOrganization/neoos-kernel)** — Kernel source
- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — musl libc (kernel dependency)
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Guides and architecture
- **[Port Examples](https://github.com/NeoOSOrganization/neoos-busybox)** — See how to structure a port
EOF

cat > USAGE.md << 'EOF'
# NeoOS OS Builder — Usage Guide

## Installation

```bash
git clone https://github.com/NeoOSOrganization/neoos-os-builder
cd neoos-os-builder
# Full installation steps in Phase 4
```

## Interactive Mode (TUI)

```bash
neoos-builder
```

Walk through:
1. Select kernel version (shows available tags, default: latest)
2. Choose CPU features (shows auto-detected, or pick manually)
3. Select optimization level (O0–O3)
4. Choose ports to include (shows available, checkboxes)
5. Review build summary
6. Confirm or save config for later

Output appears in `build/`.

## Config-Driven Mode

Create a config file (`config.yaml`):

```yaml
kernel:
  version: "v1.0.0"
  cpu_features: "avx2 sse4_2"
  optimization_level: "O2"

ports:
  - busybox
  - 3d-ascii-viewer

iso:
  name: "neoos-production"
  disk_size: 2G
```

Build:

```bash
neoos-builder build config.yaml
# Output in build/
```

Reproducible builds: same config = same ISO.

## QEMU Runner

After build, run the generated script:

```bash
cd build
./qemu-run.sh
# Boots the ISO in QEMU headless, logs to qemu.log
```

Or modify the script for custom QEMU flags:

```bash
./qemu-run.sh -display gtk   # Show GUI
./qemu-run.sh -smp 4          # 4 CPUs (doesn't work without modifying script)
```

## Build Metadata

After build, check what was included:

```bash
cat build/metadata.json
# Shows kernel version, ports, CPU features, timestamps
```

## Troubleshooting

**"neoos-builder: command not found"** → Make sure you're in the repo directory or install the builder globally (Phase 4).

**Build hangs** → Check disk space. Large ports can take time. Add `-v` for verbose output.

**ISO won't boot** → Check `qemu.log` for errors. Verify selected ports are compatible with kernel version.

---

**See also:** Build contracts at https://github.com/NeoOSOrganization/neoos-kernel/blob/main/BUILD.md
EOF

git add .gitignore README.md USAGE.md
git commit -m "docs: initial OS builder repo scaffolding"
git push -u origin main
echo "✅ neoos-os-builder initialized"

# === BUSYBOX REPO ===
setup_repo "neoos-busybox"
cat > .gitignore << 'EOF'
build/
*.nex
upstream/

*.o
*.a

.idea/
.vscode/
*.swp

.DS_Store
Thumbs.db
EOF

cat > README.md << 'EOF'
# BusyBox for NeoOS

BusyBox port: interactive shell (ash), file utilities, and core utilities for NeoOS.

## Quick Start

Build (requires musl and kernel):

```bash
git clone https://github.com/NeoOSOrganization/neoos-musl ../neoos-musl
cd ../neoos-musl && make

git clone https://github.com/NeoOSOrganization/neoos-busybox
cd neoos-busybox
make MUSL_DIR=../neoos-musl/build-output
# Produces: build/busybox.nex
```

## Features

- Interactive ash shell
- 200+ coreutils replacements
- File utilities (cp, mv, rm, etc.)
- Static linked with musl libc

## Usage

In NeoOS `/ETC/INITTAB`:

```
::once:/BUSYBOX ash
```

Then boot NeoOS and you have a shell.

## Build Details

- Statically linked with musl + NeoOS syscall shim
- Single `.nex` binary
- Minimal BusyBox config (no X11, no C library features beyond musl)

## Smoke Test

```bash
make smoke-test
# Runs basic shell tests in NeoOS environment
```

## Documentation

- Port-specific notes: See `PORTING-NOTES.md`
- General porting guide: https://github.com/NeoOSOrganization/neoos-docs/blob/main/docs/porting.md

## License

BusyBox is GPL v2. NeoOS patches (if any) follow the same license.

## In This Organization

This is a **port template**. Each port is its own repository.

- **[neoos-kernel](https://github.com/NeoOSOrganization/neoos-kernel)** — Kernel (see syscalls, features)
- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — libc (ports link against this)
- **[neoos-os-builder](https://github.com/NeoOSOrganization/neoos-os-builder)** — Assembles final images with selected ports
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Porting guide and best practices
EOF

cat > Makefile << 'EOF'
# BusyBox port for NeoOS
# Stub implementation (full build in Phase 3)

MUSL_DIR ?= ../neoos-musl/build-output
BUILD_DIR ?= build

.PHONY: all clean smoke-test

all:
	@echo "BusyBox build: placeholder (implementation in Phase 3)"
	@echo "When complete, will:"
	@echo "  1. Configure BusyBox for NeoOS"
	@echo "  2. Link against musl from $(MUSL_DIR)"
	@echo "  3. Produce static binary at $(BUILD_DIR)/busybox.nex"

clean:
	rm -rf $(BUILD_DIR)

smoke-test:
	@echo "BusyBox smoke test: placeholder (runs in Phase 3)"
	@echo "Will verify: shell startup, basic commands (echo, ls, etc.)"
EOF

cat > smoke-test.sh << 'EOF'
#!/bin/bash
# BusyBox smoke test for NeoOS
# Runs after /busybox.nex is installed in the OS image

set -e

echo "=== BusyBox Smoke Test ==="

# Test: shell can start
/busybox sh -c "echo hello" > /tmp/test.txt 2>&1 || {
    echo "FAILED: shell startup"
    exit 1
}

# Test: output is correct
[ "$(cat /tmp/test.txt)" = "hello" ] || {
    echo "FAILED: echo command"
    cat /tmp/test.txt
    exit 1
}

# Test: basic utilities exist
/busybox ls / > /dev/null || {
    echo "FAILED: ls command"
    exit 1
}

echo "PASSED: BusyBox basic functionality"
exit 0
EOF

chmod +x smoke-test.sh
git add .gitignore README.md Makefile smoke-test.sh
git commit -m "docs: initial BusyBox port template"
git push -u origin main
echo "✅ neoos-busybox initialized"

echo ""
echo "=== All repositories initialized! ==="
echo "✅ neoos-kernel"
echo "✅ neoos-musl"
echo "✅ neoos-os-builder"
echo "✅ neoos-busybox"
echo ""
echo "Next: initialize neoos-docs with Docusaurus (requires npm/node)"
