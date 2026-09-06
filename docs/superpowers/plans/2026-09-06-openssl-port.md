# OpenSSL 1.1.1 Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a new repo, `neoos-openssl`, produces a real, working
OpenSSL 1.1.1 static build (`libssl.a`, `libcrypto.a`, the `openssl`
CLI) for NeoOS, with genuine certificate-chain verification — the
shared TLS dependency the curl and wget ports (next two milestones)
will each link against.

**Architecture:** OpenSSL's own `linux-x86_64` `Configure` target
built against `neoos-musl`'s headers/lib via the existing
`x86_64-elf-*` cross toolchain — no invented NeoOS-specific target.
Verified by booting a real `.nex` of the `openssl` CLI under QEMU and
performing a genuine TLS handshake against a live internet host
through slirp.

**Tech Stack:** OpenSSL 1.1.1w (upstream, vendored as a git submodule
pinned to tag `OpenSSL_1_1_1w`), `neoos-musl`'s `build-output`, the
`x86_64-elf-*` cross-compiler already on `$PATH`, `mtools`, QEMU.

**Spec:** `docs/superpowers/specs/2026-09-06-openssl-port-design.md`

## Global Constraints

- OpenSSL 1.1.1 only (pinned to `OpenSSL_1_1_1w`, the final 1.1.1
  release). 3.x is a separate, future milestone — do not attempt any
  3.x-shaped configuration here.
- Static only: `no-shared no-dso no-dynamic-engine`. No dynamic linker
  exists on NeoOS.
- `no-tests`: OpenSSL's own test suite assumes a POSIX process/shell
  environment this repo cannot run inside NeoOS. Correctness is
  verified by a real boot instead (Task 3).
- `--openssldir=/etc/ssl`: the fixed, compiled-in path
  `SSL_CTX_set_default_verify_paths()` looks under. Do not change this
  path — curl/wget's future specs will depend on it being exactly this.
- Threads stay enabled (no `no-threads`) — musl's pthreads already
  work on NeoOS.
- Getting the CA bundle onto a REAL disk image at `/etc/ssl/cert.pem`
  for an actual `os-builder` build is explicitly OUT of scope (spec
  §2). Task 3's verification places the file by hand on a throwaway
  test disk image, the same way the DNS resolution milestone hand-placed
  `/etc/ssl/resolv.conf` for its own spike.

---

### Task 1: Scaffold the `neoos-openssl` repo

**Files (new repo `neoos-openssl`, cloned to `/home/neo/projects/personal/neoos-openssl`):**
- Create: `Makefile`
- Create: `build.sh`
- Create: `.gitmodules` (and the `upstream` submodule it declares)
- Create: `cacert.pem`
- Create: `README.md`
- Create: `.gitignore`

**Interfaces:**
- Produces: a `make` target `all` that a later task's `build.sh` run
  depends on; `build-output/` as the directory every later task reads
  from (not created by this task — Task 2 is what actually builds).

- [ ] **Step 1: Create the GitHub repo**

```bash
gh repo create NeoOSOrganization/neoos-openssl --public \
    --description "OpenSSL 1.1.1 (TLS) for NeoOS"
```

Expected: a new empty repo at `https://github.com/NeoOSOrganization/neoos-openssl`.

- [ ] **Step 2: Clone it locally and add the upstream submodule**

```bash
cd /home/neo/projects/personal
git clone git@github.com:NeoOSOrganization/neoos-openssl.git
cd neoos-openssl
git submodule add https://github.com/openssl/openssl.git upstream
cd upstream
git fetch --tags
git checkout OpenSSL_1_1_1w
cd ..
git add .gitmodules upstream
git commit -m "vendor OpenSSL upstream, pinned to OpenSSL_1_1_1w"
```

Expected: `upstream/` contains a full OpenSSL checkout at the
`OpenSSL_1_1_1w` tag (verify with `cd upstream && git describe --tags`
→ `OpenSSL_1_1_1w`).

- [ ] **Step 3: Fetch the real CA bundle**

```bash
cd /home/neo/projects/personal/neoos-openssl
curl -sL https://curl.se/ca/cacert.pem -o cacert.pem
```

