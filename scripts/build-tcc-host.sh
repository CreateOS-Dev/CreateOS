#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="${TCC_SOURCE:-$ROOT/kernel/lib/third_party/tinycc}"
PREFIX="${TCC_PREFIX:-$ROOT/build/toolchain/tcc-host}"
REVISION=d348a9a51d32cece842b7885d27a411436d7887b
APPLE_ARM64=0

if [ ! -f "$SOURCE/tcc.c" ] ||
   ! grep -Fxq "$REVISION" "$SOURCE/UPSTREAM_REVISION"; then
    printf 'TinyCC source in %s is missing or is not pinned to %s.\n' \
        "$SOURCE" "$REVISION" >&2
    exit 1
fi

configure_args=(--prefix="$PREFIX" --cc="${HOSTCC:-cc}")
if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
    # TinyCC 0.9.27 has no arm64-osx target in its build rules.
    configure_args+=(--cpu=x86_64)
    APPLE_ARM64=1
fi

if [ "${TCC_SOURCE_ONLY:-0}" = "1" ]; then
    (
        cd "$SOURCE"
        ./configure "${configure_args[@]}"
    )
    printf 'Prepared pinned TinyCC sources for the CreateOS guest build.\n'
    exit 0
fi

(
    cd "$SOURCE"
    if [ -f config.mak ]; then
        "${MAKE:-make}" clean
    fi
    ./configure "${configure_args[@]}"
    if [ "$APPLE_ARM64" -eq 1 ]; then
        "${MAKE:-make}" -j"${JOBS:-2}" tcc libtcc.a tcc.1
        "${MAKE:-make}" -C lib clean
        "${MAKE:-make}" -C lib ../libtcc1.a \
            x86_64-osx-libtcc1-usegcc=yes \
            CC="${CLANG:-clang} -target x86_64-apple-macosx10.15" AR=ar
    else
        "${MAKE:-make}" -j"${JOBS:-2}"
    fi
    "${MAKE:-make}" install
)

printf 'Installed native TinyCC under %s\n' "$PREFIX"
printf 'This is the build-host compiler; guest TinyCC is built separately.\n'
