#!/usr/bin/env python3
"""Compile/sign/run the production VP9 message builder against mock objects."""
from pathlib import Path
import subprocess
import tempfile


def main():
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="nred-vp9-message-") as directory:
        binary = Path(directory) / "test"
        subprocess.run([
            "/usr/bin/clang", "-arch", "x86_64", "-O1", "-fno-strict-aliasing",
            "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-Wno-deprecated-declarations", "-Wno-four-char-constants",
            str(source / "test_message.c"), str(source / "vcnvp9/vp9parse.c"),
            "-framework", "CoreFoundation", "-framework", "CoreMedia",
            "-framework", "CoreVideo", "-framework", "VideoToolbox",
            "-framework", "IOKit", "-framework", "IOSurface", "-o", str(binary),
        ], check=True)
        subprocess.run(["/usr/bin/codesign", "-f", "-s", "-", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
