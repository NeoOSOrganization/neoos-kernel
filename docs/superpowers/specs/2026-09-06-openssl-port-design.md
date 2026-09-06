# OpenSSL 1.1.1 port for NeoOS

## 1. Problem

`curl`/`wget` (the next two milestones after this one, in the agreed
sequence DNS resolution → **OpenSSL** → curl → wget) need TLS to be
useful against the real web: almost every URL a person actually types
is `https://`. Nothing resembling a crypto or TLS library exists
anywhere in the org today. This spec covers producing a working,
real, verified OpenSSL 1.1.1 build for NeoOS — the shared dependency
curl and wget will each link against — and nothing about curl or wget
themselves.

## 2. Scope

**In scope:** a new repo, `neoos-openssl`, producing `libssl.a` +
`libcrypto.a` + the `openssl` CLI tool, all static, all built against
`neoos-musl`, with real certificate-chain verification working against
a bundled CA store. Verified with a real TLS 1.2/1.3 handshake against
a real internet host through slirp — not a mock, not a loopback
self-test.

**Out of scope, deferred:**

- **OpenSSL 3.x.** Explicit user decision: 1.1.1 now, 3.x "on other
  milestones" later. Its provider architecture (algorithms loaded as
  pluggable providers, a default/legacy split) is a substantially
  larger port and a distinct future project, not a variant of this one.
- **How curl/wget get the CA bundle onto a disk image at the fixed
  path OpenSSL's default verify logic expects (`/etc/ssl/cert.pem`,
  see §4).** This repo's job ends at producing that file as a build
  artifact. Whether it lands on disk via `os-builder`'s `PORT_DIRS`
  (which currently only knows `/opt/<name>/` and `/usr/local/bin/`,
  neither of which is the fixed system path OpenSSL needs), a new
  system-file mechanism, or something else, is curl/wget's own
  port-integration concern — solved in those specs, not this one, to
  keep this milestone from absorbing a second, unrelated design
  question about `os-builder`'s install mechanism.
- **A generic TLS abstraction / swappable backend.** curl/wget will
  link OpenSSL directly. Nothing here asks "what if a future port
  wants mbedTLS instead" — YAGNI until a real second consumer needs it.

## 3. Repo structure

Mirrors `neoos-musl` exactly (the closest precedent: a large upstream
C project, vendored as a git submodule, built by a thin `Makefile` +
`build.sh`):

```
neoos-openssl/
├── Makefile              # thin wrapper: submodule-init, all, clean, verify
├── build.sh              # the actual Configure + make invocation
├── upstream/             # git submodule -> https://github.com/openssl/openssl,
│                         # pinned to the OpenSSL_1_1_1w tag (the final 1.1.1
│                         # release -- reproducible, not a moving branch)
├── cacert.pem            # curl's own published Mozilla CA bundle, checked in
│                         # (small, text, and pinning it avoids a network
│                         # fetch becoming part of every build)
├── README.md
└── docs/
```

## 4. Build strategy

`build.sh` runs, in `upstream/`:

```sh
./Configure linux-x86_64 \
    --cross-compile-prefix=x86_64-elf- \
    no-shared no-dso no-dynamic-engine no-tests \
    --openssldir=/etc/ssl \
    --prefix="$(cd .. && pwd)/build-output" \
    -isystem "$MUSL_DIR/include" \
    -static -nostdlib -mcmodel=large -fno-pic -mno-red-zone -fno-stack-protector -O2
make -j"$(nproc)"
make install_sw install_ssldirs
```

**Why `linux-x86_64` and not a custom target:** OpenSSL's `Configure`
is a Perl script consulting a static table of named targets (unlike
autoconf, it never probes the compiler by running test programs), and
the `linux-x86_64` entry only assumes libc functions musl already
provides — it never touches Linux syscall numbers directly, those are
musl's problem via the existing shim. Alpine Linux, a real musl-based
distribution, builds real OpenSSL packages with exactly this target
today, which is the existing-precedent proof this works before NeoOS
attempts it. Inventing a `neoos-x86_64` target would fight NeoOS's own
stated strategy (CLAUDE.md: look POSIX/Linux-shaped to userland) for
no benefit.

**Why these flags:**
- `no-shared no-dso no-dynamic-engine` — NeoOS has no dynamic linker;
  every other userland binary in this org is `-static -nostdlib`, and
  OpenSSL is no exception.
- `no-tests` — OpenSSL's test suite assumes a POSIX process/shell
  environment (`perl`, forking test harnesses) this repo has no way to
  run inside NeoOS itself; correctness is verified by this spec's own
  §6 instead.
- `--openssldir=/etc/ssl` — the compiled-in default location
  `SSL_CTX_set_default_verify_paths()` looks under. Fixed at `/etc/ssl`
  (matching real Linux distributions' own convention) rather than
  anything NeoOS-specific, so a real-world CA bundle format/tooling
  needs no NeoOS awareness either.
- Threads are left ENABLED (no `no-threads`): musl's pthreads already
  work on NeoOS (the busybox/doom ports both link against musl
  unmodified), so there is no reason to cripple OpenSSL's locking away
  from its normal default.

## 5. Certificate bundle

`cacert.pem` (curl's own published Mozilla CA bundle — the exact file
real curl distributes, regularly updated upstream) is checked into
`neoos-openssl` and copied into `build-output/etc/ssl/cert.pem` by
`build.sh`'s `install_ssldirs` step, landing at the same relative
layout OpenSSL's own `--openssldir` install step expects. This is a
build **artifact**, not something this repo installs onto any disk
image itself (see §2's scope cut).

## 6. Testing

No host runtime exists for this code (bare-metal target, matching
every other repo in the org) — verification is a real NeoOS boot,
exactly like the DNS resolution milestone:

1. `build.sh` completes: `build-output/lib/libssl.a`,
   `build-output/lib/libcrypto.a`, and a linkable `openssl` CLI exist.
2. A `.nex` of the `openssl` CLI, embedded via the same throwaway
   `EMBED_DIRS` mechanism the DNS spike used (not a permanent
   regression-suite entry — this repo has nothing to do with
   `neoos-kernel-tests-common`), boots and runs:
   - `openssl version` — confirms the binary runs at all and reports
     `1.1.1w`.
   - `openssl s_client -connect example.com:443 -CAfile /etc/ssl/cert.pem`
     (with `cert.pem` manually placed at that path on the test disk
     image, the same manual step the DNS spike used for
     `/etc/ssl/resolv.conf`) — a REAL TLS 1.2 or 1.3 handshake against
     a real internet host through slirp's NAT, with `Verify return
     code: 0 (ok)` confirming chain validation genuinely works, not
     just that bytes moved.
3. The existing 15/15 gauntlet stays green — this milestone touches no
   kernel code at all, only a new, independent repo, so this is a
   sanity check rather than an expectation of finding anything.

## 7. Interface for curl/wget (the next two milestones)

`build-output/` from this repo becomes an `OPENSSL_DIR` an upstream
consumer's `Makefile` points at, mirroring the existing `MUSL_DIR`/
`LIBNEOOS_DIR` convention:

```
build-output/
├── include/          # openssl/*.h
└── lib/
    ├── libssl.a
    └── libcrypto.a
```

curl/wget's own build steps link `-L$(OPENSSL_DIR)/lib -lssl -lcrypto`
alongside their existing `-L$(MUSL_DIR)/lib -lc`. Getting the CA
bundle (`build-output/etc/ssl/cert.pem` from this repo) onto a real
disk image at the path OpenSSL's binary actually looks for it is,
again, that milestone's own concern (§2).
