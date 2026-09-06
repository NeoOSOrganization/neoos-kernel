# curl port for NeoOS — design spec

## Purpose

Port curl (CLI + libcurl) to NeoOS, supporting HTTP, HTTPS, and SFTP.
This is the next milestone in the agreed DNS resolution -> OpenSSL ->
SSH client (libssh2) -> curl -> wget sequence; the SSH server milestone
is deferred, and curl proceeds directly on top of the now-proven
OpenSSL (TLS) and libssh2 (SFTP) ports.

Unlike the libssh2 milestone (library only, no interactive CLI), curl's
primary value IS the CLI: a `curl <url>` command usable directly from
NeoOS's shell, or from a script, to fetch things. This port builds
both the `curl` executable and `libcurl.a`.

Protocol scope is deliberately narrow: **HTTP, HTTPS, and SFTP only.**
curl supports many more protocols by default (FTP, TELNET, DICT,
GOPHER, IMAP, LDAP, MQTT, POP3, RTMP, RTSP, SMB, SMTP, TFTP, ...); all
of those are explicitly disabled at build time, matching this org's
established pattern of building only what a milestone actually needs
(OpenSSL skipping its CLI, libssh2 skipping an interactive ssh tool).

## Repo & directory structure

New repo `NeoOSOrganization/neoos-curl`, cloned locally at
`/home/neo/projects/personal/neoos-curl`, following the
`neoos-libssh2` pattern exactly:

```
neoos-curl/
  Makefile           # thin wrapper: submodule-init, build.sh, verify
  build.sh           # the real build recipe
  toolchain.cmake    # CMake cross-compile toolchain (copied from
                      # neoos-libssh2, unchanged)
  README.md
  .gitignore         # build-output/, build-tmp/
  upstream/          # git submodule, pinned to curl-8_22_0
```

## Dependencies

- `neoos-musl` (`../neoos-musl/build-output`) — libc, crt1.o
- `neoos-openssl` (`../neoos-openssl/build-output`) — TLS backend
  (`libssl.a`/`libcrypto.a`) and its `cacert.pem`
- `neoos-libssh2` (`../neoos-libssh2/build-output`) — SFTP/SCP backend
  (`libssh2.a`)

All three must already be built before this port runs, exactly like
`neoos-libssh2`'s own dependency check on `neoos-musl`/`neoos-openssl`.

## Build configuration

`build.sh` runs CMake against `upstream/` (curl ships an official
CMake build) with:

