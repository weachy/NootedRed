# Chromium / Electron / OpenGL & Metal Visual Corruption and User-Space Patching

- **Upstream Issue**: [ChefKissInc/NootedRed#158](https://github.com/ChefKissInc/NootedRed/issues/158) (`open` — *Chromium / OpenGL graphics corruption & user-space patching*)
- **Resolved In**: `v1.2.2` – `v1.2.6` (`80c8c04`, `0884bc7`, `c81e686`, `8e32f46`)
- **Relevant Source Files**:
  - [UserPatches.hpp](../NootedRed/UserPatches.hpp) / [UserPatches.cpp](../NootedRed/UserPatches.cpp)
  - [NRed.hpp](../NootedRed/NRed.hpp) / [NRed.cpp](../NootedRed/NRed.cpp)

---

## 1. Problem Description

On AMD Vega APUs running macOS 13 – 26, Chromium-based browsers, Electron apps, and CEF applications exhibit severe **OpenGL / Metal visual corruption** (garbled textures, Dual-Source / Dual-Quad blending artifacts, and broken dropdown/UI compositing). In addition, user-space driver patches frequently broke across minor macOS updates or failed to persist under memory pressure.

---

## 2. Root Cause & Upstream Gaps

1. **Discrete-GPU Topology & Blending Assumptions in Apple's User-Space Drivers**:
   - `AMDRadeonX5000GLDriver` and `AMDRadeonX5000MTLDriver` inside `gfx9_HwInfoInitData` hardcode `numShaderEngines = 4` (`movl $4, 0x78(%rdi)`), whereas AMD Vega APUs only have `1` Shader Engine.
   - `AMDRadeonX5000GLDriver` contains a Dual-Quad blending mask (`and $0xFE, %eax` / `0x83 0xE0 0xFE`) and `AMDRadeonX5000MTLDriver` enforces Gate/Allow legacy blend checks that misrender translucent/dropdown layers on APUs.
2. **Lilu `UserPatcher` Limitations on macOS 11+**:
   - Upstream Lilu's `UserPatcher` is disabled on macOS 11 and newer and only wraps `_cs_validate_range`. However, vnode-backed bundles such as `AMDRadeonX5000GLDriver` and `dyld_shared_cache` pages are validated via **`_cs_validate_page`** (as well as `_cs_validate_range`).
3. **Missing Multi-OS Coverage & UBC Page Eviction Handling**:
   - Upstream lacked dynamic signature matching across macOS 13 – 26, and without tracking patched offsets, kernel Unified Buffer Cache (UBC) page eviction under memory pressure can re-fault clean unpatched pages.

---

## 3. Final Solution

1. **Self-Contained Process-Agnostic UBC Patcher (`UserPatches::wrapCsValidatePage` / `wrapCsValidateRange`)**:
   - Hooks kernel code-signing validation directly on macOS 11+ without requiring `AMFIPass.kext` or injecting `__RESTRICT` into user processes.
   - Operates **process-agnostically** (independent of `current_proc` and I/O fault order) and only patches pages that pass code-signature validation (`*validated != 0`).
2. **Dynamic Multi-OS Signature Resolution across macOS 13 – 26**:
   - Uses masked byte-sequence signatures (no hardcoded offsets or static page tables) targeting **Ventura (13)**, **Sonoma (14)**, **Sequoia (15)**, and **Tahoe (26)**:
     - **`AMDRadeonX5000GLDriver`**: Patches `gfx9_HwInfoInitData` (`movl $4, 0x78(%rdi)` -> `movl $1, 0x78(%rdi)`) and Dual-Quad blending (`0x83 0xE0 0xFE` -> `0x83 0xC8 0x01`).
     - **`AMDRadeonX5000MTLDriver` (in `dyld_shared_cache`)**: Patches `gfx9_HwInfoInitData` (`4 -> 1` across all 3 Vega ASIC branches on the matched page) and atomically patches co-located Metal **Gate** and **Allow** blend sites (`diff < 0x500` bytes on the same 4KB page).
3. **UBC Page Eviction Cache (`v1.2.6`, `8e32f46`)**:
   - `recordCachedSite()` and `reapplyCachedSites()` record resolved `(vnodeKind, pageOffset, inPageOffset)` patch locations so any evicted page re-validated by the kernel is patched again immediately without re-scanning.
4. **Opt-Out Boot Arguments**:
   - Enabled by default; diagnostic opt-out flags `-NRedNoGLFix` and `-NRedNoLegacyBlend` are available.

---

## 4. Compatibility & Verification

- **macOS Versions**: Verified dynamically across **macOS 13 (Ventura)**, **macOS 14 (Sonoma)**, **macOS 15 (Sequoia)**, and **macOS 26 (Tahoe)**.
- **Telemetry**: Publishes `NRedGLResolverApplied`, `NRedMTLHwInfoApplied`, and `NRedLegacyBlendApplied` in IORegistry when matched.
