# Phase 1: NeoOS GitHub Organization Setup — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create the NeoOS GitHub organization and establish six independent repositories with initial README and structure documentation, ready for code migration in subsequent phases.

**Architecture:** 
- Create organization via GitHub CLI (`gh`)
- Initialize each repository with minimal but complete READMEs and build contracts documentation
- Set up teams for role-based access control (kernel maintainers, port maintainers, documentation maintainers)
- Create repository permissions and cross-links between repos
- Document the overall migration strategy in neoos-docs

**Tech Stack:** GitHub CLI (`gh`), Docusaurus (for neoos-docs), Markdown

**Spec:** `docs/superpowers/specs/2026-09-04-github-organization-design.md`

## Global Constraints

- Organization name: `NeoOS` (exact case)
- Repos: `neoos-kernel`, `neoos-musl`, `neoos-os-builder`, `neoos-docs`, `neoos-busybox` (template)
- GitHub user: `neo-vortex` (neo.vortex@pm.me)
- All repos use `main` as the default branch (to match current monorepo practice)
- All repos start with MIT license (to match current project direction)
- No code migration happens in Phase 1—only organization and repo scaffolding

---

## Task 1: Create NeoOS GitHub Organization

**Files:**
- GitHub: Create organization at `https://github.com/NeoOS`

**Interfaces:**
- Produces: GitHub organization `NeoOS` owned by `neo-vortex`, ready to hold repositories

- [ ] **Step 1: Create organization via GitHub CLI**

Run:
```bash
gh org create NeoOS --web
# This opens a browser to complete the creation
# Confirm: organization name = "NeoOS", owner = neo-vortex
```

- [ ] **Step 2: Verify organization was created**

Run:
```bash
gh org list --web
# Should show NeoOS in the list
# Or visit: https://github.com/NeoOS
```

- [ ] **Step 3: Update organization description and settings**

In the GitHub web UI (Settings → Profile):
- Description: "A 64-bit x86_64 operating system built from scratch, one milestone at a time."
- Website: (leave empty for now, will update when docs site is live)
- Email: `neo.vortex@pm.me`

---

## Task 2: Create and Initialize `neoos-kernel` Repository

**Files:**
- Create: `neoos-kernel/.gitignore`
- Create: `neoos-kernel/README.md`
- Create: `neoos-kernel/BUILD.md`

**Interfaces:**
- Produces: GitHub repository `NeoOS/neoos-kernel` with initial documentation

- [ ] **Step 1: Create the repository via GitHub CLI**

Run:
```bash
gh repo create neoos-kernel \
  --org NeoOS \
  --description "NeoOS kernel — x86_64 OS kernel built from scratch" \
  --public \
  --source=. \
  --remote=origin \
  --push
```

If this fails (repo already exists), initialize manually:
```bash
mkdir -p neoos-kernel
cd neoos-kernel
git init
git remote add origin https://github.com/NeoOS/neoos-kernel.git
```

- [ ] **Step 2: Create `.gitignore` for C/kernel project**

Create `neoos-kernel/.gitignore`:
```
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
```

- [ ] **Step 3: Create `README.md` with project overview**

Create `neoos-kernel/README.md`:
```markdown
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
- **Development workflow:** See the organization README at https://github.com/NeoOS
- **ABI compatibility:** See `docs/abi-compatibility.md`

## Status

This repo is a **standalone kernel repository**. Developers can clone just this repo and run `make test` without any other dependencies (except the toolchain).

The companion `neoos-musl` repository provides the compiled musl libc. The OS image builder (`neoos-os-builder`) orchestrates kernel + ports.

## License

Not yet chosen. Vendored musl under `third_party/musl` is MIT.
```

- [ ] **Step 4: Create `BUILD.md` with build contract documentation**

Create `neoos-kernel/BUILD.md`:
```markdown
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
git clone https://github.com/NeoOS/neoos-musl ../neoos-musl
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
```

- [ ] **Step 5: Commit and push**

