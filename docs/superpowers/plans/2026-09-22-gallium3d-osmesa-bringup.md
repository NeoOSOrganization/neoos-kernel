# Gallium3D OSMesa Bring-up Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cross-compile Mesa's OSMesa target (real OpenGL API + `softpipe`
software pipe driver, no windowing system) for NeoOS, and prove it by
rendering one real triangle off-screen and reading back correct pixels
under headless QEMU.

**Architecture:** A new sibling repo, `neoos-mesa`, follows the existing
`neoos-<port>` convention (vendored upstream submodule, `build.sh`,
`build-output/`) but is built with the **hosted**
`x86_64-neoos-linux-musl` GCC/G++ toolchain via **Meson** (Mesa's only
supported build system) instead of the freestanding `x86_64-elf-*`
toolchain every prior C/C++ port has used. A NeoOS-side test program
links against the resulting `libOSMesa.a`, draws a triangle through the
real `glBegin`/`glVertex2f`/`glColor3f` API, and asserts specific pixel
values in the read-back buffer.

**Tech Stack:** Mesa (Meson build), the hosted `x86_64-neoos-linux-musl`
GCC 9.4.0/G++ toolchain (`~/opt/cross-x86_64-neoos`), `softpipe` (no
LLVM), OSMesa, Meson 1.10.1 + Ninja 1.13.2 (host tools).

**Spec:**
`docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`

## Global Constraints

- Toolchain: `x86_64-neoos-linux-musl-{gcc,g++,ar,strip}` at
  `~/opt/cross-x86_64-neoos/bin`, **not** `x86_64-elf-*`. Any C library
  this plan ends up building as a Mesa dependency (zlib, expat) MUST
  also use this hosted toolchain, not the freestanding one every prior
  port used — mixing toolchains produces ABI-incompatible objects.
- Meson options are fixed by the spec: `-Dosmesa=true
  -Dgallium-drivers=softpipe -Dplatforms= -Dglx=disabled
  -Degl=disabled -Dgbm=disabled -Dvulkan-drivers=
  -Dllvm=disabled -Dshader-cache=disabled`.
- No windowing system integration, no `llvmpipe`, no change to
  `neoos-wm`/TinyGL in this plan — all explicitly out of scope per the
  spec's Non-goals.
- zlib/expat are ported ONLY if Mesa's configure step actually reports
  them as hard requirements with the option set above. Do not
  pre-emptively port either.
- Every boot-verification step uses this project's standard headless
  QEMU + serial-log-capture convention (`-serial file:...`, checked for
  `PANIC`/`FAILED`/an explicit pass marker), matching every existing
  port's own test tasks (see `docs/superpowers/plans/2026-09-06-curl-port.md`
  Task 4 for the exact established pattern this plan reuses).

---

## Task 1: `neoos-mesa` repo scaffold and vendored Mesa

**Files:**
- Create (new repo `neoos-mesa`, cloned to
  `/home/neo/projects/personal/neoos-mesa`): `README.md`, `.gitmodules`,
  `upstream/` (git submodule)

**Interfaces:**
- Consumes: nothing.
- Produces: `neoos-mesa/upstream/` — a Mesa checkout pinned to a
  specific tag, available to every later task's build commands.

- [ ] **Step 1: Create the repo and vendor Mesa as a submodule**

```bash
mkdir -p /home/neo/projects/personal/neoos-mesa
cd /home/neo/projects/personal/neoos-mesa
git init
git submodule add https://gitlab.freedesktop.org/mesa/mesa.git upstream
cd upstream
git fetch --tags
git checkout mesa-22.3.5
cd ..
git add .gitmodules upstream
git commit -m "vendor Mesa 22.3.5 as a submodule"
```

Expected: `git -C upstream describe --tags` prints `mesa-22.3.5`. Mesa
22.3.5 is picked because it predates the C++20/newer-GCC baseline bump
later Mesa releases made for hardware drivers this project doesn't use,
and is comfortably within GCC 9.4.0's C++17 support.

- [ ] **Step 2: Verify the pinned tag actually configures under this
  toolchain's C++ mode before going further**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
