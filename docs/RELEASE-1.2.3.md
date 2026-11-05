# NootedRed v1.2.3

We are pleased to announce **NootedRed v1.2.3**, a major performance and stability release following v1.2.2. This release significantly **optimizes patching performance and lowers system overhead**, resolves **intermittent boot-time black screens on laptop internal eDP displays**, adds **adaptive VCN engine teardown protection**, and officially **eliminates all requirements for `AMFIPass.kext` and disabling System Integrity Protection (SIP)**.

---

### What's New & Resolved in v1.2.3

#### 1. Zero-Overhead Dynamic Patching & Complete AMFI / SIP Independence
- **Elimination of Shared Region Hooks**: Stripped the intrusive `_shared_region_check_np` system hook and user-space memory modification routines (`vm_protect`, `copyin`, `copyout`). Process launch overhead across the entire system is drastically reduced.
- **Native Code Signing Compliance via `cs_validate_page`**: All user-space dynamic patches now apply exclusively within the kernel page fault validation stage before page hashes are evaluated. This permanently prevents `SIGKILL (Code Signature Invalid)` / `Invalid Page` crashes.
- **No AMFIPass.kext or SIP Disabling Required**: Standard System Integrity Protection (SIP) can remain fully enabled, and `AMFIPass.kext` is no longer needed.
- **Corrected Metal Allow Logic**: Fixed the conditional branch replacement for Metal dual-source blending on macOS 14 (Sonoma), 15 (Sequoia), and 26 (Tahoe), properly asserting capability (`mov al, 1` / `mov cl, 1`).
- **Compositor Isolation & Fast Exits**: WindowServer and loginwindow strictly bypass VDA media scans, and standalone driver bundles exit immediately once resolved.

#### 2. Resolution of Early-Boot Black Screens on Internal eDP Displays
- **Internal Panel Link Training Synchronization**: Fixed an early-boot race condition on laptops where fast boot environments (fast NVMe drives, `SetApfsTrimTimeout=0`, quiet boot) prompted WindowServer to query display connection status before asynchronous eDP link training completed. In `Backlight.cpp`, bounded retries (up to ~100ms) ensure connection readiness before reporting status.
- **IOFramebuffer Connector Index Correction**: Corrected `X5000::fixedGetDisplayInfo` to query connection index 0 first rather than passing the card-level `fbIndex` (which previously returned `kIOReturnBadArgument`).
- **Primary Display Online Fallback Guard**: Added an online fallback guard when CRT is active with valid display geometry, preventing CoreDisplay from logging `Setting offline display 0x00000000 main` and aborting WindowServer.

#### 3. VCN Engine Teardown Drain & Granular Codec Control
- **Adaptive VCN Teardown Drain**: Implemented a 3-stage adaptive drain (`drainVCN`) before accelerator shutdown, client stop, and `AMDRadeonX5000_AMDAccelTask::freeAllGPUMappings`. This prevents in-flight VCN write operations from triggering VM Protection Faults (NACK) when GPU page tables are unmapped on application exit.
- **Granular 10-bit HEVC Opt-Out**: Introduced the `-NRedNoHEVC10` boot argument, allowing users to disable HEVC Profile 2/3 (Main 10) hardware decoding if desired, while keeping AVC/H.264 and 8-bit HEVC fully accelerated.

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
   - **`AMFIPass.kext` is NO LONGER REQUIRED.**
   - **System Integrity Protection (SIP) NO LONGER NEEDS TO BE DISABLED.** NootedRed's page patches now comply natively with Apple code signing validation during kernel page faulting.
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
- **`NootedRed-1.2.3-RELEASE.zip`**: `784cbcbe2979a75a30181c51a3be5cff62882ff976a01ebc7e07e152ad075dd7`
