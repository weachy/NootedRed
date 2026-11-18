# macOS 15.8.1 `AMDRadeonVADriver2` user-space patch data for VDA

Status: generated data for the Phase 4 user-space track of `feature/vda-hardware-decode`. This document
describes `tools/gen_vda_va_data.py` and the header it produces, `NootedRed/VDAVAData.hpp`.

Read-only analysis: no kext was installed or loaded, no system file or dyld cache byte was written, no
GPU/VideoToolbox API was called. The generator only opens the cache and the system image for reading.

Evidence labels: **[F]** fact directly observed in the 15.8.1 image/cache; **[I]** inference from the
disassembly or from the task's semantics; **[?]** unknown, not provable without hardware.

## 1. Inputs and verification pins

| item | value |
| --- | --- |
| macOS / Darwin | 15.8.1 (24H32) / Darwin 24.6 |
| dyld shared cache | `/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/dyld_shared_cache_x86_64h*` |
| cache UUID / base | `42EC2890-BC40-317E-BB27-45142BFF7F26` / `0x7FF800000000` |
| image path | `/System/Library/Extensions/AMDRadeonVADriver2.bundle/Contents/MacOS/AMDRadeonVADriver2` |
| driver version | `7.0.25` (`Contents/version.plist`) |
| image base / Mach-O UUID | `0x7FFB08606000` / `662187E49F38375B9BF6CCAC3F60979A` |
| `__text` | `0x7FFB086069E0` .. `0x7FFB0868DF53`, 554355 (`0x87573`) bytes |
| `__text` SHA-256 | `bd079ec5c035db1ca6db3888989d7728d751b434b46a3332c5ae957cd1896044` |

The image has no on-disk `Contents/MacOS` binary; everything is read from the cache. The generator
reads the cache through the on-disk mapping table, resolves the image by its image-table path, parses
the Mach-O load commands/symtab itself, and refuses to continue if the cache UUID, cache base or any
verified byte differs. [F]

Segment layout of the image (cache addresses; `__TEXT` file offset is the offset inside the
`dyld_shared_cache_x86_64h.03` subcache): **[F]**

| segment | vmaddr (end) | cache file offset |
| --- | --- | --- |
| `__TEXT` | `0x7FFB08606000` – `0x7FFB086BE268` | `0x8606000` |
| `__DATA_CONST` | `0x7FFB412AD680` | `0x20585680` |
| `__DATA` | `0x7FFB4346D690` | `0x22745690` |
| `__LINKEDIT` | `0x7FFB80000000` | `0x239D4000` |

## 2. Generated page images (and live `VDASignatures.inc` targets)

> **Sync Note (v1.2.6):** The driver has migrated from static page images (`VDAVAData.hpp`) to dynamic context signatures in `NootedRed/VDASignatures.inc` (`kVDAResolverSignatures_Modern`), enabled by default. On macOS 15.8.1 (`24H32`), all 9 target pages below live in `dyld_shared_cache_x86_64h.03` at `__TEXT` fileoff `0x8606000 + page`. **[F]**

| index | image offset | cache file page offset | target symbol(s) |
| --- | --- | --- | --- |
| `Page0` | `0x000000` | `0x8606000` | `VAAcceleratorInfo::identify` |
| `Page1` | `0x002000` | `0x8608000` | `VAAddrLibInterface::init` |
| `Page2` | `0x009000` | `0x860F000` | `VAFactory::create*VP`, `createGraphicsEngine`, `createImageBlt` |
| `Page3` | `0x046000` | `0x864C000` | `Vcn2Encoder::setEncodeParams` (`Vcn2EncoderSourcePitch`) |
| `Page4` | `0x047000` | `0x864D000` | `Vcn2EncCommand::getFeedbackSize` |
| `Page5` | `0x049000` | `0x864F000` | `AVDVcn2AvcEncDataProcessor::ProcessEncodeStatus` |
| `Page6` | `0x04A000` | `0x8650000` | `AVDVcn2HevcEncDataProcessor::ProcessEncodeStatus` |
| `Page7` | `0x061000` | `0x8667000` | `Addr::Lib::Create` |
| `Page8` | `0x077000` | `0x867D000` | `Addr::V2::Gfx9Lib::HwlConvertChipFamily` |

