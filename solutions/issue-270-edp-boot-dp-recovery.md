# Internal eDP Early-Boot Black Screen & External DP Power-Off Recovery

- **Upstream Issue**: [ChefKissInc/NootedRed#270](https://github.com/ChefKissInc/NootedRed/issues/270) (*Internal display does not work in a very small amount of laptops*)
  - Related issues: [ChefKissInc/NootedRed#206](https://github.com/ChefKissInc/NootedRed/issues/206) (*Internal display not working only HDMI output*), [ChefKissInc/NootedRed#333](https://github.com/ChefKissInc/NootedRed/issues/333) (DisplayPort output / signal issues)
- **Resolved In**: `v1.2.3` – `v1.2.7` (`d573a2d`, `544dd02`, `dd3720b`, `7cb1fe5`, `0cff0c6`)
- **Relevant Source Files**:
  - [Backlight.hpp](../NootedRed/Backlight.hpp) / [Backlight.cpp](../NootedRed/Backlight.cpp)
  - [X5000.cpp](../NootedRed/X5000.cpp)
  - [X6000FB.cpp](../NootedRed/X6000FB.cpp)

---

## 1. Problem Description

1. **Internal eDP Early-Boot Black Screen / `CoreDisplay` Abort**: On fast-booting laptops (e.g., fast NVMe, `SetApfsTrimTimeout=0`, non-verbose boot), `WindowServer` queries display status before asynchronous eDP link training completes, causing a black screen or `CoreDisplay` assertion abort.
2. **External DP Monitor Fails to Recover After Power-Off**: After turning an external DisplayPort monitor off and back on (or after prolonged power-off), macOS fails to re-detect the monitor and restore video signal.

---

## 2. Root Cause & Upstream Gaps

1. **Early-Boot `kConnectionEnable` & `getDisplayInfo` Race on Internal Panels**:
   - `WindowServer` queries `kConnectionEnable` and `AMDHWDisplay::getDisplayInfo` while DCN/eDP link training is still settling, causing `getAttributeForConnection` to temporarily return `0` / error.
2. **Unscoped `online` / Delay Handling Affecting External Displays**:
   - Treating external displays as `online` when disconnected or applying unconditional delays on DP receiver power-off paths interferes with external DP hotplug/power-cycle state machines, preventing signal recovery when a monitor is powered back on.
   - Similarly, retrying `kConnectionEnable` globally across all framebuffers would add unnecessary blocking delays to external outputs.

---

## 3. Final Solution

1. **Scoped eDP Link-Training Retry (`Backlight::wrapGetAttributeForConnection`)**:
   - When `kConnectionEnable` fails or returns `0` on `connectIndex == 0`, polls `orgGetAttributeForConnection` up to 5 times (`20ms` each, ~`100ms` max) **strictly limited to `FB0` when `embeddedPanelLink != nullptr`**, avoiding any blocking delay on external outputs.
2. **Embedded-Only Early-Boot `online` Guarantee (`X5000::fixedGetDisplayInfo`)**:
   - Queries `getAttributeForConnection(0, kConnectionEnable, &isOnline)` (with fallback to `fbIndex`).
   - Only forces `online = true` when `!online && isCRTEnabled && fbIndex == 0 && Backlight::singleton().hasEmbeddedPanel()` (covering internal eDP and LVDS panels), leaving external displays able to transition offline and cleanly recover when powered back on.
3. **Power-On-Only DP Receiver Delay (`X6000FB::wrapDpReceiverPowerCtrl`)**:
   - Restricts the `250ms` `IOSleep` settling delay strictly to `if (powerOn)`, preventing unnecessary delays during DP receiver power-off transitions.

---

## 4. Compatibility & Verification

- **Laptops (Internal eDP + External DP)**: Eliminates fast-boot black screens while preserving clean external DP power-off/on signal recovery.
- **Desktops / Mini PCs (No Internal Panel)**: `hasEmbeddedPanel()` and `embeddedPanelLink` remain `false`/`nullptr`, so external outputs experience zero eDP polling overhead.
