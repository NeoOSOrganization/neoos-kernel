#!/bin/bash
# Builds the x86_64-elf cross-compiler (binutils + GCC, no libc) that
# every build/CI job in this org needs. Follows the standard OSDev.org
# "GCC Cross-Compiler" recipe: --target=x86_64-elf, --without-headers,
# so the resulting compiler emits freestanding code with no host ABI
# assumptions.
#
# Installs to $PREFIX (default ~/opt/cross-x86_64-elf) and does nothing
# if the target compiler already exists there -- safe to call from CI
# on every run; pair with actions/cache keyed on this file's hash plus
# the version pins below so a cache hit skips the ~20-minute build
# entirely.
set -euo pipefail

BINUTILS_VERSION="${BINUTILS_VERSION:-2.42}"
GCC_VERSION="${GCC_VERSION:-13.2.0}"
PREFIX="${PREFIX:-$HOME/opt/cross-x86_64-elf}"
TARGET=x86_64-elf
JOBS="${JOBS:-$(nproc)}"

export PATH="$PREFIX/bin:$PATH"

if command -v "${TARGET}-gcc" >/dev/null 2>&1; then
    echo "toolchain: ${TARGET}-gcc already installed at $(command -v "${TARGET}-gcc"), skipping build"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

echo "toolchain: fetching binutils $BINUTILS_VERSION and gcc $GCC_VERSION..."
curl -fsSL "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" | tar xJ
curl -fsSL "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" | tar xJ

echo "toolchain: building binutils..."
mkdir build-binutils && cd build-binutils
"../binutils-$BINUTILS_VERSION/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j"$JOBS" >/dev/null
make install >/dev/null
cd ..

echo "toolchain: building gcc (this is the slow part -- 15-25 minutes)..."
cd "gcc-$GCC_VERSION"
./contrib/download_prerequisites >/dev/null
cd ..
mkdir build-gcc && cd build-gcc
"../gcc-$GCC_VERSION/configure" \
    --target="$TARGET" --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j"$JOBS" all-gcc >/dev/null
make -j"$JOBS" all-target-libgcc >/dev/null
make install-gcc >/dev/null
make install-target-libgcc >/dev/null

echo "toolchain: installed to $PREFIX"
"${TARGET}-gcc" --version | head -1