echo 'int main(){}' > /tmp/cxx17check.cpp
x86_64-neoos-linux-musl-g++ -std=c++17 -c /tmp/cxx17check.cpp -o /tmp/cxx17check.o && echo "toolchain C++17 OK"
```

Expected: `toolchain C++17 OK`. (This just re-confirms the toolchain
precondition the spec already verified — cheap, and worth re-checking
now that it's this repo's own load-bearing assumption.) If Mesa
22.3.5's own `meson.build` later (Task 3) rejects this compiler
version outright, fall back to `mesa-21.3.9` instead — re-run Step 1
with that tag — before proceeding.

- [ ] **Step 3: Write the README documenting scope**

```bash
cat > README.md <<'EOF'
# neoos-mesa

Sub-project 1 of the Gallium3D-on-NeoOS goal (see NeoOS's own
`docs/project-goal.md`): Mesa's OSMesa target (real OpenGL API +
`softpipe` software pipe driver), cross-compiled for NeoOS with the
hosted `x86_64-neoos-linux-musl` toolchain.

No windowing-system integration yet -- `test/triangle_test.c` renders
off-screen via OSMesa's own API. See
`docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`
in the neoos-kernel repo for the full design.
EOF
git add README.md
git commit -m "docs: scope README"
```

- [ ] **Step 4: Commit**

Already committed per-step above; verify clean tree:

```bash
git status
```

Expected: `nothing to commit, working tree clean`.

---

## Task 2: Meson cross-file and first configure attempt

**Files:**
- Create: `neoos-mesa/cross-file.txt`
- Create: `neoos-mesa/build.sh` (the standard per-port build wrapper;
  this task only needs it to run the configure step, later tasks
  extend it)

**Interfaces:**
- Consumes: `upstream/` (Task 1), the hosted toolchain on `$PATH`.
- Produces: a `build-tmp/` Meson build directory whose configure-time
  log (`build-tmp/meson-logs/meson-log.txt`) Task 3/4's conditional
  branch reads to decide whether zlib/expat are required.

- [ ] **Step 1: Write the Meson cross-file**

```bash
cd /home/neo/projects/personal/neoos-mesa
cat > cross-file.txt <<'EOF'
[binaries]
c = '/home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-gcc'
cpp = '/home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-g++'
ar = '/home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-gcc-ar'
strip = '/home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-strip'
pkg-config = '/usr/bin/pkg-config'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
git add cross-file.txt
git commit -m "build: Meson cross-file for x86_64-neoos-linux-musl"
```

- [ ] **Step 2: Install the one host-side build prerequisite Mesa's
  build needs**

```bash
pip install --user mako
python3 -c "import mako; print('mako OK')"
```

Expected: `mako OK`. (`python3`, `bison`, `flex` are already present on
this host — confirmed during design.)

- [ ] **Step 3: Write `build.sh`'s configure step**

```bash
cat > build.sh <<'EOF'
#!/bin/bash
set -e

PREFIX="${PREFIX:-$(pwd)/build-output}"
BUILD_TMP="${BUILD_TMP:-$(pwd)/build-tmp}"

if [ ! -f upstream/meson.build ]; then
    echo "Error: upstream Mesa checkout not found (git submodule update --init)" >&2
    exit 1
fi

mkdir -p "$PREFIX"
rm -rf "$BUILD_TMP"

meson setup "$BUILD_TMP" upstream \
    --cross-file="$(pwd)/cross-file.txt" \
    --prefix="$PREFIX" \
    --default-library=static \
    -Dosmesa=true \
    -Dgallium-drivers=softpipe \
    -Dplatforms= \
    -Dglx=disabled \
    -Degl=disabled \
    -Dgbm=disabled \
    -Dvulkan-drivers= \
    -Dllvm=disabled \
    -Dshader-cache=disabled

