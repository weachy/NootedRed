# Renoir AVC / HEVC / VP9 capability expansion

**November 17 follow-up:** [Revision 8](media-revision8-20261117.md) records
the current candidate, the verified encoder initialization repair, corrected
test attribution and the remaining square-resolution limitations.

Target: Ryzen 7 PRO 4750U, PCI `1002:1636`, VCN 2.2, macOS 26.7.2
(`25G332`). These results describe tested driver behavior, not a guarantee
of every feature or the peak frame rate of every video.

Later on November 16: the 22:23:15 +08:00 reboot loaded revision 6.
Revision 7's portability changes, subsequent public-API acceptance, and the
22:19 HEVC experiment's GPU reset/watchdog incident are recorded in
[media-portability-20261116.md](media-portability-20261116.md).
The installation history below describes its earlier observation time.

## Code changes

Revision 5 added four reviewed resolution-check instruction windows: three for
HEVC and one for AVC. Revision 6 adds two AppleGVA instruction windows to connect
AVC's `NumberOfSlices` property to its existing native initialization path, and
one VA window to raise AVC's separate encode-height limit to 4096.
The resulting manifest has **13 pages and 25 sites**. Eleven revision-5 pages
are unchanged; the existing capability page gains only the AVC encode window.
Kernel/cache/image identities and complete masked page fingerprints remain
required.

| Path | Previous restriction | New bounded behavior |
| --- | --- | --- |
| AVC decode | Width 4096, height 2304, 9,437,184 pixels | Height up to 4096; existing width, minimum, profile and pixel checks retained |
| HEVC Main/Main10 decode | Ordinary-client width check reads the 4096-wide field | Reads the existing 8192-wide field; height 4352 and 35,651,584-pixel maximum |
| HEVC decode advertisement | Level 5.1 (`153`) | Level 6.2 (`186`), only on the reviewed Renoir build/backend |
| VP9 profile 0/2 decode | Revision 5 rejected 8192x4352 due to inter-frame chroma errors | Full 8192x4352 rectangle, 35,651,584 pixels; explicit tier-1 reference layout on Renoir |
| AVC `NumberOfSlices` | Setter returned success without saving the request | Configurable before the first frame; native row/256 bound checks and matching slice headers |
| AVC encode | Height limited to 2304 | Width/height 4096; native profile/count checks and automatic Level 6 signaling |

`-NRedNoVDA`, `-NRedNoHEVC` and `-NRedNoHEVC10` retain their policies.
Main-only decode can use the larger HEVC size; removed ten-bit profiles are
not reintroduced. `NRedHEVC8KDecodeAdvertised` is cleared after a bridge failure,
and effective capabilities are withdrawn as before.

The VP9 session maximum rectangle and coded-frame validation have separate
purposes: a session can contain either wide or tall valid streams, while every
coded frame must satisfy the pixel budget. Invalid format descriptions are
rejected before VCN creation, and frames are checked again before submission.
The new Renoir tier-1 message specifies each reference plane's pitch, height and
size. Simply increasing the old implicit tier-0 allocation did not fix the
maximum corner. The fixed nine-picture allocation is 481,296,384 bytes for
eight-bit references, or 721,944,576 bytes for ten-bit references. Navi retains
the former tier-0 pixel guard. No GL budget, source-pitch divisor, GPU clock or
power policy is changed by this revision.

AppleGVA already replays `glob+0x10E68` through native `SetParameter(8)` at encoder
initialization. The old setter only logged that the property was unsupported.
The new setter converts a CFNumber into existing stack scratch, saves it only
before the first frame, and lets native initialization validate the count
against macroblock rows and 256 entries before allocating matching slice
headers. It does not force the driver's final count or hardcode four slices.
The neighboring keyframe-duration property's conversion, value, update flag
and return status are preserved; its optional diagnostic log supplies space.

The native setter reports success for runtime changes but VCN continues using
the original count, even when the native initialization guard and slice-control
packet suppression are removed in an isolated experiment. Revision 6 therefore
returns `-12915` after encoding has started, without changing the saved count.
Changing the count requires a new compression session. Failed CFNumber
conversions also leave the saved configuration untouched.

The VP9 parser additionally records the previous frame type. A keyframe has no
motion vectors to reuse, even though its `intra_only` syntax field is zero.
The new condition matches libvpx 1.15.2's `use_prev_frame_mvs` rule.

## Hardware correctness checks before installation

Initial tests used running kext revision 4. New AVC/HEVC instructions were tested
only in isolated processes using the original Apple decoder factories, with
explicit GPU selection and hardware-required VideoToolbox sessions. The HEVC
test process also raised its capability level. The final VP9 bundle was loaded
in-process with its exact dynamic decoder ID. These tests do not replace
post-reboot testing of the system decoder service.