## 3. Patch inventory

`skip`/`count` are the occurrence selector inside the page image; in `VDASignatures.inc`, `Addr::Lib::Create` and `HwlConvertChipFamily` use non-destructive wildcard instructions (`cmp eax, eax` / `cmp esi, esi`) so both Metal (`0x8D`) and VA (`0x8E`) drivers in `dyld_shared_cache` pass simultaneously. **[F]**

| # | table name (semantics) | function (symbol) | function address | site / find offset | page / page offset | find → replace (live `VDASignatures.inc`) | conf. |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | `IdentifyForceFamily5` | `VAAcceleratorInfo::identify(unsigned, unsigned)` | `0x7FFB086069E0` | `0x7FFB08606B96` (+0x1B6) | `Page0` (`0x000000`) / `0xB96` | `BB 0B 00 00 00` → `BB 05 00 00 00` | high |
| 1 | `AddrLibInitVegaToRV` | `VAAddrLibInterface::init(VAIOConnect*, sHardwareInfo*)` | `0x7FFB086083BE` | `0x7FFB08608449` (+0x8B) | `Page1` (`0x002000`) / `0x449` | `41 81 FC 8D 00 00 00` → `41 81 FC 8E 00 00 00` | high |
| 2 | `ForceGfx9VideoProcessors` (3 sites) | `VAFactory::createColourFormatConverterVPEi`, `createDitheringVPEi`, `createDeblockerVPEi` | `0x7FFB0860F980`, `0x7FFB0860FA42`, `0x7FFB0860FAC4` | +0x7 each: `0xF987`, `0xFA49`, `0xFACB` | `Page2` (`0x009000`) / `0x987`, `0xA49`, `0xACB` | `8D 46 FF 83 F8 02 72 1F` → `8D 46 FF 83 F8 02 EB 1F` | high |
| 3 | `ForceGfx9EngineAndBlt` (2 sites) | `VAFactory::createGraphicsEngine(VAContext*)`, `createImageBlt(VAContext*, unsigned)` | `0x7FFB0860FBC6`, `0x7FFB0860FC5E` | +0xA / +0xB: `0xFBD0`, `0xFC69` | `Page2` (`0x009000`) / `0xBD0`, `0xC69` | `48 8B 86 60 04 00 00 8B 40 0C 8D 48 FF 83` → `B8 02 00 00 00 90 90 90 90 90 8D 48 FF 83` | high |
| 4 | `Vcn2EncoderSourcePitch` | `Vcn2Encoder::setEncodeParams(void*, VAVendorTexture*)` | `0x7FFB0864CABA` | `0x7FFB0864CAEE` (+0x34) | `Page3` (`0x046000`) / `0xAEE` | `8B 8A 84 00 00 00 ... 25 00 FF FF FF` (25B) → `8B 82 80 00 00 00 8B 8A 84 00 00 00 90...` | high (encode follow-up) |
| 5 | `Vcn2EncCommandFeedbackStride` | `Vcn2EncCommand::getFeedbackSize()` | `0x7FFB0864DF84` | `0x7FFB0864DF88` (+0x4) | `Page4` (`0x047000`) / `0xF88` | `B8 A4 00 00 00 5D C3` → `B8 30 00 00 00 5D C3` | high (encode follow-up) |
| 6 | `Vcn2AvcProcessEncodeStatusStride` | `AVDVcn2AvcEncDataProcessor::ProcessEncodeStatus(...)` | `0x7FFB0864F5FC` | `0x7FFB0864F626` (+0x2A) | `Page5` (`0x049000`) / `0x626` | `48 69 C0 A4 00 00 00` → `48 69 C0 30 00 00 00` | high (encode follow-up) |
| 7 | `Vcn2HevcProcessEncodeStatusStride` | `AVDVcn2HevcEncDataProcessor::ProcessEncodeStatus(...)` | `0x7FFB08650802` | `0x7FFB0865082C` (+0x2A) | `Page6` (`0x04A000`) / `0x82C` | `48 69 C0 A4 00 00 00` → `48 69 C0 30 00 00 00` | high (encode follow-up) |
| 8 | `AddrLibCreateVegaToRV` | `Addr::Lib::Create(_ADDR_CREATE_INPUT const*, _ADDR_CREATE_OUTPUT*)` | `0x7FFB08667BB0` | `0x7FFB08667C65` (+0xB5) | `Page7` (`0x061000`) / `0xC65` | `3D 8D 00 00 00` → `39 C0 90 90 90` (`cmp eax, eax`) | high |
| 9 | `Gfx9ConvertChipFamilyVegaToRV` | `Addr::V2::Gfx9Lib::HwlConvertChipFamily(unsigned, unsigned)` | `0x7FFB0867D770` | +0x0 | `Page8` (`0x077000`) / `0x770` | `81 FE 8D 00 00 00` → `39 F6 90 90 90 90` (`cmp esi, esi`) | medium |

