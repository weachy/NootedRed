# IOAcceleratorFamily2 VRR Support & DisplayPipe Transaction Sync

- **Upstream Issue**: [ChefKissInc/NootedRed#441](https://github.com/ChefKissInc/NootedRed/issues/441) (`open` — *IOAcceleratorFamily2 VRR support*)
  - Related local issue: [weachy/NootedRed#17](https://github.com/weachy/NootedRed/issues/17) (120Hz internal panel DisplayPipe stalls & VCN concurrency contention)
- **Relevant Source Files**:
  - [X5000.hpp](../NootedRed/X5000.hpp) / [X5000.cpp](../NootedRed/X5000.cpp)
  - [X6000FB.hpp](../NootedRed/X6000FB.hpp) / [X6000FB.cpp](../NootedRed/X6000FB.cpp)

---

## 1. Problem Description

Starting with macOS 13 (Ventura), `IOAcceleratorFamily2` (`AMDRadeonX5000_AMDAccelDisplayPipe`) and `AMDRadeonX6000Framebuffer` (`AMDRadeonX6000_AmdFbPresenter`) exchange a shared `'vrrs'` (`0x76727273`) timing structure and VUpdate/VBlank timestamps on every frame commit. On AMD Vega/Renoir APUs, enabling `'vrrs'` (or using high-refresh 120Hz / ProMotion modes) triggers:

1. Massive kernel log spam (`Invalid actual_host_time`, `AMD ERROR! VRR FAILED TIMESTAMPS ... Next Frame: 0`, `[FB:0][IOP] AmdFbPresenter::handleTransactions() ??? Busy`).
2. `IOAccelDisplayPipe transaction stamp index 30 time out! Resetting Pipe`, leading to `WindowServer` watchdog freezes and reboots under load.
3. Hardware DRR (`dce110_timing_generator_set_drr`) fails to adjust OTG `V_TOTAL_MIN` / `V_TOTAL_MAX` on Raven (DCN 1.0) and Renoir (DCN 2.1).

---

## 2. Root Cause & Upstream Gaps

Static disassembly of `AMDRadeonX5000` and `AMDRadeonX6000Framebuffer` across macOS 13 – 26 revealed two distinct layers of defects:

### Layer 1: `IOAcceleratorFamily2` ⇄ `AmdFbPresenter` `'vrrs'` Transaction & Timestamp Protocol
1. **Unconditional Interrupt Latch Clear (`AMDRadeonX5000_AMDAccelDisplayPipe::processTransactionInterrupt`)**:
   - At the end of `processTransactionInterrupt`, Apple unconditionally clears the one-shot interrupt latch (`0x3A8(%pipe) = 0`) even when `liveTransaction` (`0x248(%pipe)`) is still non-null, stranding the active transaction until the 30-stamp watchdog fires.
2. **Zero-Retry Spin-Flag Collision (`AmdFbPresenter::handleTransactions`)**:
   - Uses a single-shot `OSCompareAndSwap(0, 1, &this->c8)` without waiting. At 120Hz, `WindowServer` commits frequently collide with the ~5–20us VBlank/VUpdate ISR holding `this->c8`, immediately logging `??? Busy` and returning `kIOReturnNotReady` (`0xE00002D8`).
3. **Zeroed / Stale `'vrrs'` Shared Timestamps**:
   - `AmdFbPresenter::prepareVRR()` zeroes `+0x20`, `+0x48`, and `+0x58` before the first `VUpdate` interrupt, causing `VRR FAILED TIMESTAMPS`.
   - On fixed-refresh commits (`requestedHostTime == 0`) or inactive VRR (`+0xBC == 0`), `maxNextFrame` (`+0x30`) diverges from `minNextFrame` (`+0x28`), causing `Invalid actual_host_time`.
4. **Stale Return Value (`AmdFbPresenter::getOutstandingTransactions`)**:
   - Caches `0xD8(%self)` before `handleStuckQueue()` clears it to `0`, returning stale `1` and forcing an extra timeout cycle.

### Layer 2: DCN Hardware OTG DRR Register Offsets
1. **`dce110_timing_generator_set_drr` Hardcoded Navi 10 Offsets**:
   - Reads and writes hardcoded DCN 2.0 (Navi 10) offsets `0x1B88` / `0x1B89` / `0x1B8A` (`OTG_V_TOTAL_MIN` / `MAX` / `CONTROL`) instead of Raven (DCN 1.0) / Renoir (DCN 2.1) offsets **`0x1B48` / `0x1B49` / `0x1B4A`**.
2. **Controller DRR Feedback (`getDrrStatus` / `getVUpdateVBlankInterval`)**:
   - Addressed alongside [ChefKissInc/NootedRed#449](https://github.com/ChefKissInc/NootedRed/issues/449) (see [issue-449-navi-controller-reimpl.md](./issue-449-navi-controller-reimpl.md)).

---

## 3. Final Solution

1. **Preserve Interrupt Latch (`X5000::wrapProcessTransactionInterrupt`)**:
   - Disassembled and verified `liveTransaction` (`0x248`) and interrupt latch (`0x3A8`) across macOS 13 – 26. After calling the original `processTransactionInterrupt`, restores `0x3A8(%pipe) = 1` whenever `0x248(%pipe)` remains non-null.
2. **Pre-Wait & Retry on `this->c8` + `'vrrs'` Timestamp Guards (`X6000FB::wrapPresenterHandleTransactions`)**:
   - Pre-waits on `this->c8` (and retries `kIOReturnNotReady`) in `5us` increments up to `400` iterations (`2ms` max) to absorb VBlank/VUpdate ISR collisions.
   - Initializes zeroed `'vrrs'` timestamps (`+0x20`, `+0x48`, `+0x58`) from `mach_absolute_time()` and keeps `maxNextFrame == minNextFrame` on fixed-refresh commits.
   - Keeps `'vrrs'` **enabled by default**, with `-NRedNoVRR` available as an opt-out boot argument via `X6000FB::wrapFbGetAttribute`.
3. **Re-Read Post-Clear Mask (`X6000FB::wrapPresenterGetOutstandingTransactions`)**:
   - Re-reads `0xD8(%self)` after the original call if non-zero so cleared stuck queues are reported immediately.
4. **DCN 1.0 / 2.1 DRR OTG Register Patch (`X6000FB::processKext`)**:
   - Cross-version `MaskedLookupPatch` (macOS 13/14 and macOS 15/26 instruction variants) replacing all 6 occurrences of `0x1B88..0x1B8A` with **`0x1B48..0x1B4A`** inside `dce110_timing_generator_set_drr`, with graceful fallback if unmatched.

---

## 4. Compatibility & Verification

- **macOS Versions**: Symbols and instruction layouts verified across **macOS 13.6.7, 14.3, 15.8.1, and 26.7.1**.
- **Layer 1 Verified (120Hz & ProMotion mode)**: `Invalid actual_host_time`, `VRR FAILED TIMESTAMPS`, `handleStuckQueue`, and `transaction stamp index 30 time out` are eliminated (`0` occurrences).
- **Layer 2 Pending External Hardware Test**: The `dce110_timing_generator_set_drr` OTG patch (`0x1B48..0x1B4A`) builds and loads cleanly; dynamic hardware refresh-rate scaling remains to be verified on a confirmed Adaptive-Sync/FreeSync panel.
