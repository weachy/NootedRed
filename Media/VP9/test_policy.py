#!/usr/bin/env python3
"""Test the actual Security.framework policy against signed code; no GPU work."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def command(*args):
    return subprocess.run(args, capture_output=True, text=True)


def verify(policy, code, expected, label):
    result = command("/usr/bin/codesign", "--verify", "--strict",
                     "-R", str(policy), str(code))
    success = result.returncode == 0
    if success != expected:
        raise RuntimeError(f"{label}: {result.stdout}{result.stderr}")
    print("PASS", label)


def main():
    bundle = (Path(sys.argv[1]) if len(sys.argv) > 1 else
              ROOT / "build/media-vp9/VP9VCNDecoder.bundle")
    header = (ROOT / "NootedRed/VP9PluginAllowlist.inc").read_text()
    with tempfile.TemporaryDirectory(prefix="nred-vp9-policy-test-") as directory:
        temporary = Path(directory)
        policies = {}
        for name in ("Original", "Replacement"):
            encoded = re.search(
                rf"{name}\[\] = \{{(.*?)\}};", header, re.DOTALL)[1]
            data = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", encoded))
            assert len(data) == 200
            policies[name] = temporary / name
            policies[name].write_bytes(data)
        verify(policies["Original"], "/usr/bin/true", True, "original accepts Apple code")
        verify(policies["Replacement"], "/usr/bin/true", True, "replacement accepts Apple code")
        verify(policies["Original"], bundle, False, "original rejects the third-party decoder")
        verify(policies["Replacement"], bundle, True, "replacement accepts the paired decoder")

        other = temporary / "unrelated"
        shutil.copyfile("/usr/bin/true", other)
        signed = command("/usr/bin/codesign", "-f", "-s", "-", str(other))
        if signed.returncode:
            raise RuntimeError(signed.stderr)
        verify(policies["Replacement"], other, False, "replacement rejects unrelated adhoc code")

        changed = temporary / "VP9VCNDecoder.bundle"
        shutil.copytree(bundle, changed)
        executable = changed / "Contents/MacOS/VP9VCNDecoder"
        content = bytearray(executable.read_bytes())
        # Alter a signed executable page without re-signing it.
        content[4096] ^= 1
        executable.write_bytes(content)
        verify(policies["Replacement"], changed, False, "replacement rejects altered decoder code")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
