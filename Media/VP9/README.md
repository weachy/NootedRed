# Experimental VP9 decoding on VCN 2.2

This directory contains an optional VideoToolbox VP9 decoder using the shared
PCI/media-IP policy in `NootedRed/MediaHardware.h`. The NootedRed bridge covers
`1636`, `1638`, `164C` and `15E7` (VCN 2.2); hardware testing in this fork is on
the Ryzen 7 PRO 4750U / `1002:1636`. Other same-IP devices remain experimental.
It is paired with NootedRed's `-NRedVP9` option. VCN 1.x and unknown GPUs remain
disabled; no AV1 decoder or VP9 encoder is added.

The decoder is adapted from `wh0ami7/vcn-video-decoders` at
`c7ed7ea687971759321559b6cdff8ba0319fea15` (November 14, 2026). The upstream
decoder and its AMFI adapter are MIT licensed; see [LICENSE](LICENSE).
`vcnvp9/ac_vcn_vp9_default.h` also retains AMD's original MIT notice.

## Hardware changes

The CPU parses VP9 headers and prepares VCN messages. Actual image decoding runs
on the VCN engine. The Apple driver's CPU-side AV1 processor object is a carrier;
the submitted session and codec messages are VP9, not AV1.

VCN 2.2 requires `mif_wrc_en = 0`; the original Navi 21 implementation used 1.
Mesa's `ac_vcn_dec.c` sets that field only for VCN 3 and newer. The VCN 2.2 path
also requires the validated AMDRadeonVADriver2 UUID
`1181E2CA-31B3-3DF0-90E2-3AE70552D753`. A different driver is rejected.

Profiles 0 and 2, 4:2:0, 8/10-bit, are accepted up to 8192 pixels wide,
4352 pixels high, and **35,651,584 pixels per frame**, including 8192x4352.
VCN 2.2 uses a tier-1 dynamic-DPB descriptor to give the firmware explicit
reference-plane pitches, heights and sizes. Increasing the allocation alone
left inter-frame chroma wrong in the implicit tier-0 path. The fixed nine-picture
store uses 459 MiB for eight-bit references or 688.5 MiB for ten-bit references,
and does not reallocate when coded resolution changes.
See the [capability audit](../../analysis/media-capability-20261116.md).
This DPB is separate from the GL resource-list budget correction. Navi retains
the preceding tier-0 pixel guard. The policy and actual message builder are
tested for all four bridge IDs and both original Navi IDs (`73BF`, `73AF`);
host tests do not replace hardware tests on those machines.
See the [portability and reboot investigation](../../analysis/media-portability-20261116.md).

The parser also excludes a preceding keyframe from motion-vector reuse, as
libvpx does. A keyframe's `intra_only` syntax bit alone cannot express this.

## Build

From the repository root:

```sh
zsh Media/VP9/build.sh
python3 Media/VP9/test_policy.py
python3 Media/VP9/test_library_validation.py
python3 Media/VP9/test_message.py
python3 tools/test_media_hardware.py
python3 tools/test_media_policy.py
python3 tools/test_userpatch_lifecycle.py
tools/build-branch.sh -y
```

The plugin and test harness are under `build/media-vp9/`. The build generates
`NootedRed/VP9PluginAllowlist.inc` for that exact signed plugin. Rebuild and install
the kext and plugin together whenever the plugin changes.

## System loading

The plugin belongs at `/Library/Video/Plug-Ins/VP9VCNDecoder.bundle`. Add
`-NRedVP9` to OpenCore's boot arguments and use the matching NootedRed build.
This option defaults off and targets the four VCN 2.2 bridge IDs on the exact
reviewed Tahoe build. Kernel, cache, driver UUID, page and paired-plugin hash
checks still apply; a shared hardware generation does not bypass them.

Two checks allow just the paired plugin:

* CoreMedia's existing 200-byte Apple signing policy is replaced with the
  equivalent factored Apple policy **or the plugin's exact cdhash**. The
  original Apple clauses remain. Unrelated adhoc plugins still fail.
* AMFI's library-validation failure adapter checks the same 20-byte cdhash and
  otherwise forwards all seven arguments to the original function.

The CoreMedia data patch requires the exact cache filename, file page
`0x0F923000`, in-page offset `0x110`, and all original policy bytes from Tahoe
26.7.2 (25G332), CoreMedia UUID `5B257039-4FC4-37F2-8E81-B310002CC57A`.
No CoreMedia branch is bypassed. No system-volume files are changed, and
authenticated root does not need to be disabled. These checks do not make an
untested future macOS build supported.

After reboot, check the following IORegistry properties:

```text
NRedVP9RegistryEnabled = 1
NRedVP9LibraryValidation = 1
NRedVP9RegistryApplied >= 1  (after a fresh media process starts)
```

Then use a fresh `build/media-vp9/vtvp9test list` process, with no `-load`
argument. Applications must request supplemental VP9 decoders through
`VTRegisterSupplementalVideoDecoderIfAvailable`.

To disable the integration, remove `-NRedVP9` and reboot. To remove it completely,
also restore the preceding NootedRed build and remove the plugin bundle.

## Validation on this machine

On November 16, 2026, process-local hardware tests on Renoir / 25G332 passed:

| Sample | Frames | Comparison |
| --- | ---: | --- |
| 640x360, profile 0 | 30 | CPU-decoded frame hashes, all equal |
| 1920x1080, profile 0, two loops | 60 | CPU-decoded frame hashes, all equal |
| 3840x2160, profile 0 | 30 | CPU-decoded frame hashes, all equal |
| FFmpeg FATE `vp92-2-20-10bit-yuv420.webm`, profile 2 | 10 | FATE frame hashes, all equal |
| 3840x2160, profile 2, real ten-bit source | 3 | CPU-decoded frame hashes, all equal |
| 7680x4320, profiles 0 and 2 | 30 each | CPU-decoded frame hashes, all equal |
| 8192x4320 and 7680x4352, profile 0 | 3 each | CPU-decoded frame hashes, all equal |
| 8192x4352, profile 0, two loops | 60 | CPU-decoded frame hashes, all equal |
| 8192x4352, profile 2, real ten-bit source | 30 | CPU-decoded frame hashes, all equal |
| One session: 640x360 → 1920x1080 → 3840x2160 → 640x360 | 120 | CPU-decoded frame hashes, all equal |

The final production bundle passed these 11 regression cases (379 frames,
excluding the earlier separate 640x360 test), with no mismatches or errors.
The host message test checks both DPB tiers, both bit depths, allocation bounds,
and rejection of invalid dimensions before submission.

The VCN decode submission/completion counters increased, with no GPU recovery.
Paced 1080p60 and 4K60 tests decoded 300 frames each without errors, but 4K60
showed substantial queue latency. The 60 fps feed rate is not proof of smooth
browser presentation or sustained throughput. Browser validation is required
after the matching kext is loaded.

The system-loading policy was tested in one process: original Apple code still
passes; the paired plugin passes; unrelated adhoc code and an altered plugin
fail. The independent decoder service needs the policy too, so a process-local
patch alone does not enable system-wide decoding.
