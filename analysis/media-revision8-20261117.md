# Media revision 8: initialization repair and remaining limits

This report supersedes revision 7 as the current **source/build candidate**.
Branch: `codex/gpu-page-lifecycle-20261115`; HEAD: `4559035`; fork base:
`548631e`. Changes remain uncommitted and uninstalled. The machine still runs
revision 6, booted November 16, 2026 at 22:23:15 UTC+08:00.

## Conclusions

| Requested issue | Evidence and disposition |
| --- | --- |
| AVC 4096×4096 blank decode | Cause proved: codec-7 firmware wraps 65,536 macroblocks to zero in a u16 count and rejects every slice. No safe firmware repair delivered. This is a firmware implementation limit, not proof of a silicon limit. |
| HEVC 4096×4096 encode hang | Not resolved. The initialization defect below is repaired, but there is no evidence that it repairs the square hang. A 128 MiB bitstream-buffer boundary remains a lead, not a proved cause. |
| Freeze/reboot | The November 16 tall-HEVC diagnostic triggered VCN encode timeout, repeated ASIC resets, WindowServer watchdog failures and reboot. Unsafe diagnostic submission is retired; GPU recovery itself is not repaired. |
| Portability | Shared VCN2.2 policy covers GPU IDs `1636/1638/164C/15E7`; hardware validation is on `1636` only. |
| Encoder initialization | Repaired, with native CPU tests and hardware encoding that explicitly witnesses constructor execution. |
| Build packaging | Complete build exits 0 after correcting the shared AppleForge archive invocation. |

**The requested “both square paths repaired, or proved impossible” closure
has not been achieved.** A restriction, a speculative bitstream clamp, or
tests at smaller dimensions cannot establish that closure.

## Proven initialization defect and repair

Reviewed `AMDRadeonVADriver2` UUID:
`1181E2CA-31B3-3DF0-90E2-3AE70552D753`.
`Vcn2EncCommand::Vcn2EncCommand()` (`C2`, `0x7ffb10b4a6d4`, 94 bytes) clears
`[+0x38,+0x80)` and `[+0xdc,+0x5c4)`, leaving four records uninitialized:

| Offset | Record |
| --- | --- |
| `+0x80` | Rate-control session |
| `+0x88` | Rate-control layer |
| `+0xa8` | 32-byte per-picture rate control |
| `+0xc8` | Quality parameters |

The base constructors initialize fields below `+0x38`. Both codec-specific
constructors call this `C2` and initialize their later fields. HEVC
`SetConfigParam(16)` reads packet 8 and changes min/max QP, maximum AU size,
skip and HRD fields, preserving word 4, filler-data enable. The per-picture
update also preserves this word. Dirty storage can therefore reach a real
firmware packet.

The saved 4096×2304 dry packet has filler word `44913776`; the submitted
square / 2048-CTB trial has `1110879973`. The separate square inspection with
word 0 was a **dry capture** (`NO_FRAME_SUBMITTED`), not a zero-filler frame
that hung. This establishes bad initialization, not causation of the hang.

The repair moves the existing `bzero` start to `+0x80` and increases length
from `0x4e8` to `0x544`, retaining its end at `+0x5c4`. Explicit setters
remain effective:

```text
C2 +0x18: 488dbbdc000000 -> 488dbb80000000
C2 +0x35: bee8040000     -> be44050000
```

Added manifest page: `dyld_shared_cache_x86_64h.03`, file offset `0x10b4a000`,
sites `0x6ec` and `0x709`, masked SHA-256:

```text
586186650960fe43ce5e3ce22ab822d828306f682a95dea7618ab5b55d01464e
```

Revision 8 requires **14 pages / 27 sites**. All prior identities, page
fingerprints and 25 sites are retained. The same reviewed binary and
effective VCN2.2 policy gate the repair, not a CPU model name.

`python3 tools/test_encoder_initialization.py` executes the actual native
constructor, packet getter/setter and HEVC configuration code on mock
objects. It reproduces the old leak with `A5/5A/FF/00` storage and passes
**128 checks** for clearing bounds, adjacent canaries, native parameter
updates, and explicit filler 0/1 settings. No GPU work is performed.

## Hardware acceptance and corrected test attribution

The first `r8-probe` patched its own process and used the default public
encoder factory. Its 24 successful frames did not witness execution of the
changed constructor; a later process-local hook did not fire through that
factory. Those results are retained as **public baseline regression**, not
proof that revision 8's constructor ran.

The corrected `r8-local-probe` explicitly registers Apple's encoder in the
test process, selects the GPU, applies the UUID/page-verified patch, and
instruments the constructor. Every passing case must log:

```text
R8_CTOR_EXECUTED count=1 rate_records_zero=1
R8_LOCAL_RESULT status=0 constructor_calls=1 clean=1
```

