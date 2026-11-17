# Media portability and GPU restart investigation — 2026-11-16

**November 17 follow-up:** [Revision 8](media-revision8-20261117.md) adds a
verified encoder initialization repair, a 14-page/27-site manifest,
witnessed process-local hardware acceptance and a complete successful build.
The revision-7 state and artifacts below are historical. Neither square
resolution issue is represented as repaired.

Work follows fork base `548631e`, with revision-6 HEAD `4559035`. The new source
reports `NRedUserPatchRevision=7`. Its binary patch manifest is still the same
13 pages / 25 sites; no HEVC encode-height or AVC decode-pixel-budget override
has been added. Revision 7 is a candidate, not installed or boot-tested.
The user's pre-existing `LICENSE.md` deletion is outside these changes.

**The two requested square-resolution fixes are not complete.** This work
removes the single-machine media gates, tests the shared policy, prevents
replaying the known hanging diagnostics, and records the crash evidence.
Keeping the native restrictions is containment, not proof of a repaired
4096×4096 hardware path.

## The reboot at 22:23

All times below are on November 16, 2026, UTC+08:00. The triggering process
was our diagnostic experiment, not an ordinary application:

| Time | Evidence |
| --- | --- |
| 22:19:53.454 | `local-inspect`, PID 5551, builds/submits one HEVC 4096×4096 frame with a process-only height override and 2048 CTBs per slice/segment |
| 22:19:54.585 | Fence wait starts stalling |
| 22:19:58.585 | Waiting for stamp 687 times out; completed stamp remains 686 |
| 22:19:59.331 | Channel 14, `VCN0EncLLQ`, reports event timeout |
| 22:19:59.932–22:20:00.332 | Channel restart; `FenceNotRetired=1`, full ASIC reset requested |
| 22:20:00 onward | MM read-protection fault at GPU VA `0x0000000400010000`, VMID 4, client 29; the same pending stamp is replayed through repeated resets |
| 22:20:55 / 22:21:34 | WindowServer watchdog terminations |
| Before next boot | Panic: no successful WindowServer check-ins for 120 seconds, two induced crashes |
| 22:23:15 | New boot loads the previously installed revision 6 |

Both GPU restart reports explicitly identify PID 5551 / `local-inspect` as
the first pending video command. GFX, SDMA and VCN decode were completed/idle
in the initial report. The first reset/fault occurs before the test process's
20-second timeout. Killing userspace cannot retract a command already sent
to VCN and did not contain this hang.

The evidence establishes the trigger and the reset/watchdog sequence. It does
**not** identify which firmware access caused the bad read. The saved builder
packets precede kernel resource-address relocation, so their zero address
placeholders cannot be equated with the faulting GPU VA. Apple's dump calls
client 29 unknown. Linux v6.18 `gmc_v9_0.c` names Renoir MMHUB client 29
`VCNU`; this identifies the client, not the failing buffer or the first
invalid access. The reported fault follows a reset.

The experiment was process-local. Neither the installed revision 6 nor this
revision-7 source contains its HEVC height override. This incident does not
establish that normal workloads are indefinitely stable, nor that the GPU
recovery path has been repaired.

Local diagnostic containment:

* Removed the `NRED_LOCAL_SUBMIT` return path from `local_inspect.h`, rebuilt
  and signed `local-inspect`, and checked that the old submission environment
  exits before session setup. Its HEVC builder hook unconditionally exits
  before returning to the submitting caller; other codec/decode modes are refused.
* Retired `media-gap-fixes/run_tall_trials.py` and removed the HEVC height
  patch from the older AVC debugger script.
* The repository's public `vda_codec_probe.m` now refuses HEVC encoding above
  2304 pixels high before creating a session. New `encode-only` mode saves the
  encoded stream for independent CPU reconstruction without requiring that
  the same hardware can decode it.
* The actual VA capability instruction test covers the dangerous square and
  adjacent dimensions, portrait and pixel-budget boundaries, and 32-bit
  overflow inputs with mock objects. It submits no GPU work.

Raw evidence is outside Git:

```text
installer-staging/verification/gpu-status-2026-11-16/media-portability-fixes/
  reboot-222315/
    Kernel-2026-11-16-222342.panic
    Kernel_2026-11-16-222030_<test-host>.gpuRestart
    Kernel_2026-11-16-222151_<test-host>.gpuRestart
    WindowServer-2026-11-16-222055.ips
    WindowServer-2026-11-16-222134.ips
    kernel-video.log
```

## What remains unresolved

**November 17 update:** H.264's progressive square failure is now traced to
an actual firmware 16-bit count overflow and unconditional slice rejection.
The native 32-bit dimensions are correct; the overflow occurs later inside
the codec-7 firmware parser. The exact call path, instructions, progressive
flag and executable byte-derived regression are recorded in
[avc-firmware-limit-20261117.md](avc-firmware-limit-20261117.md).
Newer pinned DEC-8 firmware has the same rejecting block. This identifies
the cause, but no safe firmware repair or square hardware decode is delivered.
Feedback word +20 is named `value` in AMD's header; its precise semantics
remain unestablished and no patch based only on its correlation is shipped.