Totals after the encode pitch follow-up: **10 patch records**, **9 page images**, **13 patched byte sites** (`Page0`–`Page8` at `0x000000`, `0x002000`, `0x009000`, `0x046000`, `0x047000`, `0x049000`, `0x04A000`, `0x061000`, `0x077000`). Note that `0x008000` is **not** patched.

## 4. Per-patch evidence

### 4.1 `VAAcceleratorInfo::identify` — classify 0x1638 as family 5 (Navi10/VCN2 class)

The 15.8.1 function starts with a hardware query, then classifies the returned device id: **[F]**

```
7ffb086069e9: 48 c7 45 f0 18 01 00 00   movq   $0x118, -0x10(%rbp)      ; output-struct size
7ffb086069f1: bb 0b 00 00 00            movl   $0xb, %ebx                 ; default: family 11 = unsupported
7ffb086069f6: 83 fe 01                  cmpl   $0x1, %esi
7ffb086069f9: 75 36                     jne    0x7ffb08606a31             ; arg2 != 1 -> return 0xb
7ffb086069fb: ...                       ...    ; IOConnectCallMethod(selector 0x100), outputStruct 0x7FFB4347D080
7ffb08606a3a: 0f b7 05 43 66 e7 3a      movzwl 0x3ae76643(%rip), %eax     ; device id at struct+4
...
7ffb08606b96: bb 0b 00 00 00            movl   $0xb, %ebx                 ; classification fallback
7ffb08606b9b: e9 91 fe ff ff            jmp    0x7ffb08606a31
```

The classification maps known device IDs to families 0..10 and falls back to `0xb`; emulating the
comparison chain over all 16-bit IDs confirms **0x1638 → 11 (unsupported)**. [F] The caller
`_AVD_CreateAVDAccelerator` rejects identify results 11/12 and continues for 5, then uses 5 for
`AvdCapability::initDriverCapability` and `VAContext`'s `_VAHWConfig`. [F]

Patch: change the classification fallback immediate from `0xB` to `5`, keeping the rest of the
function (including the hardware query) untouched:

```
7ffb08606b96: bb 05 00 00 00            movl   $0x5, %ebx
7ffb08606b9b: e9 91 fe ff ff            jmp    0x7ffb08606a31
```

**Why the hardware query is kept.** `_AVD_CreateAVDAccelerator` calls identify with the connection
obtained from `IOAccelVideoContextGetConnect`, and identify's `IOConnectCallMethod` writes its output
struct to `0x7FFB4347D080` (a `__common`/BSS buffer in the image). `VAAcceleratorInfo::getExtraIdInfo`
then stores that same address as the first field of `_VAHWConfig`, and `VAContext::Init` passes it to
`VAAddrLibInterface::init` as `sHardwareInfo` (family at `[0]`, revision at `[8]`). Skipping the query
(the early-return mode the legacy Ventura patch forced) would leave the buffer zeroed, make
`Addr::Lib::Create` fail unconditionally and break decode — so the table patches the *classification
fallback* instead. This is a deliberate deviation from the legacy `kVAAcceleratorInfoIdentifyVentura*`
patch, justified by the 15.8.1 data flow. [F]

