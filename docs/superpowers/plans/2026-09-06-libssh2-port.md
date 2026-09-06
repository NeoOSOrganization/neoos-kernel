# libssh2 Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a new repo, `neoos-libssh2`, produces a real, working static
`libssh2.a` for NeoOS, using `neoos-openssl` as its crypto backend —
the SSH client library curl's future `sftp://` support (a later
milestone) will link against.

**Architecture:** `libssh2`'s own CMake build (its officially
supported, recommended path) against a NeoOS cross-compile toolchain
file, pointed at `neoos-musl` and `neoos-openssl`. Verified by a real
SFTP file write/read/verify round trip against a throwaway,
unprivileged `asyncssh`-based SFTP server on the host, reached through
slirp's `10.0.2.2` gateway.

**Tech Stack:** libssh2 1.11.1 (upstream, vendored as a git submodule),
CMake 4.x (already on this host), `neoos-openssl`'s `build-output`,
`neoos-musl`'s `build-output`, the `x86_64-elf-*` cross toolchain,
Python's `asyncssh` (already installed, version 2.21.1) for the
throwaway test server.

**Spec:** `docs/superpowers/specs/2026-09-06-libssh2-port-design.md`

## Global Constraints

- libssh2 1.11.1 only (pinned to tag `libssh2-1.11.1`).
- Static only, one crypto backend: `CRYPTO_BACKEND=OpenSSL` pointed at
  `neoos-openssl`'s `build-output`. No other backend is built.
- No interactive `ssh` CLI, no SSH server — both explicitly out of
  scope (spec §2). This plan produces a LIBRARY only.
- `BUILD_EXAMPLES=OFF BUILD_TESTING=OFF`: libssh2's own examples/tests
  assume a POSIX process/shell environment this repo cannot run
  inside NeoOS. Correctness is verified by a real boot instead (Task 3).
- The throwaway SFTP test server must be unprivileged, need no system
  package installed, and need no root — `asyncssh` (already present on
  this host) rather than a system `sshd`.

---

### Task 1: Scaffold the `neoos-libssh2` repo

**Files (new repo `neoos-libssh2`, cloned to `/home/neo/projects/personal/neoos-libssh2`):**
- Create: `Makefile`
- Create: `build.sh`
- Create: `toolchain.cmake`
- Create: `.gitmodules` (and the `upstream` submodule it declares)
- Create: `README.md`
- Create: `.gitignore`

**Interfaces:**
- Produces: a `make` target `all` Task 2 runs; `build-output/` as the
  directory Task 2/3 read from.

- [ ] **Step 1: Create the GitHub repo**

```bash
gh repo create NeoOSOrganization/neoos-libssh2 --public \
    --description "libssh2 (SSH/SFTP client library) for NeoOS"
```

- [ ] **Step 2: Clone it locally and add the upstream submodule**

```bash
cd /home/neo/projects/personal
git clone git@github.com:NeoOSOrganization/neoos-libssh2.git
cd neoos-libssh2
git submodule add https://github.com/libssh2/libssh2.git upstream
cd upstream
git fetch --tags
git checkout libssh2-1.11.1
cd ..
git add .gitmodules upstream
git commit -m "vendor libssh2 upstream, pinned to libssh2-1.11.1"
```

Expected: `cd upstream && git describe --tags` → `libssh2-1.11.1`.

- [ ] **Step 3: Write `toolchain.cmake`**

```cmake
# NeoOS cross-compile toolchain for CMake-based ports.
#
# CMAKE_SYSTEM_NAME Generic is CMake's own spelling of "freestanding,
# cross-compiling, do not assume a hosted OS" -- the same freestanding
# posture every other userland build in this org already takes with
# -ffreestanding/-nostdlib.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_AR x86_64-elf-ar)
set(CMAKE_RANLIB x86_64-elf-ranlib)
set(CMAKE_C_FLAGS_INIT "-static -nostdlib -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2")

# CMake cross-compiling: find_program searches the HOST (we want the
# host's own cmake/make, not a target one -- there is no target one),
# but find_library/find_path must search ONLY the target root
# (CMAKE_FIND_ROOT_PATH, passed on the command line by build.sh) so a
# host-installed OpenSSL/libssl-dev is never picked up by mistake.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

- [ ] **Step 4: Write `build.sh`**

```sh
#!/bin/bash
set -e

