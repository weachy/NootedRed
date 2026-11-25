#!/usr/bin/env python3
"""Validate SwiftRDP's full release suite and isolated H.264 sessions on the booted Renoir kext."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import xml.etree.ElementTree as ET


HANDOFF = "testFullHDStreamKeepsSRGBGrayLevelsWithoutAnExtraRangeAttachment"
SUITE = "SwiftRDPCoreTests.H264WireFormatTests"
NATIVE_SUITE = "SwiftRDPCoreTests.AVCNativeVideoTests"
NATIVE_CASE = "testSerialAVC420KeepsFullHDAnd4KVideoOnTheNativeCodec"
CHANNELS = ("VCN0Dec", "VCN0Enc", "VCN0EncLLQ")


def output(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.PIPE, timeout=20).strip()
    except subprocess.TimeoutExpired as error:
        raise SystemExit(f"{args[0]} timed out while reading validation data") from error
    except (OSError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Could not read validation data from {args[0]}: {error}") from error


def check_recovery(properties=None):
    if properties is None:
        properties = output("ioreg", "-rn", "IGPU", "-d1", "-w0")
    for key in ("NRedVCNResetCount", "NRedVCNSkipReplayRetired"):
        if not re.search(rf'"{key}"\s*=\s*0\b', properties):
            raise SystemExit(f"A clean hardware run requires {key} = 0")


def submission_counts(previous=None):
    text = output("sysctl", "-n", "debug.nred_vcn_submissions")
    fields = text.split()
    if "firstReset=1" in fields:
        raise SystemExit("The driver captured a VCN reset on this boot")
    if not {"firstReset=0", "unmapVA=1", "writebackTrace=1"}.issubset(fields):
        raise SystemExit("The running driver must trace first reset, hardware writeback and unmapVA")
    counts = {channel: {"records": int(records), "failures": int(failures)}
              for channel, records, failures in re.findall(r"(VCN0\w+)=(\d+)/(\d+)", text)}
    updates = re.search(r"\bwritebacks=(\d+)/(\d+)/(\d+)\b", text)
    if counts.keys() != set(CHANNELS) or updates is None:
        raise SystemExit("Missing VCN queue submissions or writebacks")
    for channel, count in zip(CHANNELS, updates.groups()):
        counts[channel]["writebacks"] = int(count)
    if previous and any(counts[key]["failures"] != previous[key]["failures"] or
                        counts[key]["records"] < previous[key]["records"] or
                        counts[key]["writebacks"] < previous[key]["writebacks"] for key in counts):
        raise SystemExit(f"VCN submission counters changed unexpectedly: {previous} -> {counts}")
    return counts


def active_queues(before, after):
    return {key for key in CHANNELS if after[key]["records"] > before[key]["records"]
            and after[key]["writebacks"] > before[key]["writebacks"]}


def native_video_textures():
    return int(output("sysctl", "-n", "debug.nred_uma.native_vcn_textures"))


def texture_delta(before, minimum):
    if before is None:
        return {}
    delta = native_video_textures() - before
    if delta < minimum:
        raise SystemExit(f"Expected at least {minimum} native VCN texture decisions, observed {delta}")
    return {"native_texture_decisions": delta}


def reports():
    return {path.name for path in Path("/Library/Logs/DiagnosticReports").iterdir()
            if not path.name.startswith(".") and (path.suffix.lower() in (".gpurestart", ".panic")
            or "watchdog_timeout" in path.name or path.name.startswith(("panic-full-", "panic-base-")))}


def save_kernel_log(destination, boot, started, ended):
    predicate = ('process == "kernel" AND (senderImagePath ENDSWITH "AMDRadeonX5000" OR '
                 'senderImagePath ENDSWITH "AMDRadeonX5000HWLibs" OR senderImagePath ENDSWITH "IOAcceleratorFamily2")')
    try:
        result = subprocess.run(["/usr/bin/log", "show", "--style", "ndjson", "--start", f"@{int(started)}",
                                 "--end", f"@{int(ended) + 1}", "--predicate", predicate],
                                capture_output=True, text=True, errors="replace", timeout=20)
        if result.returncode:
            return {"error": result.stderr.strip() or f"log show exited with {result.returncode}"}
        with destination.open("x") as stream:
            count = 0
            for raw in result.stdout.splitlines():
                event = json.loads(raw)
                if event.get("bootUUID") != boot or not event.get("eventMessage"):
                    continue
                sender = Path(event["senderImagePath"]).name
                stream.write(f"{event['timestamp']} {sender} {event['eventMessage']}\n")
                count += 1
        return {"file": destination.name, "events": count}
    except (OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
        return {"error": str(error)}


def sample_xctest(test, log):
    try:
        workers = subprocess.run(["pgrep", "-x", "xctest"], capture_output=True, text=True, timeout=2)
        for worker in workers.stdout.split():
            try:
                if os.getsid(int(worker)) != test.pid:
                    continue
                if int(worker) != test.pid:
                    test.send_signal(signal.SIGSTOP)
                with log.with_suffix(".sample.log").open("x") as status:
                    subprocess.run(["/usr/bin/sample", worker, "1", "10", "-file", str(log.with_suffix(".sample.txt"))],
                                   stdout=status, stderr=subprocess.STDOUT, timeout=5)
                return
            except ProcessLookupError:
                continue
    except (OSError, subprocess.SubprocessError):
        pass


def stop_test_session(test):
    deadline = time.monotonic() + 3
    test.kill()
    while True:
        test.poll()
        remaining = []
        for row in output("ps", "-axo", "pid=,stat=").splitlines():
            pid, state = row.split()
            if state.startswith("Z"):
                continue
            pid = int(pid)
            try:
                if os.getsid(pid) == test.pid:
                    os.kill(pid, signal.SIGKILL)
                    remaining.append(pid)
            except ProcessLookupError:
                pass
        if not remaining or time.monotonic() >= deadline:
            return remaining
        time.sleep(0.1)


def run_tests(command, log, initial_reports, timeout, baseline):
    deadline = time.monotonic() + timeout
    with log.open("x") as stream:
        test = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while True:
                new_reports = sorted(reports() - initial_reports)
                if new_reports:
                    raise SystemExit(f"New diagnostics during {log.name}: {', '.join(new_reports)}")
                submission_counts(baseline)
                check_recovery()
                if test.poll() is not None:
                    return test.returncode
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise SystemExit(f"{log.name} exceeded {timeout} seconds")
                try:
                    test.wait(timeout=min(remaining, 1))
                except subprocess.TimeoutExpired:
                    pass
        except BaseException as error:
            try:
                sample_xctest(test, log)
            finally:
                remaining = stop_test_session(test)
            if remaining:
                raise SystemExit(f"{error}. Test session still blocked: {remaining}. See {log}") from error
            raise


@contextmanager
def record_abort(results, label, log, boot, initial_reports):
    started = time.time()
    try:
        yield
    except SystemExit as error:
        capture = save_kernel_log(log.with_suffix(".kernel.log"), boot, started, time.time())
        results.write(json.dumps({"aborted": label, "reason": str(error), "kernel_log": capture,
                                  "new_reports": sorted(reports() - initial_reports)}) + "\n")
        raise


def full_suite_command(bundle, package, digest, xml):
    if package is None:
        package = next((folder for folder in bundle.parents if (folder / "Package.swift").is_file()), None)
    if package is None or not (package / "Package.swift").is_file():
        raise SystemExit("Supply --swift-package PATH for a copied XCTest bundle")
    options = ["--package-path", str(package.resolve()), "-c", "release"]
    binaries = Path(output("xcrun", "swift", "build", *options, "--show-bin-path"))
    executable = binaries / bundle.name / "Contents/MacOS" / bundle.stem
    if not executable.is_file() or hashlib.sha256(executable.read_bytes()).hexdigest() != digest:
        raise SystemExit("The SwiftPM release test executable must match the supplied XCTest bundle")
    return ["xcrun", "swift", "test", *options, "--skip-build", "--parallel", "--num-workers", "1", "--verbose",
            "--xunit-output", str(xml.resolve())]


def case_completion(text, name, suite=SUITE):
    selector = re.escape(f"-[{suite} {name}]")
    matches = re.findall(rf"^Test Case '{selector}' (passed|failed|skipped) \(([0-9.]+) seconds?\)\.",
                         text, flags=re.MULTILINE)
    if len(matches) != 1 or matches[0][0] == "skipped":
        raise SystemExit(f"XCTest did not finish {name} exactly once: {matches}")
    outcome, seconds = matches[0]
    return outcome, float(seconds)


def full_suite_result(xml, log):
    try:
        cases = ET.parse(xml).findall(".//testcase")
    except (OSError, ET.ParseError) as error:
        raise SystemExit(f"Missing or incomplete full suite results in {xml}: {error}") from error
    text = log.read_text(errors="replace")
    announced = re.findall(r"^\[\d+/(\d+)\] Testing (.+)$", text, flags=re.MULTILINE)
    reported = {f"{case.get('classname')}/{case.get('name')}": case for case in cases}
    selector = f"{SUITE}/{HANDOFF}"
    if (len(cases) < 2 or len(reported) != len(cases) or {int(total) for total, _ in announced} != {len(cases)}
            or {name for _, name in announced} != reported.keys() or selector not in reported):
        raise SystemExit(f"Full suite results do not cover the scheduled XCTest cases and 1080p handoff. See {xml}")
    outcome, seconds = case_completion(text, HANDOFF)
    failed = [name for name, case in reported.items() if case.find("failure") is not None or case.find("error") is not None]
    if reported[selector].find("skipped") is not None or (outcome == "failed") != (selector in failed):
        raise SystemExit(f"Inconsistent 1080p handoff result. See {xml} and {log}")
    return {"xctest_cases": len(cases), "xctest_failures": len(failed), "failed_cases": failed,
            "handoff": outcome, "handoff_seconds": seconds}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("kext", type=Path)
    parser.add_argument("test_bundle", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--verify-video-placement", action="store_true",
                        help="check the native VCN texture decision counter during the hardware controls")
    parser.add_argument("--swift-package", type=Path, help="SwiftRDP package containing the matching release build")
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
    check_recovery(props)
    test_binary = args.test_bundle / "Contents/MacOS" / args.test_bundle.stem
    metadata = {"kext": candidate, "boot": output("sysctl", "-n", "kern.bootsessionuuid"),
                "os": output("sw_vers", "-buildVersion"), "test_bundle": str(args.test_bundle.resolve()),
                "test_sha256": hashlib.sha256(test_binary.read_bytes()).hexdigest()}
    bundle = args.test_bundle.resolve()
    xml = args.output / "full.xml"
    baseline = submission_counts()
    metadata["bridge"] = baseline
    full = full_suite_command(bundle, args.swift_package, metadata["test_sha256"], xml)
    args.output.mkdir(parents=True, exist_ok=False)
    (args.output / "boot.json").write_text(json.dumps(metadata, indent=2) + "\n")
    initial_reports = reports()
    llq_rounds = 0
    failures = []
    with (args.output / "results.jsonl").open("x", buffering=1) as results:
        native_log = args.output / "native-avc420.log"
        with record_abort(results, NATIVE_CASE, native_log, metadata["boot"], initial_reports):
            textures_before = native_video_textures() if args.verify_video_placement else None
            command = ["xcrun", "xctest", "-XCTest", f"{NATIVE_SUITE}/{NATIVE_CASE}", str(bundle)]
            results.write(json.dumps({"started": NATIVE_CASE, "command": command, "before": baseline}) + "\n")
            status = run_tests(command, native_log, initial_reports, 90, baseline)
            outcome, seconds = case_completion(native_log.read_text(errors="replace"), NATIVE_CASE, NATIVE_SUITE)
            after = submission_counts(baseline)
            active = active_queues(baseline, after)
            results.write(json.dumps({"finished": NATIVE_CASE, "xctest": outcome, "exit": status,
                                      "test_seconds": seconds, "after": after, "active": sorted(active),
                                      **texture_delta(textures_before, 4)}) + "\n")
            if status or outcome != "passed" or "VCN0Dec" not in active or not active.intersection(CHANNELS[1:]):
                raise SystemExit(f"The native 1080p/4K hardware control did not pass. See {native_log}")
        results.write(json.dumps({"started": "full", "command": full}) + "\n")
        full_log = args.output / "full.log"
        with record_abort(results, "full", full_log, metadata["boot"], initial_reports):
            status = run_tests(full, full_log, initial_reports, 1800, baseline)
            details = full_suite_result(xml, full_log)
        new_reports = sorted(reports() - initial_reports)
        results.write(json.dumps({"finished": "full", "exit": status, "new_reports": new_reports, **details}) + "\n")
        if new_reports or status not in (0, 1) or (bool(status) != bool(details["xctest_failures"])):
            raise SystemExit(f"Full suite exited abnormally. See {full_log}")
        if details["xctest_failures"]:
            failures.append(f"Full suite had {details['xctest_failures']} XCTest failures")
        for index, case in enumerate([HANDOFF] * args.rounds + ["testEncoderEmitsOnlyExplicitIDRs"]):
            bridge_before = submission_counts(baseline)
            label = f"{index + 1:02d}-{case}"
            results.write(json.dumps({"started": label, "bridge": bridge_before}) + "\n")
            os.fsync(results.fileno())
            log = args.output / (label + ".log")
            selector = f"{SUITE}/{case}"
            with record_abort(results, label, log, metadata["boot"], initial_reports):
                textures_before = native_video_textures() if args.verify_video_placement and case == HANDOFF else None
                status = run_tests(["xcrun", "xctest", "-XCTest", selector, str(bundle)], log, initial_reports, 90,
                                   bridge_before)
                text = log.read_text(errors="replace")
                outcome, seconds = case_completion(text, case)
                if (status not in (0, 1) or bool(status) != (outcome == "failed")
                        or not re.search(r"\bExecuted 1 test, with \d+ failures?", text)):
                    raise SystemExit(f"XCTest exited abnormally. See {log}")
                time.sleep(2)
                check_recovery()
                bridge_after = submission_counts(bridge_before)
                placement = texture_delta(textures_before, 2)
            new_reports = sorted(reports() - initial_reports)
            delta = {key: bridge_after[key]["writebacks"] - bridge_before[key]["writebacks"] for key in CHANNELS}
            results.write(json.dumps({"finished": label, "xctest": outcome, "exit": status, "test_seconds": seconds,
                                      "writeback_delta": delta, "bridge": bridge_after,
                                      "new_reports": new_reports, **placement}) + "\n")
            active = active_queues(bridge_before, bridge_after)
            if new_reports:
                raise SystemExit(f"Check the channel counters and diagnostic reports in {results.name}")
            if outcome == "failed":
                failures.append(f"{label} failed")
            if not active.intersection({"VCN0Enc", "VCN0EncLLQ"}) or (case == HANDOFF and "VCN0Dec" not in active):
                failures.append(f"{label} lacks hardware completion evidence")
            if case == HANDOFF and "VCN0EncLLQ" in active:
                llq_rounds += 1
            print(f"{label}: XCTest {outcome}, active queues {sorted(active)}", flush=True)
        time.sleep(10)
        submission_counts(baseline)
        check_recovery()
        final_reports = sorted(reports() - initial_reports)
        if not llq_rounds:
            failures.append("No full-HD case showed VCN0EncLLQ completion on channel 14")
        results.write(json.dumps({"full_hd_llq_rounds": llq_rounds, "full_hd_rounds": args.rounds,
                                  "new_reports": final_reports, "failures": failures}) + "\n")
        if final_reports:
            raise SystemExit("A diagnostic report arrived after XCTest teardown")
        if failures:
            raise SystemExit(f"Validation failed. See {results.name}")
        print(f"VCN0EncLLQ advanced during {llq_rounds}/{args.rounds} full-HD cases")


if __name__ == "__main__":
    main()