**Dependency [I]:** the kernel `AMDRadeonX5000` user client must accept selector `0x100` and return
hardware info for this device; if it fails, identify returns the initial `0xb` and driver creation
fails safely (no decode). The device-id classification path for 0x1638 is the fallback at
`0x7FFB08606B96`; changing that immediate affects only IDs that had no family mapping, and the table is
only installed for the 0x1638 profile.

Family 5 makes `getExtraIdInfo` fill the Navi10 `_VAHWConfig` (`hwFamily` field `+0xC = 3`), which is
what the decoder/command factories expect for a VCN2 decoder (`VAFactory::createUVDCommand` maps
`hwFamily 3 → Vcn2DecCommand`); it is **deliberately not changed** (see §5.2). [F]

### 4.2 `VAAddrLibInterface::init` — accept family 0x8E

`init` takes `sHardwareInfo*` in `rdx`, copies `hardwareInfo[0]` (chip family) and maps it for
`ADDR_CREATE_INPUT.chipFamily`: `{0x6E,0x78,0x82} → 0xA`, `0x8D (AI/Vega) → 0xD`, everything else
leaves 0. **[F]**

```
7ffb08608449: 41 81 fc 8d 00 00 00      cmpl   $0x8d, %r12d
7ffb08608450: 75 0b                     jne    0x7ffb0860845d
7ffb08608452: b8 0d 00 00 00            movl   $0xd, %eax
```

Renoir is RV-class GFX9. The same `0x8D → 0x8E` remap is already used by this tree for the kernel
X5000 driver's own `Addr::Lib::Create` check, with the comment "The check in `Addr::Lib::Create` on
<=10.15 and 13.4+ is `familyId == 0x8D` instead of `familyId - 0x8D < 2`"
(`NootedRed/X5000.cpp:58-62`), which is independent evidence that the kernel side sees family `0x8E`
for Renoir. The VA driver queries the same kernel driver (`IOConnectCallMethod` selector `0x100`) and
passes the returned struct to `VAAddrLibInterface::init` as `sHardwareInfo`, so `sHardwareInfo[0]`
is expected to be `0x8E` too. Retargeting the compare to `0x8E` selects `0xD`. The legacy VA driver
achieved the same by NOPing the compare and `jne`; the targeted immediate remap is preferred because
it leaves other families untouched. **Assumption [I]:** the kernel-reported `sHardwareInfo[0]` is
`0x8E` on 15.8.1 Renoir; if it were something else, the NOP-only variant is the documented fallback
candidate.

### 4.3 `Addr::Lib::Create` — take `Addr::Gfx9HwlInit` for 0x8E

With `chipFamily = 0xD`, `Create` compares the real family (`input+8`) against `0x8F` (Gfx10HwlInit)
and `0x8D` (Gfx9HwlInit): **[F]**

```
7ffb08667c5e: 41 8b 46 08               movl   0x8(%r14), %eax
7ffb08667c65: 3d 8d 00 00 00            cmpl   $0x8d, %eax
7ffb08667c6a: 0f 85 b4 01 00 00         jne    0x7ffb08667e24          ; error path
7ffb08667c70: ...                       ...    call Addr::Gfx9HwlInit
```

Renoir reports `0x8E`, so the compare is retargeted to `0x8E`; verify with the patched disassembly:

```
7ffb08667c65: 3d 8e 00 00 00            cmpl   $0x8e, %eax
7ffb08667c6a: 0f 85 b4 01 00 00         jne    0x7ffb08667e24
7ffb08667c74: e8 a7 33 01 00            callq  Addr::Gfx9HwlInit(Addr::Client const*)
```

### 4.4 `Addr::V2::Gfx9Lib::HwlConvertChipFamily` — configure the 0x8E family

The function returns immediately (with `eax = 8`) unless `chipFamily == 0x8D`, in which case it derives
the `Gfx9Lib` settings word at `this+0x5B10`/`+0x5B18` from the chip revision: **[F]**