| Test | Frames | Result |
| --- | ---: | --- |
| HEVC Main, 7680x4320 | 3 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main10, 7680x4320 | 3 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main, 8192x4352 | 30 | Hardware; every effective-pixel hash matches CPU |
| HEVC Main10, 8192x4352 | 3, then 30 | Hardware; every effective-pixel hash matches CPU |
| AVC, 2304x4096, including B frames | 3, then 30 | Hardware; every effective-pixel hash matches CPU |
| VP9 profile 0, 1920x1080 / 3840x2160 | 60 / 30 | Hardware; every frame matches CPU |
| VP9 profile 2, real ten-bit 3840x2160 | 3 | Hardware; every frame matches CPU |
| VP9 profiles 0 and 2, 7680x4320 | 30 each | Hardware; every frame matches CPU |
| VP9 profile 0, 8192x4320 / 7680x4352 | 3 each | Hardware; every frame matches CPU |
| VP9 profile 2, FATE ten-bit sample | 10 | Hardware; every frame matches reference |
| VP9 profile 0, 8192x4352, two loops | 60 | Hardware; every frame matches CPU |
| VP9 profile 2, 8192x4352 | 30 | Hardware; every frame matches CPU |
| VP9 single session: 640x360 → 1080p → 4K → 640x360 | 120 | Hardware; every frame matches CPU |
| AVC 1080p, requested counts 1 / 4 / 6 | 6 / 30 / 6 | Actual slice NAL counts and hardware/CPU decode agree |
| AVC 1080p, requested counts 0 / -1 / 1,000,000 | 3 each | Native bounds normalize to 1 / 68 / 68 slices; correct pixels |
| AVC 4096x2304, four slices | 30 | Actual slice NAL count and hardware/CPU decode agree |
| AVC 1080p, runtime 4 → 2 request | 12 | Request rejected with -12915; all twelve frames retain four slices and correct pixels |
| AVC encode 4096x4096, Baseline/CAVLC, Main/CABAC, High/CABAC | 30 each | Hardware encoding, independent CPU reconstruction, native SPS Level 6; hardware square decoding remains unresolved |

The final production VP9 bundle passes 11 cases / 379 frames with zero
mismatches or errors. The slice matrix passes 93 encoded frames; independent
source reconstruction and hardware-vs-software decoding both pass.
New streams, logs, binaries and all experiments live outside the repository:

```text
installer-staging/verification/gpu-status-2026-11-16/media-max-capability/
installer-staging/verification/gpu-status-2026-11-16/media-gap-fixes/
```

## Encoding and throughput

Public VideoToolbox sessions under the running revision 4 passed 30-frame
encode/decode tests at **4096x2304** for:

* H.264 Baseline with CAVLC, Main with CABAC, and High with CABAC.
* HEVC Main and Main10, the latter using source values that vary all ten bits.

All 150 hardware-decoded output frames match independent CPU decoding.
Independent reconstruction against the source patterns also passes; minimum
sampled per-plane PSNR is 38.694 dB for AVC, 48.985 dB for HEVC Main, and
47.761 dB for HEVC Main10. PPS inspection confirms the requested AVC entropy
modes. These encoder paths need no new format patch in this revision.

A separate 300-frame test reuses eight synthetic source buffers, keeps the
hardware requirement, and skips decoder pixel readback:

| 4096x2304 path | Encode fps, including first eight source fills | Decode fps without pixel readback |
| --- | ---: | ---: |
| AVC High | 49.98 | 122.50 |
| HEVC Main | 38.18 | 96.28 |
| HEVC Main10 | 22.61 | 65.46 |

VP9's short repeated ten-bit samples measured 77.6 fps at 3840x2160 and
12.7 fps at 7680x4320 without pixel hashing. Content, keyframe frequency,
shared power and memory bandwidth, concurrent GPU work and presentation
overhead all matter. These measurements are not sustained peak guarantees,
nor proof that a browser presents every frame smoothly.

## Remaining gaps and rejected experiments

This revision **does not claim complete hardware feature parity**.

1. **AVC 4096x4096 / Level 6:** increasing the pixel budget makes the API
   report hardware success, but all three output frames are identical blank
   images and fail CPU comparison. That pixel-budget patch is rejected.
   Portrait Level-5.2 streams work with only the height fix and the existing
   9,437,184-pixel budget. Isolated budget-override diagnostics succeed at
   4096x4080 and 4080x4096 but fail at 4096x4096 for Main and High streams.
   SPS/avcC level changes, two-slice streams, and progressive bottom-offset
   changes do not fix it. Legacy codec 0 also fails at 1080p. Native submitted
   message dimensions and buffer sizes do not show truncation. Feedback value
   at offset 20 differs (zero for blank output, one for correct output), but
   that feedback correlation alone does not establish its semantics.
   **November 17 update:** the bundled firmware's codec-7 slice parser
   demonstrably truncates 65,536 macroblocks to zero, rejects every slice
   start and returns error 6. See
   [avc-firmware-limit-20261117.md](avc-firmware-limit-20261117.md) for the
   byte-derived proof and newer-firmware comparison. The cause is identified;
   a safe firmware repair and square hardware decode are still not delivered.
