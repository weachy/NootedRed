#!/usr/bin/env python3
"""CPU-only regression checks for the Renoir hardware validation runner."""

import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET

import test_swiftrdp_vcn as runner


class QueueEvidence(unittest.TestCase):
    counters = ("VCN0Dec=6/0 VCN0Enc=0/0 VCN0EncLLQ=28/0 writebacks=6/0/28 "
                "writebackTrace=1 firstReset=0 unmapVA=1")

    def test_video_placement_requires_new_decisions(self):
        with patch.object(runner, "native_video_textures", return_value=7) as sample:
            self.assertEqual(runner.texture_delta(None, 2), {})
            sample.assert_not_called()
            self.assertEqual(runner.texture_delta(3, 4), {"native_texture_decisions": 4})
            for start in (6, 7, 8):
                with self.subTest(start=start), self.assertRaises(SystemExit):
                    runner.texture_delta(start, 2)

    def test_queue_updates_match_submission_activity(self):
        with patch.object(runner, "output", return_value=self.counters):
            before = runner.submission_counts()
        text = self.counters.replace("6/0 VCN", "7/0 VCN").replace("28/0 write", "29/0 write")
        with patch.object(runner, "output", return_value=text.replace("6/0/28", "6/0/29")):
            after = runner.submission_counts(before)
        self.assertEqual(runner.active_queues(before, after), {"VCN0EncLLQ"})

    def test_missing_decreasing_or_disabled_writebacks(self):
        with patch.object(runner, "output", return_value=self.counters):
            before = runner.submission_counts()
        for text in (self.counters.replace("writebacks=", "unused="), self.counters.replace("6/0/28", "5/0/28"),
                     self.counters.replace("writebackTrace=1", "writebackTrace=0")):
            with self.subTest(text=text), patch.object(runner, "output", return_value=text), self.assertRaises(SystemExit):
                runner.submission_counts(before)

    def test_recovery_cannot_supply_hardware_evidence(self):
        props = '"NRedVCNResetCount" = 0\n"NRedVCNSkipReplayRetired" = 0'
        runner.check_recovery(props)
        for text in (props.replace("0", "1", 1), props[:-1] + "1", ""):
            with self.subTest(text=text), self.assertRaises(SystemExit):
                runner.check_recovery(text)


class KernelReportCollection(unittest.TestCase):
    def test_boot_mismatch_and_log_footer(self):
        event = {"bootUUID": "test-boot", "senderImagePath": "/driver/AMDRadeonX5000",
                 "timestamp": "20:52:07", "eventMessage": "GPU report\nqueue stalled"}
        rows = [{**event, "bootUUID": "old-boot", "eventMessage": "old fault"}, event, {"finished": 1}]
        result = subprocess.CompletedProcess([], 0, stdout="\n".join(map(json.dumps, rows)), stderr="")
        with tempfile.TemporaryDirectory() as directory, patch.object(runner.subprocess, "run", return_value=result):
            log = Path(directory) / "full.kernel.log"
            capture = runner.save_kernel_log(log, "test-boot", 1, 2)
            self.assertEqual(capture, {"file": log.name, "events": 1})
            self.assertEqual(log.read_text(), "20:52:07 AMDRadeonX5000 GPU report\nqueue stalled\n")

    def test_log_error_preserves_reset_reason(self):
        results = io.StringIO()
        with patch.object(runner.subprocess, "run", side_effect=OSError("log unavailable")), \
                patch.object(runner, "reports", return_value=set()), self.assertRaisesRegex(SystemExit, "VCN reset"):
            with runner.record_abort(results, "full", Path("full.log"), "test-boot", set()):
                raise SystemExit("VCN reset")
        self.assertEqual(json.loads(results.getvalue()), {"aborted": "full", "reason": "VCN reset",
                         "kernel_log": {"error": "log unavailable"}, "new_reports": []})