```
7ffb0867d770: 81 fe 8d 00 00 00         cmpl   $0x8d, %esi
7ffb0867d776: 0f 85 88 00 00 00         jne    0x7ffb0867d804           ; return 8 without initialising settings
7ffb0867d77c: ...                       ...    ; settings computed from edx (chip revision)
7ffb0867d7f5: 44 89 97 18 5b 00 00      movl   %r10d, 0x5b18(%rdi)
7ffb0867d7fc: 48 89 8f 10 5b 00 00      movq   %rcx, 0x5b10(%rdi)
7ffb0867d804: b8 08 00 00 00            movl   $0x8, %eax
```

`Addr::V2::Gfx9Lib::HwlInitGlobalParams` gates on bit 0 of `+0x5B10` and returns 0 (failure) otherwise,
so without this patch the addrlib initialisation fails after patch 4.3. Retargeting the compare to
`0x8E` runs the native Gfx9 settings path for Renoir: **medium confidence** — the instruction bytes and
disassembly are verified, but the fork used a hand-written RV-specific replacement for this function,
so it is **unknown** whether reusing Apple's AI/Vega settings produces RV-correct tiling equations for
decode surfaces. This is the first thing to revisit if decoded pictures are corrupt but the decoder
opens. A full-function RV replacement would exceed the 32-byte `find`/`replace` budget of the current
`UserPatches` backend; it would need a separate full-function patch mechanism.

### 4.5 `VAFactory::createGraphicsEngine` / `createImageBlt` — force the GFX9 engine type

Both factories derive the engine type from the `_VAHWConfig` embedded in the context (`ctx+0x460`):
values 1/2 select the Gfx9 classes and 3/4 the Gfx10 classes; family 5 fills 3. **[F]**

```
createGraphicsEngine:
7ffb0860fbd0: 48 8b 86 60 04 00 00      movq   0x460(%rsi), %rax
7ffb0860fbd7: 8b 40 0c                  movl   0xc(%rax), %eax
7ffb0860fbda: 8d 48 ff                  leal   -0x1(%rax), %ecx
7ffb0860fbdd: 83 f9 02                  cmpl   $0x2, %ecx
7ffb0860fbe0: 72 22                     jb     0x7ffb0860fc04           ; Gfx9GraphicsEngine
...
createImageBlt:
7ffb0860fc69: 48 8b 86 60 04 00 00      movq   0x460(%rsi), %rax
7ffb0860fc70: 8b 40 0c                  movl   0xc(%rax), %eax
7ffb0860fc73: 8d 48 ff                  leal   -0x1(%rax), %ecx
7ffb0860fc76: 83 f9 02                  cmpl   $0x2, %ecx
7ffb0860fc79: 72 0f                     jb     0x7ffb0860fc8a           ; Gfx9ImageBlt::createInstance
```

Renoir is GFX9 hardware, so the two loads are replaced with `movl $2,%eax` + five NOPs, keeping the
following `lea/cmp/jb` bytes and therefore the Gfx9 branch: **[F, patched disassembly]**

```
7ffb0860fbd0: b8 02 00 00 00            movl   $0x2, %eax
7ffb0860fbd5: 90 ... 90                  nop *5
7ffb0860fbda: 8d 48 ff                  leal   -0x1(%rax), %ecx          ; ecx = 1
7ffb0860fbdd: 83 f9 02                  cmpl   $0x2, %ecx
7ffb0860fbe0: 72 22                     jb     ... Gfx9 engine
```

The 14-byte find includes the `lea/cmp` prefix so it matches only these two factories. The third
occurrence of the 10-byte load prefix in the page, `VAFactory::createUVDCommand` (which must keep
`hwFamily 3 → Vcn2DecCommand`), does not match and is not patched. `createImageBlt` is needed by
`VADecoder::Init` (`VAContext::createImageBlt(0x12)`); the graphics engine is created by the video
processor path (`VAVideoProcessor::init` → factory). [F/I]

### 4.6 `VAFactory::create*VP` — force the GFX9 video processors

