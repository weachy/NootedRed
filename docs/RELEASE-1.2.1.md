# NootedRed v1.2.1

We are pleased to announce **NootedRed v1.2.1**, a critical stability and compatibility release following v1.2.0. This release continues to **perfect multi-version dynamic adaptation** across macOS Ventura (13.x), Sonoma (14.x), Sequoia (15.x), and forward-compatible Tahoe (26.x), while addressing **kernel-level scheduler crashes**, **VRAM allocator underflows**, and **coremedia performance optimizations**.

---

### What's New & Resolved in v1.2.1

#### 1. Continued Refinement of Multi-Version Dynamic Adaptation
- **Expanded OS VCN Bridge Support**: Broadened the kernel VCN bridge load validation across Darwin 22 through Darwin 25 (macOS 13.x Ventura to macOS 26.x Tahoe), supporting future minor build revisions seamlessly.
- **Decoupled Sonoma vs. Sequoia/Tahoe VDA Signatures**: Disentangled differing instruction generation between macOS 14 (Sonoma) and macOS 15/26 (Sequoia/Tahoe) in `VDASignatures.inc`. Added dedicated Sonoma signature sets (`kVDACtx_ConvertFamily_Sonoma`, `kVDACtx_StatusStride_Sonoma`, `kVDACtx_SourcePitch_Sonoma`) to ensure 100% reliable pattern matching across disparate clang compiler optimizations.
- **Resolver Signature Hardening**: Relaxed strict stack frame register constraints (e.g. `%rbp` offsets) via wildcard masks in `matchSignature`, enabling pattern matching to withstand minor byte shifts introduced by incremental macOS security updates.
- **Offline Multi-OS Data Collection**: Upgraded `tools/collect_compat_data.py` with `--sys-root` support, allowing direct extraction of dyld shared caches, kernel collections, and driver binaries from external mounted disks without rebooting into each OS.

#### 2. Kernel Stability & GPU Scheduler Fixes
- **Elimination of SWScheduler Page Fault (`CR2: 0x48`)**: Resolved a critical null-pointer dereference inside `AMDSWScheduler::advance` that previously caused kernel panics during user login on single-GPU APUs. Single-GPU configurations no longer attempt to synchronize non-existent peer channels.
- **VRAM Allocator Underflow & Fragmentation Fallback**: Fixed an unsigned arithmetic underflow in `IOAccelMemoryAllocator2::allocPages`. Added an automatic non-contiguous allocation fallback when physical VRAM is fragmented, resolving video context initialization failures and black screens.
- **Permanent APU GFXOFF Suppression**: Completely disabled GPU power gating (GFXOFF) for APUs in `X5000::setAcceleratorProperties`, preventing engine spin-up timeouts and firmware handshake stalls during heavy graphics workloads.
- **System Compositor Isolation**: Hardened the dynamic shared-cache scanner to strictly bypass `WindowServer` and `loginwindow`, eliminating unnecessary page touches and preserving system compositor stability.

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

1. **Fix for App Store Stalls & HEIC Preview Freezes (macOS 14+ Sonoma, Sequoia & Tahoe)**:
   - On macOS 14 (Sonoma), macOS 15 (Sequoia), and macOS 26 (Tahoe), Apple's `CoreMedia` framework attempts to use an unsupported Metal transfer session path by default, which may cause App Store UI stutter or freezes when opening/previewing HEIC photos in Finder.
   - To fix this issue, run the following two commands in **Terminal** and reboot:
     ```bash
     sudo defaults write /Library/Preferences/com.apple.coremedia allowMetalTransferSession -bool NO
     sudo chmod 644 /Library/Preferences/com.apple.coremedia.plist
     ```
2. **AMFI & Code Signing Requirements (macOS 13+)**:
   - Because NootedRed dynamically resolves and patches user-space AMD graphics/media driver pages in memory, macOS AMFI (Apple Mobile File Integrity) enforces code signature checks on system processes.
   - **`AMFIPass.kext` is strongly recommended** in your OpenCore EFI (along with `-amfipassbeta` in `boot-args` on newer macOS versions like Sonoma 14.8+ / Sequoia 15.x) to avoid `WindowServer` code signature invalidation.
   - Alternatively, add `amfi=0x80` to `boot-args`.
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
- **`NootedRed-1.2.1-RELEASE.zip`**: `4d2231100484d4bd1e35fc8a4485dce48ab4ad0acbda3c73a6deab2a9589551f`

