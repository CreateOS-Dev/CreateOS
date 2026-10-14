#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SOURCE="$ROOT/build/deps/lwip"
REVISION=0a0452b2c39bdd91e252aef045c115f88f6ca773

if [ ! -d "$SOURCE/.git" ]; then
    mkdir -p "$ROOT/build/deps"
    git clone --no-checkout https://github.com/lwip-tcpip/lwip.git "$SOURCE"
    git -C "$SOURCE" checkout --detach "$REVISION"
elif [ "$(git -C "$SOURCE" rev-parse HEAD)" != "$REVISION" ]; then
    printf 'Unexpected lwIP checkout in %s; refusing to change it.\n' "$SOURCE" >&2
    exit 1
fi

printf '%s\n' "$SOURCE"
