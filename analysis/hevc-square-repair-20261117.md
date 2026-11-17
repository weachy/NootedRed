# HEVC square encode repair: VCN2.2 bitstream descriptor

Branch `codex/gpu-page-lifecycle-20261115`, source revision 9. This supersedes
the HEVC "not resolved" conclusion in `media-revision8-20261117.md`.
The tested hardware is Renoir PCI `1002:1636`, Ryzen 7 PRO 4750U,
macOS 26.7.2 build `25G332`. The installed driver was still revision 6
during these process-local trials. The exact OS/cache/VA identities, shared
VCN2.2 policy and per-page hashes gate the production patch; this is not a
claim of hardware acceptance on every VCN2.2 APU or other macOS builds.

## Controlled result

On November 17, 2026, after the user's explicit approval of reboot-inducing
experiments, a dry capture and a successful 4096×2304 control were recorded.
One 4096×4096 HEVC Main frame with the repaired encoder initialization,
native 128 MiB circular bitstream descriptor, and zero filler was submitted
at 09:47:50. The VCN0EncLLQ stamp `167` did not retire. The first GPU reset
report was at 09:47:57; repeated ASIC resets and WindowServer failures
preceded a userspace-watchdog panic and the 09:51:01 new boot. The panic
report was saved at 09:51:31 after reboot; that is its file timestamp, not
the moment of the panic. The first failure was an encoder fence timeout.
No GPU virtual-address fault was observed before this reset in the recovered
logs. This rules out the uninitialized filler word as a sufficient explanation.

The login LaunchAgent's read-only collector saved 17 GPU restart reports,
WindowServer diagnostics and the panic. It did not replay any GPU work.
The initial runner's `ioreg` sampler became uninterruptible during the GPU
stall, so its live stream stopped early; recovery used persistent unified
logs and diagnostic reports. Its subprocess timeout was subsequently changed
to keep the observer alive if a killed `ioreg` cannot be reaped.

An isolated second trial left the actual 128 MiB backing allocation,
HEVC mode 1 (circular), slice/segment CTB count and source unchanged, but
serialized 64 MiB in packet `0x12`. The square Main frame returned in
approximately 0.21 seconds. The callback and final Annex B files were
byte-identical, decoded independently on the CPU with source PSNR
`58.562/56.798/56.045 dB` for Y/U/V. This one-field comparison provides
direct causal evidence for the descriptor-size boundary on this command
path. It does not by itself identify the exact private firmware register
that interprets the 128 MiB value.

The production-equivalent process-local test replaced precisely the
HEVC-only builder's call to `addBitstreamBufferPacket` with a reviewed
helper. It changes byte `+0x43b` of the command's output-length field from
`0x08` to `0x04` only when it equals `0x08`, calls the original packet writer,
then restores the original byte. The packet receives 64 MiB and the command
retains 128 MiB. The helper runs in five verified, executable zero alignment
gaps in the VA image's `__text`; no unrelated function body is replaced.
The initial plan to put it in the large gap after `__TEXT` was rejected by
runtime page-protection checks because that region was not executable.

Production-equivalent outcomes:

| Trial | Packet/backing | Hardware and independent CPU result |
| --- | --- | --- |
| 4096×4096 HEVC Main | 64/128 MiB | One frame succeeds; CPU Main, source PSNR `58.562/56.798/56.045 dB` |
| 4096×4096 HEVC Main10 | 64/128 MiB | One frame succeeds; CPU Main 10, source PSNR `54.677/54.330/54.354 dB` |
| 4096×2304 HEVC Main | 72/72 MiB | One frame succeeds; original packet length retained |

Main and Main10 encoded output hashes are saved alongside the callbacks.
An additional **three-frame** run of each row passed with per-frame packet
captures and independent CPU reconstruction. The 4096×4096 Main encoded
frames had Y/U/V PSNR minima `58.562/56.484/54.600 dB`; Main10 minima
`54.534/53.353/53.422 dB`. Every square packet carried a 64 MiB descriptor
while the native command object retained 128 MiB. Three 4096×2304 control
packets remained 72 MiB. The first two diagnostic attempts to extend the
single-frame harness stopped in its own checks before submitting all frames:
VideoToolbox queues frames before building their commands, and later frames
omit the session geometry packet. Those harness assumptions were corrected
and no GPU recovery was observed. Sustained 30-frame, public-factory,
concurrent-application and boot-time acceptance remain separate checks.
A successful callback alone was not counted as a correct picture.

## Production scope

`tools/hevc_bitstream_patch.py` derives six replacement windows: the call
site and five helper fragments. `tools/media_patch_manifest.py` also changes
the separate HEVC height field to 4096. Revision 9 has **18 pages and 34
sites**, including the revision-8 initialization fix. The generator compares
the exact original or patched bytes at each site, pins the two driver image
UUIDs/cache/kernel/build, hashes all other bytes of every page, and checks
that each helper stays within the executable VA `__text`. The runtime only
enables this manifest for the existing VCN2.2 profile/bridge policy and no
external video device. This covers `1636/1638/164C/15E7` as policy; only
`1636` was tested on hardware here. Other VCN generations are excluded.