If `curl` is not available on this machine, use `wget -O cacert.pem
https://curl.se/ca/cacert.pem` instead — either produces the same
file. Verify it downloaded a real bundle, not an error page:

```bash
grep -c "BEGIN CERTIFICATE" cacert.pem
```

Expected: a large number (100+) of certificate blocks, and the file
starts with curl's own header comment (`## Certificate data from
Mozilla...`).

- [ ] **Step 4: Write `build.sh`**

```sh
#!/bin/bash
set -e

MUSL_DIR="${MUSL_DIR:-../neoos-musl/build-output}"
CC="${CC:-x86_64-elf-gcc}"
PREFIX="${PREFIX:-build-output}"
UPSTREAM_DIR="${UPSTREAM_DIR:-upstream}"

if [ ! -d "$MUSL_DIR/include" ]; then
    echo "Error: musl not found at $MUSL_DIR (build neoos-musl first)" >&2
    exit 1
fi
if [ ! -f "$UPSTREAM_DIR/Configure" ]; then
    echo "Error: upstream OpenSSL checkout not found at $UPSTREAM_DIR" >&2
    exit 1
fi

ABS_PREFIX="$(mkdir -p "$PREFIX" && cd "$PREFIX" && pwd)"
ABS_MUSL_DIR="$(cd "$MUSL_DIR" && pwd)"

echo "Building OpenSSL 1.1.1 for NeoOS..."
cd "$UPSTREAM_DIR"

# linux-x86_64, not a NeoOS-specific target: it only assumes libc
# functions musl already provides, never Linux syscall numbers
# directly (those are musl's own shim's problem). Alpine Linux proves
# this combination works -- real OpenSSL, built with this exact
# target, against musl. See docs/superpowers/specs/
# 2026-09-06-openssl-port-design.md section 4.
./Configure linux-x86_64 \
    --cross-compile-prefix=x86_64-elf- \
    no-shared no-dso no-dynamic-engine no-tests \
    --openssldir=/etc/ssl \
    --prefix="$ABS_PREFIX" \
    -isystem "$ABS_MUSL_DIR/include" \
    -static -nostdlib -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2

make -j"$(nproc)"
make install_sw install_ssldirs

cd ..
mkdir -p "$PREFIX/etc/ssl"
cp cacert.pem "$PREFIX/etc/ssl/cert.pem"

if [ -f "$PREFIX/lib/libssl.a" ] && [ -f "$PREFIX/lib/libcrypto.a" ]; then
    echo ""
    echo "OK OpenSSL built successfully at $PREFIX"
    ls -lh "$PREFIX/lib/libssl.a" "$PREFIX/lib/libcrypto.a"
else
    echo "ERROR: build finished but libssl.a/libcrypto.a not found" >&2
    exit 1
fi
```

```bash
chmod +x build.sh
```

- [ ] **Step 5: Write the `Makefile`**

```make
# NeoOS OpenSSL 1.1.1 build

MUSL_DIR ?= ../neoos-musl/build-output
PREFIX ?= build-output
UPSTREAM_DIR ?= upstream

.PHONY: all clean verify help submodule-init

all: build-output/lib/libssl.a

submodule-init:
	@if [ ! -f "$(UPSTREAM_DIR)/Configure" ]; then \
		echo "Initializing upstream submodule..."; \
		git submodule update --init upstream; \
	fi

build-output/lib/libssl.a: submodule-init
	@[ -d "$(MUSL_DIR)/include" ] || { \
		echo "Error: musl not found at $(MUSL_DIR) -- build neoos-musl first"; \
		exit 1; \
	}
	@MUSL_DIR="$(MUSL_DIR)" PREFIX="$(PREFIX)" ./build.sh

clean:
	rm -rf $(PREFIX)
	cd $(UPSTREAM_DIR) && git clean -fdx && git checkout .

verify:
	@if [ -f "$(PREFIX)/lib/libssl.a" ] && [ -f "$(PREFIX)/lib/libcrypto.a" ]; then \
		echo "OK libssl.a/libcrypto.a built"; \
	else \
		echo "ERROR libssl.a/libcrypto.a not found"; \
		exit 1; \
	fi