MUSL_DIR="${MUSL_DIR:-../neoos-musl/build-output}"
OPENSSL_DIR="${OPENSSL_DIR:-../neoos-openssl/build-output}"
PREFIX="${PREFIX:-build-output}"
UPSTREAM_DIR="${UPSTREAM_DIR:-upstream}"
BUILD_TMP="${BUILD_TMP:-build-tmp}"

if [ ! -d "$MUSL_DIR/include" ]; then
    echo "Error: musl not found at $MUSL_DIR (build neoos-musl first)" >&2
    exit 1
fi
if [ ! -f "$OPENSSL_DIR/lib/libcrypto.a" ]; then
    echo "Error: OpenSSL not found at $OPENSSL_DIR (build neoos-openssl first)" >&2
    exit 1
fi
if [ ! -f "$UPSTREAM_DIR/CMakeLists.txt" ]; then
    echo "Error: upstream libssh2 checkout not found at $UPSTREAM_DIR" >&2
    exit 1
fi

ABS_PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
ABS_MUSL_DIR="$(cd "$MUSL_DIR" && pwd)"
ABS_OPENSSL_DIR="$(cd "$OPENSSL_DIR" && pwd)"

echo "Building libssh2 for NeoOS..."
rm -rf "$BUILD_TMP"

# CMAKE_FIND_ROOT_PATH restricts find_library/find_path (see
# toolchain.cmake's MODE ONLY settings) to exactly these two prefixes
# -- this, not just OPENSSL_ROOT_DIR alone, is what guarantees a
# host-installed OpenSSL (very likely present on any real dev machine)
# is never found instead of the cross-built one.
cmake -S "$UPSTREAM_DIR" -B "$BUILD_TMP" \
    -DCMAKE_TOOLCHAIN_FILE="$(pwd)/toolchain.cmake" \
    -DCMAKE_FIND_ROOT_PATH="$ABS_OPENSSL_DIR;$ABS_MUSL_DIR" \
    -DCMAKE_INSTALL_PREFIX="$ABS_PREFIX" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DCRYPTO_BACKEND=OpenSSL \
    -DOPENSSL_ROOT_DIR="$ABS_OPENSSL_DIR" \
    -DOPENSSL_INCLUDE_DIR="$ABS_OPENSSL_DIR/include" \
    -DCMAKE_C_FLAGS="-isystem $ABS_MUSL_DIR/include"

cmake --build "$BUILD_TMP" -j"$(nproc)"
cmake --install "$BUILD_TMP"

if [ -f "$PREFIX/lib/libssh2.a" ]; then
    echo ""
    echo "OK libssh2 built successfully at $PREFIX"
    ls -lh "$PREFIX/lib/libssh2.a"
else
    echo "ERROR: build finished but libssh2.a not found" >&2
    exit 1
fi
```

```bash
chmod +x build.sh
```

- [ ] **Step 5: Write the `Makefile`**

```make
# NeoOS libssh2 build

MUSL_DIR ?= ../neoos-musl/build-output
OPENSSL_DIR ?= ../neoos-openssl/build-output
PREFIX ?= build-output
UPSTREAM_DIR ?= upstream

.PHONY: all clean verify help submodule-init

all: build-output/lib/libssh2.a

submodule-init:
	@if [ ! -f "$(UPSTREAM_DIR)/CMakeLists.txt" ]; then \
		echo "Initializing upstream submodule..."; \
		git submodule update --init upstream; \
	fi

build-output/lib/libssh2.a: submodule-init
	@[ -d "$(MUSL_DIR)/include" ] || { \
		echo "Error: musl not found at $(MUSL_DIR) -- build neoos-musl first"; \
		exit 1; \
	}
	@[ -f "$(OPENSSL_DIR)/lib/libcrypto.a" ] || { \
		echo "Error: OpenSSL not found at $(OPENSSL_DIR) -- build neoos-openssl first"; \
		exit 1; \
	}
	@MUSL_DIR="$(MUSL_DIR)" OPENSSL_DIR="$(OPENSSL_DIR)" PREFIX="$(PREFIX)" ./build.sh