The HEVC builder call is separate from the AVC builder. On HEVC frames with
other output lengths, the helper subtracts and restores zero, so the
serialized packet is unchanged. The native backing allocation is unchanged.
The 64 MiB firmware ring is much larger than these synthetic encoded frames,
but maximum-size real inputs and long-running sessions still need acceptance
testing. No claim of 4096×4096 silicon impossibility is made.

The AVC 4096×4096 decoder issue is independent. The bundled codec-7 firmware
multiplies 256×256 macroblocks, truncates 65,536 to zero in a 16-bit field,
and rejects every unsigned slice start. A newer pinned Renoir firmware also
contains the same operation. This is a proved obstruction for the current
firmware path; fixing it requires coherent firmware changes across all
consumers or a different decoding implementation. Raising a host-side
advertisement or bypassing one branch cannot make that path decode correctly.
See `avc-firmware-limit-20261117.md` for bytes and executable checks.

## Evidence and verification

Durable trial directory:
`~/Library/Application Support/NootedRedDiagnostics/media-20261117/`.
The IDs are `20261117-094749-square-r8-native`,
`20261117-100048-square-cap64`,
`20261117-101755-square-production-equivalent`,
`20261117-101839-square-main10-production`, and
`20261117-102150-control-production-equivalent`. Each includes the
instrumented packet and command, before-submit records, health/logs and
independent CPU validation when a callback succeeded. The recovery
directory under the native failing trial has the reset and panic reports.

`python3 tools/media_patch_manifest.py` passed the exact 18-page/34-site
preflight. The pinned xmake 3.1.2 release build exited 0 and generated a
revision-9 candidate under the branch-specific `build/branches` directory.
`python3 tools/test_hevc_bitstream_patch.py` executed the exact helper bytes
against five sizes and verified the packet writer's observed size, restored
native object and adjacent canaries without GPU work.
All seventeen host test files passed on the final source tree; their results
are in `r9-tests-final.json` in the durable experiment directory. The
process-local experiments did not alter the installed EFI or VP9 plugin.
After those experiments, the live EFI was mounted as `/dev/disk0s1` with
administrator privileges. Its complete partition image was checksum-verified,
and its files and old VP9 plugin were independently backed up. The revision-9
kext (SHA-256 `0211a59d8b2911b39066807de3df726dd6419fb47f15f49bf031abb824b6a3a1`)
was installed on the live EFI and the paired VP9 plugin
(cdhash `4d5dd34757ad8e5bb175fa3f777abe4b14923794`) was installed in
`/Library/Video/Plug-Ins`. Both installed files were checked against their
build products; the EFI FAT verification and VP9 code-signature verification
passed.

## Booted revision 9

The November 17 reboot began at 10:58:14 local time. IORegistry reported
`NRedUserPatchRevision=9`, the expected 18 pages/34 sites, complete observed
page coverage (`262143`), `NRedVCNMediaFailure` absent,
`NRedVCNResourceFailures=0`, and the paired VP9 plugin allowed/registered.
The strict public VideoToolbox matrix in
`r9-public-square-30frames/results.json` ran 14 cases of 30 frames each,
including 4096×4096 HEVC Main and Main10. All 420 frames were hardware
encoded/decoded where applicable and matched independent CPU decoding at
the effective pixel level. The 30-frame square Main source quality minimum
was 45.539 dB across sampled Y/U/V planes; Main10 was 38.998 dB.
The first three-frame matrix also passed all 14 cases.

Separately, public VideoToolbox encoded 30 H.264 High 4096×4096 frames.
Independent CPU decoding reconstructed all 30 with sampled source PSNR
at least 37.455 dB. This is an **encode-only** result: the AVC square
hardware decode path remains blocked as proved above. The optional VP9
plugin registered as a hardware decoder; a 1080p 8-bit sample and a
separate 10-bit FATE sample,
then 8192×4352 8-bit and 10-bit samples (30 frames each at the maximum
tested dimensions) matched their CPU reference hashes after reboot.
No new GPU reset, WindowServer watchdog or panic diagnostic was found after
the 10:58:14 boot; the boot time remained unchanged and the VCN engine
remained ready. These bounded tests do not establish continuous recording,
concurrent workloads, other CPUs or later macOS cache builds.

For external context, AMD's AMF documentation requires querying
device-specific capability limits, and AMD has documented unrelated
HEVC-encoder timeouts on other GPU generations. Neither establishes a
Renoir square-frame limit. The experimental packet comparison above is the
source of this repair.