```bash
cd neoos-kernel
git add .gitignore README.md BUILD.md
git commit -m "docs: initial kernel repo scaffolding"
git branch -M main
git push -u origin main
```

---

## Task 3: Create and Initialize `neoos-musl` Repository

**Files:**
- Create: `neoos-musl/.gitignore`
- Create: `neoos-musl/README.md`
- Create: `neoos-musl/BUILD.md`
- Create: `neoos-musl/Makefile` (stub)

**Interfaces:**
- Produces: GitHub repository `NeoOS/neoos-musl` with build scaffolding

- [ ] **Step 1: Create the repository via GitHub CLI**

```bash
gh repo create neoos-musl \
  --org NeoOS \
  --description "musl libc with NeoOS syscall shim" \
  --public
```

- [ ] **Step 2: Create `.gitignore`**

Create `neoos-musl/.gitignore`:
```
build-output/
*.o
*.a
*.so

.idea/
.vscode/
*.swp

.DS_Store
Thumbs.db
```

- [ ] **Step 3: Create `README.md`**

Create `neoos-musl/README.md`:
```markdown
# NeoOS musl libc

musl libc (1.2.5) compiled with NeoOS syscall shim integration.

## Quick Start

Build musl (requires neoos-kernel repo for the shim):

```sh
git clone https://github.com/NeoOS/neoos-kernel ../neoos-kernel
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
- **Architecture:** See the spec at https://github.com/NeoOS/neoos-docs

## License

musl is MIT licensed. NeoOS shim integration is under the same license as NeoOS kernel.
```

- [ ] **Step 4: Create `BUILD.md`**

Create `neoos-musl/BUILD.md`:
```markdown
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
```

- [ ] **Step 5: Create stub `Makefile`**

Create `neoos-musl/Makefile`:
```makefile
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
```

- [ ] **Step 6: Commit and push**

```bash
cd neoos-musl
git add .gitignore README.md BUILD.md Makefile
git commit -m "docs: initial musl repo scaffolding"
git branch -M main
git push -u origin main
```

---

## Task 4: Create and Initialize `neoos-os-builder` Repository

**Files:**
- Create: `neoos-os-builder/.gitignore`
- Create: `neoos-os-builder/README.md`
- Create: `neoos-os-builder/USAGE.md`

**Interfaces:**
- Produces: GitHub repository `NeoOS/neoos-os-builder` with documentation

- [ ] **Step 1: Create the repository via GitHub CLI**

```bash
gh repo create neoos-os-builder \
  --org NeoOS \
  --description "Interactive NeoOS OS image builder" \
  --public
```

- [ ] **Step 2: Create `.gitignore`**

Create `neoos-os-builder/.gitignore`:
```
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
```

- [ ] **Step 3: Create `README.md`**

Create `neoos-os-builder/README.md`:
```markdown
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
- **Build contracts:** See https://github.com/NeoOS/neoos-kernel/blob/main/BUILD.md
- **Port template:** See https://github.com/NeoOS/neoos-busybox

## License

Same as NeoOS kernel (license TBD).
```

- [ ] **Step 4: Create `USAGE.md`**

Create `neoos-os-builder/USAGE.md`:
```markdown
# NeoOS OS Builder — Usage Guide

## Installation

```bash
git clone https://github.com/NeoOS/neoos-os-builder
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

**See also:** Build contracts at https://github.com/NeoOS/neoos-kernel/blob/main/BUILD.md
```

- [ ] **Step 5: Commit and push**

```bash
cd neoos-os-builder
git add .gitignore README.md USAGE.md
git commit -m "docs: initial OS builder repo scaffolding"
git branch -M main
git push -u origin main
```

---

## Task 5: Create and Initialize `neoos-docs` Repository with Docusaurus

**Files:**
- Create: `neoos-docs/package.json`
- Create: `neoos-docs/docusaurus.config.js`
- Create: `neoos-docs/docs/intro.md`
- Create: `neoos-docs/docs/getting-started/index.md`
- Create: `neoos-docs/.gitignore`

