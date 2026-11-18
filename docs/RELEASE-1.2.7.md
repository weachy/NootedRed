# NootedRed v1.2.7

We are pleased to announce **NootedRed v1.2.7**, a major milestone for external display connectivity and multimedia output on AMD Vega APUs. This release significantly **strengthens HDMI and DisplayPort (including USB-C DP Alt-Mode) external display capabilities**, delivering **robust DisplayPort hotplug and monitor power-off recovery** alongside a **completely redesigned, event-driven DCN 1.0/2.1 HDMI & DisplayPort audio output pipeline** (resolving Issue #225).

---

### What's New & Resolved in v1.2.7

#### 1. Redesigned Event-Driven HDMI & DisplayPort Audio Output (Issue #225)
- **Complete DCN Audio Pipeline Configuration (`NRedAudio`)**: Previously, NootedRed only hooked the HDA controller (`AppleGFXHDA`) and never programmed the Display Core Next (DCN) display audio registers, causing external HDMI/DP audio devices to appear in macOS Sound Settings while remaining completely silent. NootedRed v1.2.7 introduces an event-driven audio manager (`NRedAudio`) that dynamically configures the full DCN audio pipeline without polling threads or hardcoded framebuffer indices.
- **Multi-Generation DCN Support (DCN 1.0 & DCN 2.1)**: Dynamically dispatches register layouts across both **DCN 1.0** (Raven / Raven2 / Picasso) and **DCN 2.1** (Renoir / Lucienne / Cezanne / Barcelo), seamlessly supporting both laptops (internal eDP/LVDS + external) and desktops / AIOs / Mini PCs (no internal panel).
- **End-to-End Azalia, DCCG, AFMT & DP_SEC Bring-Up**:
  - Derives the target Azalia endpoint from `audio-codec-info` Pin NID, programming `SPEAKER_ALLOCATION` (`HDMI_CONNECTION` vs `DP_CONNECTION`), 2-channel LPCM `AUDIO_DESCRIPTOR0`, and `AUDIO_ENABLED`.
  - Programs `DCCG_AUDIO_DTO0_MODULE` using the active external pipe's true pixel clock (`IOFBCurrentPixelClock / 100`) and binds `DTO_SEL` / `DTO0_SOURCE_SEL` to the active OTG pipe.
  - Aligns `AFMT_AUDIO_SRC_SELECT`, enables `AFMT_AUDIO_CHANNEL_ENABLE`, and sequentially activates DisplayPort Secondary Data Packet streams (`DP_SEC_CNTL`, `DP_SEC_AUD_N`, `DP_SEC_TIMESTAMP`).
- **DisplayPort Hotplug Audio Recovery**: Schedules a `+2000ms` post-link-training confirmation pass to pulse `AFMT_AUDIO_INFO_UPDATE` and `DP_SEC_CNTL` (`0 -> 1`) when `AFMT_STATUS` is not yet active, resolving silent audio after unplugging and replugging DP / USB-C DP cables.
- **`AppleGFXHDA` Fallback Fix**: Fixed a fallback typo in `AppleGFXHDA::wrapCreateAppleHDAWidget()` where the fallback path mistakenly invoked `orgCreateAppleHDAFunctionGroup` instead of `orgCreateAppleHDAWidget`.

#### 2. Robust DisplayPort Hotplug & Monitor Power-Off Recovery (PR #7)
- **Monitor Power-Off Signal Recovery (`X5000::fixedGetDisplayInfo`)**: Restricted the early-boot `fbInfo->isOnline` fallback strictly to internal embedded displays (`fbIndex == 0 && Backlight::singleton().hasEmbeddedPanel()`), and removed the unconditional geometry-based online override. Powered-off or unplugged external monitors are now accurately reported as offline, restoring reliable wake/signal recovery when the monitor is powered back on or replugged.
- **Targeted DP Receiver Power-On Delay (`X6000FB::wrapDpReceiverPowerCtrl`)**: Constrained the `250ms` `IOSleep` receiver stabilization delay strictly to power-on (`powerOn == true`) transitions, eliminating unnecessary blocking during monitor power-off, sleep, or cable unplug events.

---

### Boot Arguments & Configuration Flags

> [!IMPORTANT]
> **Out-of-the-Box Operation:**
> Starting with v1.2.0+, **GLFix, Metal fixes, OpenCL optimizations, VDA hardware video acceleration (AVC + HEVC), and HDMI/DP audio are ENABLED by default**. Most users do not need to specify any boot arguments!

If you need to diagnose or disable specific features, use the following opt-out boot arguments in your OpenCore `config.plist`:

| Boot Argument | Function | Default | Description / Diagnostic Use |
| :--- | :--- | :--- | :--- |
| **`-NRedNoVDA`** | Completely disable all hardware video acceleration | Disabled | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disabled | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. |
| **`-NRedNoHEVC10`** | Restrict HEVC hardware acceleration to 8-bit only | Disabled | Disables 10-bit HEVC (Main 10) decoding while retaining 8-bit HEVC and AVC acceleration. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Disabled | Diagnostic switch. Turns off Chromium/Electron/CEF application workarounds. |
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
     - Raven Ridge (Ryzen 2000 series): Core graphics, GLFix, and HDMI/DP audio (DCN 1.0) supported; VDA unsupported (VCN 1.0).
     - Picasso / Raven2 (Ryzen 3000 series): Core graphics, GLFix, and HDMI/DP audio (DCN 1.0) supported; VDA unsupported (VCN 1.0).
     - Renoir / Lucienne (Ryzen 4000 / 5000 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC) supported.
     - Cezanne / Barcelo (Ryzen 5000 / 7030 series): Core graphics, GLFix, Metal/OpenCL compute, HDMI/DP audio (DCN 2.1), and full VDA (AVC + HEVC) supported.
   - **RDNA-based APUs** (6000 / 7040 / 8000 series with Radeon 680M/780M) are **NOT supported**.
5. **DRM & Streaming Services**:
   - FairPlay 1.x hardware DRM (e.g. Netflix in Safari, Apple TV+) has known architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Chrome, Edge, Firefox) which leverage Widevine software DRM.

---

### Package Hashes (SHA-256)
- **`NootedRed-1.2.7-RELEASE.zip`**: `c42c445496aef0d699cadbedbd6efbaf3a857eea9e3c8ec11019d0a3486c6dbc`