help:
	@echo "NeoOS OpenSSL 1.1.1 build"
	@echo "Usage: make [MUSL_DIR=path]"
```

- [ ] **Step 6: Write `README.md`**

```markdown
# NeoOS OpenSSL

OpenSSL 1.1.1 (pinned to `OpenSSL_1_1_1w`, the final 1.1.1 release),
built for NeoOS: static `libssl.a`/`libcrypto.a`, with real
certificate-chain verification against a bundled CA store (curl's own
published `cacert.pem`).

The shared TLS dependency the `neoos-curl` and `neoos-wget` ports each
link against. A NeoOS-native `openssl` CLI binary is NOT produced by
this repo (linking all of `upstream/apps/*.c` against a freestanding
target is a real, separately-sized problem) — verified instead by a
small test program exercising the public `SSL_CTX`/`SSL_connect` API
directly, which is also the exact way curl/wget consume these
libraries.

## Quick Start

Build musl first (`neoos-musl`), then:

```sh
make MUSL_DIR=../neoos-musl/build-output
# Produces: build-output/lib/{libssl,libcrypto}.a,
#           build-output/include/openssl/*.h,
#           build-output/bin/openssl,
#           build-output/etc/ssl/cert.pem
```

## Why `linux-x86_64` and not a NeoOS-specific target

OpenSSL's `Configure` is a Perl script consulting a static table of
named targets — unlike autoconf, it never probes the compiler by
running test programs. The `linux-x86_64` entry only assumes libc
functions musl already provides, never Linux syscall numbers directly
(that translation is `neoos-musl`'s own shim's job). Alpine Linux, a
real musl-based distribution, builds real OpenSSL packages with
exactly this target today — the existing proof this combination works
before NeoOS attempts it.

## Documentation