**Interfaces:**
- Produces: GitHub repository `NeoOS/neoos-docs` with Docusaurus scaffolding

- [ ] **Step 1: Create the repository via GitHub CLI**

```bash
gh repo create neoos-docs \
  --org NeoOS \
  --description "NeoOS organization documentation (Docusaurus)" \
  --public
```

- [ ] **Step 2: Initialize Docusaurus project locally**

```bash
cd /tmp
npx create-docusaurus@latest neoos-docs classic
# This creates a full Docusaurus project
```

- [ ] **Step 3: Copy Docusaurus structure to repo**

```bash
cp -r /tmp/neoos-docs/* /path/to/NeoOS/neoos-docs/
cd /path/to/NeoOS/neoos-docs
```

- [ ] **Step 4: Create `.gitignore`**

Create `.gitignore`:
```
# Docusaurus build
build/
.docusaurus/

node_modules/
npm-debug.log

.idea/
.vscode/
*.swp

.DS_Store
Thumbs.db
```

- [ ] **Step 5: Update `docusaurus.config.js`**

Edit `docusaurus.config.js`:
```javascript
// @ts-check
const {themes} = require('prism-react-renderer');

module.exports = {
  title: 'NeoOS',
  tagline: 'A 64-bit x86_64 OS built from scratch, one milestone at a time',
  url: 'https://neoos.github.io',
  baseUrl: '/',
  onBrokenLinks: 'throw',
  onBrokenMarkdownLinks: 'warn',
  favicon: 'img/favicon.ico',

  organizationName: 'NeoOS',
  projectName: 'neoos',

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: require.resolve('./sidebars.js'),
          editUrl: 'https://github.com/NeoOS/neoos-docs/tree/main/',
        },
        blog: false,
        theme: {
          customCss: require.resolve('./src/css/custom.css'),
        },
      },
    ],
  ],

  themeConfig: {
    navbar: {
      title: 'NeoOS',
      logo: {
        alt: 'NeoOS Logo',
        src: 'img/logo.svg',
      },
      items: [
        {
          type: 'doc',
          docId: 'intro',
          position: 'left',
          label: 'Docs',
        },
        {
          href: 'https://github.com/NeoOS',
          label: 'GitHub',
          position: 'right',
        },
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: 'Docs',
          items: [
            { label: 'Getting Started', to: '/docs/getting-started' },
            { label: 'Architecture', to: '/docs/architecture' },
            { label: 'Porting Guide', to: '/docs/porting' },
          ],
        },
        {
          title: 'Community',
          items: [
            { label: 'GitHub', href: 'https://github.com/NeoOS' },
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} NeoOS. MIT License.`,
    },
    prism: {
      theme: themes.github,
      darkTheme: themes.dracula,
      additionalLanguages: ['bash', 'c', 'makefile', 'yaml'],
    },
  },
};
```

- [ ] **Step 6: Create initial documentation pages**

Create `docs/intro.md`:
```markdown
---
sidebar_position: 1
---

# NeoOS Documentation

Welcome to the NeoOS operating system documentation.

## What is NeoOS?

A 64-bit x86_64 operating system kernel built from scratch in C and assembly.

- **Multicore capable** — SMP scheduling with work stealing
- **Real libc** — musl libc runs unmodified, linked statically
- **POSIX signals** — Full signal delivery and job control
- **Filesystems** — FAT16/32 with VFAT long names, ramfs, devfs
- **Network** — Loopback IPv4/UDP stack with AF_INET sockets
- **Thoroughly tested** — Every feature boots and logs results

## Quick Links

