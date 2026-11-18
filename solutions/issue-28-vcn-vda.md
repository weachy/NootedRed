# Hardware Image & Video Decode/Encode Support (VCN2 / VDA / AppleGVA)

- **Upstream Issue**: [ChefKissInc/NootedRed#28](https://github.com/ChefKissInc/NootedRed/issues/28) (`open` — *Image & video hardware decoding/encoding*)
- **Resolved In**: `v1.2.2` – `v1.2.6` (`80c8c04`, `0884bc7`, `2bd1b74`, `572bc29`, `8e32f46`)
- **Relevant Source Files**:
  - [VCNBridge.hpp](../NootedRed/VCNBridge.hpp) / [VCNBridge.cpp](../NootedRed/VCNBridge.cpp)
  - [VDA.hpp](../NootedRed/VDA.hpp) / [VDA.cpp](../NootedRed/VDA.cpp)
  - [UserPatches.hpp](../NootedRed/UserPatches.hpp) / [UserPatches.cpp](../NootedRed/UserPatches.cpp)
  - [VDASignatures.inc](../NootedRed/VDASignatures.inc)

---

## 1. Problem Description

AMD Vega APUs lack legacy UVD/VCE blocks and instead implement **VCN** (Video Core Next) for hardware-accelerated video/image decoding and encoding (H.264/AVC, HEVC, HEIF/HEIC). On unpatched macOS, `AMDRadeonX5000` provides no VCN engine, and Apple's user-space video stack (`AMDRadeonVADriver2` and `AppleGVA`) rejects APU device IDs and chip family codes, forcing all VideoToolbox playback and HEIC decoding onto CPU software paths.

---

## 2. Root Cause & Upstream Gaps

1. **Missing Kernel VCN2 Engine in `AMDRadeonX5000`**:
   - Apple's `AMDRadeonX5000` kext does not implement `AMDVCN2HWEngine`, whereas `AMDRadeonX6000` does.
2. **User-Space Family & Device Rejection in `AMDRadeonVADriver2` / `AppleGVA`**:
   - `VAAcceleratorInfo::identify` does not map APU device IDs (e.g. `0x1638`) to VA family `5` (`Navi10/VCN2`), defaulting to unsupported family `0xB`.
   - `VAAddrLibInterface::init`, `Addr::Lib::Create`, and `Gfx9ConvertChipFamily` expect `0x8D` instead of APU family `0x8E` (Renoir/RV).
3. **Shared `AddrLib` Conflict inside `dyld_shared_cache`**:
   - Apple's `dyld_shared_cache` statically links `AddrLib` into both Metal drivers (`0x8D`: `AMDRadeonX5000MTLDriver` / `AMDMTLBronzeDriver` / `AMDRadeonX6000MTLDriver`) and `AMDRadeonVADriver2` (`0x8E`).
   - Replacing `0x8D` with `0x8E` breaks Metal initialization, while leaving `0x8D` breaks VDA.

---

## 3. Final Solution

1. **Kernel VCN2 Engine Bridge (`VCNBridge`)**:
   - Force-loads `com.apple.kext.AMDRadeonX6000` as a symbol donor from inside `AMDRadeonX5000` accelerator initialization.
   - Validates per-OS vtable and getter ABI across macOS 13 – 26 (`VCNKernelData.hpp`, `VCNKernelDataVentura.hpp`, `VCNKernelDataTahoe.hpp`) before patching, installs cross-driver call-site patches with rollback, and attaches `AMDVCN2HWEngine` at `kAMDHWEngineTypeVCN0`.
   - Every failure path clears readiness gracefully without panicking, falling back to graphics-only mode, and task teardown is non-blocking so playback and QuickLook scrubbing do not stutter.
2. **Dynamic VDA / GVA Signatures & Non-Destructive `AddrLib` Wildcards ([VDASignatures.inc](../NootedRed/VDASignatures.inc), `572bc29`)**:
   - **`IdentifyForceFamily5`**: Maps APU device ID (`0x1638`) to VA family `5` (`Navi10/VCN2`).
   - **`AddrLibInitVegaToRV`**: Maps family `0x8E` to AddrLib family `0xD`.
   - **`AddrLibCreateVegaToRV` & `Gfx9ConvertChipFamilyVegaToRV`**: Replaces `cmp eax, 0x8d` / `cmp esi, 0x8d` with **non-destructive `cmp eax, eax` / `cmp esi, esi` + `nop`** (`ZF=1`), allowing both `0x8D` (Metal) and `0x8E` (VDA) to pass cleanly inside `dyld_shared_cache`.
   - Applied process-agnostically via `_cs_validate_page` / `_cs_validate_range` and protected against page eviction by `recordCachedSite()` / `reapplyCachedSites()` (`8e32f46`).
3. **Policy & Boot Arguments (`VDA.cpp`)**:
   - Hardware VDA acceleration (AVC + HEVC) is **enabled by default**.
   - Diagnostic opt-out boot arguments: `-NRedNoVDA` (disable all hardware VDA), `-NRedNoHEVC` (AVC/H.264 only), and `-NRedNoHEVC10`.

---

## 4. Compatibility & Verification

- **macOS Versions**: Verified dynamically across **macOS 13 (Ventura)**, **macOS 14 (Sonoma)**, **macOS 15 (Sequoia)**, and **macOS 26 (Tahoe)**.
- **Result**: Hardware H.264/AVC and HEVC decode/encode via VideoToolbox and `AMDRadeonVADriver2` work out of the box alongside Metal without cross-driver `AddrLib` regressions.