- **Design spec:** [neoos-kernel's
  docs/superpowers/specs/2026-09-06-openssl-port-design.md](https://github.com/NeoOSOrganization/neoos-kernel/blob/main/docs/superpowers/specs/2026-09-06-openssl-port-design.md)

## In This Organization

- **[neoos-kernel](https://github.com/NeoOSOrganization/neoos-kernel)** — Kernel source
- **[neoos-musl](https://github.com/NeoOSOrganization/neoos-musl)** — musl libc (this repo's build dependency)
- **[neoos-docs](https://github.com/NeoOSOrganization/neoos-docs)** — Guides and architecture
```

- [ ] **Step 7: Write `.gitignore`**

```
build-output/
```

- [ ] **Step 8: Commit and push**

```bash
cd /home/neo/projects/personal/neoos-openssl
git add build.sh Makefile README.md cacert.pem .gitignore
git commit -m "scaffold: build.sh, Makefile, README, cacert.pem"
git push origin main
```

---

### Task 2: Build OpenSSL against `neoos-musl`

**Files:** none new — this task runs Task 1's `build.sh` for real and
fixes whatever it finds broken. `neoos-musl`'s own `build-output` must
already exist (it does, from earlier in this session:
`/home/neo/projects/personal/neoos-musl/build-output`).

**Interfaces:**
- Consumes: `neoos-musl`'s `build-output/include`, `build-output/lib/libc.a`
  (Task 1's `build.sh`, `MUSL_DIR` argument).
- Produces: `build-output/lib/libssl.a`, `build-output/lib/libcrypto.a`,
  `build-output/include/openssl/*.h`, `build-output/etc/ssl/cert.pem` —
  Task 3 and every future curl/wget task read these exact paths. (The
  `openssl` CLI binary `make install_sw` also produces is a HOST
  binary, not runnable on NeoOS, and is not part of this port's
  deliverable — see Task 3's note on why.)

- [ ] **Step 1: Run the build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/neoos-openssl
make MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -60
```

Expected: ends with `OK OpenSSL built successfully at build-output`.
OpenSSL's own build is large (hundreds of `.c` files) — expect this to
take several minutes.

- [ ] **Step 2: If it fails, fix forward — do not weaken the target**

The most likely failure classes, and their fixes (per this project's
"translation, not emulation" principle — a missing musl symbol belongs
in `neoos-musl`, not papered over here):

- **A missing libc symbol at link time** (e.g. `undefined reference to
  'some_func'`): check whether it's something musl genuinely lacks
  (rare — musl is a complete libc) versus a symbol OpenSSL only needs
  for a feature this port doesn't need (e.g. `dlopen`-family symbols
  for dynamic engines, already excluded via `no-dso
  no-dynamic-engine` in Step 4 of Task 1 — if one of these still
  surfaces, the `Configure` flags need revisiting, not a stub).
  Report the exact symbol and which OpenSSL object references it
  before deciding.
- **`Configure` itself fails to find the `linux-x86_64` target**:
  confirm `upstream/Configurations/10-main.conf` (or `.pl` files
  under `Configurations/`) still defines `"linux-x86_64"` at the
  `OpenSSL_1_1_1w` tag — it does in every 1.1.1 release; if this
  fails, the submodule checkout is wrong, not the target name.
- **A perl module missing** (`Configure` is Perl): install it via the
  system package manager; this is a host build-tool gap, unrelated to
  NeoOS.

- [ ] **Step 3: Verify the build output shape**

```bash
cd /home/neo/projects/personal/neoos-openssl
ls -la build-output/lib/libssl.a build-output/lib/libcrypto.a
ls build-output/include/openssl/ssl.h build-output/include/openssl/crypto.h
ls -la build-output/etc/ssl/cert.pem
```

Expected: all four paths exist. (`build-output/bin/openssl`, if
present, is a HOST binary from OpenSSL's own `apps/` link step using
the host's default C runtime — it is not evidence of anything about
the NeoOS-targeted `.a` files, which is why this check does not
inspect it. See Task 3's note for why this port does not attempt to
produce a NeoOS-native `openssl` CLI.)

- [ ] **Step 4: Confirm `libssl.a`/`libcrypto.a` are the RIGHT architecture**

```bash
cd /home/neo/projects/personal/neoos-openssl
x86_64-elf-ar t build-output/lib/libcrypto.a | head -5
x86_64-elf-objdump -f build-output/lib/libcrypto.a 2>&1 | grep -m1 "architecture"
```

Expected: `x86_64-elf-ar` lists real `.o` member names without error
(proves the archive was built by the cross-`ar`, not the host's), and
`objdump` reports `architecture: i386:x86-64`.

- [ ] **Step 5: Commit**

No source changes are expected from this task if Step 1 succeeded
outright — `build-output/` is gitignored. If Step 2's fix-forward
required a `build.sh`/`Makefile` change, commit that:

```bash
cd /home/neo/projects/personal/neoos-openssl
git add build.sh Makefile
git commit -m "build: fix $(whatever Step 2 found)"
git push origin main
```

---

### Task 3: Boot verification — a real TLS handshake

**Working directory:** `/home/neo/projects/personal/NeoOS` for the
kernel-side steps (embedding, disk image, QEMU); this session's
scratchpad directory for the test program.

**Files:**
- Create (scratch, not committed anywhere — throwaway per the spec's
  §6, matching the DNS resolution milestone's own precedent of a
  throwaway `getaddrinfo_test.c`): `tls_test.c`, built into a nexified
  `tls_test.nex`.

**Interfaces:**
- Consumes: Task 2's `build-output/lib/{libssl,libcrypto}.a`,
  `build-output/include/openssl/*.h`, `build-output/etc/ssl/cert.pem`.
- Produces: nothing later tasks consume — this is the milestone's
  final proof, matching how the DNS resolution milestone's `getaddrinfo`
  probe was the terminal verification step for that spec.

**Note on why this is a custom test program, not the `openssl` CLI:**
building a NeoOS-native `openssl` CLI binary means linking ALL of
`upstream/apps/*.c` (dozens of subcommand source files: `s_client`,
`x509`, `req`, `genrsa`, ...) against a freestanding, `-static
-nostdlib` target whose final link step OpenSSL's own build system
was never designed to produce — a real, open-ended sub-problem with
unclear size, not something to guess at here. What actually matters
for curl/wget (§7 of the spec: they link `libssl.a`/`libcrypto.a`
directly, neither ever shells out to the `openssl` CLI) is that the
LIBRARIES work end to end with real certificate verification — which
a small, self-contained C program calling the public `SSL_CTX_new`/
`SSL_connect`/`SSL_get_verify_result` API proves precisely as well,
without any of that risk. The `openssl` CLI itself remains a
NeoOS-native gap, explicitly deferred (note this in Task 4's report).

- [ ] **Step 1: Write the test program**

```c
// tls_test.c -- throwaway: proves libssl.a/libcrypto.a work end to
// end on NeoOS, including real certificate-chain verification. Not
// committed anywhere, matching the DNS resolution milestone's own
// getaddrinfo_test.c precedent.
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

int main(void) {
    SSL_library_init();
    SSL_load_error_strings();

    const SSL_METHOD *method = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        printf("[tlstest] FAILED: SSL_CTX_new returned NULL\n");
        return 1;
    }

    if (SSL_CTX_load_verify_locations(ctx, "/etc/ssl/cert.pem", NULL) != 1) {
        printf("[tlstest] FAILED: SSL_CTX_load_verify_locations\n");
        return 1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("example.com", "443", &hints, &res) != 0) {
        printf("[tlstest] FAILED: getaddrinfo\n");
        return 1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        printf("[tlstest] FAILED: connect\n");
        return 1;
    }
    printf("[tlstest] TCP connected to example.com:443\n");

    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, "example.com");   // SNI

    if (SSL_connect(ssl) != 1) {
        printf("[tlstest] FAILED: SSL_connect (TLS handshake failed)\n");
        ERR_print_errors_fp(stdout);
        return 1;
    }
    printf("[tlstest] TLS handshake OK, using %s\n", SSL_get_version(ssl));

    long verify_result = SSL_get_verify_result(ssl);
    if (verify_result != X509_V_OK) {
        printf("[tlstest] FAILED: certificate verification failed, code=%ld\n", verify_result);
        return 1;
    }
    printf("[tlstest] PASSED: certificate chain verified (X509_V_OK)\n");

    SSL_shutdown(ssl);
    close(fd);
    return 0;
}
```

- [ ] **Step 2: Build and nexify it**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
MUSL_DIR=../neoos-musl/build-output
OPENSSL_DIR=../neoos-openssl/build-output
SCRATCH="$(pwd)/../openssl-probe-scratch"   # or this session's actual scratchpad dir
mkdir -p "$SCRATCH"
cp /path/to/tls_test.c "$SCRATCH/tls_test.c"   # from Step 1
x86_64-elf-gcc -static -nostdlib -nostdinc -ffreestanding \
  -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
  -isystem "$MUSL_DIR/include" -isystem "$OPENSSL_DIR/include" \
  -T userland/user.ld -z noexecstack \
  -o "$SCRATCH/tls_test.elf" \
  "$MUSL_DIR/lib/crt1.o" "$SCRATCH/tls_test.c" \
  -Wl,--start-group -L"$OPENSSL_DIR/lib" -lssl -lcrypto -Wl,--end-group \
  -L"$MUSL_DIR/lib" -lc -lgcc
./tools/nexify.sh "$SCRATCH/tls_test.elf" "$SCRATCH/tls_test.nex"
echo '{"category":"bin"}' > "$SCRATCH/tls_test.test.json"
```

Expected: links successfully — every symbol `tls_test.c` needs
(`socket`/`connect`/`getaddrinfo` from musl, `SSL_*`/`X509_*` from
`libssl.a`/`libcrypto.a`) is either already proven working (DNS
resolution milestone) or a plain static archive symbol, so this is a
much smaller link surface than the full `openssl` CLI would be. If a
symbol IS missing, report the exact undefined reference — likely
either a musl gap (belongs in `neoos-musl`, not a stub here) or an
OpenSSL internal symbol this program doesn't actually need (check
whether `tls_test.c` genuinely calls it before adding anything).

- [ ] **Step 3: Build a kernel image embedding it**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
SCRATCH="$(pwd)/../openssl-probe-scratch"
rm -f build/embedfs_table.c build/embedfs_table.o build/embedfs-objs.txt \
      build/embedfs-inittab-patch.json build/embedfs-markers.txt \
      build/kernel.elf build/disk.img build/disk2.img
rm -rf build/embedfs-obj
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output \
    EMBED_DIRS="$SCRATCH" iso disk-image
```

Expected: builds clean, same as every earlier `EMBED_DIRS`-based
scratch test this session.

- [ ] **Step 4: Place the CA bundle, resolv.conf, and a test inittab**

```bash
cd /home/neo/projects/personal/NeoOS
mcopy -o -i build/disk.img /home/neo/projects/personal/neoos-openssl/build-output/etc/ssl/cert.pem ::etc/ssl/cert.pem
printf 'nameserver 10.0.2.3\n' > /tmp/resolv.conf
mcopy -o -i build/disk.img /tmp/resolv.conf ::etc/resolv.conf
printf 'wait /bin/tls_test.nex\n' > /tmp/tls-inittab
mcopy -o -i build/disk.img /tmp/tls-inittab ::etc/inittab
```

- [ ] **Step 5: Boot and confirm the real TLS handshake**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
cd /home/neo/projects/personal/NeoOS
timeout 40 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
  -no-reboot -display none -serial file:build/tls-test.log
grep -i "tlstest\|exception\|panic\|halted" build/tls-test.log
```

Expected: `[tlstest] TCP connected to example.com:443`, `[tlstest] TLS
handshake OK, using TLSv1.3` (or `TLSv1.2`), and `[tlstest] PASSED:
certificate chain verified (X509_V_OK)` — a genuine, chain-validated
TLS handshake against the real `example.com` through slirp's NAT, with
no exception/panic/halted line. If `SSL_connect` fails, check first
whether DNS resolution (a dependency of this exact test, via
`getaddrinfo`) is what's actually failing — rerun with the DNS
milestone's own `dnstest.nex` alongside to isolate which layer broke,
rather than assuming the fault is in OpenSSL itself.

- [ ] **Step 6: Confirm the existing gauntlet is unaffected**

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
code, only a new independent repo, so this is a sanity check rather
than an expectation of finding anything.

- [ ] **Step 7: Report findings**

No commit for this task in `neoos-kernel` (nothing there changed) or
`neoos-openssl` (`tls_test.c`/`.nex` are throwaway, matching the DNS
resolution milestone's own `getaddrinfo_test.c` — not committed
anywhere). Report Step 5's three `[tlstest]` lines as this milestone's
proof, matching how the DNS resolution milestone's own report centered
on its `getaddrinfo` probe's real resolved addresses.

---

### Task 4: Finish

**Files:** `neoos-openssl/README.md` (only if Task 2/3 revealed
anything the README should say — e.g. a `Configure` flag that needed
adjusting).

- [ ] **Step 1: Confirm `neoos-openssl`'s tree is clean and pushed**

```bash
cd /home/neo/projects/personal/neoos-openssl
git status --short
git log origin/main..HEAD --oneline
```

Expected: no local changes, nothing ahead of `origin/main` — everything
from Tasks 1–3 already pushed as it was committed.

- [ ] **Step 2: Confirm `neoos-kernel`'s tree is clean**

```bash
cd /home/neo/projects/personal/NeoOS
git status --short
```

Expected: clean (Task 3 makes no permanent kernel-repo changes).

- [ ] **Step 3: Report completion**

Summarize for the user: OpenSSL 1.1.1w built and verified with a real
TLS handshake against a live host; `OPENSSL_DIR` convention
(`build-output/{include,lib}`) ready for curl/wget's own specs, which
are the next two milestones in the agreed sequence. Note explicitly
that a NeoOS-native `openssl` CLI binary was NOT produced (Task 3's
note) — the libraries are proven working, the CLI itself is a
deferred gap, not silently dropped.
