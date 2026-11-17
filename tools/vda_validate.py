#!/usr/bin/env python3
"""Bounded, hardware-required AVC/HEVC correctness tests using public APIs.

Requires macOS command line tools, PyAV and NumPy. All generated media, logs and
binaries go to --work-dir or a temporary directory, never beside this source.
This is not a sustained throughput, HDR metadata or long-term stability test.
"""
import argparse
from fractions import Fraction
import hashlib
import json
import math
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def run(command, log, timeout=120, env=None):
    with log.open("w") as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, env=env)
    if result.returncode:
        raise ValueError(f"command exited {result.returncode}: {log}")
    return log.read_text()


def hardware_rows(path, width, height, pixfmt):
    rows = [line.split() for line in path.read_text().splitlines()]
    if not rows:
        raise ValueError(f"no hardware output frames: {path}")
    if any(len(row) != 5 for row in rows):
        raise ValueError("invalid hardware output record")
    times = [Fraction(row[0]) for row in rows]
    if len(times) != len(set(times)):
        raise ValueError("duplicate output PTS")
    for row in rows:
        if row[2:] != [str(width), str(height), pixfmt]:
            raise ValueError(f"unexpected dimensions/format: {row}")
        if len(row[1]) != 32 or any(c not in "0123456789abcdef" for c in row[1]):
            raise ValueError("invalid effective-pixel MD5")
    return [row[1] for row in sorted(rows, key=lambda row: Fraction(row[0]))]


def compare_frames(actual, reference, expected):
    if len(actual) != expected or len(reference) != expected:
        raise ValueError(f"frame count mismatch: hardware={len(actual)}, software={len(reference)}, expected={expected}")
    mismatch = [i for i, (a, b) in enumerate(zip(actual, reference)) if a != b]
    if mismatch:
        raise ValueError(f"{len(mismatch)} effective-pixel mismatches; first display frame={mismatch[0]}")