echo "OK: Meson configure succeeded -- see build-tmp/meson-logs/meson-log.txt"
EOF
chmod +x build.sh
git add build.sh
git commit -m "build: configure step for the OSMesa/softpipe minimal build"
```

- [ ] **Step 4: Run the configure step and read the result**

```bash
cd /home/neo/projects/personal/neoos-mesa
git submodule update --init
./build.sh
```

Three possible outcomes, handled as follows:

- **Configure succeeds** (`OK: Meson configure succeeded`): proceed
  directly to Task 5 (skip Tasks 3 and 4 — zlib/expat weren't needed).
- **Configure fails citing `zlib` as a required dependency** (grep
  `build-tmp/meson-logs/meson-log.txt` for `zlib`): do Task 3, then
  retry this step.
- **Configure fails citing `expat` as a required dependency** (grep
  the same log for `expat`): do Task 4, then retry this step.

Both can also be true at once — do both Task 3 and Task 4 before
retrying.

---

## Task 3 (conditional — only if Task 2 required it): Port zlib

**Files:**
- Create (new repo `neoos-zlib`): `Makefile`, `build.sh`,
  `build-output/`, `upstream/` (git submodule)

**Interfaces:**
- Consumes: nothing beyond the hosted toolchain.
- Produces: `neoos-zlib/build-output/lib/libz.a`,
  `neoos-zlib/build-output/include/zlib.h`, `zconf.h` — consumed by
  Task 2's retried configure step via
  `PKG_CONFIG_PATH`/`-Dzlib` search paths (see Step 3 below).

- [ ] **Step 1: Vendor zlib**

```bash
mkdir -p /home/neo/projects/personal/neoos-zlib
cd /home/neo/projects/personal/neoos-zlib
git init
git submodule add https://github.com/madler/zlib.git upstream
cd upstream && git checkout v1.3.1 && cd ..
git add .gitmodules upstream
git commit -m "vendor zlib 1.3.1"
```

- [ ] **Step 2: Cross-compile with the hosted toolchain**

zlib's own `configure` script is a plain shell script that honours
`CC`/`AR`/`prefix` directly (no CMake/autotools cross-file needed):

```bash
cat > build.sh <<'EOF'
#!/bin/bash
set -e
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
PREFIX="${PREFIX:-$(pwd)/build-output}"
mkdir -p "$PREFIX"
cd upstream
CC=x86_64-neoos-linux-musl-gcc AR=x86_64-neoos-linux-musl-ar \
    ./configure --static --prefix="$PREFIX"
make clean 2>/dev/null || true
make -j"$(nproc)"
make install
EOF
chmod +x build.sh
./build.sh
```

Expected: `build-output/lib/libz.a` and `build-output/include/zlib.h`
exist.

- [ ] **Step 3: Commit and point Task 2's retry at it**

```bash
git add build.sh
git commit -m "build: cross-compile for x86_64-neoos-linux-musl"
```

Back in `neoos-mesa`, Task 2's `build.sh` gains, before the `meson
setup` line:

```bash
export PKG_CONFIG_PATH="/home/neo/projects/personal/neoos-zlib/build-output/lib/pkgconfig:$PKG_CONFIG_PATH"
export PKG_CONFIG_LIBDIR="/home/neo/projects/personal/neoos-zlib/build-output/lib/pkgconfig"
```

(zlib's own `make install` generates a `zlib.pc` under
`lib/pkgconfig/` — this is what lets Meson's `dependency('zlib')`
resolve it during a cross build without a system zlib being findable.)

---

## Task 4 (conditional — only if Task 2 required it): Port libexpat

**Files:**
- Create (new repo `neoos-expat`): `Makefile`, `build.sh`,
  `build-output/`, `upstream/` (git submodule)

**Interfaces:**
- Consumes: nothing beyond the hosted toolchain.
- Produces: `neoos-expat/build-output/lib/libexpat.a`,
  `neoos-expat/build-output/include/expat.h` — consumed the same way
  as Task 3's zlib, via `PKG_CONFIG_PATH`.

- [ ] **Step 1: Vendor expat**

```bash
mkdir -p /home/neo/projects/personal/neoos-expat
cd /home/neo/projects/personal/neoos-expat
git init
git submodule add https://github.com/libexpat/libexpat.git upstream
cd upstream && git checkout R_2_6_2 && cd ..
git add .gitmodules upstream
git commit -m "vendor libexpat 2.6.2"
```

- [ ] **Step 2: Cross-compile with the hosted toolchain**

expat uses CMake. This repo follows the same `toolchain.cmake` pattern
`neoos-curl`/`neoos-libssh2` already established, retargeted at the
hosted toolchain:

```bash
cat > toolchain.cmake <<'EOF'
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER /home/neo/opt/cross-x86_64-neoos/bin/x86_64-neoos-linux-musl-gcc)
set(CMAKE_FIND_ROOT_PATH /home/neo/opt/cross-x86_64-neoos/x86_64-neoos-linux-musl)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF

