# NeoOS GitHub Organization — Design

**Date:** 2026-09-04
**Status:** approved, ready for migration planning
**Trigger:** Scale beyond single monorepo; support OS image builder and multi-port development in parallel

---

## 1. Problem

Today, NeoOS is a single repository (`Neo-vortex/NeoOS`) containing kernel, ports, utilities, and tooling mixed together. This works for early development but creates friction:

1. **Kernel developers and port developers step on each other.** A kernel change that breaks a port breaks the entire repo's CI. Conversely, a broken port can block kernel development.
2. **Ports can't move independently.** Each port is tied to the exact kernel version at the time it was added. No way to fork a port for independent iteration.
3. **OS image assembly is ad-hoc.** No structured way for users to select which ports to include, which kernel options to enable, or to reproduce a build later.
4. **Development is tightly coupled to real hardware.** Every build includes everything; there's no way to build just the kernel or just a single port for testing.

## 2. Scope

This design establishes a GitHub organization called `NeoOS` with the following repositories and responsibilities:

**In scope:**
1. Create organization `NeoOS` on GitHub.
2. Split monorepo into five independent repositories:
   - `neoos-kernel` — kernel source, build system, regression tests
   - `neoos-os-builder` — TUI + config-driven OS image assembly
   - `neoos-<portname>` (one per port: busybox, 3d-ascii-viewer, etc.) — individual port build, test, and maintenance
   - `neoos-docs` — Docusaurus site for organization-wide documentation and porting guide
3. Define build contracts and conventions so each repo can be built independently or orchestrated by the OS builder.
4. Plan migration from monorepo to multi-repo structure without disrupting active development (network stack agent work).

**Out of scope:**
- Immediate implementation (waits for network stack agent to complete TCP/IP/ICMP work)
- Changes to kernel development workflow (developers still work on `main` directly)
- Modification of existing kernel milestones or roadmap

### 2.1 Decisions Taken

- **Kernel repo is completely independent.** A developer cloning `neoos-kernel` can run `make test` and get full regression results without any other repository. This preserves workflow during transition.
- **No monolithic "kernel + tooling" repo.** Ports are always separate, never baked into kernel tests or builds.
- **OS builder is orchestration, not coupling.** It clones kernel and ports as dependencies, leaving each independent. The builder is a thin wrapper around each repo's own build system.
- **Docusaurus as coordination hub.** Infrastructure, conventions, and porting guide live in one place to reduce duplication across repos. But repos are not code-dependent on docs.
- **Phased migration, not flag day.** Each port migrates one at a time. Current monorepo stays available during transition to avoid disrupting in-progress work.

## 3. Organization & Repository Structure

**GitHub Organization:** `NeoOS`

**Repositories:**

1. **`neoos-kernel`** — Kernel source, built independently and completely standalone
2. **`neoos-musl`** — musl libc with integrated NeoOS shim, compiled for kernel and ports to link against
3. **`neoos-os-builder`** — Interactive image assembly tool
4. **`neoos-<portname>`** — Individual ports (one repo each)
5. **`neoos-docs`** — Docusaurus documentation hub

**Detailed responsibilities below:**

### 3.1 `neoos-kernel`

**Responsibility:** Kernel development, testing, and release.

**Contains:**
- `kernel/` — kernel source (arch, dev, fs, ipc, mm, net, sched, smp, sync, syscall)
- `lib/` — libneoos (native NeoOS C library)
- `boot/` — bootloader code
- `third_party/shim/` — musl syscall shim (Linux syscall numbers → NeoOS syscall numbers)
- `userland/` — test programs (one per subsystem)
- `tools/` — build tools (cross-compiler setup, disk image generators, etc.)
- `docs/` — stdlib reference, ABI compatibility, internal implementation notes
- `shared/` — shared code used by kernel and userland
- `Makefile`, `CMakeLists.txt`, `linker.ld` — build system
- `.github/workflows/` — CI: build kernel, run `make test` headless in QEMU
- `README.md` — build instructions, how to run standalone

**Does NOT contain:**
- musl source (in separate `neoos-musl` repo)
- Ports (each in its own repo)
- OS builder orchestration
- ISO assembly logic

**Build contract:**

