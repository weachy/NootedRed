# NootedRed v1.2.4

We are pleased to announce **NootedRed v1.2.4**, an essential stability and smoothness update following v1.2.3. This release resolves the **periodic micro-stutters and freezes occurring every few seconds during video playback or inside Electron applications**, and delivers **further critical optimizations to eliminate boot-time black screens** through non-destructive shared cache patching, process-agnostic page validation, and targeted display link training.

---

### What's New & Resolved in v1.2.4

#### 1. Elimination of Periodic Stutters in Video Playback & Electron Apps
- **Removal of Blocking Teardown Drain**: In v1.2.3, an engine drain mechanism intercepted `AMDRadeonX5000_AMDAccelTask::freeAllGPUMappings` with delay and sleep loops. Because video playback engines and Electron/Chromium applications (such as Google Chrome, VS Code, Slack, Discord, and WeChat) continually allocate and free short-lived GPU tasks, these synchronous wait loops caused visible micro-freezes every few seconds.
- **Silky Smooth Playback & UI**: Completely stripped the blocking task teardown hook, restoring seamless, uninterrupted hardware-accelerated video decoding and smooth UI rendering across all applications.

#### 2. Advanced Boot Black Screen Fixes & Shared Cache Harmonization
- **Targeted eDP Link Training (FB0 Only)**: In `Backlight.cpp`, restricted the bounded polling for internal display readiness strictly to the primary internal panel (`FB0`). External display channels (FB1, FB2, etc.) and headless desktop APUs will no longer incur unnecessary polling delays, preventing WindowServer watchdog timeouts during multi-display or external monitor boot.
- **Non-Destructive AddrLib Wildcards**: Replaced destructive family ID overwriting (`0x8D` -> `0x8E`) with non-destructive wildcard instructions (`cmp reg, reg; nop...` asserting `ZF=1`). In macOS, `dyld_shared_cache` bundles Metal drivers (`X5000MTLDriver`, `AMDMTLBronzeDriver`, `X6000MTLDriver`) and video decode drivers (`AMDRadeonVADriver2`) together, statically linked to a shared `AddrLib`. The new wildcard patch allows both Metal (family `0x8D`) and VA (family `0x8E`) to pass validation simultaneously, preventing WindowServer graphics initialization failures and black screens.
- **Process-Agnostic UBC Page Validation**: Removed timing and process-name dependencies (such as gating on `WindowServer` or accelerator initialization state) from `_cs_validate_page`. Because XNU maps shared cache pages globally into memory on first fault, page patching is now completely invariant to disk I/O speed, BootCache preloading, or process startup order.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix and VDA hardware video acceleration (AVC + HEVC) are ENABLED by default**. Most users do not need to specify any boot arguments!

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
2. **Fix for App Store Stalls & HEIC Preview Freezes (macOS 14+ Sonoma, Sequoia & Tahoe)**:
   - On macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe), Apple's `CoreMedia` framework attempts to use an unsupported Metal transfer session path by default, which may cause App Store UI stutter or freezes when opening/previewing HEIC photos in Finder.
   - To fix this issue, run the following two commands in **Terminal** and reboot:
     ```bash
     sudo defaults write /Library/Preferences/com.apple.coremedia allowMetalTransferSession -bool NO
     sudo chmod 644 /Library/Preferences/com.apple.coremedia.plist
     ```
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
- **`NootedRed-1.2.4-RELEASE.zip`**: `68167fdca0558b90281fe3f521abd4d477127593d8930f4a2ea4ebccc8ec40ad`
