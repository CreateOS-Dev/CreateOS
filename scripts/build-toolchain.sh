#!/usr/bin/env bash
set -euo pipefail

TARGET="${TARGET:-x86_64-elf}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ROOT="${TOOLCHAIN_BUILD_ROOT:-$ROOT/build/toolchain/src}"
PREFIX="${TOOLCHAIN_PREFIX:-$ROOT/build/toolchain/$TARGET}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
MAKE_CMD="${MAKE:-make}"
BINUTILS_VERSION=2.44
GCC_VERSION=14.2.0
BINUTILS_SHA256=ce2017e059d63e67ddb9240e9d4ec49c2893605035cd60e92ad53177f4377237
GCC_SHA256=a7b39bc69cbf9e25826c5a60ab26477001f7c08d85cec04bc0e29cabed6f3cc9
DOWNLOADS="$BUILD_ROOT/downloads"
SOURCES="$BUILD_ROOT/sources"
BUILDS="$BUILD_ROOT/build"

case "$TARGET" in
    *-elf) ;;
    *)
        printf 'Unsupported target: %s (expected *-elf)\n' "$TARGET" >&2
        exit 2
        ;;
esac

mkdir -p "$DOWNLOADS" "$SOURCES" "$BUILDS" "$PREFIX"

toolchain_is_ready() {
    local tool
    for tool in gcc ld nm objcopy ar; do
        if [ ! -x "$PREFIX/bin/$TARGET-$tool" ]; then
            return 1
        fi
    done
    [ "$("$PREFIX/bin/$TARGET-gcc" -dumpmachine)" = "$TARGET" ] &&
        [ -s "$PREFIX/lib/gcc/$TARGET/14.2.0/libgcc.a" ]
}

if toolchain_is_ready; then
    printf 'Verified %s toolchain is already installed under %s\n' \
        "$TARGET" "$PREFIX"
    exit 0
fi

fetch_verified() {
    local url="$1"
    local destination="$2"
    local expected="$3"
    local actual

    if [ ! -s "$destination" ]; then
        rm -f "$destination.part"
        curl --fail --location --retry 3 "$url" --output "$destination.part"
        mv "$destination.part" "$destination"
    fi
    if command -v sha256sum >/dev/null 2>&1; then
        actual="$(sha256sum "$destination" | awk '{print $1}')"
    elif command -v shasum >/dev/null 2>&1; then
        actual="$(shasum -a 256 "$destination" | awk '{print $1}')"
    else
        printf 'Neither sha256sum nor shasum is available.\n' >&2
        exit 1
    fi
    if [ "$actual" != "$expected" ]; then
        rm -f "$destination"
        printf 'SHA-256 mismatch for %s\n' "$url" >&2
        exit 1
    fi
}

fetch_verified \
    "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
    "$DOWNLOADS/binutils-$BINUTILS_VERSION.tar.xz" "$BINUTILS_SHA256"
fetch_verified \
    "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
    "$DOWNLOADS/gcc-$GCC_VERSION.tar.xz" "$GCC_SHA256"

if [ ! -x "$SOURCES/binutils-$BINUTILS_VERSION/configure" ]; then
    rm -rf -- "$SOURCES/binutils-$BINUTILS_VERSION"
    tar -xJf "$DOWNLOADS/binutils-$BINUTILS_VERSION.tar.xz" -C "$SOURCES"
fi
if [ ! -x "$SOURCES/gcc-$GCC_VERSION/configure" ]; then
    rm -rf -- "$SOURCES/gcc-$GCC_VERSION"
    tar -xJf "$DOWNLOADS/gcc-$GCC_VERSION.tar.xz" -C "$SOURCES"
fi

if [ ! -f "$SOURCES/gcc-$GCC_VERSION/.createos-prerequisites-ready" ]; then
    (
        cd "$SOURCES/gcc-$GCC_VERSION"
        ./contrib/download_prerequisites
        touch .createos-prerequisites-ready
    )
fi

build_one() {
    local name="$1"
    local source="$2"
    shift 2
    local build="$BUILDS/$name"

    mkdir -p "$build"
    if [ ! -x "$source/configure" ]; then
        printf 'Missing configure script for %s: %s/configure\n' \
            "$name" "$source" >&2
        exit 1
    fi
    (
        cd "$build"
        "$source/configure" "$@"
        "$MAKE_CMD" -j"$JOBS"
        "$MAKE_CMD" install
    )
}

build_one "binutils-$TARGET" "$SOURCES/binutils-$BINUTILS_VERSION" \
    --target="$TARGET" --prefix="$PREFIX" --with-sysroot \
    --with-system-zlib --disable-nls --disable-werror

GCC_BUILD="$BUILDS/gcc-$TARGET"
mkdir -p "$GCC_BUILD"
(
    cd "$GCC_BUILD"
    "$SOURCES/gcc-$GCC_VERSION/configure" \
        --target="$TARGET" --prefix="$PREFIX" --without-headers \
        --with-newlib --with-system-zlib --enable-languages=c \
        --disable-nls --disable-shared \
        --disable-threads --disable-libssp --disable-libquadmath \
        --disable-libgomp --disable-werror
    "$MAKE_CMD" -j"$JOBS" all-gcc
    "$MAKE_CMD" install-gcc
    "$MAKE_CMD" -j"$JOBS" all-target-libgcc
    "$MAKE_CMD" install-target-libgcc
)

if ! toolchain_is_ready; then
    printf 'Toolchain installation did not pass its executable/target checks.\n' >&2
    exit 1
fi

printf 'Installed and verified %s binutils and GCC under %s\n' "$TARGET" "$PREFIX"
