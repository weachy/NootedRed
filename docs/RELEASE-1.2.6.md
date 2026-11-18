# NootedRed v1.2.6

We are pleased to announce **NootedRed v1.2.6**, a significant compute reliability and memory resilience update following v1.2.5. This release introduces **intelligent OpenCL kernel workgroup optimization and scratch-aware redirection** to prevent SPI scratch wave hangs and enhance compute unit occupancy on single-SE APUs, alongside an **instantaneous UBC page eviction cache** that ensures patch permanence under heavy system memory pressure.

---

### What's New & Resolved in v1.2.6

#### 1. OpenCL Kernel Workgroup Optimization & Scratch Wave Hang Prevention
- **Intelligent OpenCL Kernel Interception**: In `AMDRadeonX5000GLDriver`, NootedRed now dynamically intercepts `_gldExecuteKernel` across macOS 13 (Ventura), macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe).
- **Workgroup Geometry Clamping for Auto-Calculated Dispatches**: When a kernel is dispatched with `local_work_size == NULL` (auto-calculator), the dispatch geometry is intelligently clamped to a maximum of 64 threads per workgroup (1 wavefront/workgroup). This prevents SPI (Shader Processor Input) scratch allocation stalls, eliminates hardware wave hangs, and substantially improves CU (Compute Unit) occupancy on single-SE Renoir and Cezanne APUs.
- **Selective Scratch-Aware Redirection**: When explicit workgroup dimensions are provided (`local_work_size != NULL`) with workgroup width exceeding 64, NootedRed inspects the kernel's private memory requirements (`private_mem_size > 0`). Kernels requiring scratch memory are safely redirected to the auto-calculator to avoid scratch space overruns, while zero-scratch kernels (such as video background blur, CoreImage filters, and compositing shaders) retain their native explicit workgroup geometry for maximum throughput.

#### 2. UBC Page Eviction Cache & Long-Horizon Patch Resilience
- **Persistent Patch Site Caching (`CachedPatchSite`)**: Under high memory pressure, the macOS XNU kernel may evict clean, file-backed shared-cache and driver pages to free physical RAM. Previously, subsequent page re-faults from disk could load unpatched pages if signature counters reached their caps.
- **O(1) Instantaneous Re-Application**: NootedRed now records all successfully matched patch locations in a compact, thread-safe cache table protected by `IOSimpleLock`. Any evicted page that re-faults into memory is immediately and safely re-patched in \(O(1)\) time without redundant pattern searches, guaranteeing absolute driver stability throughout long-running uptime and memory-intensive workflows.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, and VDA hardware video acceleration (AVC + HEVC) are ENABLED by default**. Most users do not need to specify any boot arguments!

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
     - Raven Ridge (Ryzen 2000 series): Core graphics & GLFix supported; VDA unsupported (VCN 1.0).
     - Picasso / Raven2 (Ryzen 3000 series): Core graphics & GLFix supported; VDA unsupported (VCN 1.0).
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, Metal/OpenCL compute, and full VDA (AVC + HEVC) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, Metal/OpenCL compute, and full VDA (AVC + HEVC) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.2.6-RELEASE.zip`**: `f0d19122eb8fbb7c503212435e404b7936ec1fb61f65cbc066f8902276e0f2a5`
