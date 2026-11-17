#!/usr/bin/env python3
"""Execute the current GL signature selector, not the removed PatchSet table.

Media enablement has separate exact-build and device gates in test_media_policy
and test_userpatch_lifecycle. Selecting GL signatures is not media approval.
"""
from pathlib import Path
import re
import subprocess
import tempfile

from test_userpatch_lifecycle import REPO, STUBS, function


def main():
    source = (REPO / "NootedRed/UserPatches.cpp").read_text()
    header = re.sub(r"^#(?:include|pragma).*$", "",
                    (REPO / "NootedRed/UserPatches.hpp").read_text(), flags=re.M)
    header = header.replace("private:", "public:")
    begin = source.index("namespace\n{")
    tables = source[begin:source.index("}    // namespace", begin) + 1]
    selector = function(source, "void UserPatches::selectPatchSet()")
    plan = re.sub(r"^#(?:include|pragma).*$", "",
                  (REPO / "NootedRed/MediaPatchPlan.hpp").read_text(), flags=re.M)
    unit = STUBS + plan + r"""
namespace KernelVersion {
    constexpr int Ventura = 22, Sonoma = 23, Sequoia = 24, Tahoe = 25;
}
int runningMajor;
int getKernelVersion() { return runningMajor; }
""" + header + tables + selector + r"""
int main() {
    unsigned checks = 0;
    for (runningMajor = 19; runningMajor <= 27; ++runningMajor) {
        UserPatches patches;
        patches.selectPatchSet();
        const auto* expected = runningMajor >= 25 ? kResolverSignatures_Tahoe :
            runningMajor == 24 ? kResolverSignatures :
            runningMajor == 23 ? kResolverSignatures_Sonoma : kResolverSignatures_Ventura;
        if (patches.signatures != expected ||
            patches.signatureCount != (expected ? 5U : 0U)) {
            std::fprintf(stderr, "FAIL Darwin %d actual GL selector\n", runningMajor);
            return 1;
        }
        ++checks;
    }
    std::printf("PASS %u actual GL signature selections; no GPU work\n", checks);
}
"""
    with tempfile.TemporaryDirectory(prefix="nred-selector-") as directory:
        cpp, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        cpp.write_text(unit)
        subprocess.run([
            "xcrun", "clang++", "-std=c++17", "-O1", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            "-I", str(REPO / "NootedRed"), str(cpp), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
