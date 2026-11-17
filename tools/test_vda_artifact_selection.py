#!/usr/bin/env python3
"""Check real artifact-selection scripts using a temporary repo and mock ESP.

No real EFI, installed kext, boot argument or GPU is accessed. Deployment is
exercised only against ordinary files inside the temporary directory.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    if not shutil.which("ditto"):
        print("SKIP: macOS ditto is required for the temporary deployment fixture")
        return 77
    checks = 0

    def check(condition, detail=""):
        nonlocal checks
        assert condition, detail
        checks += 1

    with tempfile.TemporaryDirectory(prefix="nred-artifact-selection-") as directory:
        root = (Path(directory) / "repo").resolve()
        tools = root / "tools"
        tools.mkdir(parents=True)
        for name in ("build-branch.sh", "release_kext_path.sh", "deploy_vda.sh"):
            shutil.copy2(ROOT / "tools" / name, tools / name)
        subprocess.run(["git", "init", "-q", str(root)], check=True)
        subprocess.run(["git", "-C", str(root), "symbolic-ref", "HEAD",
                        "refs/heads/fixture-review"], check=True)
        subprocess.run(["git", "-C", str(root), "-c", "user.name=NootedRed test",
                        "-c", "user.email=test@invalid", "-c", "commit.gpgSign=false",
                        "-c", "core.hooksPath=/dev/null", "commit", "--allow-empty",
                        "-qm", "artifact selection fixture"], check=True)
        fake_bin = root / "fake-bin"
        fake_bin.mkdir()
        xmake = fake_bin / "xmake"
        xmake.write_text("#!/bin/sh\nexit 0\n")
        xmake.chmod(0o755)
        env = dict(os.environ)
        env.pop("NRED_BUILD_BRANCH", None)
        env["PATH"] = str(fake_bin) + os.pathsep + env["PATH"]

        def run(name, *args, extra=None):
            return subprocess.run(
                ["bash", str(tools / name), *map(str, args)], cwd=root,
                env={**env, **(extra or {})}, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15)

        for override, branch in (
                (None, "fixture-review"), ("codex/review candidate", "codex-review-candidate"),
                ("///", "unknown")):
            extra = {} if override is None else {"NRED_BUILD_BRANCH": override}
            resolved = run("release_kext_path.sh", extra=extra)
            build = run("build-branch.sh", extra=extra)
            expected = root / "build/branches" / branch
            check(resolved.returncode == build.returncode == 0)
            check(resolved.stdout.strip() ==
                  str(expected / "macosx/x86_64/release/NootedRed.kext"), resolved.stdout)
            check(f"artifacts: {expected}\n" in build.stdout)

        esp = root / "mock-esp"
        destination = esp / "EFI/OC/Kexts/NootedRed.kext"

        def put_kext(path, data):
            executable = path / "Contents/MacOS/NootedRed"
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(data)
            return executable

        installed = put_kext(destination, b"original-installed-fixture")
        for branch in ("fixture-review", "shared", "unrelated"):
            put_kext(root / "build/branches" / branch /
                     "macosx/x86_64/release/NootedRed.kext", b"stale-fixture")
        put_kext(root / "build/macosx/x86_64/release/NootedRed.kext", b"legacy-fixture")
        selected = root / "build/branches/wanted/macosx/x86_64/release/NootedRed.kext"
        result = run("deploy_vda.sh", "--esp", esp, extra={"NRED_BUILD_BRANCH": "wanted"})
        check(result.returncode != 0 and str(selected) in result.stdout)
        check(installed.read_bytes() == b"original-installed-fixture")
        check(not list((root / "build/efi-backup").glob("*.kext")))

        put_kext(selected, b"selected-fixture")
        result = run("deploy_vda.sh", "--esp", esp, extra={"NRED_BUILD_BRANCH": "wanted"})
        check(result.returncode == 0)
        check(installed.read_bytes() == b"selected-fixture")
        backups = list((root / "build/efi-backup").glob("*.kext/Contents/MacOS/NootedRed"))
        check(len(backups) == 1 and backups[0].read_bytes() == b"original-installed-fixture")

        explicit = root / "explicit candidate/NootedRed.kext"
        put_kext(explicit, b"explicit-fixture")
        result = run("deploy_vda.sh", "--esp", esp, "--kext", explicit,
                     extra={"NRED_BUILD_BRANCH": "missing"})
        check(result.returncode == 0)
        check(installed.read_bytes() == b"explicit-fixture")
        backups = list((root / "build/efi-backup").glob("*.kext/Contents/MacOS/NootedRed"))
        check(len(backups) == 2 and
              {p.read_bytes() for p in backups} ==
              {b"original-installed-fixture", b"selected-fixture"})
        result = run("deploy_vda.sh", "--esp", esp, "--kext", destination)
        check(result.returncode != 0 and "outside the install destination" in result.stdout)
        check(installed.read_bytes() == b"explicit-fixture")
        result = run("deploy_vda.sh", "--kext")
        check(result.returncode != 0 and "missing --kext path" in result.stdout)

    print(f"PASS {checks} build/deployment artifact checks; temporary files only")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
