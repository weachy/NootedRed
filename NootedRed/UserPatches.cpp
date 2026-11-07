// User-Space Driver Patches
//
// Copyright (C) 2026  ChefKiss.
//
// Licensed under the ChefKiss Open Knowledge License version 1.
//
// This work is licensed to you under the terms of the ChefKiss Open Knowledge
// License, either version 1 of the License, or (at your option) any later
// version published by ChefKiss.
//
// This work may not be used to train or develop artificial intelligence,
// and may not be used with artificial intelligence in any other way. See
// the ChefKiss Open Knowledge License for details.
//
// This work is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// ChefKiss Open Knowledge License for more details.
//
// You should have received a copy of the ChefKiss Open Knowledge License
// along with this work.  If not, see https://git.chefkiss.dev/ChefKiss/OpenKnowledge/raw/branch/master/LICENSE-v1.md.

#include <Headers/kern_api.hpp>
#include <Headers/kern_mach.hpp>
#include <Headers/kern_util.hpp>
#include <DeviceProfile.hpp>
#include <NRed.hpp>
#include <PenguinWizardry/KernelVersion.hpp>
#include <UserPatches.hpp>
#include <VCNBridge.hpp>
#include <libkern/OSAtomic.h>
#include <libkern/OSTypes.h>
#include <mach/i386/vm_types.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSBoolean.h>
#include <sys/vnode.h>

static UserPatches moduleInstance;

namespace
{

    const char* const kBootArgNoGLFix          = "-NRedNoGLFix";
    const char* const kBootArgGLFixOff         = "-NRedGLFixOff";
    const char* const kBootArgGLFixResolve     = "-NRedGLFixResolve";
    const char* const kBootArgGLFixResolveOnly = "-NRedGLFixResolveOnly";
    const char* const kBootArgGLFixResolveOff  = "-NRedGLFixResolveOff";
    const char* const kBootArgGLFixNoResolve   = "-NRedGLFixNoResolve";
    const char* const kBootArgNoVDA            = "-NRedNoVDA";
    const char* const kBootArgNoHEVC           = "-NRedNoHEVC";

    // The patch bytes and page images below are specific to one driver build, and the native backend hooks
    // code signing page validation. Enabled by default; pass -NRedNoGLFix to opt out.
    constexpr bool kGLFixDefaultEnabled = true;

    // Arbitrary non-zero section id for `UserPatcher::BinaryModPatch`; Lilu's ABI treats zero as disabled.
    constexpr UInt32 kLiluPatchSection = 1;

    // Exact patches for
    // `/System/Library/Extensions/AMDRadeonX5000GLDriver.bundle/Contents/MacOS/AMDRadeonX5000GLDriver`.
    //
    // TODO: Populate this table from analysis/gl-hwinfo.md, or call `UserPatches::singleton().registerGLDriverFix`
    // with a table in `NRed::init`.  Each entry needs:
    //  - `find`/`replace`/`size`: the bytes to replace (both backends),
    //  - `skip`/`count`: which occurrences to replace; `count` must be non-zero (both backends),
    //  - `segment`: the Mach segment/section holding the patch (Lilu backend only, `SegmentTextText` for `__text`),
    //  - `page`: the exact 4 KiB file page containing the first `find` (native backend only, macOS 11 and newer).
    //
    // A page image can be extracted with, for example:
    //  - `dd if="$kGLDriverPath" of=page.bin bs=4096 skip=$((0xdf000 / 0x1000)) count=1` and then `xxd -i page.bin`.
    //
    // Common GL Driver patch patterns (shared across macOS 14, 15, 26)
    // A1: `_gfx9_HwInfoInitData` hardcodes four shader engines in the Vega10 branch. Renoir has one
    // shader engine, so `rec+0x64 = rec+0x60 * rec+0x78 * rec+0x80` becomes 32 instead of 8 compute
    // units. Site: file offset 0xE081 = page 0xE000 + 0x81.
    static const UInt8 kGLDriverHwInfoFind[]    = {0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00};
    static const UInt8 kGLDriverHwInfoReplace[] = {0xC7, 0x47, 0x78, 0x01, 0x00, 0x00, 0x00};

