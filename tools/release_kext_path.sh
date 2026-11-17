#!/bin/sh
# Resolve the same release artifact as build-branch.sh, without a fallback to
# another branch or an old shared build. This command only prints a path.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BRANCH=${NRED_BUILD_BRANCH:-}
if [ -z "$BRANCH" ]; then
    BRANCH=$(git -C "$ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || true)
    if [ -z "$BRANCH" ] || [ "$BRANCH" = "HEAD" ]; then
        BRANCH=detached-$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)
    fi
fi
BRANCH=$(printf '%s' "$BRANCH" | sed -E 's/[^A-Za-z0-9._-]+/-/g; s/^-+//; s/-+$//')
[ -n "$BRANCH" ] || BRANCH=unknown
printf '%s/build/branches/%s/macosx/x86_64/release/NootedRed.kext\n' "$ROOT" "$BRANCH"