cat > build.sh <<'EOF'
#!/bin/bash
set -e
PREFIX="${PREFIX:-$(pwd)/build-output}"
BUILD_TMP="${BUILD_TMP:-$(pwd)/build-tmp}"
mkdir -p "$PREFIX"
rm -rf "$BUILD_TMP"
cmake -S upstream/expat -B "$BUILD_TMP" \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/toolchain.cmake" \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_EXAMPLES=OFF \
    -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_SHARED_LIBS=OFF
cmake --build "$BUILD_TMP" -j"$(nproc)"
cmake --install "$BUILD_TMP"
EOF
chmod +x build.sh
./build.sh
```

Expected: `build-output/lib/libexpat.a` and
`build-output/include/expat.h` exist.

- [ ] **Step 3: Commit and point Task 2's retry at it**

```bash
git add toolchain.cmake build.sh
git commit -m "build: cross-compile for x86_64-neoos-linux-musl"
```

Back in `neoos-mesa`, Task 2's `build.sh` gains the same
`PKG_CONFIG_PATH` treatment as zlib, pointed at
`/home/neo/projects/personal/neoos-expat/build-output/lib/pkgconfig`.

---

## Task 5: Build and verify `libOSMesa.a`

**Files:**
- Modify: `neoos-mesa/build.sh` (add the `ninja`/install step after
  the configure step Task 2 wrote)

**Interfaces:**
- Consumes: `build-tmp/` (Task 2, successfully configured).
- Produces: `neoos-mesa/build-output/lib/libOSMesa.a`,
  `neoos-mesa/build-output/include/GL/osmesa.h`,
  `neoos-mesa/build-output/include/GL/gl.h` — consumed by Task 6's
  test program.

- [ ] **Step 1: Extend `build.sh` with the build+install step**

```bash
cd /home/neo/projects/personal/neoos-mesa
cat >> build.sh <<'EOF'

ninja -C "$BUILD_TMP"
ninja -C "$BUILD_TMP" install

if [ -f "$PREFIX/lib/libOSMesa.a" ]; then
    echo "OK: libOSMesa.a built at $PREFIX/lib/libOSMesa.a"
else
    echo "ERROR: build finished but libOSMesa.a not found" >&2
    exit 1
fi
EOF
git add build.sh
git commit -m "build: ninja build+install step, verify libOSMesa.a"
```

- [ ] **Step 2: Run the full build**

```bash
./build.sh
```

Expected: `OK: libOSMesa.a built at .../build-output/lib/libOSMesa.a`.

- [ ] **Step 3: Confirm the library actually exports OSMesa's public
  symbols (catches a silently-empty or misconfigured build)**

```bash
x86_64-neoos-linux-musl-nm build-output/lib/libOSMesa.a | grep -w T | grep -i "OSMesaCreateContext\|OSMesaMakeCurrent"
```

Expected: both symbols listed as defined (`T`) somewhere in the
archive.

- [ ] **Step 4: Confirm the headers landed where Task 6 expects them**

```bash
ls build-output/include/GL/osmesa.h build-output/include/GL/gl.h
```

Expected: both files exist.

---

## Task 6: The proof-of-life triangle test program

**Files:**
- Create: `neoos-mesa/test/triangle_test.c`
- Create: `neoos-mesa/test/Makefile`

**Interfaces:**
- Consumes: `build-output/lib/libOSMesa.a`, `build-output/include/`
  (Task 5).
- Produces: `neoos-mesa/test/build/triangle_test.nex` — consumed by
  Task 7's boot test.

- [ ] **Step 1: Write the test program**

```bash
mkdir -p /home/neo/projects/personal/neoos-mesa/test
cat > /home/neo/projects/personal/neoos-mesa/test/triangle_test.c <<'EOF'
/* neoos-mesa proof of life: draw one triangle through the real OpenGL
 * API (OSMesa's off-screen context, softpipe underneath), and verify
 * specific pixels in the read-back buffer -- not just "didn't crash".
 *
 * Fixed-function glBegin/glVertex2f is used deliberately for this
 * first proof: even the compatibility profile is rasterized through
 * Mesa's real internal pipeline (translated to shaders internally by
 * st/mesa), so it already exercises softpipe/TGSI genuinely, while
 * avoiding GLSL toolchain issues as a variable in this first test.
 */