class SwiftPMValidation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.package = Path(scratch.name)
        (cls.package / "Package.swift").write_text(
            '// swift-tools-version: 5.9\nimport PackageDescription\n'
            'let package = Package(name: "Fixture", targets: [.testTarget(name: "SwiftRDPCoreTests")])\n')
        source = cls.package / "Tests/SwiftRDPCoreTests"
        source.mkdir(parents=True)
        (source / "Fixture.swift").write_text(f"""
import Foundation
import XCTest

final class H264WireFormatTests: XCTestCase {{
    func {runner.HANDOFF}() throws {{
        let environment = ProcessInfo.processInfo.environment
        if environment["NRED_FIXTURE_CASE"] == "skip" {{ throw XCTSkip("fixture") }}
        if let marker = environment["NRED_FIXTURE_WORKER"] {{
            try "\\(getpid()) \\(getpgrp()) \\(getsid(0))".write(toFile: marker, atomically: true, encoding: .utf8)
            Thread.sleep(forTimeInterval: 30)
        }}
    }}
    func testAnotherCase() {{}}
}}
""")
        build = ["xcrun", "swift", "build", "--package-path", str(cls.package), "-c", "release"]
        subprocess.run([*build, "--build-tests"], check=True, stdout=subprocess.DEVNULL)
        cls.bundle = Path(runner.output(*build, "--show-bin-path")) / "FixturePackageTests.xctest"
        executable = cls.bundle / "Contents/MacOS" / cls.bundle.stem
        cls.digest = hashlib.sha256(executable.read_bytes()).hexdigest()

    def setUp(self):
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.xml = Path(scratch.name) / "full.xml"
        self.log = self.xml.with_suffix(".log")
        self.command = runner.full_suite_command(self.bundle, self.package, self.digest, self.xml)

    def test_release_binary_mismatch(self):
        with self.assertRaisesRegex(SystemExit, "must match"):
            runner.full_suite_command(self.bundle, self.package, "0" * 64, self.xml)

    def test_complete_missing_and_partial_xml(self):
        with self.assertRaisesRegex(SystemExit, "Missing or incomplete"):
            runner.full_suite_result(self.xml, self.log)
        with self.log.open("w") as log:
            subprocess.run(self.command, stdout=log, stderr=subprocess.STDOUT, check=True)
        result = runner.full_suite_result(self.xml, self.log)
        self.assertEqual((result["xctest_cases"], result["handoff"]), (2, "passed"))
        tree = ET.parse(self.xml)
        suite = tree.find("testsuite")
        suite.remove(suite.find("testcase"))
        tree.write(self.xml)
        with self.assertRaisesRegex(SystemExit, "do not cover"):
            runner.full_suite_result(self.xml, self.log)

    def test_skipped_handoff(self):
        with self.log.open("w") as log:
            subprocess.run(self.command, stdout=log, stderr=subprocess.STDOUT, check=True,
                           env={**os.environ, "NRED_FIXTURE_CASE": "skip"})
        with self.assertRaisesRegex(SystemExit, "skipped"):
            runner.full_suite_result(self.xml, self.log)

    def test_reset_stops_xctest_worker(self):
        marker = self.xml.with_suffix(".worker")

        def check_reset(baseline):
            if marker.exists():
                raise SystemExit("simulated VCN reset")

        with patch.dict(os.environ, {"NRED_FIXTURE_WORKER": str(marker)}), patch.object(runner, "reports", set), \
                patch.object(runner, "submission_counts", check_reset), patch.object(runner, "check_recovery"), \
                self.assertRaisesRegex(SystemExit, "simulated VCN reset"):
            runner.run_tests(self.command, self.log, set(), 20, {})
        worker, group, session = map(int, marker.read_text().split())
        self.assertNotEqual(group, session)
        state = subprocess.run(["ps", "-p", str(worker), "-o", "stat="], text=True, stdout=subprocess.PIPE)
        self.assertTrue(state.returncode == 1 or state.stdout.strip().startswith("Z"))
        self.assertFalse(self.xml.exists())


if __name__ == "__main__":
    unittest.main()
