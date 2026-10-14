#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
CHECK_ONLY=0
BUILD=0

DOCS_URL="${CREATEOS_DOCS_URL:-https://github.com/CreateOS-Dev/Docs.git}"
USERLAND_URL="${CREATEOS_USERLAND_URL:-https://github.com/CreateOS-Dev/rootfs_maker.git}"
SDK_URL="${CREATEOS_SDK_URL:-https://github.com/CreateOS-Dev/sdk.git}"

usage() {
    cat <<'EOF'
Usage: scripts/assemble-project.sh [--check] [--build]

Clone or fast-forward the userland, documentation, and SDK component Git
repositories into this kernel checkout. --check validates existing
component clones without fetching. --build assembles the components and
builds the complete OS.

Component Git URLs can be overridden with CREATEOS_DOCS_URL,
CREATEOS_USERLAND_URL, and CREATEOS_SDK_URL.
EOF
}

while (($#)); do
    case "$1" in
        --check)
            CHECK_ONLY=1
            ;;
        --build)
            BUILD=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'Unknown option: %s\n' "$1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

if ((CHECK_ONLY && BUILD)); then
    printf '%s\n' '--check and --build cannot be combined.' >&2
    exit 2
fi

for required in Makefile linker.ld configs/default.config kernel/main.c boot/grub/grub.cfg; do
    if [[ ! -e "$ROOT_DIR/$required" ]]; then
        printf 'Not a CreateOS kernel checkout; missing %s\n' "$required" >&2
        exit 1
    fi
done

kernel_origin="$(git -C "$ROOT_DIR" remote get-url origin 2>/dev/null || true)"
case "${kernel_origin%.git}" in
    https://github.com/CreateOS-Dev/CreateOS|git@github.com:CreateOS-Dev/CreateOS)
        ;;
    *)
        printf 'Expected the CreateOS kernel checkout; origin is %s\n' \
            "${kernel_origin:-<none>}" >&2
        exit 1
        ;;
esac

if [[ ! -f "$ROOT_DIR/.gitignore" ]]; then
    printf 'Kernel checkout is missing its .gitignore: %s\n' "$ROOT_DIR/.gitignore" >&2
    exit 1
fi
for rule in '/rootfs_maker/' '/docs/' '/sdk/'; do
    if ! grep -Fqx -- "$rule" "$ROOT_DIR/.gitignore"; then
        if ((CHECK_ONLY)); then
            printf 'Kernel .gitignore is missing %s\n' "$rule" >&2
            exit 1
        fi
        [[ ! -s "$ROOT_DIR/.gitignore" ]] || printf '\n' >> "$ROOT_DIR/.gitignore"
        printf '%s\n' "$rule" >> "$ROOT_DIR/.gitignore"
    fi
done

normalize_url() {
    local url="${1%/}"
    url="${url%.git}"
    case "$url" in
        git@github.com:*) printf 'https://github.com/%s' "${url#git@github.com:}" ;;
        *) printf '%s' "$url" ;;
    esac
}

validate_component() {
    local name="$1"
    local directory="$2"
    shift 2
    local required
    for required in "$@"; do
        if [[ ! -e "$directory/$required" ]]; then
            printf '%s is incomplete; missing %s\n' "$name" "$required" >&2
            return 1
        fi
    done
}

update_component() {
    local name="$1"
    local url="$2"
    local directory="$3"
    shift 3
    local actual_origin branch upstream staging

    if [[ -L "$directory" ]]; then
        printf 'Refusing to replace symbolic-link component: %s\n' "$directory" >&2
        return 1
    fi

    if [[ ! -e "$directory" ]]; then
        if ((CHECK_ONLY)); then
            printf 'Missing component Git checkout: %s\n' "$directory" >&2
            return 1
        fi

        staging="$(mktemp -d "$ROOT_DIR/.${name}.checkout.XXXXXX")"
        rmdir "$staging"
        if ! GIT_TERMINAL_PROMPT=0 git clone "$url" "$staging"; then
            rm -rf "$staging"
            printf 'Could not clone %s from %s\n' "$name" "$url" >&2
            return 1
        fi
        if ! validate_component "$name" "$staging" "$@"; then
            rm -rf "$staging"
            printf 'The %s Git repository is empty or incomplete.\n' "$name" >&2
            return 1
        fi
        if [[ -e "$directory" ]]; then
            rm -rf "$staging"
            printf 'Component destination appeared during clone: %s\n' "$directory" >&2
            return 1
        fi
        mv "$staging" "$directory"
        printf 'Cloned %s into %s\n' "$name" "${directory#"$ROOT_DIR"/}"
        return
    fi

    if [[ ! -d "$directory/.git" ]] ||
       [[ "$(git -C "$directory" rev-parse --show-toplevel 2>/dev/null || true)" != "$directory" ]]; then
        if [[ -f "$directory/.createos-public-assembled" ]]; then
            printf 'Component %s is a local assembled copy, not a Git checkout.\n' "$name" >&2
            printf 'Move or remove this exact directory before assembling from Git: %s\n' "$directory" >&2
        else
            printf 'Refusing to replace non-Git component directory: %s\n' "$directory" >&2
        fi
        return 1
    fi

    actual_origin="$(git -C "$directory" remote get-url origin 2>/dev/null || true)"
    if [[ "$(normalize_url "$actual_origin")" != "$(normalize_url "$url")" ]]; then
        printf 'Unexpected origin for %s: %s\n' "$name" "${actual_origin:-<none>}" >&2
        return 1
    fi
    if ! validate_component "$name" "$directory" "$@"; then
        return 1
    fi
    if ((CHECK_ONLY)); then
        printf 'Checked %s in %s\n' "$name" "${directory#"$ROOT_DIR"/}"
        return
    fi

    if [[ -n "$(git -C "$directory" status --porcelain --untracked-files=all)" ]]; then
        printf 'Refusing to update %s because it has local changes.\n' "$name" >&2
        return 1
    fi
    branch="$(git -C "$directory" symbolic-ref --quiet --short HEAD || true)"
    upstream="$(git -C "$directory" rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
    if [[ -z "$branch" || -z "$upstream" ]]; then
        printf 'No current branch/upstream is configured for %s.\n' "$name" >&2
        return 1
    fi

    GIT_TERMINAL_PROMPT=0 git -C "$directory" fetch --prune origin
    git -C "$directory" merge --ff-only "$upstream"
    validate_component "$name" "$directory" "$@"
    printf 'Updated %s in %s\n' "$name" "${directory#"$ROOT_DIR"/}"
}

update_component documentation "$DOCS_URL" "$ROOT_DIR/docs" \
    kernel/overview.md userland/userland.md sdk/app-development.md
update_component userland "$USERLAND_URL" "$ROOT_DIR/rootfs_maker" \
    Makefile lib/user_syscall.h root/prog/shell/shell.c
update_component SDK "$SDK_URL" "$ROOT_DIR/sdk" \
    Makefile app.mk include/createos/uikit.h

if ((BUILD)); then
    make -C "$ROOT_DIR" all
else
    printf 'All Git components are ready. Run make to build CreateOS.\n'
fi
