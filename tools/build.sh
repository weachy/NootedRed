#!/bin/sh
# Build the offscreen OpenGL blending probe (tools/gl_blend_probe.c).
#
# Compiling this tool does NOT touch the GPU; only running it does. The default
# runtime mode is --inspect (context + GL strings), and the risky --render mode
# is consent-gated. Read tools/README.md first.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OUT="$ROOT/build/tools"
SRC="$ROOT/tools/gl_blend_probe.c"

mkdir -p "$OUT"

if ! xcrun --find clang >/dev/null 2>&1; then
    echo "error: xcrun clang not found (Xcode command line tools required)" >&2
    exit 1
fi

SDK=$(xcrun --show-sdk-path)
CC=$(xcrun --find clang)

# OpenGL.framework is deprecated since macOS 10.14 but still present (and still
# the only way to reach the AMD user-space GL driver that issue #158 is about),
# hence the silenced deprecation warnings.
set -x
"$CC" \
    -x objective-c \
    -fobjc-arc \
    -std=c11 \
    -O1 \
    -g \
    -Wall \
    -Wextra \
    -Wno-deprecated-declarations \
    -Wno-unused-parameter \
    -isysroot "$SDK" \
    -mmacosx-version-min=15.0 \
    -framework OpenGL \
    -framework CoreFoundation \
    -framework AppKit \
    "$SRC" \
    -o "$OUT/gl_blend_probe"

set +x
echo "built: $OUT/gl_blend_probe"
echo
echo "inspect    : $OUT/gl_blend_probe --inspect"
echo "no-draw    : $OUT/gl_blend_probe --link"
echo "render     : $OUT/gl_blend_probe --render --yes   # can reproduce a GPU reset"
