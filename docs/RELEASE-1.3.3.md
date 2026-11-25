# NootedRed v1.3.3

We are pleased to announce **NootedRed v1.3.3**, a major stability and reliability release building upon v1.3.1 and v1.3.2. This release **dramatically reduces the probability of VCN-induced `gpuRestart` (GPU hangs and WindowServer resets)** through lock-free replay command-buffer retirement, properly ordered VCN2 hardware power-cycle resets, extended self-healing cooldowns, and post-timeout wait guards—alongside **IOAcceleratorFamily2 VRR & DisplayPipe transaction synchronization** (Issue #441) and **APU reimplementation of remaining Navi controller methods** (Issues #449, #450, #451).

---

### What's New & Resolved Since v1.3.1

#### 1. Dramatically Reduced VCN-Induced `gpuRestart` & Hardware Recovery Hardening
- **Lock-Free Retirement of Skipped Replay CBs (`VCNBridge::vcnSubmitCommandBuffer`)**: Intercepts `AMDRadeonX6000_AMDHWChannel::submitCommandBuffer` to retire skipped guilty replay command buffers (`flags & 0x2`) lock-free via `AMDHWChannel` offsets (`+0x80` submitted, `+0x84` completed, `+0xC0` HW timestamp pointer). This prevents unretired hardware timestamps and avoids `AMDSWScheduler::advance()` mutex self-deadlocks that previously escalated into full `gpuRestart`.
- **Real VCN2 Power-Cycle Ordering on Reset (`VCNBridge::engineReset`)**: Scopes resets strictly to `vcnEngine`, performs a genuine `AMDVCN2HWEngine::releaseHardware` + `initHardware` power-cycle **before** re-initializing the ring (avoiding ring/PSP state corruption while VCN2 is powered down), and only advertises `*resetMaskOut` when hardware re-initialization succeeds—falling back cleanly to software decode otherwise.
- **Extended 15s Self-Healing Cooldown & Post-Timeout Wait Guard**: Extends the self-healing cooldown window from `3s` to `15s` (outliving `IOAccelEventMachine2::restart_channel()` ~5s) and arms it when skipped replay CBs are retired. Adds `vcnWaitTimeoutPending` tracking in `accelWaitForStamp` so post-timeout waits retire cleanly without wedging the accelerator into a `gpuRestart`.
- **Video Context Concurrency Quota (`vcnmaxctx`)**: Tracks active VCN video contexts (default `6`, configurable via `vcnmaxctx=<N>`) with graceful software decode fallback when exceeded, preventing VCN firmware overload under heavy concurrent media workloads.

#### 2. IOAcceleratorFamily2 VRR & DisplayPipe Transaction Sync (Issue #441)
- **Interrupt Latch Preservation (`X5000::wrapProcessTransactionInterrupt`)**: Preserves the one-shot interrupt latch (`+0x3A8`) whenever a live `AMDAccelDisplayPipe` transaction (`+0x248`) remains pending across macOS 13–26, eliminating stranded transactions and 1-second DisplayPipe watchdog timeouts (`transaction stamp index 30 time out`).
- **`AmdFbPresenter` Collision & `'vrrs'` Timestamp Guards**:
  - `X6000FB::wrapPresenterHandleTransactions` pre-waits and retries `kIOReturnNotReady` ISR collisions in `5us` increments (up to `2ms`), initializes zeroed `'vrrs'` timestamps from `mach_absolute_time()`, and keeps `maxNextFrame == minNextFrame` on fixed-refresh commits.
  - `X6000FB::wrapPresenterGetOutstandingTransactions` re-reads the post-clear stuck-queue mask so cleared queues are reported immediately.
  - `'vrrs'` support is enabled by default and can be disabled with `-NRedNoVRR`.
- **DCN 1.0 / 2.1 DRR OTG Register Patch**: Dynamically patches `dce110_timing_generator_set_drr` across macOS 13–26 to target Raven/Renoir OTG offsets (`0x1B48..0x1B4A`) instead of hardcoded Navi 10 offsets (`0x1B88..0x1B8A`).

#### 3. Navi Controller Reimplementation for APUs (Issues #449, #450, #451)
- **APU-Native `isPoweredUp` (#451)**: Checks `m_flags & 0x8` and live MMIO directly, removing Navi 10 dGPU BACOS `0x53` checks and the spurious `10ms` `0x16061` MP1 polling loop.
- **APU `setupBootWatermarks` (#450)**: Propagates UEFI-initialized `DCHUBBUB_ARB_*_WATERMARK_A` registers (`0x39C9..0x39CD`) to Sets B, C, and D without touching Navi 10/14 5-pipe HUBP offsets.
- **DRR Status & VUpdate VBlank Interval (#449)**: Enforces the APU 4-pipe limit (`otgInst < 4`), reads true DCN 1.0/2.1 OTG registers, and resolves Apple's unconditional `kIOReturnError` (`0xE0000001`) return in `getDrrStatus`.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, VDA hardware video acceleration (AVC + HEVC + VP9), VRR DisplayPipe sync, low-VRAM optimizations, and HDMI/DP audio are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or toggle specific features, use the following boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
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

1. **AMFI & SIP Status (Important for VP9 Hardware Decoding)**:
   - **`AMFIPass.kext` is NOT REQUIRED.**
   - **For VP9 Hardware Decoding**: In v1.3.0, `VP9UserPatch` uses per-process Copy-On-Write (COW) shared-cache patching. **You must disable System Integrity Protection (SIP) to use VP9 hardware decoding**; with SIP enabled, Hardened Runtime / `VideoToolbox` apps (e.g., Hackintool, UURemote, etc.) may crash with `SIGKILL (Code Signature Invalid)`.
   - **To Keep SIP Enabled**: Add **`-NRedNoVP9`** to your `boot-args` to disable VP9 hardware decoding. All other NootedRed v1.3.0+ features (AVC/HEVC Main10/8K hardware acceleration, Metal/OpenCL, GLFix, and HDMI/DP audio) comply natively with Apple code signing validation and work with SIP enabled.
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
- **`NootedRed-1.3.3-RELEASE.zip`**: `21fc9d9372dcd828cfe5b0228b3275d890a23abe349017d70112b3e739f999e2`
