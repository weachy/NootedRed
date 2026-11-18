# NootedRed Resolved Issues & Final Solutions

This directory documents major issues investigated and resolved in this fork—primarily corresponding to long-standing open or partially resolved issues in [ChefKissInc/NootedRed](https://github.com/ChefKissInc/NootedRed/issues). Each document records the **problem description, upstream issue reference, root cause / upstream gaps, and final solution** so contributors and upstream maintainers can review the conclusions directly without digging through historical investigation logs.

---

## Index

| Document | Upstream Issue | Summary | Verification |
| :--- | :--- | :--- | :--- |
| [VCN2 / VDA Hardware Video Acceleration](./issue-28-vcn-vda.md) | [ChefKissInc/NootedRed#28](https://github.com/ChefKissInc/NootedRed/issues/28) | Kernel VCN2 bridge (`X6000` -> `X5000`), dynamic VDA/GVA signatures, and non-destructive `AddrLib` wildcards | Verified (macOS 13 – 26) |
| [Kernel Stability, VRAM Allocator & GFXOFF](./issue-137-kernel-stability-gfxoff.md) | [ChefKissInc/NootedRed#137](https://github.com/ChefKissInc/NootedRed/issues/137) | `AMDSWScheduler::advance` null-deref panic fix, `allocPages` underflow fix, and permanent APU GFXOFF disable | Verified (macOS 13 – 26) |
| [Chromium / OpenGL & Metal Blend](./issue-158-chromium-opengl.md) | [ChefKissInc/NootedRed#158](https://github.com/ChefKissInc/NootedRed/issues/158) | Process-agnostic UBC `_cs_validate_page` patcher, dynamic GL/Metal signatures, and UBC eviction cache | Verified (macOS 13 – 26) |
| [HDMI / DisplayPort External Audio Silence](./issue-225-hdmi-dp-audio.md) | [ChefKissInc/NootedRed#225](https://github.com/ChefKissInc/NootedRed/issues/225) | External audio visible in macOS Sound settings but silent, DP hotplug silence, and desktop/AIO Pin NID mismatch | Verified (DCN 1.0 HDMI + DCN 2.1 HDMI/DP) |
| [Metal Compute, VideoToolbox & QuickLook Hangs](./issue-235-metal-compute-vcn.md) | [ChefKissInc/NootedRed#235](https://github.com/ChefKissInc/NootedRed/issues/235) | Shared-cache `AMDRadeonX5000MTLDriver` `numShaderEngines` fix (`4 -> 1`), non-blocking VCN teardown, OpenCL clamp | Verified (macOS 13 – 26) |
| [Internal eDP Boot Race & DP Power-Off Recovery](./issue-270-edp-boot-dp-recovery.md) | [ChefKissInc/NootedRed#270](https://github.com/ChefKissInc/NootedRed/issues/270) | Scoped `FB0` eDP link-training retry, embedded-only early `online` override, and DP power-on-only delay | Verified (Laptop eDP + External DP) |
