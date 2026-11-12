# NootedRed v1.2.5

We are pleased to announce **NootedRed v1.2.5**, a major milestone for AMD APU graphics and compute acceleration on macOS. This release officially introduces **native Metal Compute acceleration** and **permanently resolves Issue #235** by restoring the true 1 SE / CU hardware topology within `dyld_shared_cache` for AMD Vega APUs. Users no longer need any workaround to disable Metal transfer sessions and can now enjoy the full, buttery-smooth Metal Compute acceleration pipeline across macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe).

---

### What's New & Resolved in v1.2.5

#### 1. Native Metal Compute Acceleration & Elimination of Issue #235
- **Restoration of True Hardware Shader Engine (1 SE) Topology**: In previous versions, `AMDRadeonX5000MTLDriver` inside Apple's `dyld_shared_cache` misidentified integrated Vega APUs with `numShaderEngines = 4` (a legacy dGPU Vega 10/20 topology), conflicting with the physical single Shader Engine (1 SE) architecture of APUs. When modern Apple frameworks (VideoToolbox, QuickLook, CoreMedia) dispatched compute wavefronts via Metal Transfer Session (`VTMTSComputeFunction`), the MEC (Micro Engine Compute) suffered an unrecoverable hardware wavefront deadlock, stalling fences and causing SDMA0 to freeze waiting for timestamps.
- **Precision dyld Shared Cache Patching**: NootedRed now dynamically intercepts `gfx9_HwInfoInitData` across all 3 Vega ASIC revisions within the `dyld_shared_cache` page validation pipeline, correcting `numShaderEngines` from 4 to 1 (`NRedMTLHwInfoApplied = 3`).
- **Authentic Compute Unit (CU) Calculation Preserved**: Total compute units in GFX9 drivers are natively calculated by multiplying `cuPerShaderArray * numShaderEngines * numShaderArraysPerEngine`. By correcting the SE count to 1 at the source, native 32-bit hardware multiplication automatically computes and reports the genuine hardware compute unit count (e.g., 8 CUs for Vega 8, 7 CUs for Vega 7) without requiring complex assembly rewrites or bloated code relocations.
- **Flawless Multi-Media & System UI Experience**: Completely eliminates system-wide freezes, display lockups, and kernel watchdog crashes when viewing high-resolution HEIC images in QuickLook, browsing rich-media pages in the App Store, applying dynamic aerial wallpapers, or running applications that leverage Metal Compute shaders.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, and VDA hardware video acceleration (AVC + HEVC) are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or disable specific features, use the following opt-out boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedNoVDA`** | Completely disable all hardware video acceleration | Disabled | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disabled | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. |
| **`-NRedNoHEVC10`** | Restrict HEVC hardware acceleration to 8-bit only | Disabled | Disables 10-bit HEVC (Main 10) decoding while retaining 8-bit HEVC and AVC acceleration. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Disabled | Diagnostic switch. Turns off Chromium/Electron/CEF application workarounds. |
| **`-NRedDebug`** | Enable verbose kernel logging (`dmesg` / log stream) | Disabled | Useful for reporting issues and submitting compatibility logs. |
| **`-NRedOff`** | Completely disable all NootedRed drivers and hooks | Disabled | Recovery / vanilla baseline boot. |

---

### Important Notes & Prerequisites

1. **AMFI & SIP Status**:
   - **`AMFIPass.kext` is NOT REQUIRED.**
   - **System Integrity Protection (SIP) DOES NOT NEED TO BE DISABLED.** NootedRed's page patches comply natively with Apple code signing validation during kernel page faulting.
2. **Re-enabling Full Metal Compute Acceleration (macOS 14+ Sonoma, Sequoia & Tahoe)**:
   - In previous releases, users were advised to disable Metal transfer sessions (`allowMetalTransferSession -bool NO`) to avoid system freezes.
   - **With v1.2.5, this workaround is NO LONGER NEEDED!**
   - If you previously applied this workaround, please run the following command in **Terminal** and reboot:
     ```bash
     sudo defaults delete /Library/Preferences/com.apple.coremedia allowMetalTransferSession
     ```
     This will delete the override and allow macOS to utilize the native, hardware-accelerated Metal Compute pipeline for buttery-smooth performance.
3. **Prerequisites & Load Order**:
   - **Lilu.kext** (>= 1.7.0) is strictly required and must load **before** `NootedRed.kext`.
   - **Avoid loading `AppleALC.kext` before `NootedRed.kext`**; load `AppleALC.kext` after `NootedRed.kext` to avoid kext load order conflicts.
   - **DO NOT** use `WhateverGreen.kext` or `BFixup.kext` alongside `NootedRed.kext` (they will conflict).
4. **Supported Hardware**:
   - **AMD APUs with Vega / GCN 5 Architecture**:
     - Raven Ridge (Ryzen 2000 series): Core graphics & GLFix supported; VDA unsupported (VCN 1.0).
     - Picasso / Raven2 (Ryzen 3000 series): Core graphics & GLFix supported; VDA unsupported (VCN 1.0).
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, and full VDA (AVC + HEVC) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, and full VDA (AVC + HEVC) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.2.5-RELEASE.zip`**: `7a664e8e74f4fbd8dc0bc1df02a122fedbf2b3ad8205a2ad4f99b9fe564fa1db`
