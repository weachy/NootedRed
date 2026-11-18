# AMD APU External Display Audio (HDMI / DisplayPort) Implementation Plan (Completed)

> **Status**: Core implementation completed and verified in `v1.2.7` (see [solutions/issue-225-hdmi-dp-audio.md](../solutions/issue-225-hdmi-dp-audio.md)). This document has been updated based on live hardware debugging and disassembly verification to correct early-stage register offsets, protocol assumptions, and hook selection.

## 1. Executive Summary & Problem Statement

### 1.1 Review of Earlier Tester Patch (`hdmi-audio-patch`)
During initial testing on AMD Renoir (DCN 2.1) APUs, an external patch using brute-force register polling (`hdmi-audio-patch`) demonstrated the root hardware gap: Apple's `AMDRadeonX6000` driver leaves DCN audio clocks (DCCG DTO), Azalia endpoint controls, and AFMT/DP packet gates unconfigured on APUs. However, that prototype had critical flaws:
1. **Hardcoded DIG index**: Hardcoded `kHdmiDigIndex = 3`, matching only a single laptop's board routing;
2. **Destructive to DisplayPort**: Forced `HDMI_CONNECTION = 1` and cleared `DP_CONNECTION = 0`, breaking DisplayPort audio;
3. **Broken Clock Selection**: Picked the global maximum pixel clock across all displays, causing pitch distortion when an external 1080p display was paired with a high-refresh internal panel (e.g., 2.8K@120Hz);
4. **Hardcoded DCN 2.1 Offsets**: Unsafe on Raven/Picasso (DCN 1.0);
5. **Kernel Thread Polling Loop**: Used `kernel_thread_start` with an infinite `for (;;) { IOSleep(2000); }` loop, preventing CPU deep sleep (C8/C10) and risking PCIe bus errors / panics on sleep/wake.

### 1.2 Observed Symptoms & Protocol-Level Root Causes
On unpatched NootedRed, external HDMI/DP audio devices appear in macOS Sound settings, yet produce **no audio** (or HDMI works via VBIOS pre-init while DisplayPort / Type-C DP Alt-Mode is completely silent, or DP fails permanently after hotplug).

Three hardware/driver gaps cause this:
1. **HDMI Audio Path (AFMT)**:
   HDMI audio relies on **AFMT (Audio Formatter)** to generate HDMI Audio InfoFrames and sample packets. On some boards, UEFI/VBIOS partially initializes HDMI AFMT at boot, whereas on others (or after mode-set/hotplug), AFMT, DCCG DTO, or Azalia remain unconfigured.
2. **DisplayPort Audio Path (AFMT + Secondary Data Packets - SDP)**:
   ~~An early assumption was that DP does not use AFMT~~; live DCN 2.1 debugging proved that **DisplayPort also depends on AFMT** for audio source routing, channel enablement, and InfoFrame updates (`AFMT_AUDIO_SRC_CONTROL`, `AFMT_AUDIO_PACKET_CONTROL2`, `AFMT_INFOFRAME_CONTROL0`, `AFMT_CNTL`, `AFMT_AUDIO_PACKET_CONTROL`), followed by the **Stream Encoder's `DP_SEC_*` (Secondary Data Packets)** to packetize audio samples and timestamps into the DP link. Missing either AFMT or `DP_SEC_*` leaves DP silent.
3. **Azalia Endpoint & `audio-codec-info` Mismatch**:
   The `audio-codec-info` Pin NID reported by `IOFramebuffer` to `AppleGFXHDA` (e.g., `0x09` -> Endpoint 3) often differs from the Endpoint Display Core initialized (commonly Endpoint 0), causing `AppleGFXHDA` to stream audio to an unconfigured endpoint.

---

## 2. Core Architecture & Three Design Principles

