#!/bin/zsh
# Build the optional VCN VP9 plugin and its diagnostic harness.
# All products go under the repository's ignored build/ directory.
set -eu
cd "${0:A:h}"
ROOT="${PWD:A:h:h}"
OUT="${ROOT}/build/media-vp9"
BUNDLE="${OUT}/VP9VCNDecoder.bundle"
mkdir -p "${BUNDLE}/Contents/MacOS"
cp vtvp9/Info.plist "${BUNDLE}/Contents/Info.plist"
clang -arch x86_64 -O2 -bundle -fvisibility=hidden \
  -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security \
  -fno-strict-overflow -Wno-four-char-constants -Wno-deprecated-declarations \
  -mmacosx-version-min=13.0 \
  vtvp9/VP9VCNDecoder.c vcnvp9/vcnvp9.c vcnvp9/vp9parse.c \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -framework IOKit -framework IOSurface \
  -o "${BUNDLE}/Contents/MacOS/VP9VCNDecoder"
codesign -f -s - "${BUNDLE}"
codesign --verify --strict "${BUNDLE}"
clang -arch x86_64 -O1 -Wall -Wextra -Wno-unused-parameter \
  -fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wformat -Werror=format-security \
  -fno-strict-overflow -Wno-four-char-constants -Wno-deprecated-declarations \
  -mmacosx-version-min=13.0 \
  vtvp9/vtvp9test.c vcnvp9/vp9parse.c \
  -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
  -framework VideoToolbox -o "${OUT}/vtvp9test"
python3 generate_allowlist.py "${BUNDLE}" "${ROOT}/NootedRed/VP9PluginAllowlist.inc"
echo "Built ${BUNDLE}; rebuild NootedRed to pair it with this plugin."