The kernel depends on compiled musl from `neoos-musl`. When building:
```sh
# Option A: Build with musl repo cloned nearby
git clone https://github.com/NeoOS/neoos-musl ../neoos-musl
cd ../neoos-musl && make
cd ../neoos-kernel
make MUSL_DIR=../neoos-musl/build-output

# Option B: Or specify musl location directly
make MUSL_DIR=/path/to/musl/artifacts

# Standard targets (after setup):
make             # Builds kernel binary, produces build/neoos.bin
make iso disk-image  # Creates ISO and data disk images
make test        # Full regression suite in headless QEMU with serial capture
make run         # Interactive QEMU boot with display
```

**CI/Regression Testing:**
- On every push to `main`: build kernel, run `make test`, capture serial output
- Fails if any subsystem reports `FAILED` in the log
- No gating on ports; kernel is always releasable independent of port status

**Development model:**
- Developers work on `main` directly (as today)
- Network stack agent (TCP/IP/ICMP) continues work here until complete
- When a new kernel feature becomes visible to userland (new syscall, new flag), the developer **must also**:
  1. Update `docs/stdlib.md` to document the feature or divergence
  2. Update the musl shim in `third_party/shim/` if the feature is exposed through musl
  3. Consider whether a test in `userland/` validates it from user space

**Versioning:**
- Semantic versioning (e.g., `v1.0.0`, `v1.1.0-dev`, `v1.2.0-rc1`)
- Each release tagged and accompanied by release notes

### 3.2 Standard Library Repository (`neoos-musl`)

**Responsibility:** Build musl libc with NeoOS syscall shim, provide consistent headers and library for kernel and all ports.

**Contains:**
- `upstream/` — vanilla musl libc source (submodule, never edited in place)
- `Makefile` — build script that integrates kernel shim and compiles musl
- `build-output/` (generated) — compiled headers in `include/`, static library `lib/libc.a`
- `.github/workflows/` — CI: build musl, verify shim integrates correctly
- `README.md` — build instructions

**Does NOT contain:**
- Syscall shim (lives in `neoos-kernel/third_party/shim/`)
- Kernel source
- Port source

**How it works:**