```
+-----------------------------------------------------------------------------------+
|                         NootedRed Unified Audio Architecture                      |
+-----------------------------------------------------------------------------------+
                                         │
                 ┌───────────────────────┴───────────────────────┐
                 ▼                                               ▼
    [Principle 1: Dynamic Topology]                [Principle 2: Event-Driven Hooks]
   - Enumerate active IOFramebuffer               - Power-up / wake (ControllerPowerUp)
   - Extract PixelClock & protocol type           - Link/hotplug (ReportCapabilitiesLinkInfo)
   - Match internal vs external DIG/OTG/EP        - Bounded retries + post-link DP pulse
                 │                                               │
                 └───────────────────────┬───────────────────────┘
                                         ▼
                        [Principle 3: Dual-Path Hardware Engine]
                                         │
                 ┌───────────────────────┴───────────────────────┐
                 ▼                                               ▼
            [HDMI Path]                                   [DisplayPort Path]
   - Azalia: HDMI_CONNECTION = 1                   - Azalia: DP_CONNECTION = 1
   - DCCG: Audio DTO (Pipe PixClk)                 - DCCG: Audio DTO (Pipe PixClk)
   - AFMT: Clock, Src, Channel & InfoFrame         - AFMT + Stream Encoder: DP_SEC_* SDP
```

### Principle 1: Dynamic Topology & Pipeline Resolution (No Hardcoding)
- **No hardcoded DIG/OTG/Endpoint**: Dynamically scans `IOFramebuffer` instances, `DIG_BE_CNTL`, and `OTG_MASTER_EN`, and parses `audio-codec-info` (`b[1] == 0x01 && b[2] >= 3` -> `ep = (b[2] - 3) / 2`).
- **1:1 Pixel Clock Binding**: Reads `IOFBCurrentPixelClock` directly from the target external `IOFramebuffer` to compute `DCCG_AUDIO_DTO0_MODULE`, preventing pitch drift in multi-monitor setups.
- **Full Topology Coverage**:
  - **APU Laptops**: Distinguishes internal panels (`connector-type == 2`, `kConnectorLVDS` including eDP) from external HDMI (`2048`) / DP (`1024`) outputs, skipping `DIG0`/`OTG0` only when an internal panel is active;
  - **Clamshell Mode**: External display takes over properly when the internal panel is inactive;
  - **Mini PCs / Desktops / AIOs**: Without an internal panel, enumerates starting from `DIG0`/`OTG0`.

### Principle 2: Event-Driven Lifecycle Hooks (No Polling Thread)
- **No persistent kernel thread**: Completely avoids `kernel_thread_start` and infinite `IOSleep` loops.
- **Aligned with `AMDRadeonX6000Framebuffer` lifecycle**:
  1. **Power-up / Wake**: Hooks `X6000FB::wrapControllerPowerUp`;
  2. **Link capabilities / Hotplug**: Hooks optional `X6000FB::wrapReportCapabilitiesLinkInfo` (gracefully cleared with `clearError()` if absent);
  3. **Bounded Delayed Retries + Post-Link Confirmation Pulse**: Uses `thread_call_enter_delayed` (`2000ms` initial delay, `{1000, 1000, 1500, 1500}ms` retry schedule while waiting for link settlement), followed by a `+2000ms` confirmation pass (`postLinkPassDone`) that pulses `AFMT_INFOFRAME_CONTROL0` and re-latches `DP_SEC_CNTL` (`0 -> 1`) if `AFMT_STATUS` is not yet active—eliminating permanent DP hotplug silence.

### Principle 3: Dual-Track Protocol Support (HDMI & DisplayPort)
- **HDMI Branch**: Configures Azalia `HDMI_CONNECTION = 1`, `DCCG_AUDIO_DTO`, and `AFMT` (`AFMT_AUDIO_SRC_CONTROL`, `AFMT_AUDIO_PACKET_CONTROL2`, `AFMT_CNTL`, `AFMT_AUDIO_PACKET_CONTROL`, `AFMT_INFOFRAME_CONTROL0`).
- **DP Branch**:
  - Configures Azalia `DP_CONNECTION = 1`;
  - Aligns `AFMT_AUDIO_SRC_CONTROL`, `AFMT_AUDIO_PACKET_CONTROL2` (`0x300` for FL|FR), `AFMT_INFOFRAME_CONTROL0`, etc.;
  - Programs `DP_SEC_AUD_N` (`0x8000`), `DP_SEC_TIMESTAMP` (bit 0), and enables `DP_SEC_CNTL` sequentially (`ASP -> ATP|AIP -> STREAM_ENABLE`).
  - *Note: `Personalities/com.apple.driver.AppleGFXHDA.xml` and `codecAddressMask` required no changes in practice.*

