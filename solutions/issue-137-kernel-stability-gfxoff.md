# Kernel Stability: SWScheduler Page Fault, VRAM Allocator Underflow & GFXOFF Hangs

- **Upstream Issue**: [ChefKissInc/NootedRed#137](https://github.com/ChefKissInc/NootedRed/issues/137) (`open` — *Heavy compute activities hang the system*) and related `AMDRadeonX5000` kernel panics / context initialization failures
- **Resolved In**: `v1.2.2` (`c9467c9`)
- **Relevant Source Files**:
  - [X5000.hpp](../NootedRed/X5000.hpp) / [X5000.cpp](../NootedRed/X5000.cpp)

---

## 1. Problem Description

On AMD Vega APUs running macOS 13 – 26:
1. Users experience a **Kernel Panic (`CR2: 0x48`)** in `AMDRadeonX5000_AMDSWScheduler::advance` during user login on single-GPU APUs.
2. Under fragmented physical VRAM, graphics/video context creation fails or triggers black screens.
3. Under heavy compute or graphics workloads, the GPU intermittently **hangs or times out** due to GFXOFF power gating.

---

## 2. Root Cause & Upstream Gaps

1. **`AMDSWScheduler::advance` Null Peer Channel Dereference**:
   - `AMDRadeonX5000_AMDSWScheduler::advance(HwChannelCtx&)` dereferences a peer channel pointer that can be `nullptr` on single-GPU APUs during login, causing a `CR2: 0x48` page fault.
2. **`IOAccelMemoryAllocator2::allocPages` Unsigned 32-Bit Underflow**:
   - When physical VRAM is fragmented, `IOAccelMemoryAllocator2::allocPages` hits an unsigned 32-bit underflow in its non-contiguous hole calculation, causing `allocateNonContiguous` to fail instead of falling back cleanly.
3. **Active GFXOFF Power Gating on APUs**:
   - Leaving GFXOFF enabled on APUs allows the SMU/firmware to power-gate the GFX block aggressively; subsequent engine wake-ups during heavy compute or mixed workloads can stall or time out.

---

## 3. Final Solution

1. **Null-Guard Patch for `AMDSWScheduler::advance` (`X5000::patchSWSchedulerAdvance`)**:
   - Dynamically locates `__ZN29AMDRadeonX5000_AMDSWScheduler7advanceERNS_12HwChannelCtxE` and patches the peer channel dereference to safely handle `nullptr` without panicking.
2. **VRAM Non-Contiguous Hole Underflow Fix (`X5000.cpp`)**:
   - Dynamically locates `IOAccelMemoryAllocator2::allocPages` via its unique 8-argument call site inside `allocateNonContiguous` across macOS 13 – 26 and patches the unsigned 32-bit underflow so fragmented VRAM allocations succeed cleanly.
3. **Permanent APU GFXOFF Suppression (`X5000.cpp`)**:
   - Patches `RTHardware` GFXOFF support (`gfxOffSupportedOffset`) and routes optional GFXOFF hooks safely (logging and continuing if an optional symbol is absent on a given OS), publishing `NRedGFXOFFPatched` / `NRedGFXOFFDisabled` in IORegistry.

---

## 4. Compatibility & Verification

- **macOS Versions**: Verified dynamically across **macOS 13 (Ventura)**, **macOS 14 (Sonoma)**, **macOS 15 (Sequoia)**, and **macOS 26 (Tahoe)**.
- **Result**: Eliminates `CR2: 0x48` login panics, fragmented-VRAM context allocation failures, and GFXOFF-related compute hangs.
