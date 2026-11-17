#!/usr/bin/env python3
"""Check the current production GL resolver tables without obsolete page files.

The old TahoePatchData.hpp/PagePatch tables no longer exist. Actual page
matching, repatching and failure behavior are exercised by
test_userpatch_lifecycle.py; media fingerprints by media_patch_manifest.py.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
ARRAY = re.compile(r"static const UInt8 (\w+)\[\]\s*=\s*\{(.*?)\};", re.S)
TABLE = re.compile(
    r"static const UserPatches::SignaturePatch (\w+)\[\]\s*=\s*\{(.*?)\n    \};", re.S)
ROW = re.compile(
    r"\{(\w+),\s*(\w+),\s*sizeof\((\w+)\),\s*(\d+),\s*(\w+),\s*"
    r"sizeof\((\w+)\),\s*(\d+),\s*(\d+),\s*~static_cast<UInt64>\(0\),\s*"
    r"(\w+),\s*sizeof\((\w+)\)\}")


def main():
    text = (ROOT / "NootedRed/UserPatches.cpp").read_text()
    arrays = {
        name: bytes(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", body))
        for name, body in ARRAY.findall(text)
    }
    arrays["kResolverMaskFF"] = bytes([255]) * 128
    tables = TABLE.findall(text)
    assert {name for name, _ in tables} == {
        "kResolverSignatures", "kResolverSignatures_Ventura",
        "kResolverSignatures_Sonoma", "kResolverSignatures_Tahoe",
    }, "unexpected or missing production GL table"
    checks = 0
    for name, body in tables:
        rows = ROW.findall(body)
        assert len(rows) == 5 and body.count("{") == 5, (name, "unparsed or missing rows")
        for index, (context, mask, context_size, offset, replace, replace_size, skip, count, find, find_size) in enumerate(rows):
            assert context == context_size and replace == replace_size and find == find_size
            c, m, f, r = (arrays[key] for key in (context, mask, find, replace))
            at = int(offset)
            assert len(c) <= len(m) and (len(c) == 32 if index != 2 else len(c) >= 32)
            assert len(f) == len(r) > 0 and at + len(f) <= len(c)
            assert c[at:at + len(f)] == f, (name, find, "original site differs from context")
            assert m[at:at + len(f)] == b"\xff" * len(f), (name, find, "site must be exact")
            assert all(byte in (0, 255) for byte in m[:len(c)]) and sum(bool(byte) for byte in m[:len(c)]) >= 20
            assert int(skip) == 0 and int(count) == 1
            checks += 1
    print(f"PASS {checks} current GL signatures in {len(tables)} tables")


if __name__ == "__main__":
    main()
