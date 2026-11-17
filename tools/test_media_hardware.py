#!/usr/bin/env python3
"""Check the shared kernel/plugin IP policy in C and C++; no GPU or kext work."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r"""
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "MediaHardware.h"
#ifdef __cplusplus
static_assert(nredMediaIP(0x1636) == NRedMediaIPVCN22, "constexpr kernel policy");
static_assert(!nredMediaUsesVCN2Bridge(0x15DD), "VCN1 needs its own backend");
#endif
int main(void) {
    unsigned checks = 0;
    for (unsigned device = 0; device <= 0xFFFF; ++device) {
        const bool bridge = device == 0x1636 || device == 0x1638 ||
                            device == 0x164C || device == 0x15E7;
        const bool navi = device == 0x73BF || device == 0x73AF;
        const bool vcn1 = device == 0x15DD || device == 0x15D8;
        const enum NRedMediaIP expected = bridge ? NRedMediaIPVCN22 :
            navi ? NRedMediaIPVCN3 : vcn1 ? NRedMediaIPVCN1 : NRedMediaIPUnknown;
        assert(nredMediaIP(device) == expected);
        assert(nredMediaUsesVCN2Bridge(device) == bridge);
        assert(nredMediaVP9Supported(device) == (bridge || navi));
        assert(nredMediaVP9DPBTier(device) == (bridge ? 1U : 0U));
        assert(nredMediaVP9WriteCombine(device) == (navi ? 1U : 0U));
        checks += 5;
    }
    const unsigned malformed[] = {0x11636, 0x173BF, UINT_MAX};
    for (unsigned i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        assert(nredMediaIP(malformed[i]) == NRedMediaIPUnknown);
        assert(!nredMediaVP9Supported(malformed[i]));
        checks += 2;
    }
    printf("PASS %u shared PCI/IP/backend checks; no GPU work\n", checks);
    return 0;
}
"""


def main():
    with tempfile.TemporaryDirectory(prefix="nred-media-hardware-") as directory:
        source = Path(directory) / "test.c"
        source.write_text(SOURCE)
        for language, standard in (("c", "c11"), ("c++", "c++17")):
            binary = Path(directory) / ("test-" + language)
            subprocess.run([
                "xcrun", "clang", "-x", language, "-std=" + standard,
                "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "NootedRed"), str(source), "-o", str(binary),
            ], check=True)
            subprocess.run(["codesign", "-f", "-s", "-", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