HEVC square encoding still hangs. Reducing the slice/segment size from 4096
to 2048 CTBs did not solve it and triggered the incident above. The saved
4096×2304 and 4096×4096 command builders both produce a 1788-byte task
(1812 bytes including the session packet). Dimensions, five-reference plane
offsets and source pitches grow consistently. The DPB grows from `0x4380000`
to `0x7800000`; the bitstream buffer from `0x4800000` to `0x8000000`; the
session buffer remains `0x20000`. These observations do not rule out a
firmware-private buffer requirement or an address-translation error.

Linux v6.18 `soc15_query_video_codecs` reports 4096×4096 AVC/HEVC encoding for
VCN 2.2 through `vega_video_codecs_encode`, and 4096×4096 AVC decoding through
`rn_video_codecs_decode`. Mesa's VCN defaults use 4096×2304 for encoding and
can be overridden by kernel-reported capabilities. Thus the Linux capability
table is not evidence that Apple's firmware/message path handles this corner.
It is also not sound to declare the silicon incapable solely from this test.

Further root-cause work needs the relocated GPU buffer map and faulting access
for the failing command, or a known-good same-device command/firmware capture
from another driver. The current captures lack that link. Do not retry tall
HEVC on the active desktop to guess at packet fields.

## Shared hardware policy

`NootedRed/MediaHardware.h` is compiled by both the kernel and the C VP9 plugin.
Classification uses GPU PCI IDs, not CPU model strings or the VA spoofed chip
family. The existing `DeviceProfile` table has compile-time assertions that
its media generation/enablement agrees with this policy.

| GPU PCI IDs | Media backend | Revision-7 treatment |
| --- | --- | --- |
| `1636`, `1638`, `164C`, `15E7` | VCN 2.2 bridge | Shared Main10 encode, bounded HEVC 8K decode and optional VP9 gates |
| `15DD`, `15D8` | VCN 1.x | Existing graphics path; no VCN2 media enablement |
| `73BF`, `73AF` | Original standalone Navi VP9 targets | Preserve tier 0, write combining and prior area guard; never use the APU bridge |
| Other IDs | No matching backend | Reject media enablement |

All four VCN 2.2 IDs use VP9 tier-1 reference descriptors and write combining
off. Existing kernel/cache/image identities, reviewed page hashes, external
GPU exclusion, failure latch, boot opt-outs, and exact plugin cdhash checks
remain required. The VP9 AMFI installation uses the same effective VDA gate.
No global signature-policy bypass is added.

Hardware correctness testing in this fork is on `1002:1636` / Ryzen 7 PRO
4750U / macOS 26.7.2 (`25G332`). The other VCN 2.2 entries are **experimental
same-IP compatibility**, not hardware-validated machines. CPU families with
different video IP need a separate backend. VP9 encoding is not implemented.

The fork's earlier GL/allocator fixes were reviewed alongside the media work:
the GL budget derives from actual VRAM, not a fixed 512 MiB machine assumption;
the allocator retains alignment/minimum-chunk/non-visible constraints and
uses the correctly typed overload. Their CPU regression tests still pass.
Graphics signature selection remains separate from exact media eligibility.

## Source references

Primary sources were inspected and saved with SHA-256 hashes in
`linux-sources.json` and `mesa-sources.json` in the evidence directory.

```text
Linux v6.18:
https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/amd/amdgpu/amdgpu_drv.c
https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/amd/amdgpu/amdgpu_ucode.c
https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/amd/amdgpu/soc15.c
https://raw.githubusercontent.com/torvalds/linux/v6.18/drivers/gpu/drm/amd/amdgpu/gmc_v9_0.c

Mesa commit 42c04e9730ace9e5e97af6e0d97147764b71b42f:
src/amd/common/ac_video.c
src/amd/common/ac_vcn_dec.c
src/gallium/drivers/radeonsi/mm/radeon_vcn_enc_1_2.c
src/gallium/drivers/radeonsi/mm/radeon_vcn_enc_2_0.c
```

`amdgpu_drv.c` groups all four bridge PCI IDs as `CHIP_RENOIR`;
`amdgpu_ucode.c` maps VCN 2.2 to Renoir/Green Sardine firmware names.
Mesa enables VP9 tier 1 on VCN through 2.6 and HEVC Main10 encoding on VCN 2+.
These are hardware/backend references, not macOS validation results.

## Verification completed after the reboot

The running machine still uses revision 6; its kext UUID is
`5AE4002E-7E9E-3E58-8AB0-151B86ABB83D`. All public AVC/HEVC acceptance below
uses normal VideoToolbox factories without local driver instruction patches.
Candidate VP9 acceptance explicitly loads the new bundle into the diagnostic
process; it does not validate revision 7's system-wide loading before reboot.