All eight cases passed, three frames each. HEVC height above 2304 is refused
before `dlopen` or session creation; both Main/Main10 square refusals were
checked. No height override is present.

| Hardware encode case | Verification |
| --- | --- |
| AVC High 1918×1080, four slices | Constructor witnessed; independent source reconstruction; all hardware/CPU decode hashes equal |
| HEVC Main 1918×1080 | Same, without AVC slice settings |
| HEVC Main10 1918×1080 | Same, with a true ten-bit source pattern |
| HEVC Main/Main10 4096×2304 | Same for each bit depth |
| AVC Baseline/Main/High 4096×4096, four slices | Constructor witnessed; independent CPU reconstruction; profile and 12 slice NALs verified per three-frame stream |

Square AVC uses encode-only validation, not square hardware decoding.
These process-local tests do not validate revision 8's boot-time installation
or other APUs.

After acceptance: running revision 6, media pages complete, VCN resource
failures 0, GPU recovery count 0. No matching GPU-reset log or new
Kernel/WindowServer crash report was found in the November 17
08:10-to-final-health-check window. This is bounded correctness evidence,
not sleep/wake, long-running or concurrent-application stability certification.
A separate read-only registry check at 09:12:34 UTC+08:00 on November 17
again observed revision 6, complete media pages, resource failures 0 and
recovery count 0. That check submitted no GPU work and did not repeat the
earlier crash-log search.

Other completed CPU checks: exact 14-page/27-site manifest; 910 media-page
lifecycle checks; 173 media policy checks; 496 native codec boundary/profile
checks; 56 HEVC format/chroma checks; and 65,572 AVC count checks each for the
bundled and pinned Linux firmware. A further 258 byte-derived create-dispatch
checks establish that legacy codec ID 0 selects no AVC handler in the bundled
firmware's create path. The broader policy/VP9/graphics results
remain in [the portability report](media-portability-20261116.md).

## HEVC: established facts and missing evidence

The saved task is 1788 bytes, or 1812 with its session packet. The described
source/reference planes fit their allocations. DPB size grows from
`0x4380000` to `0x7800000` between 4096×2304 and 4096×4096; session size
remains `0x20000`. This does not rule out a firmware-private requirement.

Firmware packet 3 copies dimensions to `S+0x88..`; packet 8 copies rate
parameters; packet `0x11` copies the 600-byte context description; packet
`0x12` copies the bitstream descriptor to `S+0x5f8`, with size at `S+0x604`.
Geometry setup calculates CTB counts in 32-bit registers. No proof of a
“64 overflows six bits” failure was found.

The bitstream-size lead is specific:

* HEVC 4096×2304 uses `0x4800000` (72 MiB); the saved square command uses
  `0x8000000` (128 MiB).
* Firmware branch `0x60059e2d` reads `S+0x7f8`. One path writes the size to
  indirect index 6. The other, `0x60059feb..0x60059ff3`, writes
  `size | 0x08000000` to `0x9ff00600+0x300`.
* At `0x6005d6d6..0x6005d6e8`, a hardware read pointer is masked to 24 bits
  and compared with size divided by eight. This is **not** a demonstrated
  truncation of the size itself.
* Initialization opcode `0x01000001` calls `0x6004eb90`, clearing the first
  `0x8cc` session bytes, including `S+0x7f8`. The conservative pointer scan
  found reads but no direct store to that field. It is not a complete
  alias/control-flow proof.

A local stop probe captured `Vcn2Encoder::setBitstreamBufParams` inputs and
exited before packet construction. AVC 4096×4096 uses **25,165,824 bytes
(24 MiB)**; HEVC 4096×2304 uses **75,497,472 bytes (72 MiB)**. Successful
square AVC therefore does not exercise a 128 MiB output buffer and cannot
disprove this HEVC-specific lead. No new tall HEVC frame was submitted.

The public register headers and AMF documentation inspected do not establish
this private register's size semantics, including whether zero means a
maximum-sized ring. Pre-relocation packets also do not map the post-reset
fault address to a buffer. A coincident bit boundary alone is insufficient
to ship a clamp or declare the silicon incapable.

The saved Apple bitstream packet (`0x12`) uses mode 1, **circular**. Pinned
Mesa `radeon_vcn_enc_1_2.c`'s `radeon_enc_bitstream()` instead uses mode 0,
**linear**, and forwards the caller's output-buffer size without a 128 MiB
clamp. The values are defined in AMD's `ac_vcn_enc.h`. This is a concrete
command-path difference, not proof that changing that field alone is a fix:
the surrounding producer/consumer offsets, buffer ownership and feedback
handling also matter. Even a Linux capability report or successful frame
must be compared with those settings before it can resolve the Apple-path
failure. No speculative mode switch is included.

