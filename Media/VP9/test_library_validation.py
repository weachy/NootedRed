#!/usr/bin/env python3
"""Exercise the production AMFI adapter's exact hash gate and ABI forwarding."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "NootedRed/VP9LibraryValidation.cpp").read_text()
start = source.index("    int validateLibrary(")
brace = source.index("{", start)
depth, end = 1, brace + 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
unit = r"""
#include <cstdint>
#include <cstdio>
#include <cstring>
using UInt8 = uint8_t;
using UInt64 = uint64_t;
""" + (ROOT / "NootedRed/VP9PluginAllowlist.inc").read_text() + r"""
using LibraryValidation = int (*)(void*, void*, void*, UInt64, UInt64, UInt64, UInt64);
using CDHash = const UInt8* (*)(void*);
LibraryValidation originalValidation = nullptr;
CDHash copyCDHash = nullptr;
unsigned calls = 0, checks = 0, failures = 0;
void check(bool ok) { ++checks; if (!ok) ++failures; }
const UInt8* hashValue = nullptr;
const UInt8* hash(void* file) { check(file == (void*)2); return hashValue; }
int original(void* p, void* f, void* facts, UInt64 a4, UInt64 a5, UInt64 a6, UInt64 a7) {
    ++calls;
    check(p == (void*)1 && f == (void*)2 && facts == (void*)3 &&
          a4 == 0x1234 && a5 == 0xabcdef && a6 == 0xffffffffffffffffULL && a7 == 7);
    return -45;
}
""" + function + r"""
int invoke() {
    return validateLibrary((void*)1, (void*)2, (void*)3,
                           0x1234, 0xabcdef, 0xffffffffffffffffULL, 7);
}
int main() {
    originalValidation = original;
    check(invoke() == -45 && calls == 1);  // missing hash accessor
    copyCDHash = hash;
    check(invoke() == -45 && calls == 2);  // null hash
    UInt8 candidate[sizeof(VP9PluginAllowlist::CDHashBytes)];
    std::memcpy(candidate, VP9PluginAllowlist::CDHashBytes, sizeof(candidate));
    hashValue = candidate;
    check(invoke() == 1 && calls == 2);
    for (size_t i = 0; i < sizeof(candidate); ++i) {
        candidate[i] ^= 1;
        auto before = calls;
        check(invoke() == -45 && calls == before + 1);
        candidate[i] ^= 1;
    }
    originalValidation = nullptr;
    hashValue = nullptr;
    check(invoke() == 0);  // defensive failure path rejects
    std::printf("%u checks, %u failures; no kernel hooks installed\n", checks, failures);
    return failures ? 1 : 0;
}
"""
with tempfile.TemporaryDirectory(prefix="nred-vp9-library-test-") as directory:
    temporary = Path(directory)
    cpp, executable = temporary / "test.cpp", temporary / "test"
    cpp.write_text(unit)
    subprocess.run(["xcrun", "clang++", "-std=c++17", "-O1",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(executable)], check=True)
    raise SystemExit(subprocess.run([str(executable)]).returncode)