2. **Runtime AVC slice-count changes:** explicitly rejected after the first
   frame, as described above. The successful path is initial configuration.
3. **Taller HEVC encoding:** isolated 4096x4096 AVC height testing passes
   90 frames across Baseline/Main/High with correct independent CPU
   reconstruction. Revision 6 includes only that AVC height change; fresh
   ordinary-session acceptance is still required after booting it.
   A separate HEVC 4096x4096 height-override
   experiment times out after 55 seconds and leaves the native process unable
   to exit cleanly. The user rebooted at 21:39:56 +08:00. Neither encode-height
   override was in EFI at that reboot; the HEVC override remains absent from
   production source. Do not repeat the
   hanging case or claim tall HEVC support from a successful session creation.
4. Additional rate-control modes, low-latency behavior, HDR metadata,
   sleep/wake, multi-session contention and sustained thermal throughput are
   not established by these tests.

## Host regression checks

```sh
python3 tools/media_patch_manifest.py
python3 tools/test_avc_slice_params.py
python3 tools/test_codec_decode_limits.py
python3 tools/test_hevc_encode_params.py
python3 tools/test_hevc_advertisement.py
python3 tools/test_media_policy.py
python3 tools/test_userpatch_lifecycle.py
python3 Media/VP9/test_message.py
python3 Media/VP9/test_policy.py
python3 Media/VP9/test_library_validation.py
```

The slice test executes the actual original/patched setter with real
CoreFoundation conversions and replays the native row/256 validator: 71 checks,
including conversion failure, lazy initialization, runtime rejection and the
adjacent duration setter. The dimension test executes 264 original/patched
capability cases with mock objects and a stubbed lock-status helper. The page
lifecycle test passes 897 checks including all thirteen actual host page
snapshots, under address/undefined-behavior sanitizers. Its fixture selects a
page with at least three sites instead of relying on a fixed manifest index.
The VP9 test executes its
actual message builder, checks pitches, buffer bounds and ten-bit fields, tests
keyframe/inter-frame motion-vector state, tier-0 and tier-1 layouts, and invalid
dimension rejection before submission. These host tests do not submit GPU work.

## Installation and post-reboot acceptance

The kext and VP9 bundle must be rebuilt and installed together because the
allowlist contains the exact signed bundle's cdhash. Full EFI and old-bundle
backups, installed-file hashes and a rollback record are kept outside Git.

The revision-6 pair was installed at **2026-11-16 21:52:04 +08:00**:

* Kext UUID: `5AE4002E-7E9E-3E58-8AB0-151B86ABB83D`.
* Kext binary SHA-256:
  `ee0b09acc48f2154dde254db1b0870c055cf32447673343de7a824564996a7b8`.
* VP9 binary SHA-256:
  `0bfe6647619aa86338738a147b2baa1be4d4bcde01bb7e3f4bbcb499a0378729`.
* Paired plugin cdhash: `4cd8c29a47b24fca3b5d51b08e515f477f792bce`.

Both installed trees match the tested build byte for byte. The complete EFI
and previous plugin backup is
`installer-staging/backups/20261116-215203-before-media-revision6/`.
Hashes of every other EFI file and system video plugin are unchanged.
The current boot, started at **21:39:56 +08:00**, uses revision 5. It passes
twelve ordinary public-API H.264/HEVC Main/Main10 encode/decode cases (36
frames), including odd pitch, 4K and B frames, with zero mismatches. All twelve
revision-5 pages were observed (`4095`), with 22 expected sites and no VCN
resource failures. Revision 6 was installed after that reboot; it is not loaded
yet.

After the next reboot, require revision 6, the new kext UUID, 13 observed
manifest pages (`8191`), 25 expected sites, `NRedHEVC8KDecodeAdvertised=1`, and HEVC levels
`186`. Verify the installed plugin hash against its paired allowlist, then run
fresh public-API sessions **without** a local factory, injected capability
dictionary, debugger code patch or `-load` bundle override. Check GPU recovery,
VCN resource failures and new crash reports as well as frame correctness.
Include maximum VP9 at both bit depths, actual AVC slice NAL counts, a rejected
runtime count change followed by successful frames, and 4096x4096 AVC encoding
with independent CPU reconstruction. The unresolved square AVC decoder must
not be used to declare that encoder test a failure or a hardware roundtrip
success.
