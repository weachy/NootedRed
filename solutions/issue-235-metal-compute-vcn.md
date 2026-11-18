# Metal Compute, VideoToolbox & QuickLook Hangs on AMD APUs

- **Upstream Issue**: [ChefKissInc/NootedRed#235](https://github.com/ChefKissInc/NootedRed/issues/235) (`open` — *Metal Compute / VideoToolbox / QuickLook freezes on AMD APUs*)
- **Resolved In**: `v1.2.5` – `v1.2.6` (`c81e686`, `2bd1b74`, `8e32f46`)
- **Relevant Source Files**:
  - [UserPatches.hpp](../NootedRed/UserPatches.hpp) / [UserPatches.cpp](../NootedRed/UserPatches.cpp)
  - [VCNBridge.hpp](../NootedRed/VCNBridge.hpp) / [VCNBridge.cpp](../NootedRed/VCNBridge.cpp)

---

## 1. Problem Description

On AMD Vega APUs (especially on macOS 14 Sonoma and newer), Metal Compute workloads, VideoToolbox `MetalTransferSession`, Finder QuickLook / HEIC image previews, and App Store UI animations intermittently **hang or freeze** (`computeUQ` / `SDMA0` stalls). Previously, users had to work around this by disabling `CoreMedia` Metal transfer sessions via `defaults write /Library/Preferences/com.apple.coremedia allowMetalTransferSession -bool NO`.

---

## 2. Root Cause & Upstream Gaps

1. **Unpatched `AMDRadeonX5000MTLDriver` Shader Engine Count in `dyld_shared_cache`**:
   - Upstream `NootedRed` patched `gfx9_HwInfoInitData` (`numShaderEngines: 4 -> 1`) in `AMDRadeonX5000GLDriver`, but **left `AMDRadeonX5000MTLDriver` inside `dyld_shared_cache` unpatched**.
   - Because `AMDRadeonX5000MTLDriver` reported 4 Shader Engines instead of 1, Metal compute dispatches scheduled wavefronts across non-existent shader engines, stalling `computeUQ` / `SDMA0` during VideoToolbox and QuickLook operations.
2. **OpenCL Scratch Workgroup Geometry & UBC Page Eviction**:
   - Oversized OpenCL workgroup geometry for scratch kernels and UBC eviction of patched shared-cache pages under memory pressure can trigger compute stalls or drop patches after boot.

---

## 3. Final Solution

1. **Shared-Cache `AMDRadeonX5000MTLDriver` `gfx9_HwInfoInitData` Patch (`v1.2.5`, `c81e686`)**:
   - Gate 2.5 in `UserPatches::patchSignaturePage` matches `gfx9_HwInfoInitData` inside `dyld_shared_cache` and replaces all 3 Vega ASIC branches (`movl $4, 0x78(%rdi)` -> `movl $1, 0x78(%rdi)`), aligning Metal's Shader Engine count to `1`.
   - Eliminates the need for the `allowMetalTransferSession = NO` CoreMedia workaround.
2. **OpenCL Workgroup Geometry Clamp & UBC Eviction Cache (`v1.2.6`, `8e32f46`)**:
   - Added `kOpenCLDispatchFind_*` / `kOpenCLDispatchReplace_*` across macOS 13 – 26 to clamp workgroup geometry for scratch kernels.
   - Added `recordCachedSite()` / `reapplyCachedSites()` so shared-cache Metal and OpenCL patches remain intact across UBC page evictions.
3. **Non-Blocking VCN Task Teardown (`2bd1b74`)**:
   - `VCNBridge` uses asynchronous, non-blocking task teardown so QuickLook previews and video playback do not suffer periodic stalls.

---

## 4. Compatibility & Verification

- **macOS Versions**: Verified across **macOS 13 (Ventura)**, **macOS 14 (Sonoma)**, **macOS 15 (Sequoia)**, and **macOS 26 (Tahoe)**.
- **Result**: Finder QuickLook, HEIC previews, App Store UI, and VideoToolbox Metal transfer sessions run smoothly without `allowMetalTransferSession` overrides; `NRedMTLHwInfoApplied` reports the patched branch count in IORegistry.