`createColourFormatConverterVP`, `createDitheringVP` and `createDeblockerVP` take the family as `esi`
and branch the same way; this is the exact 8-byte sequence the legacy driver patched (`EB` instead of
`72`): **[F]**

```
7ffb0860f987: 8d 46 ff                  leal   -0x1(%rsi), %eax
7ffb0860f98a: 83 f8 02                  cmpl   $0x2, %eax
7ffb0860f98d: 72 1f                     jb     <Gfx9 ctor>              ; 0x7FFB0860F9AE
7ffb0860f98f: 8d 46 fd                  leal   -0x3(%rsi), %eax          ; (family 3/4 -> Gfx10)
```

Replacing `72` with `EB` (same displacement) makes the Gfx9 constructor unconditional and leaves the
argument untouched. The three sites are `0xF987`, `0xFA49`, `0xFACB`; the identical 8-byte sequence
occurs exactly three times in page 2, and the patch record selects all three with `count = 3`. Neither
`VAFactory::createVideoProcessor` (a dispatch table on the video-processor type, not on the family) nor
the other `create*VP` functions (which only test `esi == 0`) are patched. **[F]**

## 5. Not patched / out of scope

### 5.1 Swizzle-mode masks (omitted candidate)

The fork report describes eight 4-byte edits, `0x02220221 → 0x02020201` and `0x06660661 → 0x06060601`
(clearing bits 5 and 21 in bitmasks that declare which display swizzle modes are valid), in four Gfx9
addrlib functions. The same constants do exist in 15.8.1: **[F]**

| value | sites (15.8.1) | enclosing function |
| --- | --- | --- |
| `0x02220221` | `0x7FFB08680C86`, `0x7FFB0868142A`, `0x7FFB086817E0`, `0x7FFB086821C5` | `Gfx9Lib::IsValidDisplaySwizzleMode`, `ValidateSwModeParams`, `HwlComputeSurfaceInfoSanityCheck`, `HwlGetPreferredSurfaceSetting` |
| `0x06660661` | `0x7FFB08680CAF`, `0x7FFB08681437`, `0x7FFB086817EA`, `0x7FFB086821C0` | ditto |

They are **not** emitted: the fork documents neither the reason nor the resulting behaviour, the edit
would simply mark two swizzle modes invalid on this platform, and nothing in the 15.8.1 code shows why
Renoir needs that. Without hardware validation this is a guess, so it is recorded here as the first
candidate if decode output shows swizzle/tiling corruption (pages would be `Page 0x7A`, `0x7B`, `0x7C`).
The patch framework would need three more page images and two more patch records.

### 5.2 `VAFactory::createUVDCommand` — must stay native

`createUVDCommand` switches on `hwFamily` (`ctx+0x460 → +0xC`) and maps **3 → `Vcn2DecCommand`**, 2 →
`Uvd7v2Command`, 1 → `Uvd7Command`, 4 → `Vcn3DecCommand`. Family 5 keeps `hwFamily = 3`, so VCN2
decode commands are emitted; forcing the family to 2 (a tempting alternative to patches 4.5/4.6) would
have selected the wrong command class, which is why the engine/VP factories are patched instead of the
family value. Likewise `VAFactory::createDecoder` selects `VAUvdDecoder` when `_VAHWConfig+0x94` bit
`0x10` is clear (family 5 is clear), i.e. exactly the native Navi10 behaviour, so it needs no patch.
**[F]**

### 5.3 Encode-only patches

The fork's hardware-encode fixes (`Vcn2EncCommand::getFeedbackSize` `0xA4→0x30`,
`AVDVcn2AvcEncDataProcessor::ProcessEncodeStatus`/HEVC `imul 0xA4→0x30`, and
`Vcn2Encoder::setEncodeParams` source pitch) were initially out of scope for decode-only bring-up,
but have since been integrated into `NootedRed/VDASignatures.inc` (rows 4–7 above, covering pages
`0x046000`, `0x047000`, `0x049000`, `0x04A000`). The legacy VCN1 command translations remain
irrelevant: this table drives native VCN2 hardware. **[F]**

## 6. Integration contract and safety