The shim (in kernel repo) translates between Linux syscall numbers (what musl's libc expects) and NeoOS syscall numbers (defined by the kernel). To build musl with the shim integrated:

```makefile
# Simplified build flow
KERNEL_SHIM_DIR ?= ../neoos-kernel/third_party/shim

make:
    # Copy NeoOS shim into musl's arch directory
    cp $(KERNEL_SHIM_DIR)/syscall.h upstream/arch/x86_64/
    # Configure and build musl
    cd upstream && ./configure --target=x86_64 && make
    # Install to build-output
    cd upstream && make install DESTDIR=../build-output
```

**Dependency contract:**

`neoos-musl` can be built independently if you point it to the kernel shim:

```sh
git clone https://github.com/NeoOS/neoos-musl
cd neoos-musl
make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim
# Produces: build-output/include/ and build-output/lib/libc.a
```

**Integration with kernel and ports:**

When `neoos-kernel` or a port repo builds, it links against the compiled musl from `neoos-musl`:

```sh
git clone https://github.com/NeoOS/neoos-musl
git clone https://github.com/NeoOS/neoos-kernel
cd neoos-musl && make
cd ../neoos-kernel && make MUSL_DIR=../neoos-musl/build-output
```

Or use submodules for automatic integration.

**Versioning:**
- Tagged independently from kernel (e.g., `musl-1.2.5-neoos-v1`)
- Documents which kernel versions it's tested against
- Breaking changes (if the shim API changes) get a major version bump

**CI/Testing:**
- On push: build musl with shim, verify no errors
- Test that both kernel and sample ports can link against the built artifacts
- Report in CI status

### 3.3 `neoos-os-builder`

**Responsibility:** Interactive OS image assembly; produce ready-to-boot ISO with selected components.

**Contains:**
- `src/` — TUI application for interactive configuration
- `Makefile` — orchestration logic (clones kernel and port repos, assembles final ISO)
- `templates/` — build script and config templates
- `config/` — example configurations for common use cases
- `scripts/qemu-run.sh.template` — template for QEMU launcher (copied to output)
- `docs/` — builder usage guide
- `.github/workflows/` — CI: test builder with several different configurations

**Does NOT contain:**
- Kernel source
- Port source (clones them as dependencies)

**How it works:**

1. **Configuration input:** YAML or JSON file specifying:
   ```yaml
   kernel:
     version: "latest"  # Tag from neoos-kernel or "latest"
     cpu_features: "auto"  # "auto", "minimal", "standard", "optimized", or explicit list
     optimization_level: "O2"
   
   ports:
     - busybox
     - 3d-ascii-viewer
   
   iso:
     name: "neoos-custom-build"
     disk_size: 2G
   ```

2. **TUI workflow:**
   - Launch interactive app
   - User selects kernel version, CPU features (with auto-detect as default), optimization level
   - User selects which ports to include (shows description of each)
   - Preview of final build size and components
   - Option to save configuration to file
   - Proceed to build or export config

3. **Build process:**
   ```
   1. Clone neoos-kernel (at specified version)
   2. For each selected port:
      - Clone neoos-<portname> (at latest or specified version)
      - Run its build (via Makefile or neoos-build.sh)
      - Extract binary to staging area
   3. Build kernel with selected CPU features
   4. Assemble ISO with kernel binary, all port binaries, and INITTAB
   5. Create companion disk images (disk1.img, disk2.img)
   6. Generate metadata.json documenting what was built
   7. Generate qemu-run.sh (executable launcher script)
   ```

4. **Output directory:**
   ```
   build/
   ├── neoos-custom-build.iso         # Bootable ISO
   ├── disk1.img                      # Data disk (e.g., for user files)
   ├── disk2.img                      # Additional storage
   ├── metadata.json                  # Build details (kernel version, ports, options)
   ├── config.yaml                    # Saved configuration (if requested)
   └── qemu-run.sh                    # Ready-to-run QEMU launcher
   ```

5. **QEMU runner script** (`qemu-run.sh`):
   ```sh
   #!/bin/bash
   timeout 180 qemu-system-x86_64 -cpu Nehalem -boot order=d \
     -cdrom neoos-custom-build.iso \
     -drive file=disk1.img,format=raw \
     -drive file=disk2.img,format=raw \
     -display none -no-reboot -serial file:qemu.log
   ```
   User can run this immediately: `./qemu-run.sh`

**CLI usage:**
```sh
# Interactive mode (TUI)
neoos-builder

# Config-driven mode (reproducible)
neoos-builder build config.yaml

# Show what a config would produce
neoos-builder preview config.yaml
```

**Dependency contract with musl repo:**
- Clones `neoos-musl` at specified version (or "latest")
- Runs `make KERNEL_SHIM_DIR=<kernel-shim-path>`
- Expects compiled artifacts at `build-output/include/` and `build-output/lib/libc.a`

**Dependency contract with kernel repo:**
- Clones `neoos-kernel` at specified version (tag or "latest")
- First builds musl (above), then passes musl artifacts to kernel:
  ```sh
  make MUSL_DIR=/path/to/musl/build-output CPU_FEATURES="avx2 sse4_2" OPTIMIZATION_LEVEL=O3
  ```
- Expects binary at `build/neoos.bin`
- Expects disk images at `build/disk.img`, `build/disk2.img`

**Dependency contract with port repos:**
- Clones `neoos-<portname>` at specified version
- Passes musl artifacts to port:
  ```sh
  make MUSL_DIR=/path/to/musl/build-output
  ```
- Expects `.nex` binary in `build/` directory
- Exit code 0 = success

### 3.4 Port Repositories (`neoos-busybox`, `neoos-3d-ascii-viewer`, etc.)

**Responsibility:** Build, maintain, and test one third-party application on NeoOS.

**Structure for each port repo:**
```
neoos-busybox/
├── upstream/                # PRISTINE upstream source (submodule)
├── patches/                 # NeoOS-specific patches
│   ├── 0001-neons-prep.patch
│   └── 0002-remove-x11.patch
├── config/                  # NeoOS-specific configuration files
│   ├── busybox.config       # Configuration for this build
│   └── musl-env.sh          # Environment setup
├── Makefile                 # Primary build interface
├── neoos-build.sh           # Build script (called by OS builder)
├── smoke-test.sh            # Quick validation on NeoOS (run in kernel boot)
├── docs/                    # Port-specific documentation
│   └── PORTING-NOTES.md     # What was changed, why, known limitations
├── .github/workflows/       # CI: build + smoke test
├── README.md                # What this port does, usage notes
└── .gitignore
```

**Build contract (what OS builder expects):**
- Running `make` produces a `.nex` binary in `build/` (e.g., `build/busybox.nex`)
- Binary is statically linked with musl shim
- Binary is self-contained and ready to copy into the final ISO
- Exit code 0 = success

**Standalone usage:**
```sh
# Build musl first (or point to existing build)
git clone https://github.com/NeoOS/neoos-musl ../neoos-musl
cd ../neoos-musl && make

# Then build the port
git clone https://github.com/NeoOS/neoos-busybox
cd neoos-busybox
make MUSL_DIR=../../neoos-musl/build-output
# Produces build/busybox.nex
# User can copy this to a NeoOS filesystem or add to INITTAB
```

**CI/Testing:**
- Build the port
- Run `smoke-test.sh` in a NeoOS kernel boot environment (using the kernel's regression harness)
- Port CI is independent; port failures do NOT block kernel CI
- Kernel regression suite is the gate for kernel releases, NOT for port quality

**Patching strategy:**
- Upstream lives in `upstream/` as a pristine submodule (never edited in place)
- Patches live in `patches/` and are applied at build time via `git apply` or similar
- Version bumps are submodule pointer moves, not merges
- This mirrors `third_party/shim` pattern from the kernel repo

**Known limitations and divergences:**
- Document in `docs/PORTING-NOTES.md`:
  - What NeoOS features the port uses
  - What POSIX features are unavailable or differ
  - Any workarounds applied
  - Which musl functions are used and whether they're tested

### 3.5 Documentation Repository (`neoos-docs`)

**Responsibility:** Central coordination hub for the organization: architecture, porting guide, API reference, port catalog.

**Contains:**
- Docusaurus config and build system
- **Getting Started:**
  - Clone kernel and build locally
  - Clone OS builder and create custom image
  - Boot on real hardware (coming later) vs. QEMU
- **Kernel Development Guide:**
  - Subsystem overview (scheduler, memory, VFS, drivers, etc.)
  - How to add a new syscall
  - How to add a new driver
  - Regression testing and debugging
  - Code review guidelines
- **Porting Guide:**
  - Step-by-step: set up upstream submodule, apply patches, configure musl shim
  - Common pitfalls (static linking, musl limitations, NeoOS divergences)
  - Testing a port in isolation
  - Performance tuning
- **API Reference:**
  - Syscall table (with NeoOS numbers and descriptions)
  - Mapping of Linux syscall numbers to NeoOS (for musl shim developers)
  - libneoos functions
  - musl compatibility layer
- **Architecture Deep Dives:**
  - Scheduler (preemption, work stealing, per-CPU queues)
  - Memory management (paging, buddy allocator, demand paging)
  - VFS (vnode cache, mount points, FAT16/32)
  - Signals (delivery, nesting, job control)
  - Each includes trade-off rationale from the spec documents
- **Port Catalog:**
  - List of all ports in the organization
  - Description and link to each repo
  - Compatibility notes ("uses futex", "requires SSE4.2", etc.)
  - Build and test status badges from CI
- **Build Infrastructure & Conventions:**
  - Standard Makefile targets across all repos
  - CI patterns and recommended GitHub Actions
  - How to release a new kernel version
  - Troubleshooting build failures
- **ABI Compatibility:**
  - Current status (what Linux ABIs are implemented, what diverges and why)
  - Kept in sync with `neoos-kernel/docs/abi-compatibility.md`

**Does NOT contain:**
- Kernel source
- Port source
- OS builder source
- Any implementation code

**CI/Deployment:**
- On every push: build Docusaurus static site
- Deploy to GitHub Pages: `neoos.github.io`

**Sync with kernel repo:**
- Porting guide links to example repos (GitHub links)
- Architecture deep-dives reference the kernel source (GitHub line links)
- ABI compatibility is synced periodically or linked directly from kernel repo
- No hard copy of data that lives in the kernel repo (avoid divergence)

## 4. Build Contracts and Conventions

To enable the OS builder to orchestrate kernel + ports, all repos follow these standards:

### 4.1 Kernel Repo Makefile

**Required targets:**
```makefile
make              # Build kernel to build/neoos.bin
make iso          # Create ISO at build/neoos.iso
make disk-image   # Create disk images at build/disk{1,2}.img
make test         # Run regression suite headless in QEMU
make run          # Boot kernel in QEMU with display (for development)
make clean        # Remove build/
```

**Environment variables for OS builder:**
```makefile
CPU_FEATURES      # Space-separated list or "auto" (e.g., "avx2 sse4_2")
OPTIMIZATION_LEVEL # O0, O1, O2, O3
```

Example invocation from OS builder:
```sh
make CPU_FEATURES="avx2 sse4_2" OPTIMIZATION_LEVEL=O2
```

### 4.2 Musl Repo Makefile

**Required targets:**
```makefile
make              # Build musl with shim, output to build-output/
make clean        # Remove build-output/
```

**Environment variables:**
```makefile
KERNEL_SHIM_DIR   # Path to neoos-kernel/third_party/shim/ (default: ../neoos-kernel/third_party/shim)
```

**Output contract:**
- Must produce `build-output/include/` with musl headers
- Must produce `build-output/lib/libc.a` (static library)
- Header `<sys/syscall.h>` must define NeoOS syscall numbers (via integrated shim)
- Exit code 0 = success

**Example invocation:**
```sh
make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim
# Produces: build-output/include/ and build-output/lib/libc.a
```

### 4.3 Port Repo Makefile

**Required targets:**
```makefile
make              # Build port to build/<portname>.nex
make clean        # Remove build/
```

**Environment variables:**
```makefile
MUSL_DIR          # Path to neoos-musl/build-output/ (default: ../neoos-musl/build-output)
```

**Output contract:**
- Must produce exactly one `.nex` binary in `build/`
- Binary must be statically linked (musl with shim)
- Binary must be self-contained (no runtime dependencies)
- Exit code 0 = success

**Optional targets:**
```makefile
make smoke-test   # Quick validation (run on NeoOS kernel boot)
```

**Example invocation (standalone):**
```sh
cd neoos-busybox
make MUSL_DIR=../neoos-musl/build-output
# Produces: build/busybox.nex
```

**Example invocation from OS builder:**
```sh
cd neoos-busybox
make MUSL_DIR=/path/to/built/musl
# Verifies: build/busybox.nex exists and is executable
```

### 4.4 Ports Repo smoke-test.sh

**Contract:** When run inside a NeoOS boot environment (via kernel's smoke-test harness), validates the port works correctly.

**Example:**
```bash
#!/bin/bash
# busybox smoke test
/busybox echo "hello" > /tmp/test.txt
[ -f /tmp/test.txt ] || exit 1
[ "$(cat /tmp/test.txt)" = "hello" ] || exit 1
exit 0
```

Exit code 0 = test passed, reported in serial log.

## 5. Migration Strategy

**Timeline:** Coordinated with network stack agent (TCP/IP/ICMP work completion).

### Phase 1: Repository Setup (before any code changes)
1. Create GitHub organization `NeoOS`
2. Create empty repos: `neoos-kernel`, `neoos-musl`, `neoos-os-builder`, `neoos-docs`, `neoos-busybox` (port template)
3. Set up organization teams and permissions
4. Document migration plan in `neoos-docs`

### Phase 2: Kernel & Musl Repository Migration

**2a. Create musl repository:**
1. Extract `third_party/musl/` and `third_party/shim/` history from monorepo
2. Create `neoos-musl` repo with:
   - `upstream/` (musl submodule)
   - `Makefile` that integrates shim and builds musl
   - CI to build and test
3. Test: `make KERNEL_SHIM_DIR=../neoos-kernel/third_party/shim` works

**2b. Migrate kernel repository:**
1. Clone current monorepo with full history
2. Keep `third_party/shim/` in kernel repo (it's kernel-specific)
3. Remove `third_party/musl/` (now separate in `neoos-musl`)
4. Remove `ports/` directory
5. Update kernel `Makefile` to link against `neoos-musl` artifacts
6. Test: kernel can be built with separate musl repo
7. Set up CI to run `make test` on every push
8. Verify build and tests pass on fresh clone with musl repo available

### Phase 3: Port Repository Migration (one at a time)
For each port in current `ports/`:
1. Create new repo `neoos-<portname>`
2. Extract port's git history from monorepo (one-time operation)
3. Set up `upstream/` as submodule pointing to original upstream project
4. Verify clean build: `make` produces `.nex` binary
5. Set up CI: build + smoke test
6. Push to new repo
7. Add to `neoos-docs` port catalog with link

### Phase 4: OS Builder Implementation
1. Implement TUI app (Python or Rust, TBD)
2. Implement config file parser
3. Implement Makefile orchestration to:
   - Clone kernel repo at specified version
   - Clone port repos for selected ports
   - Build each (respecting build contracts)
   - Assemble ISO with kernel + ports
   - Generate metadata.json and qemu-run.sh
4. Test with multiple configurations
5. Push to `neoos-os-builder`

### Phase 5: Documentation Repository
1. Initialize Docusaurus site structure
2. Write getting-started guides
3. Convert existing kernel documentation (stdlib.md, abi-compatibility.md, etc.) to Docusaurus pages
4. Write porting guide with examples
5. Create port catalog and link all migrated ports
6. Set up GitHub Pages deployment
7. Push to `neoos-docs`

### Phase 6: Cutover
1. Archive or deprecate original monorepo (keep for reference if needed)
2. Update GitHub profile to point to organization
3. Update README in each new repo to cross-reference the others
4. Document for users:
   - "To build kernel: clone `neoos-kernel`"
   - "To build custom OS: clone `neoos-os-builder`"
5. All new development happens on new repos

**No disruption to current work:** Network stack agent continues on current repo until Phase 2 is complete; then pulls latest `neoos-kernel` repo.

## 6. Rationale & Trade-offs

### Why split at all?

**Problem solved:** Today, a broken port blocks kernel CI and development. A kernel regression breaks all ports at once. This slows down both streams.

**Alternative rejected:** Keep everything in one repo but use CI branching to test ports separately. Cost: complex CI logic, still tight coupling, doesn't scale to many ports.

### Why is the kernel repo completely independent?

**Goal:** Kernel developers should never need to think about the OS builder or ports. A fresh clone should work with no external setup.

**Alternative rejected:** Have kernel repo depend on OS builder for build logic. Cost: breaks the "independent" goal and couples the kernel to UI/UX decisions about the builder.

### Why orchestration via Makefile (not Python or Rust)?

**Choice:** The OS builder is written in Python or Rust for the TUI, but orchestration logic (clone repos, run builds, assemble ISO) uses a Makefile so it's transparent and portable.

**Alternative rejected:** Embed all orchestration in Python/Rust. Cost: opaque to users, harder to debug, harder to extend.

### Why is each port a separate repo?

**Goal:** Ports move at their own pace. A port maintainer doesn't have to wait for kernel releases.

**Alternative rejected:** Keep ports in kernel repo but separate directories. Cost: still tight coupling via CI, harder to onboard external contributors.

### Why is musl in a separate repo?

**Goal:** Consistency across kernel and all ports. A single canonical build of musl + shim that everyone uses ensures the syscall interface is consistent.

**How it works:** `neoos-musl` clones the kernel repo to get the shim, builds musl with the shim integrated, and produces headers + library. Kernel and ports then link against these artifacts.

**Alternative rejected:** Keep musl in kernel repo and have ports clone it. Cost: musl is built multiple times, harder to ensure consistency, duplication of build logic.

### Why Docusaurus for docs?

**Choice:** Centralized, searchable, familiar (many open-source projects use it), supports versioning across kernel releases.

**Alternative rejected:** Keep docs scattered in individual repos. Cost: users have to hunt for information, harder to find relationships between components.

## 7. Open Questions & Future Decisions

- **TUI implementation language:** Python (easy to iterate) vs. Rust (faster, single binary). To be decided when implementing Phase 4.
- **Port versioning:** Should ports be locked to kernel versions, or can they float independently? Initial assumption: independent with documented compatibility ranges.
- **Release cadence:** When do we release a "NeoOS v1.0" that includes kernel + blessed ports? To be decided after Phase 6.
- **External contributors:** How do we onboard maintainers for individual ports? Documented in porting guide (Phase 5).

## 8. Success Criteria

The migration is complete and successful when:

1. ✓ `neoos-musl` can be cloned and `make` produces compiled headers and library with shim integrated
2. ✓ `neoos-kernel` can be cloned and `make test` passes (with musl repo cloned nearby)
3. ✓ All current ports are in separate repos and build independently against musl
4. ✓ `neoos-os-builder` can build a custom image with selected ports in under 10 minutes
5. ✓ Output ISO boots and includes all selected ports in the filesystem
6. ✓ Docusaurus site is live and documents the architecture and porting process
7. ✓ No disruption to ongoing kernel development (network stack agent continues uninterrupted)

---

## References

- Current monorepo: `https://github.com/Neo-vortex/NeoOS`
- Project conventions: `/CLAUDE.md` (kernel-library interface, ABI compatibility, development workflow)
- Existing milestones and roadmap: `docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`
