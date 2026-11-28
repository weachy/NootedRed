# NootedRed v1.3.4

We are pleased to announce **NootedRed v1.3.4**, a memory architecture and VCN stability update following v1.3.3. This release introduces **opt-in Unified Memory Mode for Renoir-family APUs** (thanks to **[@cwr31](https://github.com/cwr31)** in PR #19) alongside **VCN2 queue draining on `stopQueue`** and **minimum hardware on-time enforcement** in `releaseHardware`.

> [!NOTE]
> **Unified Memory Mode is DISABLED by default.** To enable it, add **`-NRedUnifiedMemory`** to your `boot-args`. This feature is **recommended only for users with less than 1 GB of BIOS UMA VRAM** (e.g., 512 MB UMA).

---

### What's New & Resolved in v1.3.4

#### 1. Opt-In Unified Memory Mode for Renoir APUs (PR #19)
- **Disabled by Default (`-NRedUnifiedMemory`)**: Unified memory mode (`X5000Memory`) is gated behind `-NRedUnifiedMemory` and intended specifically for systems with **< 1 GB UMA VRAM** where native carve-out limits constrain graphics and video workloads.
- **Carve-Out Aware Working-Set & OpenGL Budgets (`X5000MemoryPolicy`)**: Computes `recommendedWorkingSetForUMA` and `glCommandBudgetKiBForUMA` based on system RAM and BIOS UMA carve-out size, preserving native budgets when the BIOS carve-out is already sufficient.
- **Pooled Memory Recycling & PTE Coalescing**: Recycles pooled unified memory, coalesces page-table writes, sizes pool buckets by total RAM and probes by carve-out, and integrates user-space budget sites cleanly via `UserPatches::applyUnifiedMemorySites`.

#### 2. VCN2 Queue Drain & Minimum Hardware On-Time
- **Safe Queue Drain on `stopQueue` (`VCNQueueDrain`)**: Dynamically extracts native queue refcounts, channel indices, and submitted timestamps in `VCNBridge` to drain in-flight VCN work before stopping queues.
- **Minimum VCN2 Hardware On-Time (`VCNBridge::releaseHardware`)**: Enforces a minimum hardware on-time (`lastHardwareInitNs`) before powering down VCN2 in `releaseHardware`, preventing rapid power-cycling glitches.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, VDA hardware video acceleration (AVC + HEVC + VP9), VRR DisplayPipe sync, and HDMI/DP audio are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or toggle specific features, use the following boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedUnifiedMemory`** | Enable Unified Memory Mode for Renoir APUs | Disabled | **Recommended only for < 1 GB UMA VRAM.** |
| **`-NRedNoVP9`** | Disable VP9 hardware decode patches | Disabled | Required if you keep SIP fully enabled. |
| **`-NRedOLED`** | Force AUX nits backlight control for OLED panels | Disabled | Use on laptops with OLED internal displays if AUX brightness is not auto-detected. |
| **`-NRedNoVRR`** | Disable `'vrrs'` VRR attribute exposure | Disabled | Diagnostic switch to opt out of VRR DisplayPipe attribute reporting. |
| **`vcnmaxctx=<N>`** | Set max concurrent tracked VCN video contexts | `6` | Tunes VCN concurrency quota (`1`–`16`). |
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
   - We recommend setting `csr-active-config` to **`03080000`** in OpenCore NVRAM (`7C436110-AB2A-4BBB-A880-FE41995C9F82` as `Data: 03080000`), which is required by the **VP9 hardware decode patch**.
   - If you prefer to keep SIP fully enabled, add **`-NRedNoVP9`** to `boot-args`.
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
     - Raven Ridge / Picasso / Raven2 (Ryzen 2000 & 3000 series): Core graphics, GLFix, HDMI/DP audio (DCN 1.0) supported; VCN 1.0 hardware decode is experimental (disabled by default).
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.3.4-RELEASE.zip`**: `72a244471467614595d3e4079ce27fba97b0a95d73f71136fd87eb06731e4791`
