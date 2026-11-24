# NootedRed ![GitHub Workflow Status](https://img.shields.io/github/actions/workflow/status/ChefKissInc/NootedRed/main.yml?branch=master&logo=github&style=for-the-badge) [![Release Badge](https://img.shields.io/github/v/release/ChefKissInc/NootedRed?include_prereleases&style=for-the-badge&sort=semver&color=blue)](https://github.com/ChefKissInc/NootedRed/releases) [![Downloads Badge](https://img.shields.io/github/downloads/ChefKissInc/NootedRed/total?style=for-the-badge)](https://github.com/ChefKissInc/NootedRed/releases/latest) ![Written by humans, not AI](https://img.shields.io/badge/written_by_humans-not_ai-blue?style=for-the-badge)

> [!CAUTION]
> Please consider donating: donations help me stay alive and pay costs; if the Hackintosh projects don't receive funding, I will (have to) stop working on them.
>
> PayPal or credit/debit card: https://ko-fi.com/chefkiss
>
> BTC: `bc1qgu56kptepex2csuzl5nhzc4vxuj8c6ggjzhcem`
>
> SOL: `4PJU3iB5rimN9BNcpzduvmTnptJyedZiAd23so33SAdi`
>
> ETH: `0x038A25849c23Bc5A736484351a8B6Ad71bC46676`

The macOS kernel extension for GPU bug fixes, QoL improvements, and extended hardware support.

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