#include <GL/osmesa.h>
#include <GL/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 64
#define H 64

int main(void) {
    OSMesaContext ctx = OSMesaCreateContext(OSMESA_RGBA, NULL);
    if (!ctx) {
        printf("FAILED: OSMesaCreateContext returned NULL\n");
        return 1;
    }

    unsigned char *buffer = malloc((size_t)W * H * 4);
    if (!buffer) {
        printf("FAILED: malloc failed\n");
        return 1;
    }
    memset(buffer, 0, (size_t)W * H * 4);

    if (!OSMesaMakeCurrent(ctx, buffer, GL_UNSIGNED_BYTE, W, H)) {
        printf("FAILED: OSMesaMakeCurrent failed\n");
        return 1;
    }

    glViewport(0, 0, W, H);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glBegin(GL_TRIANGLES);
    glColor3f(1.0f, 0.0f, 0.0f);
    glVertex2f(0.0f, 0.8f);
    glVertex2f(-0.8f, -0.8f);
    glVertex2f(0.8f, -0.8f);
    glEnd();

    glFinish();

    /* Center of the buffer sits inside the triangle -- must be red. */
    int cx = W / 2, cy = H / 2;
    unsigned char *px = &buffer[(size_t)(cy * W + cx) * 4];
    printf("center pixel: r=%d g=%d b=%d a=%d\n", px[0], px[1], px[2], px[3]);
    if (!(px[0] > 200 && px[1] < 50 && px[2] < 50)) {
        printf("FAILED: center pixel is not red (got r=%d g=%d b=%d)\n",
               px[0], px[1], px[2]);
        return 1;
    }

    /* Top-left corner sits outside the triangle -- must stay the clear
     * color (black). */
    unsigned char *corner = &buffer[(size_t)(2 * W + 2) * 4];
    printf("corner pixel: r=%d g=%d b=%d a=%d\n",
           corner[0], corner[1], corner[2], corner[3]);
    if (corner[0] != 0 || corner[1] != 0 || corner[2] != 0) {
        printf("FAILED: corner pixel is not clear color (got r=%d g=%d b=%d)\n",
               corner[0], corner[1], corner[2]);
        return 1;
    }

    printf("PASS neoos-mesa-triangle: real OpenGL triangle rendered correctly\n");
    OSMesaDestroyContext(ctx);
    free(buffer);
    return 0;
}
EOF
```

- [ ] **Step 2: Write the test Makefile**

```bash
cat > /home/neo/projects/personal/neoos-mesa/test/Makefile <<'EOF'
CC := x86_64-neoos-linux-musl-gcc
OSMESA_DIR := ../build-output
CFLAGS := -O2 -Wall -I$(OSMESA_DIR)/include
LDFLAGS := -static -L$(OSMESA_DIR)/lib -lOSMesa -lm -lpthread

.PHONY: all clean
all: build/triangle_test.nex

build/triangle_test.nex: triangle_test.c
	mkdir -p build
	$(CC) $(CFLAGS) triangle_test.c -o $@ $(LDFLAGS)

clean:
	rm -rf build