- The table is only for the Renoir `0x1638` profile (identify is forced to family 5 unconditionally
  inside the process). Do not install it on other devices or when `-NRedVDA` is absent; `-NRedVDA`
  must stay default-off.
- Delivery is `UserPatches`' native page backend: each page image must match byte-for-byte before a
  patch is applied, so a cache/driver update cannot be half-patched. The generated header also carries
  the cache UUID/base, image base, image UUID and `__text` SHA-256 (`SourceDescription`, `CacheUUID`,
  `TextSHA256`, `CacheBase`, `ImageBase`) so the integration can refuse a mismatched build.
- Required backend limits: **8 pages**, **5 sites on one page**, **14-byte** find/replace for the VA table; the combined backend is configured for 16 pages / 16 sites / 32 bytes (12 pages are used today). The encode follow-up rows 6-8 live on pages 3-5; pages 0-2 are the original decode sites and pages 6-7 are the addrlib sites.
- `VDAVAData.hpp` is fully generated; do not hand-edit. `python3 tools/gen_vda_va_data.py` rewrites it,
  `python3 tools/gen_vda_va_data.py --check` re-derives everything from the live cache, re-verifies
  every find in the exact page image and compares the committed table semantically.
- If any patch cannot be applied, decode must stay disabled and fall back to software; no kernel-side
  state is touched by these page patches.

## 7. Confidence, unknowns and next steps

Verified from bytes/disassembly: patches 4.1–4.6 (find/replace, page placement, skip/count, patched
instruction semantics). Verified only by inference: that the kernel reports `sHardwareInfo[0] = 0x8E`
for this device, and that the AI settings used by `HwlConvertChipFamily` are suitable for RV. [?]

Open items:

1. **Hardware validation is required** for the whole table: does `VTDecoderXPCService` open a hardware
   decoder on 15.8.1 Renoir, and do H.264/HEVC outputs match software decode? Nothing here was run on
   hardware. The first gate is whether the kernel X5000 user client answers identify's selector `0x100`
   for 0x1638 with a valid hardware-info struct (§4.1); without it the VA driver fails safely.
2. **RV vs AI addrlib settings.** If the decoder opens but surfaces are corrupted, replace patch 4.4
   with a dedicated 0x8E configuration path (the fork used such a replacement). That exceeds the
   32-byte patch budget and needs a different delivery mechanism.
3. **Are the engine/VP patches needed to open the decoder or only for surface handling?** They are
   included because `VADecoder::Init` creates an image blitter and the video processor creates a
   graphics engine; if a future minimal table is wanted, these are the first candidates to drop, but
   dropping them while keeping family 5 makes the driver program GFX10 objects on GFX9 hardware.
4. **Swizzle masks** (§5.1) are the documented fallback if decode output is corrupted.
5. **`sHardwareInfo[0]` assumption** (§4.2): if the kernel driver reports a different family, use the
   legacy NOP-the-compare-and-jump variant in `VAAddrLibInterface::init` instead of the 0x8D→0x8E
   remap; `Addr::Lib::Create` and `HwlConvertChipFamily` would then need the same family value
   treatment.

## 8. References

- Generated artifacts: `tools/gen_vda_va_data.py`, `NootedRed/VDAVAData.hpp`.
- Semantic references (no code/offset copied): `analysis/vcn-approaches.md`,
  `analysis/vcn-steve12311.md` §4, `analysis/vcn-legacy-nootedred.md` §4.3–4.4, and the legacy
  `DYLDPatches.hpp@4523553^` patch inventory in this repository.
- Independent in-tree evidence for the Renoir family value/remap: `NootedRed/X5000.cpp:58-62`
  (kernel `Addr::Lib::Create` 0x8D→0x8E), `NootedRed/HWLibs.cpp:463-475` (PowerTune 0x8D→0x8E).
- Tooling used for derivation: `/Library/Developer/CommandLineTools/usr/bin/llvm-objdump` on a
  reconstructed synthetic Mach-O containing the cached `__text` section; a Python dyld-cache/Mach-O
  parser in the generator; the cache `.map` file for address→subcache verification.
