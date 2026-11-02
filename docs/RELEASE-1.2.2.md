# NootedRed v1.2.2

We are thrilled to announce **NootedRed v1.2.2**, a landmark milestone release following v1.2.0. This release marks the completion of our transition to **100% universal multi-version dynamic patching across macOS 13 (Ventura), macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe)**, completely eliminating static 4KB page tables and drastically streamlining the driver binary size and memory footprint. In addition, critical kernel stability fixes, VRAM allocator robustness, and flexible SIP configurations are officially delivered.

---

### What's New & Resolved in v1.2.2

#### 1. Milestone: 100% Dynamic Multi-OS Patching (macOS 13 – 26) & Driver Streamlining
- **Complete Elimination of Static Page Tables**: Stripped all legacy hardcoded 4KB page dumps and static headers (`UserPatchesVentura.inc`, `UserPatchesSonoma.inc`, `UserPatchesSequoia.inc`, `UserPatchesTahoe.inc`, `VDAVAData*.hpp`, `VDAGVAData*.hpp`, and `TahoePatchData.hpp`) — removing over 14,000 lines of raw static hexadecimal tables.
- **Universal Dynamic Pattern Resolution**: All user-space graphics and media acceleration fixes (`GLFix`, `SubmitGuard`, `LegacyBlend`, and `VDA` hardware decode/encode bridges) now operate entirely via resilient dynamic signature scanning and runtime relocation.
- **Massive Binary & Memory Footprint Reduction**: Significantly reduced the `NootedRed.kext` executable size, drastically lowering the kernel wired memory consumption during boot and driver initialization.
- **Future-Proof Forward Compatibility**: Incremental macOS builds and future security updates across macOS 13, 14, 15, and 26 will adapt dynamically without needing manual page extraction or binary rebuilds.

#### 2. Kernel Stability & GPU Scheduler Enhancements
- **Elimination of SWScheduler Page Fault (`CR2: 0x48`)**: Resolved a null-pointer dereference inside `AMDSWScheduler::advance` during user login on single-GPU APUs.
- **VRAM Allocator Underflow & Fragmentation Fallback**: Fixed an unsigned arithmetic underflow in `IOAccelMemoryAllocator2::allocPages`. Added an automatic non-contiguous allocation fallback when physical VRAM is fragmented, resolving video context initialization failures and black screens.
- **Permanent APU GFXOFF Suppression**: Completely disabled GPU power gating (GFXOFF) for APUs in `X5000::setAcceleratorProperties`, preventing engine spin-up timeouts and firmware handshake stalls during heavy graphics workloads.
- **Compositor Isolation**: Hardened dynamic scanner logic to strictly bypass `WindowServer` and `loginwindow`, eliminating unnecessary page touches and ensuring system compositor stability.

#### 3. Developer & Diagnostic Tooling
- **Offline Multi-OS Compatibility Extraction**: Enhanced `tools/collect_compat_data.py` with `--sys-root` support, enabling direct extraction of dyld shared caches, kernel collections, and driver binaries from external mounted installations without rebooting.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, VDA hardware video acceleration (AVC + HEVC), SubmitGuard, and LegacyBlend are all ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or disable specific features, use the following opt-out boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedNoVDA`** | Completely disable all hardware video acceleration | Disabled | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disabled | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. Use if HEVC video causes instability. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Disabled | Diagnostic switch. Turns off Chromium/Electron/CEF application workarounds. |
| **`-NRedNoSubmitGuard`**| Disable Metal command queue tail-filler protection | Disabled | Diagnostic switch for Metal command queue troubleshooting. |
| **`-NRedNoLegacyBlend`** | Disable Metal DualQuad LegacyBlend fix | Disabled | Diagnostic switch for UI dropdown blend workarounds. |
| **`-NRedDebug`** | Enable verbose kernel logging (`dmesg` / log stream) | Disabled | Useful for reporting issues and submitting compatibility logs. |
| **`-NRedOff`** | Completely disable all NootedRed drivers and hooks | Disabled | Recovery / vanilla baseline boot. |

---

### Important Notes & Prerequisites

1. **AMFI & Code Signing Requirements**:
   - `AMFIPass.kext` is **no longer a mandatory requirement**.
   - If you encounter a **black screen at the login screen** after boot, please set your OpenCore NVRAM `csr-active-config` to `03080000` (in `config.plist` under `NVRAM` -> `Add` -> `7C436110-AB2A-4BBB-A880-FE41995C9F82` as `Data: 03080000` or string representation `%03%08%00%00`) to permit dynamic user-space patch relocation.
2. **Fix for App Store Stalls & HEIC Preview Freezes (macOS 14+ Sonoma, Sequoia & Tahoe)**:
   - On macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe), Apple's `CoreMedia` framework attempts to use an unsupported Metal transfer session path by default, which may cause App Store UI stutter or freezes when opening/previewing HEIC photos in Finder.
   - To fix this issue, run the following two commands in **Terminal** and reboot:
     ```bash
     sudo defaults write /Library/Preferences/com.apple.coremedia allowMetalTransferSession -bool NO
     sudo chmod 644 /Library/Preferences/com.apple.coremedia.plist
     ```
3. **Prerequisites & Incompatibilities**:
   - **Lilu.kext** (>= 1.7.0) is strictly required and must load before `NootedRed.kext`.
   - **DO NOT** use `WhateverGreen.kext` or `BFixup.kext` alongside `NootedRed.kext` (they will conflict).
4. **Supported Hardware**:
   - AMD APUs with **Vega/GCN architecture** (Raven Ridge, Picasso, Renoir, Lucienne, Cezanne, Barcelo — Ryzen 2000 through 5000 / 7030 series).
   - RDNA-based APUs (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.2.2-RELEASE.zip`**: `c32b9d7ee5d79616b77124edac12c48a474a0bc228906ba9ab7b1841e12e2a8f`
