# NootedRed v1.3.0

We are pleased to announce **NootedRed v1.3.0**, a major multimedia and memory optimization milestone following v1.2.7. This release introduces **hardware-accelerated VP9 video decoding** across macOS 13–26, unlocks **HEVC Main10 (10-bit) hardware encoding** and **8K HEVC (Level 6.2) hardware decoding** on macOS 15+, and delivers **targeted optimizations for low-VRAM devices** (special thanks to **[@cwr31](https://github.com/cwr31)**), alongside multi-display and high-refresh HDMI/DisplayPort audio refinements.

---

### What's New & Resolved in v1.3.0

#### 1. Hardware VP9 Decoding & Expanded HEVC Media Capabilities
- **Hardware VP9 Video Decoding (`VP9UserPatch`)**: Added hardware-accelerated VP9 decoding across **macOS 13 (Ventura)**, **macOS 14 (Sonoma)**, **macOS 15 (Sequoia)**, and **macOS 26 (Tahoe)** using a modular shared-cache resolver (`UserVP9DecodeResolver` and `UserSharedCache`), enabling smooth, low-CPU 4K/HDR YouTube and WebM playback on VCN 2.x APUs.
- **HEVC Main10 Hardware Encode & 8K HEVC Decode**: On macOS 15 (Sequoia) and macOS 26 (Tahoe), `DriverInjector` and `VDA` now advertise **10-bit HEVC (Main10) hardware encoding** (`IOGVAHEVCEncode` level `"2"` / Profile 2) and **8K HEVC (Level 6.2 / `186`) hardware decoding** when the VCN 2.2 media bridge is active.
- **VCN Media Failure Fallback & Command Safety**: Added `VCNBridge::fetchResource` failure monitoring and `VDA::withdrawCapabilities()` to safely withdraw media codec advertisements if the VCN engine fails to initialize, alongside `VideoCommandPatch` to ensure clean `AMDAccelVideoContext::processCommand` error exits.

#### 2. Low-VRAM Device Optimizations & Allocator Stability (Thanks to [@cwr31](https://github.com/cwr31))
- **OpenGL Resource Budget Optimization (`GLResourceBudgetPatch`)**: Dynamically patches `AMDRadeonX5000GLDriver` resource budget limits in user space and caches the site in `UserPatches` (`NRedGLResourceBudgetApplied`), preventing premature VRAM exhaustion, texture thrashing, and UI stutters on systems configured with small UMA framebuffers (e.g., 512MB–1GB BIOS UMA).
- **`allocateNonContiguous` Overload Fix & Large-Block Allocator Refinement**: Corrected the `AMDRadeonX5000_AMDHWMemory::allocateNonContiguous` symbol signature (`_eAMD_MEMORY_ALLOCATION_TYPE` overload instead of the mismatched range overload) and refined `X5000::wrapAllocateLargeBlocks` (`NRedVRAMAllocatorRevision = 2`) with `onlyNonVisible` / `minBlockSize` guards and multi-tier `16KB`/`4KB` non-contiguous UMA fallbacks for heavily fragmented low-VRAM environments.
- **Allocator Diagnostics (`-NRedAllocTrace`)**: Added optional `-NRedAllocTrace` boot argument for tracing large-block VRAM allocation fallbacks.

#### 3. Multi-Display & High-Refresh HDMI / DisplayPort Audio Refinements
- **5 Azalia Endpoints for High-Refresh DP (`ep = 4`)**: Raised DCN 2.1 `maxAzEp` from 4 to 5, enabling audio output on `Pin NID 0x0B -> Endpoint 4` (verified on 4K@120Hz Thunderbolt / DisplayPort monitors).
- **Multi-Monitor DIG Protocol Matching**: When both HDMI and DisplayPort external displays are connected simultaneously, `NRedAudio` inspects the active DIG's `DP_SEC_CNTL` state to bind audio to the matching framebuffer.
- **Dynamic Refresh-Rate Audio DTO Sync**: Automatically reprograms `DCCG_AUDIO_DTO` whenever `IOFBCurrentPixelClock` changes (such as switching between 60 Hz and 120 Hz), keeping external audio locked and glitch-free.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, VDA hardware video acceleration (AVC + HEVC + VP9), low-VRAM optimizations, and HDMI/DP audio are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or disable specific features, use the following opt-out boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedNoVDA`** | Completely disable all hardware video acceleration | Disabled | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoVP9`** | Disable hardware VP9 decoding only | Disabled | Disables `VP9UserPatch` while retaining AVC/HEVC hardware acceleration; allows keeping SIP enabled without VideoToolbox code-signing crashes. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disabled | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. |
| **`-NRedNoHEVC10`** | Restrict HEVC hardware acceleration to 8-bit only | Disabled | Disables 10-bit HEVC (Main 10) decoding while retaining 8-bit HEVC and AVC acceleration. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Disabled | Diagnostic switch. Turns off Chromium/Electron/CEF application workarounds. |
| **`-NRedAllocTrace`** | Enable VRAM large-block allocator trace logs | Disabled | Diagnostic switch for troubleshooting low-VRAM allocation fallbacks. |
| **`-NRedDebug`** | Enable verbose kernel logging & full IORegistry telemetry | Disabled | Publishes detailed `IOFramebuffer` and DCN/Azalia/DP_SEC register snapshots for diagnostics. |
| **`-NRedOff`** | Completely disable all NootedRed drivers and hooks | Disabled | Recovery / vanilla baseline boot. |

---

### Important Notes & Prerequisites

1. **AMFI & SIP Status (Important for VP9 Hardware Decoding)**:
   - **`AMFIPass.kext` is NOT REQUIRED.**
   - **For VP9 Hardware Decoding**: In v1.3.0, `VP9UserPatch` uses per-process Copy-On-Write (COW) shared-cache patching. **You must disable System Integrity Protection (SIP) to use VP9 hardware decoding**; with SIP enabled, Hardened Runtime / `VideoToolbox` apps (e.g., Hackintool, UURemote, etc.) may crash with `SIGKILL (Code Signature Invalid)`.
   - **To Keep SIP Enabled**: Add **`-NRedNoVP9`** to your `boot-args` to disable VP9 hardware decoding. All other NootedRed v1.3.0 features (AVC/HEVC Main10/8K hardware acceleration, Metal/OpenCL, GLFix, and HDMI/DP audio) comply natively with Apple code signing validation and work with SIP enabled.
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
     - Raven Ridge (Ryzen 2000 series): Core graphics, GLFix, and HDMI/DP audio (DCN 1.0) supported; VDA/VP9 unsupported (VCN 1.0).
     - Picasso / Raven2 (Ryzen 3000 series): Core graphics, GLFix, and HDMI/DP audio (DCN 1.0) supported; VDA/VP9 unsupported (VCN 1.0).
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC Main/Main10/8K + VP9) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.3.0-RELEASE.zip`**: `c264a14bdce39757707a78c7bb0f1bdf7f50faab78a3d3eb7d235d5795a2963d`
