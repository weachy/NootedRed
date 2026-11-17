# AVC 4096×4096: firmware count overflow

## Conclusion and scope

The bundled VCN firmware cannot decode a progressive 4096×4096 AVC picture
through the codec-7 (`H264_PERF`) path used by this driver. This is now a
byte-derived failure, rather than a hypothesis inferred from blank pictures:
256×256 macroblocks become **zero** in a 16-bit picture-count field. The slice
parser then rejects **every** possible unsigned slice start and returns error
6. Changing the SPS level or the number of slices does not change that test.

This establishes a limitation of this firmware implementation. It does not
establish an immutable silicon limit or the impossibility of another decoder
implementation. No safe firmware fix has been implemented or hardware-tested.
The production pixel budget still rejects this corner before submission.
Keeping that budget is containment; it is not successful square decoding.

## Reproducible evidence

Bundled file: `NootedRed/Firmware/ativvaxy_nv.dat`, 403,904 bytes.

```text
SHA-256 bb7833d842fd19f08d1087c96121b6fc501506762855bc6895e9fcd3ee711668
version at file +0x60: 0x06114007 (DEC 6 / ENC 1.20 / revision 7)
runtime address = file offset + 0x5fffff00
```

The offline test hash-pins this file, checks the relevant call-site bytes,
and interprets the actual standard Xtensa instructions in the count/branch
block and create-message dispatch ladder. Unknown instructions fail. It does
not emulate VCN hardware or the firmware's custom instructions.

```sh
python3 tools/test_avc_firmware_limits.py
```

The normal/adjacent sizes, all 65,536 possible square-picture slice starts,
and a field-picture distinction produce 65,572 checks. Passing this one block
for another size does not prove that an entire stream decodes correctly.
Another 258 checks execute the create-message codec dispatch described below.

Optional evidence captured previously, without a new GPU submission:

```sh
python3 tools/test_avc_firmware_limits.py \
  --captured-messages ../installer-staging/verification/gpu-status-2026-11-16/media-gap-fixes \
  --linux-firmware ../installer-staging/verification/gpu-status-2026-11-17/media-root-cause/renoir_vcn.bin
```

The square and 4080×4096 portrait messages both contain codec 7, decode message
ID 2 at `+0x58`, and AVC message ID 6 at `+0x114`. Both have SPS flags `0x85`,
including `frame_mbs_only_flag` at bit 2. They use the same pitch, output/DPB
sizes and plane boundaries. The portrait has 65,280 macroblocks; the square
has 65,536. The messages are saved evidence, not fixtures synthesized to
produce the expected result.

## Codec and input path

| Address | Operation |
| --- | --- |
| `0x60015250` | Dispatch create-message codec |
| `0x60015d2e` | Codec 7 calls `0x6001a5b4` |
| `0x6001a5b4` | Install callbacks `0x6001aa5c` at session `+0x298` and `0x6001acc8` at `+0x29c` |
| `0x60017bb6` / `0x60017bce` | Load/call the installed `+0x29c` parser |
| `0x6001acc8` | Parse common decode message 2 and AVC message 6 |
| `0x6001ab28` | Decode callback calls NAL dispatcher `0x6001c3e4` |
| table `0x60001410` | NAL types 1 and 5 both select `0x6001c4f9` |
| `0x6001c51f` | Call wrapper `0x6001c71c` |
| `0x6001c735` | Call slice parser `0x600211b8` |

The create-message comparison ladder at `0x60015b11..0x60015b43` is also
replayed directly from the bundled bytes. Its input is the message codec in
`a13`; `a14=17` is established at the checked setup sites `0x60015591` and
`0x60015bfd`. The test stops at the selected handler and has a 32-instruction
bound. It checks IDs 0–255, `0x80000003` and `UINT32_MAX`:

| Codec ID | Selected target |
| --- | --- |
| 1 | `0x60015dd1` |
| 2 | `0x60015dee` |
| 3 | `0x60015dfd` |
| 4 | `0x60015e3d` |
| 7 | `0x60015d21` — AVC handler above |
| 16 | `0x60015d34` |
| 17 | `0x60015d47` |
| `0x80000003` | `0x60015b43` |
| Other tested IDs, including 0 | `0x6001536c` — no AVC handler selected |

Thus changing the message from `H264_PERF`/7 to the legacy AVC codec value 0
does not provide an alternative AVC decoder through this create path. This
rules out that specific host-message workaround; it is not a claim about
every possible firmware entry point.

Let `C` denote the AVC context. The message parser reads the 32-bit coded
dimensions, shifts each right by four and stores:

```text
C+0x6b6  u16 width in macroblocks
C+0x6ba  u16 frame height in macroblocks
C+0x6c0  u16 frame macroblock count
```

At `0x6001b54a`, `mul16u a2,a15,a12` computes 65,536. The following
`s16i a2,a14,0x1c0` at `0x6001b54d` stores zero. The dimensions themselves
remain correct, explaining why inspecting only the native 32-bit messages
did not reveal this bug.