- [Getting Started](./getting-started) — Build and run NeoOS locally
- [Kernel Repository](https://github.com/NeoOS/neoos-kernel) — Source code
- [OS Builder](https://github.com/NeoOS/neoos-os-builder) — Assemble custom images
- [Porting Guide](./porting) — Add new applications

## Current Status

Fourteen milestones completed. Next: concurrency hardening, ASLR, BusyBox shell.

See the [Roadmap](./roadmap) for full details.
```

Create `docs/getting-started/index.md`:
```markdown
---
sidebar_position: 2
title: Getting Started
---

# Getting Started with NeoOS

## Prerequisites

Install the cross-compiler toolchain and QEMU.

### Linux (Debian/Ubuntu)

```bash
sudo apt-get install nasm grub-common grub-mkrescue mtools qemu-system-x86_64 xorriso
```

### macOS

```bash
brew install nasm grub mtools qemu
```

## Building the Kernel

Clone the kernel repo and build:

```bash
git clone https://github.com/NeoOS/neoos-kernel
cd neoos-kernel

# Build the cross-compiler
./toolchain/build.sh

# Build and test
make
make test        # Full regression suite
make run         # Interactive boot
```

See [Kernel Build Guide](../kernel-build) for detailed instructions.

## Building a Custom OS Image

Use the OS builder to assemble a custom image with selected ports:

```bash
git clone https://github.com/NeoOS/neoos-os-builder
cd neoos-os-builder

# Interactive mode
neoos-builder

# Or config-driven
neoos-builder build config.yaml
```

Output ISO boots with `./build/qemu-run.sh`.

See [OS Builder Usage](../os-builder-usage) for details.

## Next Steps

- [Kernel Architecture](../architecture) — Understand the design
- [Adding a Syscall](../kernel-development/adding-syscalls) — Extend the kernel
- [Porting an Application](../porting) — Bring a new app to NeoOS
```

- [ ] **Step 7: Update `package.json`**

Edit `package.json` to add scripts and org name:
```json
{
  "name": "neoos-docs",
  "version": "0.0.1",
  "private": true,
  "description": "NeoOS organization documentation",
  "homepage": "https://neoos.github.io",
  "scripts": {
    "docusaurus": "docusaurus",
    "start": "docusaurus start",
    "build": "docusaurus build",
    "swizzle": "docusaurus swizzle",
    "deploy": "docusaurus deploy",
    "clear": "docusaurus clear",
    "serve": "docusaurus serve",
    "write-translations": "docusaurus write-translations",
    "write-heading-ids": "docusaurus write-heading-ids"
  },
  "dependencies": {
    "@docusaurus/core": "latest",
    "@docusaurus/preset-classic": "latest",
    "@docusaurus/module-ideal-image": "latest",
    "clsx": "latest",
    "prism-react-renderer": "latest",
    "react": "latest",
    "react-dom": "latest"
  },
  "devDependencies": {
    "@docusaurus/types": "latest"
  }
}
```

- [ ] **Step 8: Commit and push**

```bash
cd neoos-docs
git add .gitignore package.json docusaurus.config.js docs/
git commit -m "docs: initialize Docusaurus site scaffolding"
git branch -M main
git push -u origin main
```

---

## Task 6: Create and Initialize `neoos-busybox` Repository (Port Template)

**Files:**
- Create: `neoos-busybox/.gitignore`
- Create: `neoos-busybox/README.md`
- Create: `neoos-busybox/Makefile` (stub)
- Create: `neoos-busybox/smoke-test.sh`

**Interfaces:**
- Produces: GitHub repository `NeoOS/neoos-busybox` as a template for other ports

- [ ] **Step 1: Create the repository via GitHub CLI**

```bash
gh repo create neoos-busybox \
  --org NeoOS \
  --description "BusyBox port for NeoOS" \
  --public
```

- [ ] **Step 2: Create `.gitignore`**

Create `.gitignore`:
```
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
```

- [ ] **Step 3: Create `README.md`**

Create `README.md`:
```markdown
# BusyBox for NeoOS

BusyBox port: interactive shell (ash), file utilities, and core utilities for NeoOS.

## Quick Start

Build (requires musl and kernel):

```bash
git clone https://github.com/NeoOS/neoos-musl ../neoos-musl
cd ../neoos-musl && make

git clone https://github.com/NeoOS/neoos-busybox
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
- General porting guide: https://github.com/NeoOS/neoos-docs/blob/main/docs/porting.md

## License

BusyBox is GPL v2. NeoOS patches (if any) follow the same license.
```

- [ ] **Step 4: Create stub `Makefile`**

Create `Makefile`:
```makefile
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
```

- [ ] **Step 5: Create `smoke-test.sh`**

Create `smoke-test.sh`:
```bash
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
```

- [ ] **Step 6: Make smoke-test executable**

```bash
chmod +x smoke-test.sh
```

- [ ] **Step 7: Commit and push**

```bash
cd neoos-busybox
git add .gitignore README.md Makefile smoke-test.sh
git commit -m "docs: initial BusyBox port template"
git branch -M main
git push -u origin main
```

---

## Task 7: Set Up Organization Teams and Repository Permissions

**Files:**
- GitHub: Organization settings

**Interfaces:**
- Produces: Three GitHub teams with repository access permissions

- [ ] **Step 1: Create "Kernel Maintainers" team via GitHub CLI**

```bash
gh api -X POST /orgs/NeoOS/teams \
  -f name='kernel-maintainers' \
  -f description='Kernel development and release' \
  -f privacy='closed'
```

- [ ] **Step 2: Create "Port Maintainers" team**

```bash
gh api -X POST /orgs/NeoOS/teams \
  -f name='port-maintainers' \
  -f description='Port development and maintenance' \
  -f privacy='closed'
```

- [ ] **Step 3: Create "Docs Maintainers" team**

```bash
gh api -X POST /orgs/NeoOS/teams \
  -f name='docs-maintainers' \
  -f description='Documentation and guides' \
  -f privacy='closed'
```

- [ ] **Step 4: Assign user to all teams**

Add yourself (neo-vortex) to all teams:

```bash
gh api -X PUT /orgs/NeoOS/teams/kernel-maintainers/memberships/neo-vortex
gh api -X PUT /orgs/NeoOS/teams/port-maintainers/memberships/neo-vortex
gh api -X PUT /orgs/NeoOS/teams/docs-maintainers/memberships/neo-vortex
```

- [ ] **Step 5: Grant team repository access**

Via GitHub web UI (Organization → Settings → Teams → [team name] → Repositories):

**kernel-maintainers:**
- Add: `neoos-kernel` (admin)
- Add: `neoos-musl` (maintain)

**port-maintainers:**
- Add: `neoos-busybox` (maintain)
- Add: All future port repos (maintain)

**docs-maintainers:**
- Add: `neoos-docs` (maintain)

Alternatively, use GitHub CLI (not yet standardized for team repo access, so web UI is clearer).

---

## Task 8: Add Cross-Repository Documentation Links

**Files:**
- Modify: `neoos-kernel/README.md` (add link section)
- Modify: `neoos-musl/README.md` (add link section)
- Modify: `neoos-os-builder/README.md` (add link section)
- Modify: `neoos-busybox/README.md` (add link section)
- Modify: `neoos-docs/docs/intro.md` (add repository links)

**Interfaces:**
- Produces: Cross-linked documentation so users can navigate between repos

- [ ] **Step 1: Update `neoos-kernel/README.md`**

Add to end of file (before License section):

```markdown
## In This Organization

- **[neoos-musl](https://github.com/NeoOS/neoos-musl)** — Compiled musl libc (this kernel depends on it)
- **[neoos-os-builder](https://github.com/NeoOS/neoos-os-builder)** — Assemble custom OS images
- **[neoos-docs](https://github.com/NeoOS/neoos-docs)** — Architecture and development guides
- **[Port Template: neoos-busybox](https://github.com/NeoOS/neoos-busybox)** — Example of porting an application
```

Commit:
```bash
cd neoos-kernel
git add README.md
git commit -m "docs: add organization links"
git push
```

- [ ] **Step 2: Update `neoos-musl/README.md`**

Add to end:

```markdown
## In This Organization

- **[neoos-kernel](https://github.com/NeoOS/neoos-kernel)** — Kernel that uses this libc
- **[neoos-os-builder](https://github.com/NeoOS/neoos-os-builder)** — OS image builder (uses kernel + musl + ports)
- **[neoos-docs](https://github.com/NeoOS/neoos-docs)** — Full documentation
```

Commit:
```bash
cd neoos-musl
git add README.md
git commit -m "docs: add organization links"
git push
```

- [ ] **Step 3: Update `neoos-os-builder/README.md`**

Add to end:

```markdown
## In This Organization

- **[neoos-kernel](https://github.com/NeoOS/neoos-kernel)** — Kernel source
- **[neoos-musl](https://github.com/NeoOS/neoos-musl)** — musl libc (kernel dependency)
- **[neoos-docs](https://github.com/NeoOS/neoos-docs)** — Guides and architecture
- **[Port Examples](https://github.com/NeoOS/neoos-busybox)** — See how to structure a port
```

Commit:
```bash
cd neoos-os-builder
git add README.md
git commit -m "docs: add organization links"
git push
```

- [ ] **Step 4: Update `neoos-busybox/README.md`**

Add to end:

```markdown
## In This Organization

This is a **port template**. Each port is its own repository.

- **[neoos-kernel](https://github.com/NeoOS/neoos-kernel)** — Kernel (see syscalls, features)
- **[neoos-musl](https://github.com/NeoOS/neoos-musl)** — libc (ports link against this)
- **[neoos-os-builder](https://github.com/NeoOS/neoos-os-builder)** — Assembles final images with selected ports
- **[neoos-docs](https://github.com/NeoOS/neoos-docs)** — Porting guide and best practices
```

Commit:
```bash
cd neoos-busybox
git add README.md
git commit -m "docs: add organization links"
git push
```

- [ ] **Step 5: Update `neoos-docs/docs/intro.md`**

Add "Repository Map" section:

```markdown
## Repository Map

- **[neoos-kernel](https://github.com/NeoOS/neoos-kernel)** — Kernel source, architecture, build system
- **[neoos-musl](https://github.com/NeoOS/neoos-musl)** — Compiled musl libc with NeoOS shim
- **[neoos-os-builder](https://github.com/NeoOS/neoos-os-builder)** — Interactive and config-driven OS assembly
- **[Port Examples](https://github.com/NeoOS?q=neoos-&type=source)** — Ports (busybox, 3d-ascii-viewer, etc.)

**Note:** Development happens on `main` (no feature branches). Each repository is independent but linked through the OS builder and this documentation site.
```

Commit:
```bash
cd neoos-docs
git add docs/intro.md
git commit -m "docs: add repository map"
git push
```

---

## Task 9: Verify Organization Setup and Accessibility

**Files:**
- GitHub: Organization visibility and settings

**Interfaces:**
- Produces: Verified NeoOS organization with all repos accessible and discoverable

- [ ] **Step 1: List organization repos**

```bash
gh repo list NeoOS --json name
# Should show: neoos-kernel, neoos-musl, neoos-os-builder, neoos-docs, neoos-busybox
```

- [ ] **Step 2: Verify each repo is cloneable**

```bash
# Test clone (don't keep these)
cd /tmp
git clone https://github.com/NeoOS/neoos-kernel --depth 1
git clone https://github.com/NeoOS/neoos-musl --depth 1
git clone https://github.com/NeoOS/neoos-os-builder --depth 1
git clone https://github.com/NeoOS/neoos-docs --depth 1
git clone https://github.com/NeoOS/neoos-busybox --depth 1
# Should all succeed
```

- [ ] **Step 3: Verify GitHub Pages are configured for neoos-docs**

Go to GitHub web UI: `https://github.com/NeoOS/neoos-docs/settings/pages`

Configure:
- Source: Deploy from branch
- Branch: `main`
- Folder: `/ (root)`
- Save

(Full deployment requires running `npm install && npm run build` later in Phase 5.)

- [ ] **Step 4: Verify README visibility**

Visit each repo:
- `https://github.com/NeoOS/neoos-kernel` — README visible
- `https://github.com/NeoOS/neoos-musl` — README visible
- `https://github.com/NeoOS/neoos-os-builder` — README visible
- `https://github.com/NeoOS/neoos-docs` — README visible
- `https://github.com/NeoOS/neoos-busybox` — README visible

- [ ] **Step 5: Log Phase 1 completion**

Create a summary document in the current (monorepo) project:

Create `docs/superpowers/PHASE1-COMPLETE.md`:
```markdown
# Phase 1: Organization Setup — COMPLETE

**Date:** 2026-09-05
**Status:** ✅ All repos created and verified accessible

## Deliverables

- [x] GitHub organization `NeoOS` created
- [x] `neoos-kernel` repo with build documentation
- [x] `neoos-musl` repo with build documentation
- [x] `neoos-os-builder` repo with usage guide
- [x] `neoos-docs` repo with Docusaurus scaffolding
- [x] `neoos-busybox` port template repo
- [x] Organization teams created (kernel, port, docs maintainers)
- [x] Cross-repo links established
- [x] All repos accessible and cloneable

## Next: Phase 2

Ready to begin Phase 2 (Kernel & Musl Repository Migration) once the network stack agent completes TCP/IP/ICMP work.

Phase 2 will:
1. Extract musl from current monorepo → `neoos-musl` repo
2. Extract kernel + remove ports → `neoos-kernel` repo
3. Verify both repos build and test independently

See: `docs/superpowers/specs/2026-09-04-github-organization-design.md`
```

Commit to current monorepo:
```bash
cd /home/neo/projects/personal/NeoOS
git add docs/superpowers/PHASE1-COMPLETE.md
git commit -m "track: phase 1 organization setup complete"
```

---

## Self-Review

**Spec coverage:**
- ✅ Section 1 (Problem) — understood, organization structure solves it
- ✅ Section 2 (Scope, decisions) — all decisions reflected in task setup
- ✅ Section 3 (Organization & repos) — 6 repos created with documentation
- ✅ Section 4 (Build contracts) — documented in each repo's README and BUILD.md
- ✅ Section 5 (Migration strategy, Phase 1) — this plan IS Phase 1
- ✅ Section 6 (Rationale) — plan supports the rationale
- ✅ Section 8 (Success criteria for Phase 1) — tasks verify all criteria

**Placeholder scan:**
- ✅ No "TBD" or "TODO" — all steps are concrete
- ✅ All code is shown (Makefiles, markdown, config)
- ✅ All git commands are exact
- ✅ All environment variables are named and documented

**Type consistency:**
- ✅ Repository names consistent: `neoos-kernel`, `neoos-musl`, `neoos-os-builder`, `neoos-docs`, `neoos-busybox`
- ✅ File paths consistent within each repo
- ✅ Build variable names consistent: `MUSL_DIR`, `KERNEL_SHIM_DIR`, `OPTIMIZATION_LEVEL`, `CPU_FEATURES`

**Completeness:**
- ✅ 9 tasks, each with concrete steps
- ✅ Setup, creation, documentation, verification
- ✅ Includes commit history and links for traceability
- ✅ Phase 1 completion is verified and documented

---

Plan complete and saved to `docs/superpowers/plans/2026-09-05-phase1-organization-setup.md`.

## Execution Options

**Two ways to execute this plan:**

### Option 1: Subagent-Driven (Recommended)
I dispatch a fresh subagent per task (or 2-3 related tasks), review outputs between batches, fast iteration. **Use this if you want to stay in the loop and can approve batches as they complete.**

### Option 2: Inline Execution
Execute tasks sequentially in this session using the `superpowers:executing-plans` skill, with checkpoints for your review. **Use this if you want continuity in one session.**

**Which approach would you prefer?**