# libssh2 port for NeoOS

## 1. Problem

Requested scope for "port curl" turned out to bundle in SSH client and
server support, each a substantial, largely independent project (see
this milestone's own brainstorming session for the decomposition).
Agreed sequence: SSH **client** library first (unblocks curl's
`sftp://`, the actual motivation), SSH server later, curl (with SFTP)
after that, wget last.

This spec covers exactly the SSH client library — `libssh2` — and
nothing else: not an interactive `ssh` CLI (explicitly decided
against: NeoOS gets the library curl will link, not a new user-facing
program), not the SSH server, not curl itself.

## 2. Scope

**In scope:** a new repo, `neoos-libssh2`, producing a static
`libssh2.a` built against `neoos-musl` and `neoos-openssl` (as
`libssh2`'s crypto backend — no other crypto backend is needed or
built). Verified with a real SFTP file write/read/verify round trip
against a real, throwaway SFTP server.

**Out of scope, deferred:**

- **An interactive `ssh` CLI.** `libssh2` alone is a protocol library,
  not a `ssh host` command — building one is comparable in size to a
  small port of its own (session/channel/pty/terminal-I/O plumbing).
  Explicit user decision: library only, for now.
- **The SSH server.** Entirely unrelated code path (inbound
  connections, host key management, `/etc/passwd` auth, a listening
  daemon, pty allocation) — its own future milestone.
- **Non-OpenSSL crypto backends.** `libssh2` supports several
  (mbedTLS, wolfSSL, Libgcrypt, its own internal). NeoOS already has a
  working OpenSSL build; there is no second consumer motivating
  anything else. YAGNI.

## 3. Repo structure

Mirrors `neoos-openssl`, with one difference: `libssh2` builds via
CMake (its officially supported, recommended path — the autotools
`configure` script requires bootstrapping via `autoreconf` first,
which CMake's `CMakeLists.txt` does not) rather than a bespoke
`Configure`-style script:

```
neoos-libssh2/
├── Makefile              # thin wrapper: submodule-init, all, clean, verify
├── build.sh              # cmake configure + build invocation
├── toolchain.cmake       # the NeoOS cross-compile toolchain file
├── upstream/             # git submodule -> https://github.com/libssh2/libssh2,
│                         # pinned to tag libssh2-1.11.1
├── README.md
└── docs/
```

## 4. Build strategy

`toolchain.cmake`:

```cmake
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_AR x86_64-elf-ar)
set(CMAKE_RANLIB x86_64-elf-ranlib)
set(CMAKE_C_FLAGS_INIT "-static -nostdlib -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
```

`CMAKE_SYSTEM_NAME Generic` is CMake's own spelling of "freestanding,
cross-compiling, do not assume a hosted OS" — the same freestanding
posture every other userland build in this org already takes with
`-ffreestanding`/`-nostdlib`. `build.sh` then runs:

```sh
cmake -S upstream -B build-tmp \
    -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake \
    -DCMAKE_INSTALL_PREFIX="$(pwd)/build-output" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_TESTING=OFF \
    -DCRYPTO_BACKEND=OpenSSL \
    -DOPENSSL_ROOT_DIR="$(cd ../neoos-openssl/build-output && pwd)" \
    -DOPENSSL_INCLUDE_DIR="$(cd ../neoos-openssl/build-output/include && pwd)" \
    -DCMAKE_C_FLAGS="-isystem $(cd ../neoos-musl/build-output/include && pwd)"
cmake --build build-tmp -j"$(nproc)"
cmake --install build-tmp
```

**Why `CRYPTO_BACKEND=OpenSSL` pointed at `OPENSSL_ROOT_DIR`:** this is
`libssh2`'s own supported mechanism for using a prebuilt OpenSSL
rather than one CMake's `find_package` would otherwise search the
host system for — exactly the situation here, where the real OpenSSL
this build needs is `neoos-openssl`'s cross-compiled one, not
anything on the build host.

**Following the OpenSSL milestone's own hard-won lessons:** expect and
fix-forward the same class of issues that milestone hit (a `-pthread`
driver flag `x86_64-elf-gcc` doesn't recognize, a build step that
tries to link something needing a host-only library) rather than
weakening the target — `libssh2`'s CMake build is smaller and more
conventional than OpenSSL's own, so fewer surprises are likely, but
none should be assumed away in advance.

## 5. Testing

No host runtime exists for this code (bare-metal target, matching
every other repo in the org). Verified via a real NeoOS boot
performing a genuine SFTP round trip:

1. **A throwaway SFTP server on the host**, using `asyncssh` (already
   available on this machine — `pip show asyncssh` confirms 2.21.1):
   a small, unprivileged Python script generating an ephemeral host
   key, listening on a high port (e.g. `2222`) on an interface the
   guest can reach via slirp's `10.0.2.2` gateway address, with a
   fixed test username/password. Torn down immediately after the
   test — no lasting host state, no system package installed, no
   root required (matching every other throwaway verification
   scaffold this session has used).
2. **A small NeoOS test program** (`sftp_test.c`, throwaway, not
   committed — matching `getaddrinfo_test.c`/`tls_test.c`'s own
   precedent) that: opens a TCP connection to `10.0.2.2:2222`,
   performs `libssh2_session_init`/`libssh2_session_handshake`,
   authenticates with `libssh2_userauth_password`, opens an SFTP
   session (`libssh2_sftp_init`), writes a known string to a file on
   the test server, reads it back, and confirms the bytes match.
3. Booted the same way every earlier milestone's throwaway probe was
   (`EMBED_DIRS` pointed at a scratch directory holding the nexified
   test binary, a hand-crafted `wait` inittab entry), with serial log
   output confirming each stage (`[sftptest] connected`, `[sftptest]
   authenticated`, `[sftptest] PASSED: round-trip verified`).
4. The existing 15/15 gauntlet stays green — this milestone touches no
   kernel code, only a new independent repo.

## 6. Interface for curl (a later milestone)

`build-output/` becomes a `LIBSSH2_DIR` an upstream consumer's
`Makefile` points at, mirroring `OPENSSL_DIR`/`MUSL_DIR`:

```
build-output/
├── include/          # libssh2.h, libssh2_sftp.h
└── lib/
    └── libssh2.a
```

curl's own `configure`/build will link
`-L$(LIBSSH2_DIR)/lib -lssh2` alongside its existing OpenSSL and musl
link flags, and enable SFTP/SCP via `--with-libssh2=$(LIBSSH2_DIR)`.