For these progressive inputs, firmware `0x6001b38e..0x6001b39a` shifts the AVC
SPS flags right by two, deposits the low bit at internal SPS bit 9 and stores
it. The `DEPBITS` bytes are `b0089a`. At `0x60021450`, the slice parser clears
the field-picture flag; at `0x6002145b`, the SPS bit-9 test skips field-bit
parsing when `frame_mbs_only_flag` is set. Thus `field=0` in the block below.

## Slice rejection

Inside `0x600211b8`, stack `+68` is `C+0x55c`, stack `+80` is `C+0x600`.
The parser loads the slice index from `C+0x908` and obtains width and height
from the two fields above. The index is consistent with `first_mb_in_slice`,
but that semantic name is not needed for the proof: no unsigned index is
less than zero.

```text
60021d11  l16ui a11,a13,0x15a     # width MBs
60021d17  l16ui a14,a13,0x15e     # frame height MBs
60021d20  l8ui  a8,a8,213        # field flag at C+0x6d5
60021d29  ssr   a8
60021d2c  sra   a14,a14          # picture height = frame height >> field
60021d32  ssl   a8
60021d38  s16i  a14,a13,0x160    # C+0x6bc
60021d3b  l16ui a9,a13,0x160
60021d44  mul16u a11,a11,a9      # 256 * 256 = 65536
60021d47  s16i  a11,a13,0x162    # C+0x6be = 0
60021d4a  extui a11,a11,0,16     # register also becomes 0
60021d4d  sll   a12,a11
60021d50  s16i  a12,a13,0x164    # C+0x6c0 = 0
60021d53  bltu  a10,a11,0x60021d59
60021d56  j     0x60021a42

60021a42  l32i  a8,a1,80
60021a45  movi.n a2,1
60021a47  s8i   a2,a8,235        # C+0x6eb error flag
60021a4a  movi.n a2,6
60021a4c  retw.n
```

Changing only the branch or one store would be insufficient. Other consumers
include `0x6001c602` (decoded-count comparison using u16 `C+0x6be`) and
`0x60025cf4` (reference-address calculations using u16 `C+0x6c0`).
For a field picture, this check instead sees 32,768, but the frame-count store
still wraps. The test explicitly distinguishes this from progressive input;
it does not claim that 4096×4096 interlaced decoding works.

## Newer firmware is not a demonstrated fix

Pinned linux-firmware commit `38de2359e28061cf16f599e050486003d0e557f2` supplies
identical Renoir and Green Sardine firmware:

```text
renoir_vcn.bin: 407,232 bytes, including a 256-byte Linux outer header
SHA-256 5b32a9f1ee1176babe56910fa1fa52df938b60665febbeab1ce0ee38cb4d5c98
version 0x08118012 (DEC 8 / ENC 1.24 / revision 18)
```

After removing that outer header, the matching block is
`0x60021fdb..0x60021fed`: multiplication, u16 store, explicit 16-bit extract,
frame-count store, unsigned branch and rejection. Its exact bytes are
`90aac1a25cafa0a0f400baa1b25cb0a73802c63afd`.
The optional test executes this block and passes the same 65,572 checks.
These files were neither installed nor run.

The result rules out a blind upgrade to this particular firmware as an AVC
square fix. It does not prove anything about unexamined firmware.

## Repair boundaries

A real firmware repair must audit every consumer of the two count fields,
their layout and the hardware interface. Widening the fields in place would
overwrite adjacent context data. Treating zero as a 65,536 sentinel would
also require updating all consumers. Firmware loading/authentication and
hardware behavior would need separate validation. No claim of cryptographic
impossibility is made merely because the blob contains a signed container.

Rewriting stream dimensions or splitting a compressed picture is not a
general driver fix: it changes macroblock addressing and reference-picture
semantics. Software fallback can provide pixels, but must not be reported as
VCN hardware decoding.

The currently retained 9,437,184-pixel policy is conservative. A larger policy
must check **rounded coded macroblocks**, rather than just pixel product:
4082×4082 is below `4096*4080` pixels but rounds to 256×256 macroblocks.
Interlaced pictures may require 32-line frame alignment. Adjacent progressive
hardware successes alone do not validate every interlaced/cropped input.

## Evidence location and primary references

Local disassembly and captures remain outside Git under
`installer-staging/verification/gpu-status-2026-11-17/media-root-cause/`.
The firmware itself and the arithmetic regression test are in this repository.

```text
Mesa commit 42c04e9730ace9e5e97af6e0d97147764b71b42f:
src/amd/common/ac_vcn_dec.h (message IDs, H264_PERF, SPS flag definitions)

linux-firmware commit 38de2359e28061cf16f599e050486003d0e557f2:
amdgpu/renoir_vcn.bin
amdgpu/green_sardine_vcn.bin

Cadence Xtensa ISA Summary:
https://www.cadence.com/content/dam/cadence-www/global/en_US/documents/tools/silicon-solutions/compute-ip/isa-summary.pdf
```

The custom-opcode regions were inspected separately; GNU's default Xtensa
configuration mislabels some `DEPBITS` encodings as floating-point operations.
The executable count proof uses standard integer opcodes and does not depend
on assigning undocumented instructions guessed semantics.