Closure requires a known-good same-device square-HEVC packet/firmware
capture, or the failing command's relocated buffer map and pre-reset
firmware/hardware state. Retrying the unresolved command on the active
desktop is not a safe substitute. Production HEVC height remains 2304.
The initialization repair is not presented as a square-resolution repair.

## Reboot and capability boundaries

The [portability report](media-portability-20261116.md) records the crash
chain and timestamps. GPU VA `0x400010000` faults after an ASIC reset; it
does not identify the first invalid access. Linux's Renoir client-29 label
`VCNU` identifies the unit, not the buffer.

The known hanging diagnostic return-to-submit path is removed, the tall
runner is retired, and the public probe checks height before session
creation. These changes do not repair GPU recovery after an arbitrary hang.

[The AVC firmware report](avc-firmware-limit-20261117.md) contains the actual
rejection instructions and newer-firmware comparison. Removing one branch
or widening a u16 store in place leaves other consumers wrong and can
overwrite adjacent state. Software fallback is not VCN hardware decoding.

| Codec path | Established behavior on `1002:1636` |
| --- | --- |
| AVC decode | Width/height up to 4096, within the retained 4096×2304 pixel budget; portrait and B-frame tests passed |
| AVC encode | Baseline/Main/High through 4096×4096, with native slice/profile checks |
| HEVC decode | Main/Main10 through 8192×4352 |
| HEVC encode | Main/Main10 through 4096×2304 |
| VP9 decode | Profiles 0/2 through 8192×4352, including resolution changes |
| VP9 encode | No hardware backend implemented or advertised |

The shared four-ID VCN2.2 policy has C/C++ and policy coverage;
`1638/164C/15E7` have no hardware acceptance here. Different video-IP
generations need separate backends. Apple binary/OS identities remain exact
prerequisites. The user's pre-existing `LICENSE.md` deletion is preserved.

## Build and artifacts

Pinned xmake 3.1.2 now completes with exit **0**.
`../AppleForge/xmake/rules/AppleForge.lua` passes `{ "." }` to the archive
helper instead of `"."`: this xmake version's string branch passes an
uncreated temporary stdin file to `zip`; its table branch creates that file.
The build ZIP was opened and checked.

Final review also corrected the startup log from revision 7 to revision 8;
the binary below was rebuilt afterward. The functional patch sites are
unchanged. Preflight and deployment now resolve the same `NRED_BUILD_BRANCH`
as the build, or accept an explicit `--kext`. Missing candidates fail instead
of selecting an unrelated/shared/legacy artifact. Explicit sources inside
the installation destination are rejected. Backup names include the process
ID and existing backups cannot be overwritten.
`tools/test_vda_artifact_selection.py` passed 21 checks using a temporary
repository and a mock ESP; no real EFI or installed driver was touched.

| Artifact | Identity |
| --- | --- |
| Kext UUID | `957A8439-0C65-3325-BE83-ABD4132776D5` |
| Kext binary SHA-256 | `8c42abbde598572d67ac3fda16f77a571b5d97729367a2e5780c9c61649d3597` |
| VP9 binary SHA-256 | `4b1de4274e41a8445aa689cf0f5262c85d29898e37be26eb573a4c931c4520ed` |
| VP9 cdhash | `4d5dd34757ad8e5bb175fa3f777abe4b14923794` |

VP9 signature and kext plist validation pass; the kext embeds this exact
VP9 cdhash. Paired candidate:
`build/media-release/NootedRed-media-r8-20261117.zip`.
The archive includes this report, its linked analysis reports and the
deployment guide. `r8-package.json` records the archive hash and every
member's source/hash verification.

Raw evidence remains outside Git:

```text
../installer-staging/verification/gpu-status-2026-11-17/media-root-cause/
  r8-build.log
  r8-build-final.log
  r8-build-artifacts.json
  r8-package.json
  r8-local-probe.m
  r8-local-public-probe.m
  r8-local-regression/results.json
  r8-local-regression/*.log
  r8-bounded-regression/SCOPE-CORRECTION.txt
  bitstream-stop-h264-4096x4096.log
  bitstream-stop-hevc-4096x2304.log
  r8-final-health.json
  r8-final-gpu-recovery.log
  r8-review-health.json
  avc-firmware-checks.json
  avc-firmware-checks.log
  hevc-bitstream-comparison.json
```

No EFI files, installed kexts/plugins or firmware blobs were changed.
No reboot was performed. Revision 7's package remains historical.

Additional primary references inspected:

* AMD AMF `AMF_Video_Encode_HEVC_API.md`: frame limits are hardware-specific
  and require `AMFCaps`; its example range is not proof about this firmware.
* AMD AMF “GPU and APU HW Features and Support”: coarse VCN2-class codec/4K
  capability labels do not establish a 4096×4096 implementation limit.
* Linux v6.18 `vcn_2_0_0_offset.h` and `vcn_2_0_0_sh_mask.h`: no validated
  interpretation of the private size register was obtained from these.
