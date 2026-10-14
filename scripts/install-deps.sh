#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE="${1:-check}"

check_commands() {
    local missing=0
    local command_name
    local commands=(git curl make cc gcc g++ clang++ tar xz)

    if [ "$MODE" = "check-build" ] || [ "$MODE" = "check-all" ]; then
        commands+=(nasm xorriso)
    fi
    if [ "$MODE" = "check-all" ]; then
        commands+=(qemu-system-x86_64)
        if [ "$(uname -s)" = Darwin ]; then
            commands+=(hdiutil newfs_msdos)
        fi
    fi

    for command_name in "${commands[@]}"; do
        if ! command -v "$command_name" >/dev/null 2>&1; then
            printf 'Missing host dependency: %s\n' "$command_name" >&2
            missing=1
        fi
    done

    if [ "$MODE" = "check-build" ] || [ "$MODE" = "check-all" ]; then
        if ! command -v i686-elf-grub-mkrescue >/dev/null 2>&1 &&
           ! command -v grub-mkrescue >/dev/null 2>&1 &&
           ! command -v x86_64-elf-grub-mkrescue >/dev/null 2>&1; then
            printf 'Missing host dependency: GRUB mkrescue utility\n' >&2
            missing=1
        fi
    fi

    if [ "$missing" -ne 0 ]; then
        printf 'Install prerequisites with: make install-deps\n' >&2
        return 1
    fi

    if [ "$(uname -s)" = Darwin ]; then
        if ! xcode-select -p >/dev/null 2>&1; then
            printf 'Xcode Command Line Tools are required (run: xcode-select --install)\n' >&2
            return 1
        fi
    fi

    printf 'Host dependencies are available.\n'
    if [ "$MODE" != "check-all" ] &&
       ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
        printf 'QEMU is absent; install it to run the OS with make run.\n'
    fi
    [ -x "$ROOT/build/toolchain/x86_64-elf/bin/x86_64-elf-gcc" ] ||
        printf 'Local cross-toolchain is absent; build it with make toolchain.\n'
}

install_macos() {
    local formulas=(gmp mpfr libmpc isl zlib make nasm xorriso qemu wget gawk i686-elf-grub)
    if ! command -v brew >/dev/null 2>&1; then
        printf 'Homebrew is required to install dependencies on macOS.\n' >&2
        printf 'Install Homebrew from https://brew.sh, then retry.\n' >&2
        return 1
    fi
    xcode-select -p >/dev/null 2>&1 || {
        printf 'Install Xcode Command Line Tools first: xcode-select --install\n' >&2
        return 1
    }
    brew install "${formulas[@]}"
}

install_debian() {
    local packages=(
        build-essential bison flex gawk git curl wget make xz-utils
        nasm xorriso grub-pc-bin grub-common qemu-system-x86
        libgmp-dev libmpfr-dev libmpc-dev libisl-dev zlib1g-dev texinfo
    )
    local apt_get=(apt-get)
    if [ "$(id -u)" -ne 0 ]; then
        if ! command -v sudo >/dev/null 2>&1; then
            printf 'sudo is required to install packages; install dependencies as root.\n' >&2
            return 1
        fi
        apt_get=(sudo apt-get)
    fi
    "${apt_get[@]}" update
    "${apt_get[@]}" install -y "${packages[@]}"
}

case "$MODE" in
    check|check-toolchain|check-build|check-all)
        check_commands
        ;;
    install)
        case "$(uname -s)" in
            Darwin) install_macos ;;
            Linux)
                if command -v apt-get >/dev/null 2>&1; then
                    install_debian
                else
                    printf 'Automatic installation supports Homebrew on macOS and apt on Debian/Ubuntu.\n' >&2
                    exit 2
                fi
                ;;
            *)
                printf 'Unsupported host OS: %s\n' "$(uname -s)" >&2
                exit 2
                ;;
        esac
        MODE=check-all
        check_commands
        ;;
    *)
        printf 'Usage: %s [install|check-toolchain|check-build|check-all]\n' \
            "$0" >&2
        exit 2
        ;;
esac
