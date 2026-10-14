#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION=4.0.0
SHA256=2f3a47f7b3a541ddef450e4867eeecb7ce2ef7776093f3a11d6d43ead6bf2827
TOOLCHAIN_PREFIX="${TOOLCHAIN_PREFIX:-$ROOT/build/toolchain/x86_64-elf}"
DOWNLOADS="$ROOT/build/third_party/downloads"
SOURCE="$ROOT/build/third_party/mbedtls-$VERSION"
BUILD="$ROOT/build/mbedtls-$VERSION-x86_64-elf"
ARCHIVE="$DOWNLOADS/mbedtls-$VERSION.tar.bz2"
TOOLCHAIN_FILE="$ROOT/build/mbedtls-$VERSION-toolchain.cmake"

mkdir -p "$DOWNLOADS" "$(dirname "$SOURCE")"
if [ ! -s "$ARCHIVE" ]; then
    curl --fail --location --retry 3 \
        "https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$VERSION/mbedtls-$VERSION.tar.bz2" \
        --output "$ARCHIVE.part"
    mv "$ARCHIVE.part" "$ARCHIVE"
fi

if command -v sha256sum >/dev/null 2>&1; then
    ACTUAL="$(sha256sum "$ARCHIVE" | awk '{print $1}')"
elif command -v shasum >/dev/null 2>&1; then
    ACTUAL="$(shasum -a 256 "$ARCHIVE" | awk '{print $1}')"
else
    printf 'Neither sha256sum nor shasum is available.\n' >&2
    exit 1
fi
if [ "$ACTUAL" != "$SHA256" ]; then
    rm -f "$ARCHIVE"
    printf 'SHA-256 mismatch for Mbed TLS %s.\n' "$VERSION" >&2
    exit 1
fi

if [ ! -f "$SOURCE/include/mbedtls/ssl.h" ]; then
    rm -rf -- "$SOURCE"
    mkdir -p "$SOURCE"
    tar -xjf "$ARCHIVE" -C "$SOURCE" --strip-components=1
fi

cat > "$TOOLCHAIN_FILE" <<EOF
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER "$TOOLCHAIN_PREFIX/bin/x86_64-elf-gcc")
set(CMAKE_AR "$TOOLCHAIN_PREFIX/bin/x86_64-elf-ar")
set(CMAKE_RANLIB "$TOOLCHAIN_PREFIX/bin/x86_64-elf-ranlib")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
EOF

CFLAGS="-m64 -ffreestanding -fno-pic -fno-stack-protector -mno-red-zone -ffunction-sections -fdata-sections -I$ROOT/rootfs_maker/lib -DTF_PSA_CRYPTO_CONFIG_FILE=\\\"$ROOT/rootfs_maker/lib/createos_psa_crypto_config.h\\\""
cmake -S "$SOURCE" -B "$BUILD" \
    -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
    -DCMAKE_C_FLAGS="$CFLAGS" \
    -DMBEDTLS_CONFIG_FILE="$ROOT/rootfs_maker/lib/createos_mbedtls_config.h" \
    -DENABLE_PROGRAMS=OFF \
    -DENABLE_TESTING=OFF \
    -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build "$BUILD" --parallel "${JOBS:-2}"
