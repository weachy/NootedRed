# NootedRed

[![Release](https://img.shields.io/github/v/release/weachy/NootedRed?include_prereleases&color=007ec6)](https://github.com/weachy/NootedRed/releases) [![Downloads](https://img.shields.io/github/downloads/weachy/NootedRed/total?color=brightgreen)](https://github.com/weachy/NootedRed/releases/latest)

An open-source macOS kernel extension providing full graphics acceleration, hardware video decoding & encoding (AVC, HEVC, and VP9), HDMI/DisplayPort audio, and universal dynamic patching for AMD Vega/GCN architecture APUs across macOS 13 (Ventura) through macOS 26 (Tahoe).

> [!WARNING]
> This project is under active development. Back up your EFI configuration and ensure `Lilu.kext` is up to date before upgrading.

---

## Key Highlights & Improvements over Upstream

Compared to the upstream [ChefKissInc/NootedRed](https://github.com/ChefKissInc/NootedRed), this fork delivers major stability, multimedia, and compatibility enhancements:

- **Full Hardware Media Suite & VCN Resilience**: Bridges VCN 2.x hardware acceleration (AVC, HEVC 8-bit/10-bit/8K, and VP9 decode) with concurrency quota control, lock-free command retirement, and self-healing cooldowns to drastically reduce VCN-induced `gpuRestart`.
- **Native Metal Compute & Chromium/OpenGL Fixes**: Restores true `numShaderEngines` (`1 SE`) topology in `dyld_shared_cache` to unlock Metal Compute and eliminate QuickLook/VideoToolbox hangs, alongside dynamic GL/Metal dual-source blending fixes for Chromium and Electron apps.
- **Universal Dynamic Patching (macOS 13 – 26)**: Replaces static 4KB page tables with resilient dynamic signature matching and an O(1) page-eviction cache, adapting automatically across macOS updates without requiring `AMFIPass.kext`.
- **External Display Recovery & HDMI/DP Audio**: Implements event-driven DCN 1.0/2.1 HDMI & DisplayPort audio output (including high-refresh DP and hotplug recovery) and fixes external monitor power-off/wake signal recovery and internal eDP boot race conditions.
- **DisplayPipe VRR Sync, Navi Controller & Low-VRAM Optimizations**: Synchronizes `IOAcceleratorFamily2` VRR/DisplayPipe transactions, reimplements APU-native `AmdRadeonControllerNavi` paths, and optimizes GL resource budgets and large-block VRAM allocation for low-VRAM devices.

> For detailed root-cause analyses and technical write-ups of resolved upstream issues, see **[`solutions/README.md`](solutions/README.md)**.

---

## Supported Hardware & Compatibility

### APU Architecture & Feature Matrix

| APU Family | Processor Series | Architecture | VCN / DCN | Core Graphics & GLFix | HDMI / DP Audio | Hardware Video Acceleration (VDA) |
| :--- | :--- | :--- | :--- | :---: | :---: | :---: |
| **Raven Ridge** | Ryzen 2000 series | Vega (GFX902) | VCN 1.0 / DCN 1.0 | ✅ Supported | ✅ Supported | 🧪 Experimental (Testing only, disabled by default) |
| **Picasso / Raven2** | Ryzen 3000 series | Vega (GFX902/909) | VCN 1.0 / DCN 1.0 | ✅ Supported | ✅ Supported | 🧪 Experimental (Testing only, disabled by default) |
| **Renoir** | Ryzen 4000 series | Vega (GFX90C) | VCN 2.2 / DCN 2.1 | ✅ Supported | ✅ Supported | ✅ **Supported (AVC, HEVC, VP9)** |
| **Lucienne** | Ryzen 5000 (Zen 2) | Vega (GFX90C) | VCN 2.2 / DCN 2.1 | ✅ Supported | ✅ Supported | ✅ **Supported (AVC, HEVC, VP9)** |
| **Cezanne** | Ryzen 5000 (Zen 3) | Vega (GFX90C) | VCN 2.2 / DCN 2.1 | ✅ Supported | ✅ Supported | ✅ **Supported (AVC, HEVC, VP9)** |
| **Barcelo** | Ryzen 7030 series | Vega (GFX90C) | VCN 2.2 / DCN 2.1 | ✅ Supported | ✅ Supported | ✅ **Supported (AVC, HEVC, VP9)** |

### Unsupported Hardware
- **RDNA-based APUs** (Ryzen 6000, 7040, and 8000 series with Radeon 680M / 780M) are **NOT supported**.

### Supported macOS Versions
- macOS 13 (Ventura) – macOS 26 (Tahoe)

---

## How to Use

### 1. OpenCore Setup & Load Order

- **Lilu Requirement**: `Lilu.kext` (>= 1.7.0) is strictly required and **must be loaded BEFORE `NootedRed.kext`** in your OpenCore `config.plist`.
- **AppleALC Conflict Warning**: **Avoid loading `AppleALC.kext` before `NootedRed.kext`**, as this can trigger kernel extension load order conflicts. Ensure `AppleALC.kext` is listed **AFTER** `NootedRed.kext` in `config.plist`.
- **Strict Incompatibilities**: **DO NOT** use `WhateverGreen.kext` or `BFixup.kext` alongside `NootedRed.kext` (they will conflict).

### 2. Out-of-the-Box Defaults

**GLFix, Metal fixes, VDA hardware video acceleration (AVC + HEVC + VP9), VRR DisplayPipe sync, and HDMI/DP audio are all ENABLED by default** on supported hardware. Most users do not need to add special boot arguments.

### 3. SIP Configuration

- We recommend setting `csr-active-config` to **`03080000`** in your OpenCore `config.plist` (`NVRAM` -> `Add` -> `7C436110-AB2A-4BBB-A880-FE41995C9F82` as `Data: 03080000`), which is required by the **VP9 hardware decode patch**.
- **If you prefer to keep SIP enabled**, add **`-NRedNoVP9`** to `boot-args`. All other features (GLFix, Metal Compute, AVC/HEVC hardware acceleration, and HDMI/DP audio) operate natively with SIP enabled.

### 4. Key Boot Arguments

> [!NOTE]
> For the latest complete list of boot arguments and experimental flags, please refer to the latest **[Release Notes](https://github.com/weachy/NootedRed/releases/latest)**.

| Boot Argument | Function | Description / Diagnostic Use |
| :--- | :--- | :--- |
| **`-NRedNoVP9`** | Disable VP9 hardware decode patches | Required if you keep SIP fully enabled. |
| **`-NRedNoVDA`** | Disable all hardware video acceleration | Diagnostic switch. Reverts video playback and encoding to CPU software paths. |
| **`-NRedNoHEVC`** | Restrict hardware acceleration to AVC/H.264 only | Disables HEVC decode and encode while retaining AVC/H.264 acceleration. |
| **`-NRedNoGLFix`** | Disable user-space OpenGL/Metal dual-source patches | Diagnostic switch for Chromium/Electron/CEF application workarounds. |
| **`-NRedOLED`** | Force AUX nits backlight control for OLED panels | Use on laptops with OLED internal displays if AUX brightness is not auto-detected. |
| **`-NRedDebug`** | Enable verbose kernel logging & full IORegistry telemetry | Useful when reporting issues and submitting compatibility logs. |
| **`-NRedOff`** | Completely disable all NootedRed drivers and hooks | Recovery / vanilla baseline boot. |

---

## Known Limitations & DRM

- **FairPlay 1.x DRM**: Hardware-accelerated FairPlay DRM (such as Netflix in Safari or Apple TV+) has architectural constraints on AMD Hackintoshes. For streaming media, use Chromium-based browsers (Google Chrome, Microsoft Edge, Mozilla Firefox) which leverage Widevine software DRM.

---

## Reporting Issues

When opening an issue, please include your CPU/APU model, macOS version, OpenCore `config.plist` (with serials redacted), and kernel logs captured with `-NRedDebug`.

---

## How to Build

### Prerequisites
- macOS 13 or newer build host.
- Xcode 15 or newer (with Command Line Tools installed).
- Lilu SDK (placed alongside the repository or linked through your environment).

### Building with Xcode
1. Open `NootedRed.xcodeproj` in Xcode.
2. Select the `NootedRed` scheme and set the build configuration to `Release`.
3. Press `Cmd + B` to build.

### Building via Command Line
Run the following command from the root of the repository:
```bash
xcodebuild -project NootedRed.xcodeproj -scheme NootedRed -configuration Release -derivedDataPath build CONFIGURATION_BUILD_DIR=build/Release clean build
```

The resulting `NootedRed.kext` and zipped release archives will be generated in `build/Release/`.

---

## Credits

- https://github.com/xiaot-evo/NootedRed
- https://github.com/steve12311/NootedRed
- https://github.com/ChefKissInc/NootedRed
- https://github.com/acidanthera

---

## License

```
Copyright (C) 2022-2026  ChefKiss

Licensed under the ChefKiss Open Knowledge License version 1.

This work is licensed to you under the terms of the ChefKiss Open Knowledge
License, either version 1 of the License, or (at your option) any later
version published by ChefKiss.

This work may not be used to train or develop artificial intelligence,
and may not be used with artificial intelligence in any other way. See
the ChefKiss Open Knowledge License for details.

This work is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
ChefKiss Open Knowledge License for more details.

You should have received a copy of the ChefKiss Open Knowledge License
along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.
```

> [!WARNING]
> This project is under active research and development; There will be crashes here and there, as the project is glue between the existing buggy AMD drivers.
>
> See repository issues and [our site](https://chefkiss.dev/applehax/nootedred) for more information.

## Documentation

- [Compatibility architecture](docs/Compatibility-Architecture.md) — multi-macOS × multi-APU design:
  target identity vs hardware capability, verification ladder, manifest pipeline and milestones.
- [Contributing to UserPatches](docs/CONTRIBUTING-USERPATCHES.md) — architecture, adding macOS builds,
  APU families, patch-data generation, resolver rules, and testing checklist.
- [Chromium/Electron fix usage](docs/Chromium-Fix-Usage.md) — end-user installation and rollback.
- [Device profile / media capability layer](docs/Device-Profile.md) — graphics + future VDA capability model.
- [X5000 unified memory](docs/X5000-UnifiedMemory.md) — opt-in `-NRedUnifiedMemory` for Renoir: textures, IOSurface
  caches and buffers in pageable system memory instead of the carve-out; design, tests and measurements.
- [Compatibility data collector](tools/README-collect-compat-data.md) — collect a target machine's drivers, patch sites, VDA data and capabilities for new macOS/APU support.
- [iGPU HwInfo / UBM research](docs/IGPU-HwInfo-Research-Plan.md) — research record on where the user-space
  driver gets iGPU hardware info and what the GL fix still needs.
- [Tester guide](docs/TESTER-GUIDE.md) — archived resolver QA handoff checklist.
- [GL probe and reproduction notes](tools/README.md) — CGL/OpenGL dual-source probe and the evidence ladder.
- [HW-info probe](tools/README-hwinfo-probe.md) — IOKit probe for the UBM hardware-info blob.
- [Chromium/OpenGL issue report](Chromium-OpenGL-Issue-Report.md) — original issue write-up behind the fix.

## Build artifacts

Use `tools/build-branch.sh -y` to put the kext and zip under
`build/branches/<branch>/...`. See [docs/Build-Artifacts.md](docs/Build-Artifacts.md).