clean:
	rm -rf $(PREFIX) build-tmp

verify:
	@if [ -f "$(PREFIX)/lib/libssh2.a" ]; then \
		echo "OK libssh2.a built"; \
	else \
		echo "ERROR libssh2.a not found"; \
		exit 1; \
	fi

help:
	@echo "NeoOS libssh2 build"
	@echo "Usage: make [MUSL_DIR=path] [OPENSSL_DIR=path]"
```

- [ ] **Step 6: Write `README.md`**

```markdown
# NeoOS libssh2

libssh2 1.11.1, built for NeoOS as a static library
(`libssh2.a`) using `neoos-openssl` as its crypto backend. The SSH/SFTP
client library the `neoos-curl` port (a later milestone) will link
against for `sftp://` support.

Library only: no interactive `ssh` CLI (a separate, larger problem --
session/channel/pty/terminal-I/O plumbing on top of this library, not
part of this repo) and no SSH server (an unrelated code path, its own
future milestone).

## Quick Start

Build `neoos-musl` and `neoos-openssl` first, then:

```sh
make MUSL_DIR=../neoos-musl/build-output OPENSSL_DIR=../neoos-openssl/build-output
# Produces: build-output/lib/libssh2.a,
#           build-output/include/libssh2.h, libssh2_sftp.h, ...
```

## Documentation