| Check | Result |
| --- | --- |
| C and C++ shared hardware policy | 327,686 assertions in each language, all 65,536 PCI IDs plus malformed wider values |
| Effective media policy / failure latch | 173 checks, including all four VCN2.2 devices, unsupported generations, missing backend, dGPU and boot opt-outs |
| VP9 actual message builder | 130 checks covering six device IDs, both bit depths, both DPB tiers, buffer bounds and frame gates; ASan/UBSan |
| Actual AVC/HEVC dimension instructions | 496 checks; unsupported corners and overflow inputs rejected without GPU submission |
| Actual AVC slice setter / native row limits | 71 checks |
| Actual HEVC format / chroma instructions | 56 checks |
| Capability dictionary ownership/failure paths | 216 checks |
| Page patch lifecycle with stored host GL and all 13 media pages | 920 checks; ASan/UBSan |
| Video resource failure handling | 1,339 checks per X5000/X6000; no kernel hooks installed |
| UMA allocator / GL budget | 180 / 35 CPU checks |
| GL table integrity / actual selector | 16 signatures in four tables / nine version selections |
| Decoder signing policy / AMFI adapter | Six signed-code cases / 69 adapter checks |
| Reference-comparison validation | Three tests, including wrong/missing frames and true ten-bit patterns |
| Exact media manifest | Kernel/cache/image identities, 13 page fingerprints, 25 sites match |

Two old GL tests had become stale: one still required deleted
`TahoePatchData.hpp`, and one mirrored an obsolete selection table rather than
executing `selectPatchSet`. They now inspect/execute the current production
tables. The GL budget fixture also accepts the exact already-applied budget
site by restoring only those 50 known bytes in its temporary CPU fixture,
then checking the same full-page fingerprints. Unknown changes still fail.

| Hardware acceptance | Frames | Result |
| --- | ---: | --- |
| Public AVC/HEVC Main/Main10, 1080p, 1918-wide pitch, 4K and B frames | 12 cases × 3 | Hardware selection confirmed; all hashes match independent CPU decoding |
| Public AVC 4096×4096 Baseline/Main/High, four slices | 3 each | Hardware encode; independent CPU reconstruction passes; 12 slice NALs per three-frame stream |
| Public HEVC Main/Main10 4096×2304 | 3 each | Hardware encode; independent CPU reconstruction passes |
| Public HEVC Main 8192×4352 | 30 | All hardware/CPU pixel hashes match |
| Public HEVC Main10 8192×4352 | 3 | All hardware/CPU pixel hashes match |
| Candidate VP9 profile 0, 1080p | 30 | All reference hashes match |
| Candidate VP9 profiles 0 and 2, 8192×4352 | 30 each | All reference hashes match |
| Candidate VP9, one session 640×360 → 1080p → 4K → 640×360 | 120 | All reference hashes match |

Minimum sampled source-reconstruction PSNR: 37.365 dB for square AVC,
54.270 dB for HEVC Main and 53.260 dB for HEVC Main10. The first attempted
Baseline acceptance correctly failed its profile assertion because the old
public probe ignored the supplied settings and encoded High. Adding the
settings parser enabled the final explicit-profile tests; that first run is
not counted as a Baseline pass.

After the completed tests: observed media pages `8191`, complete `1`,
expected pages/sites `13/25`, VCN resource failures `0`, GPU recovery count
`0`. No new Kernel/WindowServer crash report appeared during the acceptance
window. This is bounded correctness evidence, not a sleep/wake, long-running,
thermal or multi-application stability certification.

Detailed outputs:

```text
media-portability-fixes/host-tests/
media-portability-fixes/revision6-public-regression/results.json
media-portability-fixes/supported-results.json
media-portability-fixes/after-public-health.json
media-portability-fixes/diagnostic-files-after-boot.json
media-portability-fixes/postboot-gpu-recovery.log
```

## Candidate artifacts

Revision 7's kext and signed VP9 bundle were rebuilt as a pair. The kext
contains the new plugin's exact cdhash, verified against `codesign`; the
bundle signature and kext plist validation pass.

| Artifact | Identity |
| --- | --- |
| Kext UUID | `8A94E467-BA19-3669-9F5C-EB8CEABACF5A` |
| Kext binary SHA-256 | `84d97392f202fd079dcfa7837b59848fd0fd7fb5f461453f4c87c93eade36e65` |
| VP9 binary SHA-256 | `4b1de4274e41a8445aa689cf0f5262c85d29898e37be26eb573a4c931c4520ed` |
| VP9 cdhash | `4d5dd34757ad8e5bb175fa3f777abe4b14923794` |

Compilation, linking and kext generation completed. The existing AppleForge
archive step still reports `cannot runv(/usr/bin/zip ...), No such file or
directory`; an attempted absolute target directory did not fix it and was
reverted. The paired review archive is packaged separately and checked against
its source files. This archive-step limitation is not a compiler pass for the
whole `xmake` command.

No EFI files or installed video plugins were changed in this continuation.
No new reboot was requested, and revision 7 is not loaded. A future deployment
must install the kext and plugin together and verify fresh system-factory
sessions and the new UUID/revision after reboot.
