# Reimplement Navi-Specific `AMDRadeonX6000_AmdRadeonControllerNavi` Methods

- **Upstream Issues**:
  - [ChefKissInc/NootedRed#449](https://github.com/ChefKissInc/NootedRed/issues/449) (`open` — *Reimplement remaining Navi-specific code from AMDRadeonX6000Framebuffer*)
  - [ChefKissInc/NootedRed#450](https://github.com/ChefKissInc/NootedRed/issues/450) (`open` sub-issue — *AMDRadeonX6000_AmdRadeonControllerNavi::setupBootWatermarks*)
  - [ChefKissInc/NootedRed#451](https://github.com/ChefKissInc/NootedRed/issues/451) (`open` sub-issue — *AMDRadeonX6000_AmdRadeonControllerNavi::isPoweredUp has Navi/dGPU-specific logic*)
- **Relevant Source Files**:
  - [X6000FB.hpp](../NootedRed/X6000FB.hpp) / [X6000FB.cpp](../NootedRed/X6000FB.cpp)

---

## 1. Problem Description

Because macOS lacks a native framebuffer kext for AMD Vega/Renoir APUs (DCN 1.0 / DCN 2.1), NootedRed attaches `AMDRadeonX6000Framebuffer` using `AMDRadeonX6000_AmdRadeonControllerNavi10` (subclass of `AMDRadeonX6000_AmdRadeonControllerNavi`). Several unpatched methods on `AmdRadeonControllerNavi` continued to access Navi 10 discrete GPU (DCN 2.0 / GDDR6 / dGPU MP1) MMIO registers and pipe counts, causing:

1. Spurious 10ms busy-waits and incorrect power-state reporting during `isPoweredUp()`.
2. `setupBootWatermarks()` previously stubbed to a no-op (`dummyIOReturnSuccess`) instead of handling APU `DCHUBBUB` watermarks.
3. Broken `getDrrStatus()` and `getVUpdateVBlankInterval()` reads against Navi 10 OTG register offsets (plus an Apple bug in `getDrrStatus()` unconditionally returning `kIOReturnError`).

---

## 2. Root Cause & Upstream Gaps

Static disassembly of `AMDRadeonX6000_AmdRadeonControllerNavi` in `AMDRadeonX6000Framebuffer` (verified across macOS 13.6.7, 14.3, 15.8.1, and 26.7.1) showed:

1. **`isPoweredUp()` (#451)**:
   - After checking `m_flags & 0x8` (`0x5F18`) and PCIe vendor ID `0x1002`, Apple's implementation reads Navi 10 dGPU BACOS register `0x53` bit 9 and, when 0, spins up to `1000 × IODelay(10)` (`10ms`) polling Navi 10 MP1 register `0x16061` (`mmMP1_SMN_C2PMSG_33`) before returning `(reg_0x53 >> 9) & 1` (`false` on APUs).
2. **`setupBootWatermarks()` (#450)**:
   - Writes hardcoded Navi 10/14 values (`0xE00050F7`, etc.) to 5 Navi 10 HUBP pipes (`0x3AEE..0x3E5E`, stride `0xDC`), whereas Raven/Renoir APUs have 4 pipes and UEFI GOP already initializes `DCHUBBUB` watermark Set A (`0x39C9..0x39CD`).
3. **`getDrrStatus()` & `getVUpdateVBlankInterval()` (#449 / #441)**:
   - Both allow `otgInst < 6` (Navi 10 has 6 pipes; APUs have 4) and read Navi 10 offsets `0x5057` (`DCN_BASE_2 + 0x1B97`) and `0x5047` (`DCN_BASE_2 + 0x1B87`).
   - Additionally, `AmdRadeonControllerNavi::getDrrStatus()` jumps to `movl $0xe0000001, %eax` (`kIOReturnError`) even on the valid `otgInst < 6` path, causing every caller in `AmdFbPresenter::transactionVUpdateHandler` to see an error.

---

## 3. Final Solution

All four `AMDRadeonX6000_AmdRadeonControllerNavi` methods are routed in [X6000FB.cpp](../NootedRed/X6000FB.cpp#L273-L295) with graceful fallback (`patcher.clearError()` on missing optional routes):

1. **`X6000FB::wrapControllerIsPoweredUp` (#451)**:
   - Checks `(getMember<UInt8>(self, 0x5F18) & 0x8U) != 0` and verifies live MMIO (`NRed::singleton().readReg32(0x0) != 0xFFFFFFFFU`), completely removing the Navi 10 `0x53` bit 9 check and `10ms` `0x16061` polling loop.
2. **`X6000FB::wrapSetupBootWatermarks` (#450)**:
   - Reads UEFI-initialized `DCHUBBUB_ARB_*_WATERMARK_A` registers (`0x39C9..0x39CD`) and propagates non-zero values to Sets B, C, and D (`+5`, `+10`, `+15`), matching DCN 1.0 and DCN 2.1 `DCHUBBUB` layout without touching Navi 10/14 5-pipe HUBP offsets.
3. **`X6000FB::wrapGetDrrStatus` & `X6000FB::wrapGetVUpdateVBlankInterval` (#449 / #441)**:
   - Enforces the APU 4-pipe limit (`otgInst >= 0 && otgInst < 4`).
   - `wrapGetDrrStatus`: Reads `DCN_BASE_2 + (isRenoir ? 0x1B57U : 0x1B5AU) + (0x80U * otgInst)`, writes `(regVal >> 16) & 0x7FFFU`, and returns **`kIOReturnSuccess` (`0`)**.
   - `wrapGetVUpdateVBlankInterval`: Reads `DCN_BASE_2 + 0x1B47U + (0x80U * otgInst)`, writes `regVal & 0x3FFU`, and returns `kIOReturnSuccess` (`0`).

---

## 4. Compatibility & Verification

- **macOS Versions**: All four mangled symbols verified present in `AMDRadeonX6000Framebuffer` across **macOS 13.6.7, 14.3, 15.8.1, and 26.7.1**.
- **Hardware Coverage**: Supports both **Raven/Picasso (DCN 1.0)** and **Renoir/Cezanne (DCN 2.1)** APUs across laptop eDP and desktop/mini-PC external topologies.
- **Runtime Verification**: Built and tested on macOS 15.8.3 (Renoir/Barcelo) with clean boot, mode switching, and zero controller timeout/power-up errors.
