# HDMI / DisplayPort External Audio Silence & Hotplug Recovery

- **Upstream Issue**: [ChefKissInc/NootedRed#225](https://github.com/ChefKissInc/NootedRed/issues/225) (`open` — *HDMI/DisplayPort usually has no audio playback despite being visible in settings*)
  - Related historical issues: [ChefKissInc/NootedRed#300](https://github.com/ChefKissInc/NootedRed/issues/300), [ChefKissInc/NootedRed#370](https://github.com/ChefKissInc/NootedRed/issues/370)
- **Resolved In**: `v1.2.7`
- **Relevant Source Files**:
  - [NRedAudio.hpp](../NootedRed/NRedAudio.hpp) / [NRedAudio.cpp](../NootedRed/NRedAudio.cpp)
  - [AppleGFXHDA.cpp](../NootedRed/AppleGFXHDA.cpp)
  - [X6000FB.hpp](../NootedRed/X6000FB.hpp) / [X6000FB.cpp](../NootedRed/X6000FB.cpp)
  - [NRed.hpp](../NootedRed/NRed.hpp) / [NRed.cpp](../NootedRed/NRed.cpp)

---

## 1. Problem Description

On AMD Vega APUs (Raven / Picasso / Renoir / Cezanne, etc.) running macOS, external HDMI and DisplayPort (including USB-C DP Alt-Mode) audio output devices appear in **System Settings -> Sound** and `AppleGFXHDAEngineOutputDP` enters active playback (`IOAudioEngineState = 1`), yet the external display produces **no audio**. On some systems, cold boot works while **unplugging and replugging DP** leaves audio permanently silent.

---

## 2. Root Cause & Upstream Gaps

1. **Upstream `NootedRed` only hooks the HDA controller side and never configures the DCN display audio pipeline**:
   - [AppleGFXHDA.cpp](../NootedRed/AppleGFXHDA.cpp) enables `AppleGFXHDA` to drive the AMD Azalia HDA controller and feed DMA, so macOS sees a healthy audio device.
   - However, `AMDRadeonX6000Framebuffer` leaves key DCN (Display Core Next) audio registers unconfigured or mismatched on APUs:
     - **Missing / stale DCCG Audio DTO**: `DCCG_AUDIO_DTO0_MODULE` is not programmed with the active external pipe's true pixel clock (`IOFBCurrentPixelClock / 100`), or `DTO_SEL` / `DTO0_SOURCE_SEL` is not bound to the external OTG pipe.
     - **Inactive or mismatched Azalia Endpoint**: The `audio-codec-info` Pin NID reported to `AppleGFXHDA` (e.g. `0x09` -> Endpoint 3) often differs from the endpoint Display Core initialized (commonly Endpoint 0). As a result, `AppleGFXHDA` streams PCM to Endpoint 3 while `AZ3` has `HOT_PLUG_CONTROL.AUDIO_ENABLED = 0` and `SPEAKER_ALLOCATION = 0`, and `DIGx_AFMT_AUDIO_SRC_CONTROL` still listens to idle Endpoint 0.
     - **Unenabled AFMT / DP SDP Packet Gates**: DisplayPort requires `AFMT_AUDIO_PACKET_CONTROL2` (`AFMT_AUDIO_CHANNEL_ENABLE`), `AFMT_INFOFRAME_CONTROL0` (`AFMT_AUDIO_INFO_UPDATE`), and the Stream Encoder's **`DP_SEC_CNTL` (`DP_SEC_STREAM_ENABLE | ASP | ATP | AIP`)**, **`DP_SEC_AUD_N`**, and **`DP_SEC_TIMESTAMP`**. Without `DP_SEC`, DP audio remains silent even when AFMT is active.
2. **Fallback typo in upstream `AppleGFXHDA.cpp`**:
   - In `AppleGFXHDA::wrapCreateAppleHDAWidget()` (line 162), the fallback path mistakenly called `orgCreateAppleHDAFunctionGroup` instead of `orgCreateAppleHDAWidget`.
3. **DP hotplug silence timing**:
   - After replugging DP, link training and mode-set finish after the initial `reportCapabilities_LinkInfo` callback. Without a post-link-training confirmation pass that pulses `AFMT_AUDIO_INFO_UPDATE` and `DP_SEC_CNTL` (`0 -> 1`), the sink fails to latch the Audio InfoFrame and secondary data packets.

---

## 3. Final Solution

1. **Event-Driven Dynamic Topology Resolution (No Polling Thread, No Hardcoded Indices)**:
   - Hooked `X6000FB::wrapControllerPowerUp` and optional `X6000FB::wrapReportCapabilitiesLinkInfo` (graceful fallback with `clearError()` if unavailable).
   - Dynamically enumerates `IOFramebuffer` instances, distinguishes internal panels (`connector-type == 2`) from external HDMI (`2048`) / DP (`1024`) outputs, handles both **laptops (internal eDP + external)** and **desktops / AIOs / Mini PCs (no internal panel)**, derives the target Azalia endpoint from `audio-codec-info`, and reads the true `IOFBCurrentPixelClock`, active `DIG`, and `OTG`.
2. **Multi-Generation DCN Register Dispatch (`DcnIpBlock`)**:
   - Dispatches register offsets from `DeviceProfile::current()->family` across **DCN 1.0** (Raven / Raven2 / Picasso, `OTG_MASTER_EN = 0x1B5F`) and **DCN 2.1** (Renoir / Cezanne / Barcelo, `OTG_MASTER_EN = 0x1B5C`), sharing `DCN_BASE_2` (`0x34C0`) and `DCCG` (`0x00C0`).
3. **Dual-Path Audio Bring-Up & Hotplug Confirmation**:
   - Configures the target Azalia endpoint's `SPEAKER_ALLOCATION` (`HDMI_CONNECTION` vs `DP_CONNECTION`), 2ch LPCM `AUDIO_DESCRIPTOR0`, and `AUDIO_ENABLED`;
   - Programs `DCCG_AUDIO_DTO`, aligns `AFMT_AUDIO_SRC_SELECT` and `AFMT_AUDIO_CHANNEL_ENABLE`, and enables `DP_SEC_CNTL` sequentially for DisplayPort;
   - Schedules a `+2000ms` post-link-training confirmation pass (`postLinkPassDone`) to pulse `AFMT_AUDIO_INFO_UPDATE` and `DP_SEC_CNTL` when `AFMT_STATUS` is not yet active, resolving DP hotplug silence.
4. **`AppleGFXHDA.cpp` Typo Fix**:
   - Fixed `wrapCreateAppleHDAWidget` fallback to call `orgCreateAppleHDAWidget`.
5. **Minimal IORegistry Telemetry**:
   - Publishes 4 summary properties by default: `NRedAudioDcnGen`, `NRedAudioCfgDIG`, `NRedAudioCfgOTG`, and `NRedAudioCfgEP`;
   - Publishes full `IOFramebuffer` and `DCCG`/`OTG`/`DIG`/`AZ`/`DP_SEC` register snapshots only when `-NRedDebug` is present in `boot-args`.

---

## 4. Hardware Verification

- **DCN 2.1 Laptop** (AMD Renoir 7730U + internal eDP, macOS 15): Both external HDMI and USB-C DP audio work cleanly, including repeated HDMI/DP hotplugging with either auto-switched or manually switched audio output.
- **DCN 1.0 All-in-One** (HP All-in-One Raven2/Picasso Ryzen 3 3250U, no internal panel): Verified by PR #11's author on `v1.2.7` test build—HDMI audio works out of the box.