- **Design spec:** [neoos-kernel's
  docs/superpowers/specs/2026-09-06-libssh2-port-design.md](https://github.com/NeoOSOrganization/neoos-kernel/blob/main/docs/superpowers/specs/2026-09-06-libssh2-port-design.md)

## In This Organization

- **[neoos-kernel](https://github.com/NeoOSOrganization/neoos-kernel)** — Kernel source
- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — musl libc (build dependency)
- **[neoos-openssl](https://github.com/NeoOSOrganization/neoos-openssl)** — OpenSSL, this repo's crypto backend
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Guides and architecture
```

- [ ] **Step 7: Write `.gitignore`**

```
build-output/
build-tmp/
```

- [ ] **Step 8: Commit and push**

```bash
cd /home/neo/projects/personal/neoos-libssh2
git add build.sh Makefile toolchain.cmake README.md .gitignore
git commit -m "scaffold: build.sh, Makefile, toolchain.cmake, README"
git push origin main
```

---

### Task 2: Build libssh2 against `neoos-musl` and `neoos-openssl`

**Files:** none new — this task runs Task 1's `build.sh` for real and
fixes whatever it finds broken.

**Interfaces:**
- Consumes: `neoos-musl`'s `build-output/include`,
  `neoos-openssl`'s `build-output/{include,lib}`.
- Produces: `build-output/lib/libssh2.a`,
  `build-output/include/libssh2.h`, `build-output/include/libssh2_sftp.h`
  — Task 3 and the future curl milestone read these exact paths.

- [ ] **Step 1: Run the build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/neoos-libssh2
make MUSL_DIR=../neoos-musl/build-output OPENSSL_DIR=../neoos-openssl/build-output 2>&1 | tail -80
```

Expected: ends with `OK libssh2 built successfully at build-output`.

- [ ] **Step 2: If it fails, fix forward — do not weaken the target**

Expect the same CLASS of issue the OpenSSL milestone hit (a driver
flag `x86_64-elf-gcc` doesn't recognize, a find_package step that
needs an explicit hint). Specific likely failure points for THIS
build, and their fixes:

- **CMake reports `Could NOT find OpenSSL`**: confirm
  `CMAKE_FIND_ROOT_PATH` (Task 1 Step 4) actually points at the
  absolute path containing `neoos-openssl/build-output/lib/libcrypto.a`
  and `include/openssl/opensslv.h` — `find_package(OpenSSL)` needs
  both under the SAME prefix its `FindOpenSSL.cmake` module expects
  (`<prefix>/include`, `<prefix>/lib`), which matches `neoos-openssl`'s
  actual layout already.
- **A missing libc symbol at link time**: check whether musl genuinely
  lacks it (rare) versus libssh2 wanting a feature this build doesn't
  need — report the exact symbol before deciding anything.
- **A CMake-detected feature test silently produces the wrong answer**
  (e.g. assumes a POSIX feature NeoOS's musl handles differently):
  CMake's `try_compile`/`try_run` checks, like autoconf's, cannot RUN
  a cross-compiled binary — `CMAKE_SYSTEM_NAME Generic` should make
  CMake treat this build as cross-compiling and skip `try_run`-based
  checks in favor of safe defaults, but if a specific check's default
  answer is wrong for NeoOS, report exactly which one before working
  around it.

- [ ] **Step 3: Verify the build output shape**

```bash
cd /home/neo/projects/personal/neoos-libssh2
ls -la build-output/lib/libssh2.a
ls build-output/include/libssh2.h build-output/include/libssh2_sftp.h
x86_64-elf-ar t build-output/lib/libssh2.a | head -5
x86_64-elf-objdump -f build-output/lib/libssh2.a 2>&1 | grep -m1 "architecture"
```

Expected: all paths exist; `x86_64-elf-ar` lists real `.o` members
without error; `objdump` reports `architecture: i386:x86-64`.

- [ ] **Step 4: Commit**

If Step 1 succeeded outright, nothing to commit (`build-output/` is
gitignored). If Step 2's fix-forward required a `build.sh`/
`toolchain.cmake` change, commit that:

```bash
cd /home/neo/projects/personal/neoos-libssh2
git add build.sh toolchain.cmake
git commit -m "build: fix $(whatever Step 2 found)"
git push origin main
```

---

### Task 3: Boot verification — a real SFTP round trip

**Working directory:** the host, for the throwaway SFTP server;
`/home/neo/projects/personal/NeoOS` for the kernel-side steps
(embedding, disk image, QEMU); this session's scratchpad directory for
the NeoOS test program.

**Files:**
- Create (host-side, throwaway, not committed anywhere):
  `sftp_server.py`.
- Create (scratch, not committed anywhere — matching
  `getaddrinfo_test.c`/`tls_test.c`'s own precedent): `sftp_test.c`,
  built into a nexified `sftp_test.nex`.

**Interfaces:**
- Consumes: Task 2's `build-output/lib/libssh2.a`,
  `build-output/include/libssh2.h`/`libssh2_sftp.h`, and
  `neoos-openssl`'s own `build-output` (libssh2.a needs libcrypto.a
  linked alongside it — OpenSSL is libssh2's crypto backend, so any
  consumer links both).
- Produces: nothing later tasks consume — this milestone's final
  proof, matching the DNS resolution and OpenSSL milestones' own
  terminal verification steps.

- [ ] **Step 1: Write the throwaway SFTP server**

```python
#!/usr/bin/env python3
# sftp_server.py -- throwaway, unprivileged SFTP test server. No
# system package installed, no root required (asyncssh is a pure
# Python SSH implementation already present on this host). Torn down
# immediately after Task 3's verification -- no lasting host state.
import asyncio
import os
import sys
import asyncssh

PORT = 2222
USERNAME = "neotest"
PASSWORD = "neotest"
HOST_KEY_PATH = "/tmp/neoos-sftp-test-hostkey"


class TestServer(asyncssh.SSHServer):
    def connection_made(self, conn):
        print(f"[sftp_server] connection from {conn.get_extra_info('peername')}", flush=True)

    def begin_auth(self, username):
        return True  # require auth

    def password_auth_supported(self):
        return True

    def validate_password(self, username, password):
        ok = username == USERNAME and password == PASSWORD
        print(f"[sftp_server] auth attempt user={username} ok={ok}", flush=True)
        return ok


async def main():
    if not os.path.exists(HOST_KEY_PATH):
        key = asyncssh.generate_private_key("ssh-rsa")
        key.write_private_key(HOST_KEY_PATH)

    await asyncssh.create_server(
        TestServer, "", PORT,
        server_host_keys=[HOST_KEY_PATH],
        sftp_factory=asyncssh.SFTPServer,
    )
    print(f"[sftp_server] listening on :{PORT}", flush=True)
    await asyncio.Future()  # run until killed


if __name__ == "__main__":
    asyncio.run(main())
```

- [ ] **Step 2: Start it and confirm it's listening**

```bash
python3 sftp_server.py > /tmp/sftp_server.log 2>&1 &
SFTP_SERVER_PID=$!
sleep 1
grep "listening" /tmp/sftp_server.log
```

Expected: `[sftp_server] listening on :2222`. Keep `$SFTP_SERVER_PID`
noted — Step 8 kills it.

- [ ] **Step 3: Write the NeoOS test program**

```c
// sftp_test.c -- throwaway: proves libssh2.a works end to end on
// NeoOS with a real SFTP round trip. Not committed anywhere, matching
// getaddrinfo_test.c/tls_test.c's own precedent.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <libssh2.h>
#include <libssh2_sftp.h>

#define TEST_STRING "hello from NeoOS over SFTP\n"

int main(void) {
    if (libssh2_init(0) != 0) {
        printf("[sftptest] FAILED: libssh2_init\n");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(2222);
    addr.sin_addr.s_addr = inet_addr("10.0.2.2");   // slirp's host gateway

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&addr, sizeof addr) != 0) {
        printf("[sftptest] FAILED: connect to 10.0.2.2:2222\n");
        return 1;
    }
    printf("[sftptest] TCP connected to 10.0.2.2:2222\n");

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        printf("[sftptest] FAILED: libssh2_session_init\n");
        return 1;
    }

    if (libssh2_session_handshake(session, sock) != 0) {
        printf("[sftptest] FAILED: libssh2_session_handshake\n");
        return 1;
    }
    printf("[sftptest] SSH handshake OK\n");

    if (libssh2_userauth_password(session, "neotest", "neotest") != 0) {
        printf("[sftptest] FAILED: libssh2_userauth_password\n");
        return 1;
    }
    printf("[sftptest] authenticated\n");

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
    if (!sftp) {
        printf("[sftptest] FAILED: libssh2_sftp_init\n");
        return 1;
    }

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(sftp, "/tmp/neoos_sftp_test.txt",
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
    if (!fh) {
        printf("[sftptest] FAILED: sftp_open for write\n");
        return 1;
    }
    if (libssh2_sftp_write(fh, TEST_STRING, sizeof(TEST_STRING) - 1) < 0) {
        printf("[sftptest] FAILED: sftp_write\n");
        return 1;
    }
    libssh2_sftp_close(fh);
    printf("[sftptest] wrote test file\n");

    fh = libssh2_sftp_open(sftp, "/tmp/neoos_sftp_test.txt",
        LIBSSH2_FXF_READ, 0);
    if (!fh) {
        printf("[sftptest] FAILED: sftp_open for read\n");
        return 1;
    }
    char buf[128] = {0};
    ssize_t n = libssh2_sftp_read(fh, buf, sizeof buf - 1);
    libssh2_sftp_close(fh);
    if (n < 0 || strncmp(buf, TEST_STRING, sizeof(TEST_STRING) - 1) != 0) {
        printf("[sftptest] FAILED: read back %ld bytes, mismatch\n", (long)n);
        return 1;
    }

    printf("[sftptest] PASSED: round-trip verified (%ld bytes matched)\n", (long)n);

    libssh2_session_disconnect(session, "done");
    libssh2_session_free(session);
    close(sock);
    libssh2_exit();
    return 0;
}
```

- [ ] **Step 4: Build and nexify it**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
MUSL_DIR=../neoos-musl/build-output
OPENSSL_DIR=../neoos-openssl/build-output
LIBSSH2_DIR=../neoos-libssh2/build-output
SCRATCH="$(pwd)/../libssh2-probe-scratch"   # or this session's actual scratchpad dir
mkdir -p "$SCRATCH"
cp /path/to/sftp_test.c "$SCRATCH/sftp_test.c"   # from Step 3
x86_64-elf-gcc -static -nostdlib -nostdinc -ffreestanding \
  -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
  -isystem "$MUSL_DIR/include" -isystem "$OPENSSL_DIR/include" -isystem "$LIBSSH2_DIR/include" \
  -T userland/user.ld -z noexecstack \
  -o "$SCRATCH/sftp_test.elf" \
  "$MUSL_DIR/lib/crt1.o" "$SCRATCH/sftp_test.c" \
  -Wl,--start-group -L"$LIBSSH2_DIR/lib" -L"$OPENSSL_DIR/lib" -lssh2 -lssl -lcrypto -Wl,--end-group \
  -L"$MUSL_DIR/lib" -lc -lgcc
./tools/nexify.sh "$SCRATCH/sftp_test.elf" "$SCRATCH/sftp_test.nex"
echo '{"category":"bin"}' > "$SCRATCH/sftp_test.test.json"
```

Expected: links successfully. `libssh2.a` needs `libcrypto.a`
symbols (its OpenSSL backend), which is why both are on the link
line — if the linker complains about an undefined `EVP_*`/`RSA_*`
etc. symbol, this is almost certainly link ORDER (archives must
generally follow what references them; `--start-group`/`--end-group`
already handles the circular libssh2↔libcrypto reference case, so a
persisting undefined symbol here means something else — report the
exact symbol before guessing).

- [ ] **Step 5: Build a kernel image embedding it**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
SCRATCH="$(pwd)/../libssh2-probe-scratch"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="$SCRATCH" iso disk-image
```

- [ ] **Step 6: Place resolv.conf and a test inittab**

DNS resolution is NOT exercised by this test (the server address,
`10.0.2.2`, is a fixed numeric slirp gateway address, not a hostname)
— `/etc/resolv.conf` is not needed here, unlike the OpenSSL milestone's
`example.com` lookup.

```bash
cd /home/neo/projects/personal/NeoOS
printf 'wait /bin/sftp_test.nex\n' > /tmp/sftp-inittab
mcopy -o -i build/disk.img /tmp/sftp-inittab ::etc/inittab
```

- [ ] **Step 7: Boot and confirm the real SFTP round trip**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
timeout 40 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -no-reboot -display none -serial file:build/sftp-test.log
grep -i "sftptest\|exception\|panic\|halted" build/sftp-test.log
```

Expected: `[sftptest] TCP connected to 10.0.2.2:2222`, `[sftptest] SSH
handshake OK`, `[sftptest] authenticated`, `[sftptest] wrote test
file`, and `[sftptest] PASSED: round-trip verified (28 bytes
matched)` — a genuine SFTP session against a real, independent SSH
server implementation, with no exception/panic/halted line. Also
check `/tmp/sftp_server.log` on the host for the server's own
`connection from ...` / `auth attempt ... ok=True` lines — both sides
agreeing is stronger evidence than either alone.

- [ ] **Step 8: Tear down the throwaway server**

```bash
kill $SFTP_SERVER_PID
rm -f /tmp/neoos-sftp-test-hostkey /tmp/neoos-sftp-test-hostkey.pub /tmp/sftp_server.log
```

Expected: no lasting host state — matching the spec's own constraint.

- [ ] **Step 9: Confirm the existing gauntlet is unaffected**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
timeout 580 tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15` — this milestone touches no kernel
code, only a new independent repo.

- [ ] **Step 10: Report findings**

No commit for this task in `neoos-kernel` (nothing there changed) or
`neoos-libssh2` (`sftp_test.c`/`.nex`/`sftp_server.py` are throwaway,
not committed anywhere). Report Step 7's `[sftptest]` lines (both
sides — NeoOS's own log and the host server's log) as this milestone's
proof.

---

### Task 4: Finish

- [ ] **Step 1: Confirm `neoos-libssh2`'s tree is clean and pushed**

```bash
cd /home/neo/projects/personal/neoos-libssh2
git status --short
git log origin/main..HEAD --oneline
```

Expected: no local changes, nothing ahead of `origin/main`.

- [ ] **Step 2: Confirm `neoos-kernel`'s tree is clean**

```bash
cd /home/neo/projects/personal/NeoOS
git status --short
```

Expected: clean (Task 3 makes no permanent kernel-repo changes).

- [ ] **Step 3: Report completion**

Summarize for the user: libssh2 1.11.1 built and verified with a real
SFTP round trip against an independent SSH server implementation;
`LIBSSH2_DIR` convention (`build-output/{include,lib}`) ready for
curl's own future spec (with SFTP), which per the agreed sequence
comes after the SSH server milestone.