EOF
git -C /home/neo/projects/personal/neoos-mesa add test/triangle_test.c test/Makefile
git -C /home/neo/projects/personal/neoos-mesa commit -m "test: OSMesa triangle proof-of-life program"
```

- [ ] **Step 3: Build the host-native version first, to isolate test
  logic bugs from cross-compilation/NeoOS-boot bugs**

```bash
cd /home/neo/projects/personal/neoos-mesa/test
cc -O2 -Wall -I../build-output/include triangle_test.c \
   -o /tmp/triangle_test_host -L../build-output/lib -lOSMesa -lm -lpthread \
   2>&1 | head -40 || true
```

This is expected to possibly fail (the library was cross-compiled for
NeoOS, not the host) — that's fine and expected; the real point of
this step is a quick syntax/logic sanity read of the compiler's
output, not a working host binary. If the host toolchain flags a real
bug in `triangle_test.c` itself (not a linkage mismatch), fix it here
before cross-compiling.

- [ ] **Step 4: Cross-compile for NeoOS**

```bash
export PATH="$HOME/opt/cross-x86_64-neoos/bin:$PATH"
cd /home/neo/projects/personal/neoos-mesa/test
make
```

Expected: `build/triangle_test.nex` exists, no linker errors.

- [ ] **Step 5: Copy into the port's `build-output` so `PORT_DIRS`
  picks it up (Task 7)**

```bash
cp build/triangle_test.nex ../build-output/triangle_test.nex
```

(Matches the exact layout `curl.nex` uses at the top level of
`build-output/` for `PORT_DIRS` — see `docs/stdlib.md`'s sibling port
convention and the Makefile's `PORT_DIRS` rule.)

---

## Task 7: Boot verification under headless QEMU

**Files:**
- None created in `neoos-kernel` beyond throwaway build artifacts
  under `build/` (not committed).

**Interfaces:**
- Consumes: `neoos-mesa/build-output/triangle_test.nex` (Task 6).
- Produces: nothing for later tasks — this is the sub-project's actual
  proof of done.

- [ ] **Step 1: Build the disk image with the test binary installed
  via `PORT_DIRS`**

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    PORT_DIRS="mesa=../neoos-mesa/build-output" iso disk-image
```

Expected: `disk: PORT_DIRS=mesa=../neoos-mesa/build-output` in the
output, and `triangle_test.nex` copied to
`::usr/local/bin/triangle_test.nex`.

- [ ] **Step 2: Boot with a custom inittab that runs the test directly**

```bash
printf 'wait /usr/local/bin/triangle_test.nex\n' > /tmp/mesa-triangle-inittab
mcopy -o -i build/disk.img /tmp/mesa-triangle-inittab ::etc/inittab
timeout 30 qemu-system-x86_64 -cpu Nehalem -boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -no-reboot -display none -serial file:build/mesa-triangle-test.log
cat build/mesa-triangle-test.log
```