- `-DCMAKE_TOOLCHAIN_FILE=toolchain.cmake`, `-DCMAKE_FIND_ROOT_PATH`
  covering the OpenSSL, libssh2, and musl prefixes (same pattern as
  `neoos-libssh2`'s `build.sh`)
- `-DCMAKE_REQUIRED_LIBRARIES="<musl>/lib/crt1.o;<musl>/lib/libc.a"` —
  the fix just proven necessary for libssh2's own `HAVE_*` feature
  probes (`check_c_source_compiles`/`check_function_exists` link a
  throwaway program using the project's own `-nostdlib` flags; without
  something to link against, every probe fails and is silently
  recorded as "unavailable" even when musl provides the symbol).
  Applied here from the start rather than rediscovered the hard way.
- `-DBUILD_SHARED_LIBS=OFF` — static, matching every other port
- `-DCURL_USE_OPENSSL=ON`, `-DOPENSSL_ROOT_DIR=<neoos-openssl output>`,
  `-DOPENSSL_INCLUDE_DIR=.../include`
- `-DCURL_USE_LIBSSH2=ON`, `-DLIBSSH2_INCLUDE_DIR=.../include`,
  `-DLIBSSH2_LIBRARY=.../lib/libssh2.a` (curl's CMake does not treat
  libssh2 as a `find_package`-able dependency the way it does OpenSSL;
  path and library must be given explicitly)
- `-DCURL_CA_BUNDLE=/opt/curl/cacert.pem` — the *compiled-in default*
  CA bundle path. Combined with `PORT_DIRS` installing `cacert.pem` to
  exactly that path (see below), this makes real certificate
  verification work with no `-k`/`--insecure` flag needed.
- Protocol scope, everything else OFF: `-DCURL_DISABLE_FTP=ON`,
  `-DCURL_DISABLE_TELNET=ON`, `-DCURL_DISABLE_DICT=ON`,
  `-DCURL_DISABLE_GOPHER=ON`, `-DCURL_DISABLE_IMAP=ON`,
  `-DCURL_DISABLE_LDAP=ON`, `-DCURL_DISABLE_LDAPS=ON`,
  `-DCURL_DISABLE_MQTT=ON`, `-DCURL_DISABLE_POP3=ON`,
  `-DCURL_DISABLE_RTSP=ON`, `-DCURL_DISABLE_SMB=ON`,
  `-DCURL_DISABLE_SMTP=ON`, `-DCURL_DISABLE_TFTP=ON`
- No extra compression/multiplexing dependencies that don't exist yet
  on NeoOS: `-DCURL_ZLIB=OFF`, `-DUSE_NGHTTP2=OFF`,
  `-DCURL_USE_LIBPSL=OFF`, `-DUSE_LIBIDN2=OFF` — HTTP/1.1 only, no
  compressed transfer support. A future milestone can add zlib/nghttp2
  as their own ports if that ever becomes necessary.
- `-DBUILD_CURL_EXE=ON`, `-DBUILD_STATIC_CURL=ON` — build the CLI,
  explicitly, matching this org's habit of never relying on an
  unstated CMake default
- `-DBUILD_TESTING=OFF`, `-DENABLE_CURL_MANUAL=OFF`,
  `-DCURL_DISABLE_INSTALL=OFF`

`cmake --build` produces `libcurl.a` (and CMake's own attempt at a
`curl` executable, which will not run on NeoOS as CMake links it —
CMake has no notion of NeoOS's `user.ld` linker script or the
`nexify.sh` step every other NeoOS binary needs). `build.sh` therefore
does one more explicit link pass for the CLI, exactly like
`neoos-libssh2`'s SFTP/exec test programs were hand-linked against
`libssh2.a`:

```
x86_64-elf-gcc -static -nostdlib -nostdinc -ffreestanding \
  -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2 \
  -isystem <musl>/include -isystem <openssl>/include -isystem <libssh2>/include \
  -isystem <curl-build>/include -isystem upstream/include \
  -T user.ld -z noexecstack \
  -o build-output/bin/curl.elf \
  <musl>/lib/crt1.o <curl CLI's own .c sources, built as objects by the
  CMake build already, or re-driven directly -- see plan for the exact
  object list CMake produces under build-tmp/src/CMakeFiles/curl.dir/> \
  -Wl,--start-group -Lbuild-output/lib -lcurl \
  -L<libssh2>/lib -lssh2 -L<openssl>/lib -lssl -lcrypto -Wl,--end-group \
  -L<musl>/lib -lc -lgcc
tools/nexify.sh build-output/bin/curl.elf build-output/bin/curl.nex
```

(`user.ld` is copied from `neoos-doom`'s, unchanged — every NeoOS
userland binary uses the same linker script.)

`build.sh` also copies `neoos-openssl`'s `cacert.pem` into
`build-output/cacert.pem`, for `PORT_DIRS` to place at `/opt/curl/`.

## PORT_DIRS integration

Following the existing convention exactly (`docs/superpowers/specs/
2026-09-06-os-builder-port-install-design.md` in neoos-kernel):
`PORT_DIRS="curl=/path/to/neoos-curl/build-output"` installs
`curl.nex` to `/usr/local/bin/curl.nex` and `cacert.pem` to
`/opt/curl/cacert.pem`.

## Testing plan

1. **Build verification**: `libcurl.a` and `curl.nex` both produced;
   `curl --version` (or equivalent, since there's no interactive
   shell in the automated test path — see below) reports `HTTP`,
   `HTTPS`, `SFTP` in its protocol list and `OpenSSL`/`libssh2` in its
   feature list.
2. **HTTP**: `curl http://example.com` against the real, live internet
   through slirp's NAT (already proven reachable in the OpenSSL
   milestone's TLS test and the DNS milestone's `getaddrinfo` test) —
   verify a 2xx response and expected page content.
3. **HTTPS**: `curl https://example.com` — verify a real TLS 1.2/1.3
   handshake completes, the certificate chain validates against the
   installed `cacert.pem` (no `-k`), and the response body matches
   plain HTTP's.
4. **SFTP**: reuse `sftp_server.py` (asyncssh, ephemeral host key,
   `neotest`/`neotest` credentials) unchanged from the libssh2
   milestone. `curl -T <localfile> sftp://neotest:neotest@10.0.2.100/tmp/curl_test.txt`
   (upload) followed by `curl sftp://neotest:neotest@10.0.2.100/tmp/curl_test.txt`
   (download), verifying the round-tripped content matches — the same
   write/read/verify bar the libssh2 milestone used, now exercised
   through curl's higher-level interface instead of raw libssh2 calls.
5. **Regression bar**: 15/15 gauntlet, zero retries. curl arrives
   purely via `PORT_DIRS`, so the embedfs-based regression suite is
   untouched; this is a pure smoke check that nothing about linking a
   fourth large port regresses boot or the existing test workload.

Each protocol test runs as its own inittab entry (`wait
/usr/local/bin/curl.nex http://example.com`, etc.), following the
libssh2 milestone's own single-purpose-boot testing pattern, since
NeoOS's automated test path is a scripted boot, not an interactive
session.

## Out of scope

- FTP, FTPS, and every other curl-supported protocol besides HTTP,
  HTTPS, and SFTP — deliberately disabled at build time (see above).
- HTTP/2, HTTP/3, and compressed transfer encoding (gzip/deflate/brotli)
  — no nghttp2/zlib/brotli port exists yet; HTTP/1.1 uncompressed only.
- An interactive SSH CLI, or anything the libssh2 milestone already
  scoped out — this port only reaches libssh2 through curl's own
  SFTP/SCP handlers, not directly.
- wget — the next milestone in the agreed sequence, not this one.