    // A2: `_glrAMD_GFX9_UpdateHwCbDualQuadMode` must set `CB_COLOR_CONTROL.DISABLE_DUAL_QUAD` on
    // the iGPU. The original code computes the bit from `rec+0xA0` bit 1, which is set only for the
    // Vega12 branch (0x4F); Renoir takes the Vega10 branch (0x4D), so dual-source blending drops the
    // draw or resets the GPU. Force `or eax, 1` instead of `and eax, ~1`.
    // Site: file offset 0x8160 = page 0x8000 + 0x160; the same 3-byte sequence also occurs at
    // page offset 0x68, hence skip = 1.
    static const UInt8 kGLDriverDualQuadFind[]    = {0x83, 0xE0, 0xFE};
    static const UInt8 kGLDriverDualQuadReplace[] = {0x83, 0xC8, 0x01};

    // Metal counterpart: Gate pattern (jnz -> jmp) for macOS 14 & 15
    static const UInt8 kMTLGateFind[]    = {0x75, 0x0B};
    static const UInt8 kMTLGateReplace[] = {0xEB, 0x0B};

    // macOS 14 (Sonoma) Allow pattern: 0F 93 C0 (setae al) -> B0 01 90 (mov al,1; nop)
    static const UInt8 kMTLAllowFind_Sonoma[]    = {0x0F, 0x93, 0xC0};
    static const UInt8 kMTLAllowReplace_Sonoma[] = {0xB0, 0x01, 0x90};

    // macOS 15 (Sequoia) Allow pattern: 0F 94 C0 (setz al) -> B0 01 90 (mov al,1; nop)
    static const UInt8 kMTLAllowFind_Sequoia[]    = {0x0F, 0x94, 0xC0};
    static const UInt8 kMTLAllowReplace_Sequoia[] = {0xB0, 0x01, 0x90};

    // macOS 26 (Tahoe) Gate: 74 28 (je +0x28) -> EB 00 (jmp +0x00, fallthrough / no-op jump)
    static const UInt8 kMTLGateFind_Tahoe[]    = {0x74, 0x28};
    static const UInt8 kMTLGateReplace_Tahoe[] = {0xEB, 0x00};

    // macOS 26 (Tahoe) Allow pattern: 0F 94 C1 (sete cl) -> B1 01 90 (mov cl,1; nop)
    static const UInt8 kMTLAllowFind_Tahoe[]    = {0x0F, 0x94, 0xC1};
    static const UInt8 kMTLAllowReplace_Tahoe[] = {0xB1, 0x01, 0x90};

    #include "VDASignatures.inc"

    static const UInt8 kResolverMask32[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };
    static const UInt8 kResolverMask32_RipMasked[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };

    // macOS 14 (Sonoma) resolver contexts.
    static const UInt8 kResolverHwCtx_Sonoma[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0xF4, 0x19, 0x0E, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Sonoma[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Sonoma[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x48, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x48, 0x8B, 0x9D, 0x78, 0xFE, 0xFF, 0xFF, 0xEB, 0x1D, 0x80, 0xBD, 0x80,
    };
    static const UInt8 kResolverAllowCtx_Sonoma[] = {
        0x95, 0xC1, 0x48, 0x8B, 0x9D, 0x78, 0xFE, 0xFF, 0xFF, 0x4A, 0x0F, 0xBA, 0x64, 0x33, 0x08, 0x22,
        0x0F, 0x93, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x70, 0xFE, 0xFF, 0xFF, 0x0F, 0xB6, 0xC0, 0x41,
    };
    // Mask stack displacements (bytes 5-8, 23-26) to wildcards for cross-minor Sonoma robustness
    static const UInt8 kResolverMask32_GateSonoma[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_AllowSonoma[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Sonoma[] = {
        {kResolverHwCtx_Sonoma, kResolverMask32_RipMasked, sizeof(kResolverHwCtx_Sonoma), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Sonoma, kResolverMask32, sizeof(kResolverDualCtx_Sonoma), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kResolverGateCtx_Sonoma, kResolverMask32_GateSonoma, sizeof(kResolverGateCtx_Sonoma), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverAllowCtx_Sonoma, kResolverMask32_AllowSonoma, sizeof(kResolverAllowCtx_Sonoma), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sonoma, sizeof(kMTLAllowFind_Sonoma)},
    };
    constexpr size_t kResolverSignatureCount_Sonoma = arrsize(kResolverSignatures_Sonoma);

    // macOS 15 (Sequoia) resolver contexts.
    static const UInt8 kResolverHwCtx[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x50, 0xFE, 0x0D, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x28, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x4C, 0x8B, 0x9D, 0x80, 0xFE, 0xFF, 0xFF, 0xEB, 0x2C, 0x83, 0xBD, 0x70,
    };
    static const UInt8 kResolverSetzCtx[] = {
        0xFF, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x49, 0x85, 0x54, 0x03, 0x08,
        0x0F, 0x94, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x88, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0xB5, 0x50,
    };
    static const UInt8 kResolverMask32_GateSequoia[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_SetzSequoia[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x00,
    };

    // Resolver fallback for the 15.8.1 GL/Metal sites.
    static const UserPatches::SignaturePatch kResolverSignatures[] = {
        {kResolverHwCtx, kResolverMask32_RipMasked, sizeof(kResolverHwCtx), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx, kResolverMask32, sizeof(kResolverDualCtx), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kResolverGateCtx, kResolverMask32_GateSequoia, sizeof(kResolverGateCtx), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverSetzCtx, kResolverMask32_SetzSequoia, sizeof(kResolverSetzCtx), 16, kMTLAllowReplace_Sequoia,
         sizeof(kMTLAllowReplace_Sequoia), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sequoia, sizeof(kMTLAllowFind_Sequoia)},
    };
    constexpr size_t kResolverSignatureCount = arrsize(kResolverSignatures);

    // macOS 26 (Tahoe) resolver contexts.
    // The rip-relative displacement at bytes 27-30 is masked to wildcards for forward-compatibility across minor builds.
    static const UInt8 kResolverMask32_HwTahoe[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF,
    };

    static const UInt8 kResolverMask32_GateTahoe[] = {
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00,
        0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
    };
    static const UInt8 kResolverMask32_AllowTahoe[] = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    };

    static const UInt8 kResolverHwCtx_Tahoe[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF8, 0x13, 0x77, 0x45, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x50, 0xF3, 0x0D, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Tahoe[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Tahoe[] = {
        0xFF, 0xFF, 0xF6, 0x81, 0xA0, 0x00, 0x00, 0x00, 0x02, 0x48, 0x8B, 0x9D, 0x70, 0xFE, 0xFF, 0xFF,
        0x74, 0x28, 0x80, 0xBD, 0x80, 0xFE, 0xFF, 0xFF, 0x00, 0x0F, 0x95, 0xC0, 0x48, 0x8B, 0x8D, 0x78,
    };
    static const UInt8 kResolverAllowCtx_Tahoe[] = {
        0xFF, 0x48, 0xBA, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x48, 0x85, 0x54, 0x0B, 0x08,
        0x0F, 0x94, 0xC1, 0x08, 0xC1, 0x0F, 0xB6, 0xC1, 0x41, 0x8B, 0x4C, 0x24, 0x28, 0x83, 0xE1, 0xFE,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Tahoe[] = {
        {kResolverHwCtx_Tahoe, kResolverMask32_HwTahoe, sizeof(kResolverHwCtx_Tahoe), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Tahoe, kResolverMask32, sizeof(kResolverDualCtx_Tahoe), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kResolverGateCtx_Tahoe, kResolverMask32_GateTahoe, sizeof(kResolverGateCtx_Tahoe), 16, kMTLGateReplace_Tahoe,
         sizeof(kMTLGateReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind_Tahoe, sizeof(kMTLGateFind_Tahoe)},
        {kResolverAllowCtx_Tahoe, kResolverMask32_AllowTahoe, sizeof(kResolverAllowCtx_Tahoe), 16, kMTLAllowReplace_Tahoe,
         sizeof(kMTLAllowReplace_Tahoe), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Tahoe, sizeof(kMTLAllowFind_Tahoe)},
    };
    constexpr size_t kResolverSignatureCount_Tahoe = arrsize(kResolverSignatures_Tahoe);

    // macOS 13 (Ventura) resolver contexts.
    static const UInt8 kResolverHwCtx_Ventura[] = {
        0xAC, 0x00, 0x00, 0x00, 0x41, 0x83, 0xF9, 0x13, 0x77, 0x4D, 0xC7, 0x07, 0x8D, 0x00, 0x00, 0x00,
        0xC7, 0x47, 0x78, 0x04, 0x00, 0x00, 0x00, 0xC5, 0xF8, 0x28, 0x05, 0x06, 0x0B, 0x0F, 0x00, 0xC5,
    };
    static const UInt8 kResolverDualCtx_Ventura[] = {
        0x88, 0xA0, 0x00, 0x00, 0x00, 0xD1, 0xE9, 0x83, 0xE1, 0x01, 0x8B, 0x83, 0x98, 0x13, 0x00, 0x00,
        0x83, 0xE0, 0xFE, 0x09, 0xC8, 0x89, 0x83, 0x98, 0x13, 0x00, 0x00, 0x48, 0x8B, 0x8B, 0x68, 0x0D,
    };
    static const UInt8 kResolverGateCtx_Ventura[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x40, 0xFE, 0xFF, 0xFF, 0xF6, 0x80, 0xA0, 0x00, 0x00, 0x00, 0x02,
        0x75, 0x0B, 0x31, 0xC0, 0x4C, 0x8B, 0xA5, 0x70, 0xFE, 0xFF, 0xFF, 0xEB, 0x24, 0x80, 0xBD, 0x80,
    };
    static const UInt8 kResolverAllowCtx_Ventura[] = {
        0xFF, 0xFF, 0x48, 0x8B, 0x85, 0x60, 0xFE, 0xFF, 0xFF, 0x49, 0x0F, 0xBA, 0x64, 0x04, 0x08, 0x22,
        0x0F, 0x93, 0xC0, 0x08, 0xC8, 0x4C, 0x8B, 0xB5, 0x50, 0xFE, 0xFF, 0xFF, 0x4C, 0x8B, 0x8D, 0x68,
    };

    static const UserPatches::SignaturePatch kResolverSignatures_Ventura[] = {
        {kResolverHwCtx_Ventura, kResolverMask32_RipMasked, sizeof(kResolverHwCtx_Ventura), 16, kGLDriverHwInfoReplace,
         sizeof(kGLDriverHwInfoReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverHwInfoFind, sizeof(kGLDriverHwInfoFind)},
        {kResolverDualCtx_Ventura, kResolverMask32, sizeof(kResolverDualCtx_Ventura), 16, kGLDriverDualQuadReplace,
         sizeof(kGLDriverDualQuadReplace), 0, 1, ~static_cast<UInt64>(0), kGLDriverDualQuadFind, sizeof(kGLDriverDualQuadFind)},
        {kResolverGateCtx_Ventura, kResolverMask32_GateSonoma, sizeof(kResolverGateCtx_Ventura), 16, kMTLGateReplace,
         sizeof(kMTLGateReplace), 0, 1, ~static_cast<UInt64>(0), kMTLGateFind, sizeof(kMTLGateFind)},
        {kResolverAllowCtx_Ventura, kResolverMask32_AllowSonoma, sizeof(kResolverAllowCtx_Ventura), 16, kMTLAllowReplace_Sonoma,
         sizeof(kMTLAllowReplace_Sonoma), 0, 1, ~static_cast<UInt64>(0), kMTLAllowFind_Sonoma, sizeof(kMTLAllowFind_Sonoma)},
    };
    constexpr size_t kResolverSignatureCount_Ventura = arrsize(kResolverSignatures_Ventura);

}    // namespace

UserPatches& UserPatches::singleton() { return moduleInstance; }

void UserPatches::init()
{
    this->enabled = kGLFixDefaultEnabled;
    if (checkKernelArgument(kBootArgNoGLFix) || checkKernelArgument(kBootArgGLFixOff)) {
        this->enabled = false;
    }

    // The resolver is a GL-only fallback: it must never be active when the GL fix itself is off.
    this->resolverOnly    = this->enabled && checkKernelArgument(kBootArgGLFixResolveOnly);
    this->resolverEnabled = this->enabled
                            && !checkKernelArgument(kBootArgGLFixResolveOff)
                            && !checkKernelArgument(kBootArgGLFixNoResolve);

    this->vdaEnabled = !checkKernelArgument(kBootArgNoVDA);

    if (!this->enabled && !this->vdaEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    if (this->enabled) {
        SYSLOG("UserPatch", "Enabling user-space GL driver patches (experimental)");
        if (this->resolverEnabled) {
            SYSLOG("UserPatch", "Resolver fallback enabled%s", this->resolverOnly ? " (resolver-only QA mode)" : "");
        }
    }
    if (this->vdaEnabled) {
        if (getKernelVersion() == KernelVersion::Ventura) {
            this->vdaSignatures     = kVDAResolverSignatures_Ventura;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Ventura;
            this->gvaSignature      = &kGVAResolverSignature_Ventura;
        } else if (getKernelVersion() == KernelVersion::Sonoma) {
            this->vdaSignatures     = kVDAResolverSignatures_Sonoma;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Sonoma;
            this->gvaSignature      = &kGVAResolverSignature_Sonoma;
        } else if (getKernelVersion() >= KernelVersion::Tahoe) {
            this->vdaSignatures     = kVDAResolverSignatures_Modern;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Modern;
            this->gvaSignature      = &kGVAResolverSignature_Tahoe;
        } else {
            this->vdaSignatures     = kVDAResolverSignatures_Modern;
            this->vdaSignatureCount = kVDAResolverSignatureCount_Modern;
            this->gvaSignature      = &kGVAResolverSignature_Sequoia;
        }
        for (auto& applied : this->vdaSignatureApplied) { applied = 0; }
        this->gvaSignatureApplied = 0;
        SYSLOG("UserPatch", "Enabling user-space VDA driver patches with dynamic Resolver fallback");
    }
}

auto UserPatches::currentApuMask() const -> ApuMask
{
    const auto& attributes = NRed::singleton().getAttributes();

    UInt8 mask = 0;
    if (attributes.isRaven2()) { mask |= kApuRaven2; }
    if (attributes.isPicasso()) { mask |= kApuPicasso; }
    if (attributes.isRenoir()) { mask |= kApuRenoir; }
    if (attributes.isRenoirE()) { mask |= kApuRenoirE; }
    if (attributes.isGreenSardine()) { mask |= kApuGreenSardine; }

    // NRed leaves the attributes empty for the base Raven (0x15DD); model that explicitly.  A zero
    // device ID means NRed has not populated its device information yet, so return an unknown mask and
    // let the selector use the generic fallback rather than guessing a family.
    if (mask == 0) {
        if (NRed::singleton().getDeviceID() == 0x15DD) { mask = kApuRaven; }
        else if (NRed::singleton().getDeviceID() == 0) { return static_cast<ApuMask>(0); }
    }

    return static_cast<ApuMask>(mask);
}

void UserPatches::selectPatchSet()
{
    if (getKernelVersion() >= KernelVersion::Tahoe) {
        SYSLOG("UserPatch", "Selecting macOS 26 (Tahoe) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Tahoe;
        this->signatureCount = kResolverSignatureCount_Tahoe;
    } else if (getKernelVersion() >= KernelVersion::Sequoia) {
        SYSLOG("UserPatch", "Selecting macOS 15 (Sequoia) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures;
        this->signatureCount = kResolverSignatureCount;
    } else if (getKernelVersion() >= KernelVersion::Sonoma) {
        SYSLOG("UserPatch", "Selecting macOS 14 (Sonoma) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Sonoma;
        this->signatureCount = kResolverSignatureCount_Sonoma;
    } else {
        SYSLOG("UserPatch", "Selecting macOS 13 (Ventura) GL/Metal resolver signatures");
        this->signatures     = kResolverSignatures_Ventura;
        this->signatureCount = kResolverSignatureCount_Ventura;
    }
    for (auto& applied : this->signatureApplied) { applied = 0; }
}

void UserPatches::processPatcher(KernelPatcher& patcher)
{
    if (!this->enabled && !this->vdaEnabled) {
        DBGLOG("UserPatch", "User-space driver patches are disabled");
        return;
    }

    const auto* const profile = DeviceProfile::current();
    auto              glEnabled = this->enabled;
    if (profile != nullptr) {
        SYSLOG("UserPatch", "Device profile: %s (family %s, level %s)", profile->name,
               DeviceProfile::familyName(profile->family), DeviceProfile::levelName(profile->level));
        if (!profile->graphics.conditionalDualQuad) {
            SYSLOG("UserPatch", "Graphics policy disables the conditional dual-quad fix for this profile");
            glEnabled = false;
        }
    }

    if (glEnabled) {
        this->selectPatchSet();
    } else {
        this->resolverEnabled = false;
        this->signatures      = nullptr;
        this->signatureCount  = 0;
    }

    const auto vdaAllowed = this->vdaEnabled && profile != nullptr && profile->media.hasVCN0
                            && profile->media.vcnGeneration == DeviceProfile::kVcn2_2
                            && !NRed::singleton().hasExternalVideo();
    if (this->vdaEnabled && !vdaAllowed) {
        SYSLOG("UserPatch", "VDA patches requested but this profile does not enable the validated VCN 2.x target");
        this->vdaEnabled = false;
    }

    if (currentKernelVersion() >= MACOS_11) {
        if (!glEnabled && !this->vdaEnabled) {
            SYSLOG("UserPatch", "No user-space driver patches are configured, not installing any hooks");
            return;
        }

        SYSLOG_COND(!this->installNativePagePatcher(patcher), "UserPatch",
                    "Failed to install the dynamic signature patch backend");
        return;
    }
}

auto UserPatches::installNativePagePatcher(KernelPatcher& patcher) -> bool
{
    KernelPatcher::RouteRequest pageRequest{"_cs_validate_page", wrapCsValidatePage, this->orgCsValidatePage};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &pageRequest, 1)) {
        SYSLOG("UserPatch", "Failed to route _cs_validate_page (%d)", static_cast<int>(patcher.getError()));
        patcher.clearError();
        return false;
    }

    KernelPatcher::RouteRequest rangeRequest{"_cs_validate_range", wrapCsValidateRange, this->orgCsValidateRange};
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, &rangeRequest, 1)) {
        DBGLOG("UserPatch", "Failed to route _cs_validate_range, continuing without it (%d)",
               static_cast<int>(patcher.getError()));
        patcher.clearError();
    }

    SYSLOG("UserPatch", "Installed the dynamic signature patch backend");
    return true;
}

static bool matchSignature(const UInt8* const bytes, const UserPatches::SignaturePatch& sig, size_t& matchOffset, size_t startOffset = 0)
{
    if (sig.context == nullptr || sig.mask == nullptr || sig.replace == nullptr) { return false; }
    if (sig.contextSize == 0 || sig.contextSize > UserPatches::kPageSize) { return false; }
    if (sig.patchOffset + sig.replaceSize > sig.contextSize) { return false; }

    size_t skipped = 0;
    for (size_t offset = startOffset; offset + sig.contextSize <= UserPatches::kPageSize; offset++) {
        bool matches = true;
        for (size_t index = 0; index < sig.contextSize; index++) {
            if ((bytes[offset + index] & sig.mask[index]) != (sig.context[index] & sig.mask[index])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            if (sig.find != nullptr && sig.findSize > 0) {
                if (offset + sig.patchOffset + sig.findSize > UserPatches::kPageSize ||
                    memcmp(bytes + offset + sig.patchOffset, sig.find, sig.findSize) != 0) {
                    continue;
                }
            }

            if (skipped < sig.skip) {
                skipped++;
                continue;
            }
            matchOffset = offset;
            return true;
        }
    }
    return false;
}

void UserPatches::patchSignaturePage(const void* const data, const UInt64 pageOffset, void* const vnode)
{
    if (data == nullptr || vnode == nullptr) {
        return;
    }

    // Gate 1: Vnode verification.
    // Ensure the page strictly belongs to graphics/media drivers or the dyld shared cache.
    // Never scan or modify random user binaries or daemons (such as NotificationCenter).
    const char* const vnodeName = vnode_getname(reinterpret_cast<vnode_t>(vnode));
    if (vnodeName == nullptr) {
        return;
    }

    const bool isGLDriver    = (strstr(vnodeName, "AMDRadeonX5000GLDriver") != nullptr);
    const bool isVADriver    = (strstr(vnodeName, "AMDRadeonVADriver") != nullptr);
    const bool isAppleGVA    = (strstr(vnodeName, "AppleGVA") != nullptr);
    const bool isSharedCache = (strstr(vnodeName, "dyld_shared_cache") != nullptr) ||
                               (strstr(vnodeName, "dyld") != nullptr);
    vnode_putname(vnodeName);

    if (!isGLDriver && !isVADriver && !isAppleGVA && !isSharedCache) {
        return;
    }

    const auto* const bytes = static_cast<const UInt8*>(data);

    // VDA Dynamic Resolver path (AMDRadeonVADriver2: embedded in dyld_shared_cache or loose bundle)
    // Note: _cs_validate_page is invoked by XNU only once per physical page when the page enters the
    // global shared-cache vm_object. Do not gate on per-process identity (e.g. WindowServer or BootCache)
    // or accelerator start timing, as skipping a page during its first fault caches it unpatched globally.
    if ((isVADriver || isSharedCache) && this->vdaEnabled && this->vdaSignatures != nullptr && this->vdaSignatureCount > 0) {
        {
            bool vdaAllApplied = true;
            for (size_t i = 0; i < this->vdaSignatureCount && i < kMaxVDASignatures; ++i) {
                if (this->vdaSignatureApplied[i] < this->vdaSignatures[i].count) {
                    vdaAllApplied = false;
                    break;
                }
            }

            if (!vdaAllApplied) {
                for (size_t sigIdx = 0; sigIdx < this->vdaSignatureCount; sigIdx++) {
                    const auto& sig = this->vdaSignatures[sigIdx];
                    if (sigIdx < kMaxVDASignatures && this->vdaSignatureApplied[sigIdx] >= sig.count) {
                        continue;
                    }

                    size_t matchOffset = 0;
                    size_t searchOffset = 0;
                    while (sigIdx < kMaxVDASignatures && this->vdaSignatureApplied[sigIdx] < sig.count &&
                           matchSignature(bytes, sig, matchOffset, searchOffset)) {
                        // Layer 4: Page boundary verification
                        if (matchOffset + sig.patchOffset + sig.replaceSize > kPageSize) {
                            SYSLOG("UserPatch", "Resolver: VDA patch out of page bounds (0x%zX + 0x%zX + 0x%zX)",
                                   matchOffset, sig.patchOffset, sig.replaceSize);
                            break;
                        }

                        if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                            SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (VDA)");
                            return;
                        }

                        memcpy(const_cast<UInt8*>(bytes) + matchOffset + sig.patchOffset, sig.replace, sig.replaceSize);

                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                        SYSLOG("UserPatch", "Resolver patched VDA signature %zu (site %zu/%zu) at page offset 0x%zX",
                               sigIdx, this->vdaSignatureApplied[sigIdx] + 1, sig.count, matchOffset);
                        if (sigIdx < kMaxVDASignatures) {
                            this->vdaSignatureApplied[sigIdx]++;
                        }
                        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
                        NRed::singleton().setProp32("NRedVCNUserApplied", applied);

                        searchOffset = matchOffset + 1;
                    }
                }
            }
        }
        if (isVADriver && !isSharedCache) {
            return;
        }
    }

    // AppleGVA Dynamic Resolver path (embedded in dyld_shared_cache or standalone AppleGVA bundle)
    if ((isAppleGVA || isSharedCache) && this->vdaEnabled && this->gvaSignature != nullptr &&
        this->gvaSignatureApplied < this->gvaSignature->count) {
        {
            size_t matchOffset = 0;
            if (matchSignature(bytes, *this->gvaSignature, matchOffset)) {
                if (matchOffset + this->gvaSignature->patchOffset + this->gvaSignature->replaceSize <= kPageSize) {
                    if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) == KERN_SUCCESS) {
                        memcpy(const_cast<UInt8*>(bytes) + matchOffset + this->gvaSignature->patchOffset,
                               this->gvaSignature->replace, this->gvaSignature->replaceSize);
                        MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);
                        this->gvaSignatureApplied++;
                        const auto applied = static_cast<UInt32>(OSIncrementAtomic(&this->vdaAppliedCount) + 1);
                        NRed::singleton().setProp32("NRedVCNUserApplied", applied);
                        SYSLOG("UserPatch", "Resolver patched AppleGVA signature at page offset 0x%zX", matchOffset);
                    } else {
                        SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (AppleGVA)");
                    }
                }
            }
        }
        if (isAppleGVA && !isSharedCache) {
            return;
        }
    }

    if (!this->resolverEnabled || this->signatures == nullptr || this->signatureCount == 0) {
        return;
    }

    bool allApplied = true;
    for (size_t i = 0; i < this->signatureCount && i < kMaxSignatures; ++i) {
        if (this->signatureApplied[i] < this->signatures[i].count) {
            allApplied = false;
            break;
        }
    }
    if (allApplied) { return; }

    // Gate 2: GL patches ONLY apply to AMDRadeonX5000GLDriver; Metal patches ONLY apply to dyld shared cache.
    if (isGLDriver) {
        for (size_t signatureIndex = 0; signatureIndex < 2 && signatureIndex < this->signatureCount; signatureIndex++) {
            const auto& signature = this->signatures[signatureIndex];
            if (signatureIndex < kMaxSignatures && this->signatureApplied[signatureIndex] >= signature.count) {
                continue;
            }

            size_t matchOffset = 0;
            if (matchSignature(bytes, signature, matchOffset)) {
                if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                    SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (GL)");
                    return;
                }

                memcpy(const_cast<UInt8*>(bytes) + matchOffset + signature.patchOffset, signature.replace,
                       signature.replaceSize);

                MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                SYSLOG("UserPatch", "Resolver patched GL signature %zu at page offset 0x%zX", signatureIndex, matchOffset);
                if (signatureIndex < kMaxSignatures) {
                    this->signatureApplied[signatureIndex]++;
                }
                static volatile UInt32 glResolverCounter{0};
                const auto glCount = static_cast<UInt32>(OSIncrementAtomic(&glResolverCounter) + 1);
                NRed::singleton().setProp32("NRedGLResolverApplied", glCount);
            }
        }
    } else if (isSharedCache) {
        // Gate 3: For Metal signatures in dyld shared cache, require co-occurrence of Gate and Allow
        // on the exact same 4KB page, within a sane relative distance (< 0x500 bytes).
        // Signatures index 2 & 3 in all tables (Sonoma, Sequoia, Tahoe) represent the Gate and Allow pair.
        if (this->signatureCount >= 4) {
            const auto& gateSig = this->signatures[2];
            const auto& allowSig = this->signatures[3];

            if (gateSig.pageOffset == ~static_cast<UInt64>(0) && allowSig.pageOffset == ~static_cast<UInt64>(0)) {
                const bool needGate = (2 >= kMaxSignatures || this->signatureApplied[2] < gateSig.count);
                const bool needAllow = (3 >= kMaxSignatures || this->signatureApplied[3] < allowSig.count);

                if (needGate && needAllow) {
                    size_t gateMatch = 0;
                    size_t allowMatch = 0;
                    if (matchSignature(bytes, gateSig, gateMatch) && matchSignature(bytes, allowSig, allowMatch)) {
                        long diff = static_cast<long>(allowMatch) - static_cast<long>(gateMatch);
                        if (diff < 0) { diff = -diff; }

                        if (diff < 0x500) {
                            if (MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock) != KERN_SUCCESS) {
                                SYSLOG("UserPatch", "Resolver: failed to disable kernel write protection (Metal)");
                                return;
                            }

                            memcpy(const_cast<UInt8*>(bytes) + gateMatch + gateSig.patchOffset, gateSig.replace,
                                   gateSig.replaceSize);
                            memcpy(const_cast<UInt8*>(bytes) + allowMatch + allowSig.patchOffset, allowSig.replace,
                                   allowSig.replaceSize);

                            MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

                            SYSLOG("UserPatch", "Resolver matched and patched Metal Gate (0x%zX) and Allow (0x%zX) atomically",
                                   gateMatch, allowMatch);
                            if (2 < kMaxSignatures) { this->signatureApplied[2]++; }
                            if (3 < kMaxSignatures) { this->signatureApplied[3]++; }

                            static volatile UInt32 blendResolverCounter{0};
                            const auto count = static_cast<UInt32>(OSIncrementAtomic(&blendResolverCounter) + 1);
                            NRed::singleton().setProp32("NRedLegacyBlendApplied", count);
                        }
                    }
                }
            }
        }
    }
}

void UserPatches::wrapCsValidatePage(void* const vnode, void* const pager, const UInt64 pageOffset,
                                     const void* const data, int* const validated, int* const tainted, int* const nx)
{
    FunctionCast(wrapCsValidatePage, singleton().orgCsValidatePage)(vnode, pager, pageOffset, data, validated, tainted,
                                                                    nx);

    // Only patch pages that passed validation, so that a rejected page is never turned into a valid-looking one.
    if (validated != nullptr && *validated != 0) { singleton().patchSignaturePage(data, pageOffset, vnode); }
}

boolean_t UserPatches::wrapCsValidateRange(void* const vnode, void* const pager, const UInt64 pageOffset,
                                           const void* const data, const UInt64 dataSize, unsigned* const tainted)
{
    const auto result = FunctionCast(wrapCsValidateRange, singleton().orgCsValidateRange)(vnode, pager, pageOffset,
                                                                                          data, dataSize, tainted);

    if (result == 0 || data == nullptr) { return result; }

    const auto* const bytes = static_cast<const UInt8*>(data);
    for (size_t offset = 0; offset + kPageSize <= dataSize; offset += kPageSize) {
        singleton().patchSignaturePage(bytes + offset, pageOffset + offset, vnode);
    }

    return result;
}