def source_samples(shape, plane, index, bits):
    """Independent reference for the native probe's effective source pixels."""
    import numpy as np
    y, x = np.indices(shape)
    high = (16 + (x // 8 + y // 8 + index * 3) % 220 if plane == 0 else
            64 + ((y if plane == 1 else x) + index) % 128)
    if bits == 8:
        return high
    if bits != 10:
        raise ValueError(f"unsupported source bit depth: {bits}")
    low = ((x + y + index) % 4 if plane == 0 else
           (x + (y if plane == 1 else 2 * y) + index + (plane == 2)) % 4)
    return high * 4 + low


def reference(path, pixfmt, encoded, expected):
    import av
    import numpy as np
    hashes, quality = [], []
    with av.open(str(path)) as container:
        stream = container.streams.video[0]
        for index, source in enumerate(container.decode(video=0)):
            frame = source.reformat(format=pixfmt)
            if len(frame.planes) != 2:
                raise ValueError("reference is not a two-plane NV12/P010 frame")
            digest = hashlib.md5()
            rowbytes = frame.width * (2 if pixfmt == "p010le" else 1)
            for plane in frame.planes:
                data = memoryview(plane)
                for y in range(plane.height):
                    digest.update(data[y * plane.line_size:y * plane.line_size + rowbytes])
            hashes.append(digest.hexdigest())
            # Independent reconstruction check catches wrong pitch/chroma that
            # hardware-vs-software decode of the same bad encode would miss.
            if encoded and index in (0, expected // 2, expected - 1):
                bits = 10 if pixfmt == "p010le" else 8
                planar = source.reformat(format="yuv420p10le" if bits == 10 else "yuv420p")
                scores = []
                for pindex, plane in enumerate(planar.planes):
                    itemsize = 2 if bits == 10 else 1
                    actual = np.ndarray((plane.height, plane.width), dtype="<u2" if bits == 10 else np.uint8,
                                        buffer=memoryview(plane), strides=(plane.line_size, itemsize))
                    target = source_samples(actual.shape, pindex, index, bits)
                    mse = np.mean((actual.astype(np.float64) - target) ** 2)
                    score = 99.0 if mse == 0 else 10 * math.log10(((1 << bits) - 1) ** 2 / mse)
                    scores.append(round(score, 3))
                quality.append({"frame": index, "source_bits": bits, "psnr_y_u_v": scores})
        profile = stream.codec_context.profile
    if quality and min(min(row["psnr_y_u_v"]) for row in quality) < 30:
        raise ValueError(f"source reconstruction PSNR below 30 dB: {quality}")
    return hashes, quality, profile


def generate(path, codec, width, height, frames, bitdepth=8):
    import av
    import numpy as np
    with av.open(str(path), "w") as container:
        stream = container.add_stream("libx264" if codec == "h264" else "libx265", rate=30)
        stream.width, stream.height = width, height
        stream.pix_fmt = "yuv420p10le" if bitdepth == 10 else "yuv420p"
        if codec == "hevc":
            stream.codec_context.codec_tag = "hvc1"
            stream.options = {"preset": "ultrafast", "x265-params": "pools=2:frame-threads=2:bframes=2:log-level=error"}
        else:
            stream.options = {"preset": "fast", "x264-params": "threads=2:bframes=2"}
        for number in range(frames):
            frame = av.VideoFrame(width, height, stream.pix_fmt)
            for index, plane in enumerate(frame.planes):
                dtype, itemsize = ("<u2", 2) if bitdepth == 10 else (np.uint8, 1)
                samples = np.ndarray((plane.height, plane.width), dtype=dtype, buffer=memoryview(plane),
                                     strides=(plane.line_size, itemsize))
                y, x = np.indices(samples.shape)
                samples[:] = ((64 + (x // 4 + y // 4 + number * 7 + index * 128) % 876)
                              if bitdepth == 10 else
                              (16 + (x // 4 + y // 4 + number * 7 + index * 32) % 220))
            frame.pts = number
            for packet in stream.encode(frame):
                container.mux(packet)
        for packet in stream.encode(None):
            container.mux(packet)


def markers():
    data = subprocess.check_output(["ioreg", "-a", "-r", "-n", "IGPU"])
    values = plistlib.loads(data)
    return {key: value for node in values for key, value in node.items() if key.startswith("NRed")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path)
    parser.add_argument("--frames", type=int, default=30)
    parser.add_argument("--require-manifest", action="store_true")
    parser.add_argument("--require-main10", action="store_true", help="fail before submitting work if Main10 encode is not advertised")
    parser.add_argument("--square-hevc", action="store_true", help="test revision-9 4096x4096 Main/Main10 encoding and decoding")
    args = parser.parse_args()
    if sys.platform != "darwin":
        print("INCOMPLETE: macOS is required"); return 77
    try:
        import av
        import numpy
    except ImportError:
        print("INCOMPLETE: PyAV and NumPy are required in this Python environment"); return 77
    if args.frames < 3 or args.frames > 120:
        raise ValueError("--frames must be 3..120")
    work = args.work_dir or Path(tempfile.mkdtemp(prefix="nred-codecs-"))
    work.mkdir(parents=True, exist_ok=True)
    if work.resolve() == ROOT or ROOT in work.resolve().parents:
        raise ValueError("--work-dir must be outside the source repository")
    print(f"Results: {work}", flush=True)
    bootargs = subprocess.check_output(["sysctl", "-n", "kern.bootargs"], text=True).split()
    if "-NRedNoVDA" in bootargs:
        print("SKIP: hardware media explicitly disabled by -NRedNoVDA"); return 77
    before = markers()
    if args.require_main10 and before.get("NRedHEVC10EncodeAdvertised") != 1:
        raise ValueError("Main10 encode not advertised by this boot; install/reboot with the matching kext first")
    for key in ("NRedVCNX5000ABIValidated", "NRedVCNX6000ABIValidated",
                "NRedVCNNativeLoadReady", "NRedVCNEngineAllocated"):
        if before.get(key) != 1:
            raise ValueError(f"{key} is not 1; no GPU work submitted")
    if args.require_manifest and before.get("NRedMediaPatchSupported") != 1:
        raise ValueError("bounded media manifest not active; reboot with the matching kext first")
    if args.square_hevc and (before.get("NRedUserPatchRevision") != 9 or
                            before.get("NRedMediaPatchPagesExpected") != 18 or
                            before.get("NRedMediaPatchSitesExpected") != 34 or
                            before.get("NRedHEVC10EncodeAdvertised") != 1 or
                            "-NRedNoHEVC" in bootargs or "-NRedNoHEVC10" in bootargs):
        raise ValueError("square HEVC requires the active revision-9 manifest and Main10 policy; no GPU work submitted")
    if before.get("NRedVCNMediaFailure", 0):
        raise ValueError("media failure already latched; no GPU work submitted")
    report = {"bootargs": bootargs, "before": before, "cases": [], "skipped": [],
              "scope": "bounded correctness; not sustained throughput/HDR/sleep/concurrency"}
    report["loaded_kext"] = subprocess.check_output(["kextstat", "-l"], text=True)
    binary = work / "codec-probe"
    run(["xcrun", "clang", "-O2", "-fobjc-arc", "-Wno-deprecated-declarations",
         str(ROOT / "tools/vda_codec_probe.m"), "-framework", "Foundation",
         "-framework", "AVFoundation", "-framework", "VideoToolbox",
         "-framework", "CoreMedia", "-framework", "CoreVideo", "-o", str(binary)], work / "build.log")
    run(["codesign", "--force", "--sign", "-", str(binary)], work / "sign.log")
    failed = False
    cases = []
    codecs = ["h264"]
    if "-NRedNoHEVC" in bootargs:
        report["skipped"].append("HEVC: disabled by -NRedNoHEVC")
    else:
        codecs.append("hevc")
    for codec in codecs:
        for width, height in [(1920, 1080), (1918, 1080), (3840, 2160)]:
            name = f"{codec}-{width}x{height}"
            cases.append((name, codec, width, height, 8, True))
        cases.append((f"{codec}-bframes-1080p", codec, 1920, 1080, 8, False))
    if "hevc" in codecs and "-NRedNoHEVC10" not in bootargs:
        cases.append(("hevc-main10-bframes-1080p", "hevc", 1920, 1080, 10, False))
        if before.get("NRedHEVC10EncodeAdvertised") == 1:
            for width, height in [(1920, 1080), (1918, 1080), (3840, 2160)]:
                cases.append((f"hevc-main10-{width}x{height}", "hevc", width, height, 10, True))
        else:
            report["skipped"].append("Main10 encode: not advertised by this boot")
    else:
        report["skipped"].append("Main10 encode/decode: disabled by boot policy")
    if args.square_hevc:
        cases.extend((f"hevc-{profile}-4096x4096", "hevc", 4096, 4096, bits, True)
                     for profile, bits in (("main", 8), ("main10", 10)))
    for name, codec, width, height, bits, encoded in cases:
        row = {"case": name, "expected_frames": args.frames, "hardware_encode": encoded}
        try:
            prefix = work / name
            if encoded:
                env = os.environ.copy()
                if width == height == 4096 and codec == "hevc":
                    env["NRED_PROBE_SQUARE_HEVC"] = "1"
                log = run([str(binary), "encode", "hevc10" if bits == 10 else codec,
                           str(width), str(height), str(args.frames), str(prefix)],
                          work / f"{name}.log", env=env)
                source = work / f"{name}.{codec}"
                if "encode_hardware=1 query_status=0" not in log:
                    raise ValueError("encoder did not confirm actual hardware selection")
            else:
                source = work / f"{name}.mp4"
                generate(source, codec, width, height, args.frames, bits)
                log = run([str(binary), "decode", str(source), str(prefix), str(bits)], work / f"{name}.log")
            if "decode_hardware=1 query_status=0" not in log:
                raise ValueError("decoder did not confirm actual hardware selection")
            fmt = "p010le" if bits == 10 else "nv12"
            actual = hardware_rows(work / f"{name}.hw.md5", width, height, fmt)
            expected, quality, profile = reference(source, fmt, encoded, args.frames)
            compare_frames(actual, expected, args.frames)
            expected_profile = "High" if codec == "h264" else "Main 10" if bits == 10 else "Main"
            if profile != expected_profile:
                raise ValueError(f"unexpected bitstream profile: {profile}, expected {expected_profile}")
            row.update(result="PASS", actual_frames=len(actual), mismatches=0, profile=profile, source_quality=quality)
            print(f"PASS {name}: hardware confirmed, {len(actual)} reference frames identical", flush=True)
        except (ValueError, OSError, subprocess.TimeoutExpired) as error:
            row.update(result="FAIL", error=str(error))
            failed = True
            print(f"FAIL {name}: {error}", flush=True)
        report["cases"].append(row)
        # A failed native submission/decoder can indicate a stalled engine.
        # Stop this matrix instead of submitting more cases to it.
        if failed:
            break
    after = markers()
    report["after"] = after
    if after.get("NRedVCNMediaFailure", 0) or after.get("NRedVCNResourceFailures", 0) > before.get("NRedVCNResourceFailures", 0):
        failed = True
        report["state_failure"] = "new media/resource failure"
    if after.get("NRedVCNLastHardwareInit") != 1 or after.get("NRedVCNEngineAllocated") != 1:
        failed = True
        report["state_failure"] = "hardware engine no longer ready"
    if args.require_manifest and after.get("NRedMediaPatchComplete") != 1:
        failed = True
        report["manifest_failure"] = "not every required VA/GVA page observed; validation incomplete"
    report["result"] = "FAIL" if failed else "PASS"
    (work / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    for item in report["skipped"]:
        print(f"SKIP {item}")
    print(f"{'FAILED' if failed else 'PASSED'}: {len(report['cases'])}/{len(cases)} cases executed; results.json", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit(f"FAILED: {error}")