---

## 3. Hardware Register & Multi-Gen Specification

### 3.1 Verified DCN Register Base & Offset Table
Verified via `NRedAudio::resolveDcnIpBlock()` and hardware testing, **DCN 1.0** and **DCN 2.1** share the same `DCCG` and `DCN_BASE_2` bases, differing primarily in `OTG_MASTER_EN` and `maxDig` (DCN 3.x / RDNA2+ APUs are outside NootedRed's supported hardware scope):

| Module / Register | Symbol | DCN 1.0 (Raven / Raven2 / Picasso) | DCN 2.1 (Renoir / Cezanne / Barcelo) | Description |
| :--- | :--- | :--- | :--- | :--- |
| **DCCG Base** | `kDcn21DccgBase` | `0x00C0` | `0x00C0` | Display clock generator (`DTO_SOURCE`/`DTO0_PHASE`/`DTO0_MODULE`) |
| **DCN Top Base** | `DCN_BASE_2` | `0x34C0` | `0x34C0` | Pipe control, OTG, Azalia endpoints, AFMT, and `DP_SEC` |
| **OTG Master En** | `OTG_MASTER_EN` | `DCN_BASE_2 + 0x1B5F` | `DCN_BASE_2 + 0x1B5C` | OTG master enable (key cross-gen difference, stride `0x80`) |
| **AFMT / DIG** | `AFMT_*` / `DIG_BE` | `DCN_BASE_2 + 0x207C..0x20E6` (`maxDig=4`) | `DCN_BASE_2 + 0x207C..0x20E6` (`maxDig=5`) | AFMT source, channel mask, InfoFrame & packet control (stride `0x100`) |
| **DP SEC** | `DP_SEC_*` | `DCN_BASE_2 + 0x212B..0x2135` | `DCN_BASE_2 + 0x212B..0x2135` | DP audio SDP control (`DP_SEC_CNTL`/`AUD_N`/`TIMESTAMP`, stride `0x100`) |

> **Implementation Rule**: Encapsulated in `NRedAudio::DcnIpBlock` and dispatched dynamically from `DeviceProfile::current()->family`.

### 3.2 DCCG Audio DTO Configuration
Azalia requires a stable 24 MHz reference derived from the target pipe's pixel clock:

$$\text{Audio\_DTO\_Phase} = 240000 \quad (24\text{ MHz in } 100\text{ Hz units})$$
$$\text{Audio\_DTO\_Module} = \frac{\text{PixelClock (Hz)}}{100} \quad (\text{Target Pipe Pixel Clock in } 100\text{ Hz units})$$

1. Read `DCCG_AUDIO_DTO_SOURCE`, set `DTO0_SOURCE_SEL` to the active external OTG index, `DTO_SEL = 0`, `DTO0_USE_512FBR = 0`;
2. Write `DCCG_AUDIO_DTO0_MODULE` = `pixelClockHz / 100U`;
3. Write `DCCG_AUDIO_DTO0_PHASE` = `240000`.

### 3.3 Azalia Endpoint Configuration
Each endpoint (`AZF0ENDPOINT0` – `AZF0ENDPOINT3`, base `DCN_BASE_2 + 0x0386 + ep * 6`) is configured via indirect index/data registers:
- **Clock Gating & Audio Enable (`HOT_PLUG_CONTROL`, reg `0x54`)**: Set `CLOCK_GATING_DISABLE` (bit 0) during updates, set `AUDIO_ENABLED` (bit 31), then clear `CLOCK_GATING_DISABLE`;
- **Protocol & Speaker Allocation (`SPEAKER_ALLOCATION`, reg `0x25`)**:
  - **HDMI**: `HDMI_CONNECTION = 1` (`0x10000`), `DP_CONNECTION = 0`;
  - **DisplayPort**: `DP_CONNECTION = 1` (`0x20000`), `HDMI_CONNECTION = 0`;
  - `SPEAKER_ALLOCATION` & `CHANNEL_ALLOCATION`: `FL | FR` (`0x3`).
- **Audio Descriptor (`AUDIO_DESCRIPTOR0`..`3`, reg `0x28`+)**:
  - Programs safe 2ch LPCM descriptor (`32/44.1/48 kHz`, `16/20/24-bit`) into `AUDIO_DESCRIPTOR0` and clears `1..3`.

### 3.4 HDMI / Shared Track: AFMT Register Sequence
1. **`AFMT_AUDIO_SRC_CONTROL` (`0x20AD`)**: Bind to the resolved Azalia Endpoint `ep`;
2. **`AFMT_AUDIO_PACKET_CONTROL2` (`0x207C`)**: Enable 2ch `AFMT_AUDIO_CHANNEL_ENABLE` (`0x0300` in bits `15:8`);
3. **`AFMT_CNTL` (`0x20E6`)**: Enable bit 0;
4. **`AFMT_AUDIO_PACKET_CONTROL` (`0x20AA`)**: Set `AFMT_60958_CS_UPDATE` (`0x04000000`) and enable `AFMT_AUDIO_SAMPLE_SEND` (`0x1`) with FIFO reset flags (`0x00800800`);
5. **`AFMT_INFOFRAME_CONTROL0` (`0x20AC`)**: Set `AFMT_AUDIO_INFO_UPDATE` (`0x80`).

### 3.5 DisplayPort Track: AFMT + `DP_SEC_*` Configuration (Verified)
1. **Ensure AFMT & Endpoint Alignment**: Program `AFMT_AUDIO_SRC_CONTROL` (`0x20AD`), `AFMT_AUDIO_PACKET_CONTROL2` (`0x207C` = `0x300`), `AFMT_CNTL`, `AFMT_AUDIO_PACKET_CONTROL`, and `AFMT_INFOFRAME_CONTROL0` as above;
2. **Program DP Audio N & Timestamp**:
   - `DP_SEC_AUD_N` (`0x2131`): Set `0x8000` if `0`;
   - `DP_SEC_TIMESTAMP` (`0x2135`): Set bit 0;
3. **Sequentially Enable `DP_SEC_CNTL` (`0x212B`)**:
   - Enable `DP_SEC_ASP_ENABLE` (`0x10`) -> `DP_SEC_ATP_ENABLE | DP_SEC_AIP_ENABLE` (`0x1100`) -> `DP_SEC_STREAM_ENABLE` (`0x1`);
   - **Hotplug Confirmation Pulse**: If `AFMT_STATUS` bit 4 is not set after link training, pulse `AFMT_INFOFRAME_CONTROL0` (`0x80`) and clear/re-latch `DP_SEC_CNTL` (`~0x1110` -> re-enable).

---

## 4. Driver Integration & Hooking Architecture

### 4.1 Actual Hook Entry Points
In [X6000FB.cpp](../NootedRed/X6000FB.cpp), two event-driven entry points are used (no `AGDP` or `wrapDpReceiverPowerCtrl` hooks required):

```cpp
// 1. Controller power-up and wake entry point
UInt32 X6000FB::wrapControllerPowerUp(void* const self)
{
    ...
    NRedAudio::singleton().onControllerPowerUp();
    return ret;
}

// 2. Link capabilities / hotplug entry point (optional symbol, clearError() fallback)
IOReturn X6000FB::wrapReportCapabilitiesLinkInfo(void* const self)
{
    const auto ret = FunctionCast(wrapReportCapabilitiesLinkInfo, singleton().orgReportCapabilitiesLinkInfo)(self);
    NRedAudio::singleton().onLinkCapabilitiesReported();
    return ret;
}
```

### 4.2 `AppleGFXHDA` Fix
In [AppleGFXHDA.cpp](../NootedRed/AppleGFXHDA.cpp):
1. Fixed the fallback typo in `AppleGFXHDA::wrapCreateAppleHDAWidget()`, which previously called `orgCreateAppleHDAFunctionGroup` instead of `orgCreateAppleHDAWidget`;
2. Preserved `isAlreadyRouted` checks on `AppleGFXHDAController::probe` to avoid Lilu multiroute panics alongside `AppleALC`;
3. Confirmed that `Personalities/com.apple.driver.AppleGFXHDA.xml` and `codecAddressMask` do not need modification.

---

## 5. Phased Implementation Roadmap (Completed)

1. **Milestone 1 — Topology Probing**: `IOFramebuffer` enumeration, protocol/pixel-clock/codec-info extraction, and `DIG`/`OTG` discovery.
2. **Milestone 2 — Event-Driven Lifecycle**: Replaced kernel polling threads with `wrapControllerPowerUp` / `wrapReportCapabilitiesLinkInfo` + `thread_call_enter_delayed`.
3. **Milestone 3 — Dual-Path Hardware Engine**: Implemented `DcnIpBlock` (DCN 1.0 & 2.1), `DCCG_AUDIO_DTO`, Azalia endpoint setup, `AFMT`, and `DP_SEC`.
4. **Milestone 4 — `AppleGFXHDA` & Hotplug Hardening**: Fixed `wrapCreateAppleHDAWidget` fallback and added the `+2000ms` post-link-training confirmation pulse.
5. **Milestone 5 — Hardware Verification**:
   - **DCN 2.1 Laptop** (Renoir 7730U + internal eDP, macOS 15): HDMI and USB-C DP audio work cleanly, including repeated hotplugging with both auto-switched and manually switched output.
   - **DCN 1.0 All-in-One** (HP All-in-One Raven2/Picasso Ryzen 3 3250U, no internal panel): Verified on `v1.2.7` test build—HDMI audio works out of the box.

---

## 6. Risk Assessment & Fallback Strategy

1. **Safe Topology Fallback**: If no active external `IOFramebuffer`, `DIG`, or `isMmioReady()` is present, `configureAudio()` exits safely without touching unrelated hardware registers.
2. **Optional Symbol Resilience**: `wrapReportCapabilitiesLinkInfo` is routed optionally with `patcher.clearError()` on failure, ensuring boot safety across macOS 13–26.
3. **Minimal IORegistry Footprint**: Publishes 4 summary properties by default (`NRedAudioDcnGen`, `NRedAudioCfgDIG`, `NRedAudioCfgOTG`, `NRedAudioCfgEP`), emitting full register snapshots only when `-NRedDebug` is present in `boot-args`.

---

## 7. Post-Implementation Lessons Learned

Key findings from `v1.2.7` bring-up and multi-machine validation that corrected early design assumptions:

1. **DP Audio Strongly Depends on AFMT**: Early assumptions that DP bypassed AFMT were incorrect; `AFMT_AUDIO_PACKET_CONTROL2` (`0x207C`), `AFMT_AUDIO_SRC_CONTROL`, and `AFMT_INFOFRAME_CONTROL0` are mandatory for DP audio alongside `DP_SEC_*`.
2. **`audio-codec-info` Endpoint Mismatch Was the Real Blocker**: Modifying `AppleGFXHDA.xml` or `codecAddressMask` was unnecessary; aligning the Azalia endpoint and `AFMT_AUDIO_SRC_CONTROL` to `(b[2] - 3) / 2` from `audio-codec-info` resolved the silence.
3. **DP Hotplug Permanent Silence & Post-Link Pulse**: `reportCapabilities_LinkInfo` fires before link training finishes. Combining bounded `thread_call_enter_delayed` retries with a `+2000ms` `postLinkPassDone` pulse (re-latching `AFMT_INFOFRAME_CONTROL0` and `DP_SEC_CNTL`) eliminated permanent DP hotplug silence.
4. **Laptop vs. Desktop/AIO Topology**: `connector-type == 2` (`kConnectorLVDS`, including eDP) reliably identifies internal panels; skipping `DIG0`/`OTG0` only when an internal panel exists ensures desktops and AIOs (with external displays on `FB0`/`DIG0`) work out of the box.
5. **DCN 1.0 vs. DCN 2.1 Shared Bases**: Both generations share `DCCG_BASE = 0x00C0` and `DCN_BASE_2 = 0x34C0`, differing mainly in `OTG_MASTER_EN` (`0x1B5F` vs `0x1B5C`) and `maxDig` (`4` vs `5`).
6. **Deferred EDID SAD Parsing**: A safe 2ch LPCM (`32/44.1/48 kHz`, `16/20/24-bit`) descriptor handles standard displays cleanly; dynamic multi-channel EDID CEA-861 SAD parsing remains deferred for future enhancement.
