#!/usr/bin/env python3
"""Replay the reviewed host GL resource-list code using production patch bytes.

This fixture is specific to the macOS 26.7.2 x86_64 X5000 GL driver. Only the
reviewed CPU code and import stub pages become executable; every external call
used by the fixture is redirected to a CPU stub. No GPU submission takes place.
"""
import argparse
import hashlib
from pathlib import Path
import platform
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DRIVER = Path(
    "/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle"
    "/Contents/MacOS/AMDRadeonX5000GLDriver"
)
REVIEWED_PAGES = {
    0x5000: "db1fdc7a2af8ced39da83c8907b3d185d49d98f0b182a0cd4e9109dce2f4511c",
    0xEC000: "308edf7a9187f0c5f174c2fa98d3510fffbf31ade6d7eaddf7895dda38b8db71",
}


def patch_bytes(header, name):
    match = re.search(rf"\b{name}\[\]\s*=\s*\{{([^}}]+)\}}", header)
    if not match:
        raise ValueError(f"missing production byte array: {name}")
    result = bytes(int(value, 16) for value in re.findall(r"0x[0-9a-fA-F]+", match[1]))
    if len(result) != 50:
        raise ValueError(f"unexpected {name} length: {len(result)}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host-driver", type=Path, default=DEFAULT_DRIVER)
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "x86_64":
        parser.error("this machine-code fixture requires x86_64 macOS")
    driver = args.host_driver.resolve()
    image = bytearray(driver.read_bytes())
    header = (ROOT / "NootedRed/GLResourceBudgetPatch.hpp").read_text()
    original, patched = (patch_bytes(header, name) for name in ("Find", "Replace"))
    # The running per-page backend can make file reads observe our patched
    # page. Normalize only the reviewed 50-byte site, then require the SAME
    # complete page fingerprints; never accept unrelated page differences.
    site = 0x572A
    if image[site:site + len(patched)] == patched:
        image[site:site + len(original)] = original
    for offset, expected in REVIEWED_PAGES.items():
        actual = hashlib.sha256(image[offset:offset + 4096]).hexdigest()
        if actual != expected:
            parser.error(f"host driver page {offset:#x} differs from the reviewed fixture")
    with tempfile.TemporaryDirectory(prefix="nred-gl-budget-") as directory:
        temp = Path(directory)
        fixture = temp / "reviewed-driver"
        fixture.write_bytes(image)
        arrays = ["#include <stdint.h>"]
        for source, target in (("Find", "budget_find"), ("Replace", "budget_replace")):
            values = ",".join(f"0x{value:02x}" for value in patch_bytes(header, source))
            arrays.append(f"static const uint8_t {target}[] = {{{values}}};")
        (temp / "budget-bytes.h").write_text("\n".join(arrays) + "\n")
        binary = temp / "replay"
        subprocess.run([
            "xcrun", "clang", "-std=c11", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wall", "-Wextra", "-I", str(temp),
            str(ROOT / "tools/gl_resource_budget_replay.c"), "-o", str(binary),
        ], check=True)
        print("Reviewed CPU pages verified; using current production Find/Replace.", flush=True)
        subprocess.run([str(binary), str(fixture)], check=True)


if __name__ == "__main__":
    main()
