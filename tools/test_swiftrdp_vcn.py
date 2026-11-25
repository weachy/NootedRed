#!/usr/bin/env python3
"""Run the original SwiftRDP H.264 XCTest on the booted ordered-fence kext."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time


def output(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.PIPE).strip()


def channel_counts():
    text = output("ioreg", "-rc", "AMDRadeonX5000_AMDVega10GraphicsAccelerator", "-d1", "-w0")
    counts = {}
    for line in text.splitlines():
        if '"PerformanceStatisticsAccum" =' in line:
            for channel, value in re.findall(r'HWChannel (VCN0\w+) \| Commands Completed"\s*=\s*(\d+)', line):
                counts[channel] = max(counts.get(channel, 0), int(value))
    if not {"VCN0Enc", "VCN0EncLLQ", "VCN0Dec"}.issubset(counts):
        raise SystemExit("Missing cumulative VCN channel counters")
    return counts


def submission_counts():
    text = output("sysctl", "-n", "debug.nred_vcn_submissions")
    counts = {channel: {"records": int(records), "failures": int(failures)}
              for channel, records, failures in re.findall(r"(VCN0\w+)=(\d+)/(\d+)", text)}
    if counts.keys() != {"VCN0Dec", "VCN0Enc", "VCN0EncLLQ"}:
        raise SystemExit("Missing ordered-fence submission counters")
    return counts


def reports():
    return {path.name for path in Path("/Library/Logs/DiagnosticReports").iterdir()
            if path.suffix in (".gpuRestart", ".panic") or "watchdog_timeout" in path.name
            or path.name.startswith(("panic-full-", "panic-base-"))}


def run_case(case, bundle, log, initial_reports):
    selector = f"SwiftRDPCoreTests.H264WireFormatTests/{case}"
    deadline = time.monotonic() + 90
    with log.open("x") as stream, subprocess.Popen(["xcrun", "xctest", "-XCTest", selector, str(bundle)],
                                                   stdout=stream, stderr=subprocess.STDOUT) as test:
        try:
            while True:
                new_reports = sorted(reports() - initial_reports)
                if new_reports:
                    raise SystemExit(f"Diagnostics during {case}: {', '.join(new_reports)}")
                if time.monotonic() >= deadline:
                    raise SystemExit(f"{case} exceeded 90 seconds. See {log}")
                try:
                    return test.wait(timeout=0.5)
                except subprocess.TimeoutExpired:
                    continue
        finally:
            if test.poll() is None:
                test.kill()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kext", type=Path)
    parser.add_argument("test_bundle", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    binary = args.kext / "Contents/MacOS/NootedRed"
    candidate = output("dwarfdump", "--uuid", binary).split()[1]
    loaded = next((line for line in output("kmutil", "showloaded", "--list-only").splitlines()
                   if " org.ChefKiss.NootedRed " in line), "")
    if candidate not in loaded.split():
        raise SystemExit(f"Candidate {candidate} is not loaded. Running driver: {loaded.strip()}")
    props = output("ioreg", "-rn", "IGPU", "-d1", "-w0")
    for key in ("NRedVCNEngineAllocated", "NRedVCNOrderedFence"):
        if not re.search(rf'"{key}"\s*=\s*1\b', props):
            raise SystemExit(f"The running Renoir bridge did not publish {key} = 1")
    test_binary = args.test_bundle / "Contents/MacOS" / args.test_bundle.stem
    metadata = {"kext": candidate, "boot": output("sysctl", "-n", "kern.bootsessionuuid"),
                "os": output("sw_vers", "-buildVersion"), "test_bundle": str(args.test_bundle.resolve()),
                "test_sha256": hashlib.sha256(test_binary.read_bytes()).hexdigest()}
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "boot.json").write_text(json.dumps(metadata, indent=2) + "\n")
    initial_reports = reports()
    handoff = "testFullHDStreamKeepsSRGBGrayLevelsWithoutAnExtraRangeAttachment"
    llq_rounds = 0
    with (args.output / "results.jsonl").open("x", buffering=1) as results:
        for index, case in enumerate([handoff] * args.rounds + ["testEncoderEmitsOnlyExplicitIDRs"]):
            before = channel_counts()
            bridge_before = submission_counts()
            label = f"{index + 1:02d}-{case}"
            results.write(json.dumps({"started": label, "before": before, "bridge": bridge_before}) + "\n")
            os.fsync(results.fileno())
            log = args.output / (label + ".log")
            try:
                status = run_case(case, args.test_bundle.resolve(), log, initial_reports)
            except SystemExit as error:
                results.write(json.dumps({"aborted": label, "reason": str(error),
                                          "new_reports": sorted(reports() - initial_reports)}) + "\n")
                raise
            text = log.read_text()
            passed = re.search(rf"H264WireFormatTests {case}\]' passed \(([0-9.]+) seconds\)", text)
            if status or not passed or "Executed 1 test, with 0 failures" not in text:
                raise SystemExit(f"XCTest failed or did not execute {case}. See {log}")
            time.sleep(2)
            after = channel_counts()
            bridge_after = submission_counts()
            new_reports = sorted(reports() - initial_reports)
            delta = {key: after[key] - before[key] for key in before}
            results.write(json.dumps({"finished": label, "test_seconds": float(passed.group(1)),
                                      "after": after, "delta": delta, "bridge": bridge_after,
                                      "new_reports": new_reports}) + "\n")
            active = {key for key in bridge_after if delta[key] > 0
                      and bridge_after[key]["records"] > bridge_before[key]["records"]}
            failed = any(bridge_after[key]["failures"] != bridge_before[key]["failures"]
                         or bridge_after[key]["records"] < bridge_before[key]["records"] for key in bridge_after)
            if (new_reports or failed or min(delta.values()) < 0 or not active.intersection({"VCN0Enc", "VCN0EncLLQ"})
                    or (case == handoff and "VCN0Dec" not in active)):
                raise SystemExit(f"Check the channel counters and diagnostic reports in {results.name}")
            if case == handoff and "VCN0EncLLQ" in active:
                llq_rounds += 1
            print(f"{label} passed", flush=True)
        time.sleep(10)
        final_reports = sorted(reports() - initial_reports)
        results.write(json.dumps({"full_hd_llq_rounds": llq_rounds, "full_hd_rounds": args.rounds,
                                  "new_reports": final_reports}) + "\n")
        if final_reports:
            raise SystemExit("A diagnostic report arrived after XCTest teardown")
        if not llq_rounds:
            raise SystemExit("Full-HD cases completed without observed VCN0EncLLQ activity on channel 14")
        print(f"VCN0EncLLQ advanced during {llq_rounds}/{args.rounds} full-HD cases")


if __name__ == "__main__":
    main()