Expected: `PASS neoos-mesa-triangle: real OpenGL triangle rendered
correctly` appears in the log, with the printed center-pixel line
showing `r=25` (255, printed value should be near 255 -- exact
antialiasing/rounding is why the test's own threshold check is `>
200`, not `== 255`) `g=0 b=0`ish values, and no `panic`/`exception`/
`halted` line.

- [ ] **Step 3: If it fails, capture enough to diagnose before
  retrying**

```bash
grep -i "panic\|exception\|halted\|FAILED" build/mesa-triangle-test.log
```

A failure here is a real implementation bug to fix in
`triangle_test.c`, `libOSMesa`'s build configuration, or (least
likely, since it worked for every other port) the boot/PORT_DIRS
plumbing — not a plan defect requiring a design change, unless the
failure reveals a genuine architectural gap (e.g., a missing syscall
`os_*` needs) — see the spec's "Known risks" list. If that happens,
stop and escalate rather than working around it silently: it may mean
the spec's `os_*`-porting-layer assumption was wrong.

---

## Task 8: Full gauntlet regression

**Files:** none.

**Interfaces:**
- Consumes: nothing new.
- Produces: nothing — final regression confirmation.

- [ ] **Step 1: Rebuild the disk normally (no `PORT_DIRS`, no custom
  inittab) and run the standard regression suite**

```bash
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`, zero retries. `neoos-mesa` never
touches kernel code or the shared musl shim, so this run confirms
nothing regressed — the new hosted-toolchain-built binary and the new
`PORT_DIRS` entry (this run doesn't even include it, since `PORT_DIRS`
was omitted) are inert for the standard boot path.

---

## Task 9: Documentation and status update

**Files:**
- Modify: `docs/project-goal.md` (append a status note)
- Modify: `docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`
  (no change expected — verify it still matches what was built; note
  any deviation, e.g. a different Mesa version tag than planned, or
  that zlib/expat were or weren't needed)

**Interfaces:** none — documentation only.

- [ ] **Step 1: Append a status note to `docs/project-goal.md`**

Add, after the existing "What 'Gallium3D working on NeoOS' means"
section:

```markdown
## Status

**Sub-project 1 (OSMesa/softpipe bring-up) complete**, as of
2026-09-22 — see
`docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md`
and `docs/superpowers/plans/2026-09-22-gallium3d-osmesa-bringup.md`.
Mesa's OSMesa target (real `glBegin`/`glVertex2f`/GLSL-capable OpenGL
API, `softpipe` software pipe driver) cross-compiles for NeoOS via the
hosted `x86_64-neoos-linux-musl` toolchain and Meson, and a real
triangle renders off-screen with correct pixel output, verified under
headless QEMU. No application-facing windowing integration exists
yet — that is sub-project 2, not yet brainstormed.
```

- [ ] **Step 2: Note any deviations from the design spec that surfaced
  during implementation**

If Task 1 fell back to a different Mesa tag, or Task 2 needed zlib
and/or expat, add one or two sentences to the design spec's own
"Known limitation carried forward" section recording exactly what
happened and why — matching this project's established convention of
keeping specs honest about what was actually built versus planned
(e.g. how `2026-09-22-wm-tinygl-everywhere-design.md` itself was
annotated as superseded rather than silently abandoned).

- [ ] **Step 3: Commit**

```bash
cd /home/neo/projects/personal/NeoOS
git add docs/project-goal.md docs/superpowers/specs/2026-09-22-gallium3d-osmesa-bringup-design.md
git commit -m "$(cat <<'EOF'
docs: Gallium3D sub-project 1 (OSMesa/softpipe) complete

Mesa's OSMesa target builds and runs on NeoOS via the hosted
x86_64-neoos-linux-musl toolchain, rendering a real OpenGL triangle
off-screen through softpipe. Sub-project 2 (windowing integration) is
next, not yet brainstormed.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-review

**Spec coverage:**
- "A new repo, `neoos-mesa`, cross-compiles Mesa... producing a static
  `libOSMesa`" → Tasks 1, 2, 5.
- "A NeoOS test program links against `libOSMesa`... renders one
  triangle... reads back the pixel buffer" → Task 6.
- "Boots under headless QEMU... asserts specific pixels" → Task 7.
- "If Mesa's minimal build genuinely requires zlib and/or libexpat...
  port them first" → Tasks 3, 4 (conditional, as specified).
- "The full 15/15-zero-retry gauntlet regression stays green" → Task
  8.
- Non-goals (no windowing, no llvmpipe, no wm changes, no invented
  `os_*` layer) → reflected in Global Constraints and never
  contradicted by any task.

**Placeholder scan:** no TBD/TODO; both conditional tasks (3, 4) carry
full, concrete steps rather than deferred descriptions; the Mesa
version fallback (Task 1 Step 2) names the exact alternate tag rather
than saying "pick another version."

**Type/interface consistency:** `libOSMesa.a` (Task 5) →
`test/Makefile`'s `-lOSMesa` (Task 6) → `triangle_test.nex` (Task 6) →
`PORT_DIRS="mesa=../neoos-mesa/build-output"` (Task 7) all reference
the same `build-output/` layout throughout; `OSMesaCreateContext`/
`OSMesaMakeCurrent`/`OSMesaDestroyContext` names are used consistently
in both the design spec's proof-of-life description and Task 6's
actual code.
