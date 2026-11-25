#!/usr/bin/env python3
"""Collect timings, readbacks and memory counters for one Renoir boot."""

import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import re
import subprocess
import time


def output(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.PIPE).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe_dir", type=Path, help="directory containing the four compiled x5000_uma probes")
    parser.add_argument("--kext", type=Path, help="require the running UUID to match this NootedRed.kext")
    parser.add_argument("--output", type=Path, help="create this results directory")
    parser.add_argument("--pressure-groups", type=int, metavar="N", help="also run the N-group Metal reclaim check")
    args = parser.parse_args()
    if args.pressure_groups is not None and args.pressure_groups <= 0:
        parser.error("--pressure-groups must be positive")

    flags = output("sysctl", "-n", "kern.bootargs").split()
    running = output("kmutil", "showloaded", "--list-only")
    driver = next((line for line in running.splitlines() if " org.ChefKiss.NootedRed " in line), "")
    match = re.search(r"\) ([0-9A-Fa-f-]{36}) ", driver)
    if not match:
        parser.error("Could not identify the loaded NootedRed UUID")
    uuid = match[1].upper()
    if args.kext and f"UUID: {uuid} " not in output("dwarfdump", "--uuid", args.kext / "Contents/MacOS/NootedRed"):
        parser.error(f"Running NootedRed UUID {uuid} differs from {args.kext}")

    def counters():
        return output("sysctl", "-i", "debug.nred_uma")

    before = counters()
    states = dict(re.findall(r"^debug\.nred_uma\.(\w+): (\d+)$", before, re.MULTILINE))
    checks = {"active": "-NRedUnifiedMemory", "texture_cache": "-NRedUMATextureCache",
              "pool_fence_reuse": "-NRedUMAFenceReuse", "fragment_64k": "-NRedUMA64K"}
    for name, flag in checks.items():
        if (flag in flags) != (states.get(name, "0") == "1"):
            parser.error(f"Boot argument {flag} and runtime sysctl {name} disagree")
    if ("-NRedUMAFragmentStats" in flags or "-NRedUMA64K" in flags) != (states.get("fragment_stats", "0") == "1"):
        parser.error("Fragment measurement state differs from the boot arguments")
    active = states.get("active") == "1"
    if args.pressure_groups and not active:
        parser.error("The reclaim check requires an active UMA driver")
    tools = {name: args.probe_dir.resolve() / ("x5000_uma_" + name) for name in ("churn", "workingset", "probe", "gl_probe")}
    hashes = {name: hashlib.sha256(binary.read_bytes()).hexdigest() for name, binary in tools.items()}
    started = datetime.now().astimezone()
    results = args.output or Path("build") / "uma-profiles" / f"{started:%Y%m%d-%H%M%S}-{uuid[:8]}"
    results.mkdir(parents=True, exist_ok=False)
    metadata = {"time": started.isoformat(), "macos": output("sw_vers", "-buildVersion"),
                "boot": output("sysctl", "-n", "kern.bootsessionuuid"), "driver": uuid,
                "boot_args": flags, "probe_sha256": hashes, "pressure_groups": args.pressure_groups}
    (results / "boot.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (results / "display.txt").write_text(output("system_profiler", "SPDisplaysDataType") + "\n")
    (results / "before.txt").write_text(before + "\n")
    print(f"Driver {uuid}, results in {results}", flush=True)

    def reset_reports():
        reports = Path("/Library/Logs/DiagnosticReports")
        return set(reports.glob("*gpuRestart")) | set(reports.glob("*.panic"))

    initial_reports = reset_reports()
    failures = []
    status = results / "status.tsv"
    status.write_text("workload\texit\n")

    def run(label, name, *options):
        with (results / (label + ".log")).open("x") as log:
            result = subprocess.run([tools[name], *options], stdout=log, stderr=subprocess.STDOUT)
        if active:
            (results / ("after-" + label + ".txt")).write_text(counters() + "\n")
        with status.open("a") as log:
            log.write(f"{label}\t{result.returncode}\n")
        print(f"{label}: exit {result.returncode}", flush=True)
        if result.returncode:
            failures.append(label)
        new_reports = reset_reports() - initial_reports
        if new_reports:
            (results / "reset-reports.txt").write_text("".join(f"{report}\n" for report in sorted(new_reports)))
            raise SystemExit(f"New GPU reset or panic report during {label}")

    mode = [] if active else ["--baseline"]
    try:
        run("churn", "churn", "--compare", "--async", "64", "16")
        run("page-latency", "workingset", "--latency", "64")
        run("working-set", "workingset", "256", "1024")
        run("private", "probe", *mode, "--groups", "1", "--surface-ipc", "--layouts", "--purgeable")
        run("gl", "gl_probe", "--smoke" if active else "--baseline")
        run("managed", "probe", *mode, "--groups", "1", "--managed")
        if args.pressure_groups:
            (results / "before-pressure.txt").write_text(counters() + "\n")
            run("pressure", "probe", "--groups", str(args.pressure_groups), "--idle", "10", "--require-reclaim")
            time.sleep(30)
            (results / "pressure-idle-30s.txt").write_text(counters() + "\n")
            run("churn-after-pressure", "churn", "64", "16")
        run("managed-purgeable", "probe", *mode, "--groups", "1", "--managed", "--purgeable")
    finally:
        (results / "after.txt").write_text(counters() + "\n")
    if failures:
        raise SystemExit("Failed checks: " + ", ".join(failures))


if __name__ == "__main__":
    main()
