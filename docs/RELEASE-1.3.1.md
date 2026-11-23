# NootedRed v1.3.1

We are pleased to announce **NootedRed v1.3.1**, a feature and stability release following v1.3.0. This release extends **hardware video decoding to VCN 1.0 APUs (Ryzen 2000 & 3000 series)**, unlocks **HEVC Main10 hardware encoding on macOS 13 (Ventura) and macOS 14 (Sonoma)**, introduces **OLED panel support with wake display recovery**, resolves **`dyld_shared_cache` hash mismatches**, and accelerates **HDMI/DisplayPort audio convergence**.

---

### What's New & Resolved in v1.3.1

#### 1. VCN 1.0 Hardware Decode & Expanded Ventura/Sonoma HEVC Encode
- **VCN 1.0 Hardware Video Decode (`Raven / Raven2 / Picasso`)**: Migrated VCN 1.0 decode patches to cross-version dynamic signatures and unified VCN 1.0 decode boot arguments across Raven, Picasso, and Raven2 APUs, enabling hardware-accelerated video decoding on Ryzen 2000 and 3000 series APUs.
- **HEVC Main10 Encode on Ventura & Sonoma**: Enabled 10-bit HEVC (Main10) hardware encoding and added slice delta signatures in `VDASignatures.inc` for **macOS 13 (Ventura)** and **macOS 14 (Sonoma)**, bringing Main10 encode parity with Sequoia and Tahoe.
- **`dyld_shared_cache` Hash Mismatch Fix**: Resolved shared cache hash mismatch issues during dynamic user-space page patching in `UserPatches` and `VCNBridge`.

#### 2. OLED Display Support & Wake Controller Recovery
- **OLED Backlight Control & Minimum Brightness Guard (`-NRedOLED`)**: Added the `-NRedOLED` boot argument to enable AUX nits backlight control, added fallback OLED capability detection via `dcLinkCapsField`, and clamped the minimum AUX brightness level to `2%` to prevent self-emissive OLED panels from shutting off completely at the lowest brightness slider position.
- **Wake Display Recovery (`X6000FB::wrapDoPowerStateChange`)**: Hooked `doPowerStateChange` with thread-safe `fbPowerLock` state tracking. When a framebuffer transitions back to `fbPowerOn` while the cached display controller is genuinely powered down, NootedRed automatically re-invokes `wrapControllerPowerUp` to restore display output on wake, and sets OLED hardware attach flags (`0x0C`).

#### 3. HDMI / DisplayPort Audio & Tooling Improvements
- **Active Panel & DTO Mismatch Detection**: Improved `NRedAudio` to verify active internal panel status and detect `DCCG_AUDIO_DTO` source/clock mismatches on external DP/HDMI outputs.
- **Faster Audio Convergence**: Reduced the initial delayed audio probe from `2000ms` to `500ms` (with retry schedule `{500, 1000, 1000, 1500, 1500}`), speeding up HDMI/DP audio initialization after controller power-up, hotplug, or wake.
- **Compatibility Tooling Guards**: Updated `tools/collect_compat_data.py`, `gen_vda_gva_data.py`, and `gen_vda_va_data.py` to check `NootedRed.kext` load status and `-NRedOff`/`-liluoff` flags before issuing live cache warnings.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, VDA hardware video acceleration (AVC + HEVC + VP9), low-VRAM optimizations, and HDMI/DP audio are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or toggle specific features, use the following boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedOLED`** | Force AUX nits backlight control for OLED panels | Disabled | Use on laptops with OLED internal displays if AUX brightness is not auto-detected. |
| **`-NRedNoVDA`** | Completely disable all hardware video acceleration | Disabled | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disabled | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. |
| **`-NRedNoHEVC10`** | Restrict HEVC hardware acceleration to 8-bit only | Disabled | Disables 10-bit HEVC (Main 10) decoding while retaining 8-bit HEVC and AVC acceleration. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Disabled | Diagnostic switch. Turns off Chromium/Electron/CEF application workarounds. |
| **`-NRedAllocTrace`** | Enable VRAM large-block allocator trace logs | Disabled | Diagnostic switch for troubleshooting low-VRAM allocation fallbacks. |
| **`-NRedDebug`** | Enable verbose kernel logging & full IORegistry telemetry | Disabled | Publishes detailed `IOFramebuffer` and DCN/Azalia/DP_SEC register snapshots for diagnostics. |
| **`-NRedOff`** | Completely disable all NootedRed drivers and hooks | Disabled | Recovery / vanilla baseline boot. |

---

### Important Notes & Prerequisites

1. **AMFI & SIP Status**:
   - **`AMFIPass.kext` is NOT REQUIRED.**
   - **System Integrity Protection (SIP) DOES NOT NEED TO BE DISABLED.** NootedRed's page patches comply natively with Apple code signing validation during kernel page faulting.
2. **Native Metal Compute Acceleration**:
   - Starting with v1.2.5+, native Metal Compute acceleration is fully functional on macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe).
   - If you previously applied the legacy `allowMetalTransferSession -bool NO` workaround in CoreMedia, you can safely delete it:
     ```bash
     sudo defaults delete /Library/Preferences/com.apple.coremedia allowMetalTransferSession
     ```
3. **Prerequisites & Load Order**:
   - **Lilu.kext** (>= 1.7.0) is strictly required and must load **before** `NootedRed.kext`.
   - **Avoid loading `AppleALC.kext` before `NootedRed.kext`**; load `AppleALC.kext` after `NootedRed.kext` to avoid kext load order conflicts.
   - **DO NOT** use `WhateverGreen.kext` or `BFixup.kext` alongside `NootedRed.kext` (they will conflict).
4. **Supported Hardware**:
   - **AMD APUs with Vega / GCN 5 Architecture**:
     - Raven Ridge / Picasso / Raven2 (Ryzen 2000 & 3000 series): Core graphics, GLFix, HDMI/DP audio (DCN 1.0), and VCN 1.0 hardware video decode supported.
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.3.1-RELEASE.zip`**: `29ef3dffefd714372879d8f6ec7aeab82522f6b826fd59dbe2065c024d2b0b5a`
